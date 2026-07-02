// SPDX-License-Identifier: ISC
/*
 * clm-tui -- an ncurses frontend for libclm.
 *
 * Design: libclm callbacks fire on the uv loop thread and MUST NOT block or do
 * heavy work (see the contract in clm/clm.h). So every callback here only
 * mutates a small UI model -- appending styled text segments to a queue and
 * updating status fields. A single uv_timer owns ALL drawing: it drains the
 * segment queue into a scrolling transcript window and repaints the status and
 * input lines. Terminal input is read through uv_poll on stdin, so ncurses
 * shares the caller's event loop rather than blocking in getch().
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

#include <curses.h>
#include <json-c/json.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/host_uv.h"
#include "frontend.h"
#include "md_render.h"

#ifdef CLM_LUA
#include "clm/lua_plugin.h"
#endif

/* Style buckets, mapped to curses attributes in seg_attr(). */
enum ui_style {
	ST_NORMAL,
	ST_USER,     /* the user's prompt echo */
	ST_ASSIST,   /* assistant answer text */
	ST_LABEL,    /* the "clm>" answer label */
	ST_PERM,     /* tool-permission prompt (orange) */
	ST_REASON,   /* dim "thinking" channel */
	ST_TOOL,     /* tool invocation summary line */
	ST_TOOL_OUT, /* tool output body (collapsible) */
	ST_ERROR,    /* failures */
	ST_TIMEOUT,  /* timed-out tool */
	ST_META,     /* dim meta notes */
};

/* One styled run of transcript source text (queued by a callback). */
struct seg {
	enum ui_style style;
	char *text;
};

/* One run of rendered text with a resolved curses attribute, ready to draw. */
struct rseg {
	int attr;
	char *text;
};

struct ui {
	uv_loop_t *loop;
	struct clm_host *host;
	struct clm_agent *agent;
#ifdef CLM_LUA
	struct clm_lua_env *lua_env;
	struct clm_lua_cfg *lcfg;      /* kept alive for /agent switching */
	const char *plugin_dir;        /* NULL = use XDG default */
#endif

	uv_poll_t stdin_poll;
	uv_timer_t repaint;
	uv_timer_t health;
	uv_signal_t winch;

	WINDOW *txt;  /* scrolling transcript */
	WINDOW *stat; /* status bar */
	WINDOW *in;   /* input box (grows upward as text wraps) */
	int in_h;     /* current input box height in rows */

	/* Transcript source: an accumulating list of styled spans (text may
	 * contain newlines; ST_ASSIST spans hold raw markdown). */
	struct seg *segs;
	size_t nsegs, cap_segs;
	unsigned gen; /* bumped whenever the source list changes */

	/* Render cache: source resolved to (curses-attr, text) runs -- markdown
	 * expanded, styles flattened. Rebuilt when gen or width changes, then
	 * wrapped into the viewport. */
	struct rseg *rsegs;
	size_t nrsegs, cap_rsegs;
	unsigned built_gen;
	int built_width;

	size_t
	    scroll; /* wrapped rows scrolled up from the bottom; 0 = follow */

	/* Input line editor (byte buffer, UTF-8; cursor is a byte offset). */
	char input[1024];
	size_t input_len;
	size_t input_pos;
	char kill[1024];
	size_t kill_len;

	/* Tab-completion state: cached candidates and cycling index. */
	char **complete_candidates;
	size_t complete_cap, complete_n, complete_idx;

	/* Status model. */
	const char *model;
	char *agent_name; /* displayed in status bar (owned) */
	enum clm_agent_state state;
	enum clm_conn_status conn;
	char conn_detail[64];
	char usage[96];
	int64_t ctx_used; /* tokens carried forward, for the context gauge */
	char batch[64];
	int spinner;
	bool busy;           /* a turn is in flight */
	bool started_assist; /* assistant text seen this turn */

	/* Permission prompt queue: requests arrive in bursts (batched tool
	 * calls) and we present them one at a time. */
	const struct clm_permission_req **perm_queue;
	size_t perm_count;
	size_t perm_cap;
	bool perm_showing; /* true when a prompt is displayed awaiting input */

	/* Tool-batch tally, for the aggregate summary line. */
	int n_cmd, n_read, n_write, n_other;

	bool
	    expand_output; /* show full tool output vs. an ellipsized preview */
	bool built_expand; /* expand_output value the cache was built with */
	bool show_reasoning; /* render the dim "thinking" channel */
	bool
	    built_reasoning; /* show_reasoning value the cache was built with */

	/* Prompts typed while a turn is in flight, run FIFO on turn-done. */
	char **queue;
	size_t nqueue, cap_queue;

	/*
	 * Submitted-prompt history for Up/Down recall. hist_pos == nhist means
	 * "the live line"; browsing up saves the live line in hist_saved so
	 * Down past the newest entry restores it (readline-style).
	 */
	char **hist;
	size_t nhist, cap_hist;
	size_t hist_pos;
	char hist_saved[1024];

	bool dirty;       /* repaint requested */
	bool full_redraw; /* force an artifact-free repaint (resize / ^L) */
	bool quit;
};

/* ---- UI model mutation (called from libclm callbacks; keep cheap) ---- */

static void
ui_push(struct ui *u, enum ui_style style, const char *text)
{
	if (text == NULL || *text == '\0')
		return;

	/* A markdown reparse (gen bump) only makes sense at a line boundary --
	 * a partial trailing line ("**bol", an unfinished table row) must not be
	 * rendered until it completes. So bump gen (triggering rebuild_render)
	 * only when this chunk closes a line; otherwise just append and mark
	 * dirty, and the last complete render stays on screen. Turn-end pushes a
	 * trailing newline, which flushes the final line. */
	bool has_nl = strchr(text, '\n') != NULL;

	/* Coalesce consecutive same-style pushes into one span. This keeps the
	 * span count small under streaming (many tiny chunks) and, crucially,
	 * rejoins a multibyte char that was split across two stream chunks. */
	if (u->nsegs > 0 && u->segs[u->nsegs - 1].style == style) {
		char *old = u->segs[u->nsegs - 1].text;
		size_t oldlen = strlen(old);
		size_t addlen = strlen(text);
		char *p = realloc(old, oldlen + addlen + 1);
		if (p == NULL)
			return;
		memcpy(p + oldlen, text, addlen + 1);
		u->segs[u->nsegs - 1].text = p;
		if (has_nl)
			u->gen++;
		u->dirty = true;
		return;
	}

	if (u->nsegs == u->cap_segs) {
		size_t ncap = u->cap_segs ? u->cap_segs * 2 : 32;
		struct seg *p = realloc(u->segs, ncap * sizeof(*p));
		if (p == NULL)
			return; /* drop the segment rather than crash */
		u->segs = p;
		u->cap_segs = ncap;
	}
	char *dup = strdup(text);
	if (dup == NULL)
		return;
	u->segs[u->nsegs].style = style;
	u->segs[u->nsegs].text = dup;
	u->nsegs++;
	/* A new span of a different style closes the previous line's context;
	 * bump gen so the switch is rendered even without an embedded newline. */
	u->gen++;
	u->dirty = true;
}

static void
ui_set_state(struct ui *u, enum clm_agent_state st)
{
	u->state = st;
	u->dirty = true;
}

static void drain_queue(struct ui *u); /* runs the next queued prompt */

/* ---- libclm callbacks ---- */

static void
cb_assistant_text(const char *text, void *user)
{
	struct ui *u = user;
	if (!u->started_assist) {
		ui_push(u, ST_LABEL, "\nclm>\n");
		u->started_assist = true;
	}
	ui_push(u, ST_ASSIST, text);
}

static void
cb_reasoning(const char *text, void *user)
{
	ui_push(user, ST_REASON, text);
}

/* Extract a string field from a JSON args object, or NULL. Caller must not
 * free the result; it is owned by (and valid for the life of) obj. */
static const char *
json_field(struct json_object *obj, const char *key)
{
	struct json_object *v;

	if (obj == NULL || !json_object_object_get_ex(obj, key, &v) ||
	    !json_object_is_type(v, json_type_string))
		return NULL;
	return json_object_get_string(v);
}

/* Push a one-line, human-readable summary of a tool call -- never raw JSON.
 * The detail is flattened (no newlines/tabs) and clamped so one call is one
 * row; an over-long detail gets an ellipsis. */
static void
push_tool_summary(struct ui *u, const char *verb, const char *detail)
{
	char line[200];
	size_t n = 0;
	size_t cap = sizeof(line) - 5; /* room for "...\n\0" */

	line[n++] = ' ';
	line[n++] = ' ';
	for (const char *p = verb; *p != '\0' && n < cap; p++)
		line[n++] = *p;
	if (detail != NULL && *detail != '\0' && n < cap) {
		const char *p = detail;
		line[n++] = ':';
		line[n++] = ' ';
		while (*p != '\0' && n < cap) {
			line[n++] = (*p == '\n' || *p == '\t') ? ' ' : *p;
			p++;
		}
		if (*p != '\0') { /* truncated */
			line[n++] = '.';
			line[n++] = '.';
			line[n++] = '.';
		}
	}
	line[n++] = '\n';
	line[n] = '\0';
	ui_push(u, ST_TOOL, line);
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	struct ui *u = user;
	struct json_object *obj = args ? json_tokener_parse(args) : NULL;

	if (strcmp(name, "shell_exec") == 0) {
		push_tool_summary(u, "executing shell command",
		                  json_field(obj, "command"));
		u->n_cmd++;
	} else if (strcmp(name, "read_file") == 0) {
		push_tool_summary(u, "reading file", json_field(obj, "path"));
		u->n_read++;
	} else if (strcmp(name, "write_file") == 0) {
		push_tool_summary(u, "writing file", json_field(obj, "path"));
		u->n_write++;
	} else {
		push_tool_summary(u, "calling tool", name);
		u->n_other++;
	}
	json_object_put(obj);
}

static void
cb_tool_result(const char *name, const char *content,
               enum clm_tool_outcome outcome, void *user)
{
	struct ui *u = user;
	size_t clen;
	(void)name;
	switch (outcome) {
	case CLM_TOOL_OK:
		ui_push(u, ST_TOOL_OUT, content);
		/* Ensure exactly one trailing newline (don't double-count). */
		clen = content ? strlen(content) : 0;
		if (clen == 0 || content[clen - 1] != '\n')
			ui_push(u, ST_TOOL_OUT, "\n");
		break;
	case CLM_TOOL_FAILED:
		ui_push(u, ST_ERROR, "[x ");
		ui_push(u, ST_ERROR, name);
		ui_push(u, ST_ERROR, "] ");
		ui_push(u, ST_ERROR, content);
		ui_push(u, ST_ERROR, "\n");
		break;
	case CLM_TOOL_TIMEDOUT:
		ui_push(u, ST_TIMEOUT, "[timeout ");
		ui_push(u, ST_TIMEOUT, name);
		ui_push(u, ST_TIMEOUT, "] ");
		ui_push(u, ST_TIMEOUT, content);
		ui_push(u, ST_TIMEOUT, "\n");
		break;
	}
}

/* Emit "ran 2 commands, read 3 files, ..." from the batch tally. */
static void
push_batch_summary(struct ui *u)
{
	char line[128];
	size_t n = 0;
	int parts = 0;
	const struct {
		int count;
		const char *one, *many;
	} cats[] = {
	    {u->n_cmd, "ran 1 command", "commands"},
	    {u->n_read, "read 1 file", "files"},
	    {u->n_write, "wrote 1 file", "files"},
	    {u->n_other, "called 1 tool", "tools"},
	};
	static const char *verb[] = {"ran", "read", "wrote", "called"};

	n += (size_t)snprintf(line, sizeof(line), "  -- ");
	for (size_t i = 0; i < sizeof(cats) / sizeof(cats[0]); i++) {
		if (cats[i].count == 0)
			continue;
		if (parts++ > 0 && n < sizeof(line) - 4)
			n += (size_t)snprintf(line + n, sizeof(line) - n, ", ");
		if (cats[i].count == 1)
			n += (size_t)snprintf(line + n, sizeof(line) - n, "%s",
			                      cats[i].one);
		else
			n += (size_t)snprintf(line + n, sizeof(line) - n,
			                      "%s %d %s", verb[i],
			                      cats[i].count, cats[i].many);
	}
	if (parts == 0)
		return;
	if (n < sizeof(line) - 1)
		snprintf(line + n, sizeof(line) - n, "\n");
	ui_push(u, ST_META, line);
}

static void
cb_tool_batch(size_t completed, size_t total, void *user)
{
	struct ui *u = user;

	if (completed == 0) { /* batch starting: reset the tally */
		u->n_cmd = u->n_read = u->n_write = u->n_other = 0;
	}
	if (completed >= total) { /* batch done: emit the aggregate line */
		push_batch_summary(u);
		u->batch[0] = '\0';
	} else {
		snprintf(u->batch, sizeof(u->batch), "tools %zu/%zu", completed,
		         total);
	}
	u->dirty = true;
}

static void
cb_finish_reason(enum clm_finish_reason reason, void *user)
{
	struct ui *u = user;
	if (reason == CLM_FINISH_LENGTH)
		ui_push(u, ST_META, "\n[truncated: hit token limit]\n");
	else if (reason == CLM_FINISH_CONTENT_FILTER)
		ui_push(u, ST_META, "\n[stopped: content filter]\n");
}

static void
cb_usage(const struct clm_usage *usage, void *user)
{
	struct ui *u = user;

	/* Tokens carried into the next turn's prompt; drives the context gauge
	 * drawn in the status bar. */
	u->ctx_used = (int64_t)usage->prompt_tokens + usage->completion_tokens;

	if (usage->tokens_per_sec > 0)
		snprintf(u->usage, sizeof(u->usage), "%.0f tok/s",
		         usage->tokens_per_sec);
	else
		u->usage[0] = '\0';
	u->dirty = true;
}

static void
cb_state(enum clm_agent_state st, void *user)
{
	ui_set_state(user, st);
}

static void
cb_connection(enum clm_conn_status status, const char *detail, void *user)
{
	struct ui *u = user;
	u->conn = status;
	if (detail != NULL)
		snprintf(u->conn_detail, sizeof(u->conn_detail), "%s", detail);
	else
		u->conn_detail[0] = '\0';
	u->dirty = true;
}

static void
cb_turn_done(int status, void *user)
{
	struct ui *u = user;
	/* -ECANCELED is a user Escape; we already showed "[cancelled]". */
	if (status != 0 && status != -ECANCELED) {
		ui_push(u, ST_ERROR, "\nerror: ");
		ui_push(u, ST_ERROR, clm_agent_get_last_error(u->agent));
		/* A failed turn may mean the server went away; re-probe. */
		clm_agent_check_connection(u->agent);
	}
	ui_push(u, ST_NORMAL, "\n");
	u->busy = false;
	u->perm_count = 0;    /* no prompt outlives its turn */
	u->perm_showing = false;
	u->batch[0] = '\0';
	u->dirty = true;
	drain_queue(u); /* start the next queued prompt, if any */
}

/*
 * Render tool-call args for the permission prompt without the JSON envelope:
 * a single string arg shows as just its value (e.g. shell_exec's command);
 * multiple args show as "key: value" pairs. Falls back to the raw string if
 * it is not a JSON object.
 */
static void
push_perm_args(struct ui *u, const char *args)
{
	json_object *obj, *v;
	bool first = true;

	if (args == NULL || args[0] == '\0' || strcmp(args, "{}") == 0)
		return;

	obj = json_tokener_parse(args);
	if (obj == NULL || json_object_get_type(obj) != json_type_object) {
		ui_push(u, ST_PERM, ": ");
		ui_push(u, ST_PERM, args); /* not an object: show as-is */
		if (obj != NULL)
			json_object_put(obj);
		return;
	}

	ui_push(u, ST_PERM, ": ");
	{
		int n = json_object_object_length(obj);
		json_object_object_foreach(obj, key, val) {
			if (!first)
				ui_push(u, ST_PERM, "  ");
			first = false;
			/* For a lone arg, the key is noise; show only the value. */
			if (n > 1) {
				ui_push(u, ST_PERM, key);
				ui_push(u, ST_PERM, ": ");
			}
			v = val;
			ui_push(u, ST_PERM,
			    json_object_get_type(v) == json_type_string
			        ? json_object_get_string(v)
			        : json_object_to_json_string(v));
		}
	}
	json_object_put(obj);
}

/*
 * Permission handler: render an orange prompt showing the tool and its args,
 * then park in permission-input mode. handle_keys routes the next y/n/a/d key
 * (or Escape) to answer via clm_tool_permission_respond.
 */
/*
 * Show the next permission prompt from the queue (if any).
 */
static void
show_next_perm(struct ui *u)
{
	const struct clm_permission_req *req;
	const char *name, *args;

	if (u->perm_count == 0) {
		u->perm_showing = false;
		return;
	}
	req = u->perm_queue[0];
	name = clm_permission_req_name(req);
	args = clm_permission_req_args(req);

	u->perm_showing = true;
	ui_push(u, ST_PERM, "\nallow tool ");
	ui_push(u, ST_PERM, name ? name : "?");
	push_perm_args(u, args);
	ui_push(u, ST_PERM,
	    "\n(y) once  (n) deny  (a) always  (d) never  [esc = deny+cancel]\n");
	u->dirty = true;
}

static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct ui *u = user;

	/* Enqueue the request. */
	if (u->perm_count >= u->perm_cap) {
		size_t ncap = u->perm_cap ? u->perm_cap * 2 : 16;
		const struct clm_permission_req **nq = realloc(u->perm_queue,
		    ncap * sizeof(*nq));
		if (nq == NULL)
			return; /* drop on OOM; tool will time out */
		u->perm_queue = nq;
		u->perm_cap = ncap;
	}
	u->perm_queue[u->perm_count++] = req;

	/* If no prompt is currently showing, display this one. */
	if (!u->perm_showing)
		show_next_perm(u);
}

static const struct clm_callbacks tui_callbacks = {
    .on_assistant_text = cb_assistant_text,
    .on_reasoning = cb_reasoning,
    .on_tool_begin = cb_tool_begin,
    .on_permission = cb_permission,
    .on_tool_result = cb_tool_result,
    .on_tool_batch = cb_tool_batch,
    .on_finish_reason = cb_finish_reason,
    .on_usage = cb_usage,
    .on_connection = cb_connection,
    .on_state = cb_state,
    .on_turn_done = cb_turn_done,
};

/* ---- rendering (all drawing happens here, from the repaint timer) ---- */

static int
seg_attr(enum ui_style style)
{
	switch (style) {
	case ST_USER:
		return COLOR_PAIR(1);
	case ST_ASSIST:
		return A_NORMAL;
	case ST_LABEL:
		return COLOR_PAIR(8);
	case ST_PERM:
		return COLOR_PAIR(9) | A_BOLD;
	case ST_REASON:
		return COLOR_PAIR(6);
	case ST_TOOL:
		return COLOR_PAIR(2);
	case ST_TOOL_OUT:
		return A_DIM;
	case ST_ERROR:
		return COLOR_PAIR(3);
	case ST_TIMEOUT:
		return COLOR_PAIR(4);
	case ST_META:
		return A_DIM;
	case ST_NORMAL:
		break;
	}
	return A_NORMAL;
}

static const char *
state_label(enum clm_agent_state st)
{
	switch (st) {
	case CLM_STATE_IDLE:
		return "idle";
	case CLM_STATE_THINKING:
		return "thinking";
	case CLM_STATE_CALLING_TOOL:
		return "tool";
	case CLM_STATE_COMPLETE:
		return "done";
	case CLM_STATE_ERROR:
		return "error";
	}
	return "?";
}

/*
 * Fill a buffer with a block-glyph context gauge like "[████░░░░] 40%" from
 * the tokens carried forward vs the backend's context window. Writes an empty
 * string when the window is unknown (non-llama.cpp, or /props not yet seen).
 * GAUGE_CELLS filled/empty cells use U+2588 / U+2591.
 */
#define GAUGE_CELLS 10
static void
fmt_ctx_gauge(struct ui *u, char *buf, size_t len)
{
	int64_t ctx_max = clm_agent_get_ctx_max(u->agent);
	int filled, pct;
	size_t off = 0;

	if (buf == NULL || len == 0)
		return;
	buf[0] = '\0';
	if (ctx_max <= 0 || u->ctx_used <= 0)
		return;

	pct = (int)((u->ctx_used * 100) / ctx_max);
	if (pct > 100)
		pct = 100;
	filled = (pct * GAUGE_CELLS) / 100;

	off += (size_t)snprintf(buf + off, len - off, "[");
	for (int i = 0; i < GAUGE_CELLS && off < len; i++)
		off += (size_t)snprintf(buf + off, len - off, "%s",
		    i < filled ? "\u2588" : "\u2591");
	if (off < len)
		(void)snprintf(buf + off, len - off, "] %d%%", pct);
}

static void
draw_status(struct ui *u)
{
	static const char spin[] = "|/-\\";
	const char *info;
	char sp = u->busy ? spin[u->spinner & 3] : ' ';

	werase(u->stat);
	wbkgd(u->stat, COLOR_PAIR(5));
	wattron(u->stat, COLOR_PAIR(5));

	/* Live status sits on the left, next to the model, so the eye doesn't
	 * have to travel across the width of the terminal to read it. */
	if (u->batch[0] != '\0')
		info = u->batch;
	else if (u->usage[0] != '\0')
		info = u->usage;
	else
		info = state_label(u->state);

	mvwprintw(u->stat, 0, 0, " clm");
	if (u->agent_name != NULL)
		wprintw(u->stat, " [%s]", u->agent_name);
	else if (u->model != NULL)
		wprintw(u->stat, " [%s]", u->model);

	switch (u->conn) {
	case CLM_CONN_CHECKING:
		wprintw(u->stat, " [connecting]");
		break;
	case CLM_CONN_ONLINE:
		wprintw(u->stat, " [online]");
		break;
	case CLM_CONN_OFFLINE:
		if (u->conn_detail[0] != '\0')
			wprintw(u->stat, " [offline: %s]", u->conn_detail);
		else
			wprintw(u->stat, " [offline]");
		break;
	}

	wprintw(u->stat, "  %c %s ", sp, info);

	/* Context-window gauge, when the backend reports a window size. */
	{
		char gauge[64];
		fmt_ctx_gauge(u, gauge, sizeof(gauge));
		if (gauge[0] != '\0')
			wprintw(u->stat, " %s ", gauge);
	}

	/* Right-aligned key hints, shown only when they fit without colliding
	 * with the live status on the left. */
	{
		static const char hints[] =
		    "^R reasoning  ^O output  ^L redraw  PgUp/PgDn scroll ";
		int w = getmaxx(u->stat);
		int used = getcurx(u->stat);
		int hw = (int)(sizeof(hints) - 1);

		if (w - hw > used + 2)
			mvwprintw(u->stat, 0, w - hw, "%s", hints);
	}

	wattroff(u->stat, COLOR_PAIR(5));
	wnoutrefresh(u->stat);
}

/* Display columns occupied by the first n bytes of a UTF-8 string. */
static int
disp_width(const char *s, size_t n)
{
	mbstate_t ps;
	size_t i = 0;
	int cols = 0;

	memset(&ps, 0, sizeof(ps));
	while (i < n) {
		wchar_t wc;
		size_t k = mbrtowc(&wc, s + i, n - i, &ps);
		if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
			i++; /* invalid/incomplete: count as one column */
			cols++;
			continue;
		}
		int w = wcwidth(wc);
		cols += (w > 0) ? w : 1;
		i += k;
	}
	return cols;
}

/* Rows the input box needs to show all its text (plus the trailing cursor). */
static int
input_rows(struct ui *u, int w)
{
	int total;

	if (w < 1)
		w = 1;
	total = 2 + disp_width(u->input, u->input_len); /* "> " + text */
	return total / w + 1;
}

static void
draw_input(struct ui *u)
{
	int w = getmaxx(u->in);
	int h = getmaxy(u->in);
	int off, cy, cx;

	werase(u->in);
	wmove(u->in, 0, 0);
	waddstr(u->in, "> ");
	/* Let curses wrap the text across the box's rows. */
	waddnstr(u->in, u->input, (int)u->input_len);

	off = 2 + disp_width(u->input, u->input_pos);
	cy = off / w;
	cx = off % w;
	if (cy > h - 1) { /* box capped and scrolled: pin cursor to last row */
		cy = h - 1;
		cx = w - 1;
	}
	wmove(u->in, cy, cx);
	wnoutrefresh(u->in);
}

/*
 * Recompute the input box height from its contents and re-tile the screen:
 * the transcript keeps the top, the status bar sits just above the box, and
 * the box occupies the bottom, growing upward. Returns true if geometry
 * changed (so the caller can force a full redraw). force relays out even when
 * the height is unchanged (used after a terminal resize).
 */
static bool
relayout(struct ui *u, bool force)
{
	int h = LINES, w = COLS;
	int need, maxih, txt_h;

	if (h < 3)
		h = 3;

	maxih = h / 2;
	if (maxih < 1)
		maxih = 1;
	if (maxih > h - 2)
		maxih = h - 2;
	if (maxih < 1)
		maxih = 1;

	need = input_rows(u, w);
	if (need > maxih)
		need = maxih;
	if (need < 1)
		need = 1;

	if (!force && need == u->in_h)
		return false;

	u->in_h = need;
	txt_h = h - 1 - need;
	if (txt_h < 1)
		txt_h = 1;

	/* The transcript window holds scrollback content, so only resize it in
	 * place (origin stays 0,0). The status/input windows carry no state, so
	 * recreate them at the new positions -- simpler and always valid. */
	wresize(u->txt, txt_h, w);
	delwin(u->stat);
	delwin(u->in);
	u->stat = newwin(1, w, txt_h, 0);
	u->in = newwin(need, w, txt_h + 1, 0);
	scrollok(u->in, TRUE);
	keypad(u->in, TRUE);
	nodelay(u->in, TRUE);
	return true;
}

/* Append a rendered run to the cache, copying len bytes of text. */
static void
rseg_push(struct ui *u, int attr, const char *text, size_t len)
{
	char *dup;

	if (len == 0)
		return;
	if (u->nrsegs == u->cap_rsegs) {
		size_t ncap = u->cap_rsegs ? u->cap_rsegs * 2 : 32;
		struct rseg *p = realloc(u->rsegs, ncap * sizeof(*p));
		if (p == NULL)
			return;
		u->rsegs = p;
		u->cap_rsegs = ncap;
	}
	dup = malloc(len + 1);
	if (dup == NULL)
		return;
	memcpy(dup, text, len);
	dup[len] = '\0';
	u->rsegs[u->nrsegs].attr = attr;
	u->rsegs[u->nrsegs].text = dup;
	u->nrsegs++;
}

/*
 * Markdown rendering.
 *
 * The md_render module (src/md_render.c) parses markdown into abstract styled
 * runs; here we map those abstract styles to curses attributes.
 *
 * Colour model: element styles are applied uniformly across streams -- code is
 * green (COLOR_PAIR 7) whether inline or block, in the answer or the think
 * channel. A stream contributes a "floor" via base_attr: the assistant stream
 * is A_NORMAL, the reasoning channel is COLOR_PAIR(6) (Solarized base01, a
 * muted secondary tone). An element with its own colour (code) overrides the
 * floor's colour pair. On a Solarized 16-colour palette A_BOLD promotes an
 * accent colour to its grey bright-slot twin, so the reasoning stream sets
 * no_bold to keep emphasis from greying/brightening its muted text.
 */
struct md_sink {
	struct ui *u;
	unsigned base_attr;
	bool no_bold; /* drop A_BOLD (reasoning stream: bold would promote) */
};

/* Non-colour emphasis attributes for a run's abstract style. */
static unsigned
md_style_to_attr(unsigned style)
{
	unsigned a = 0;

	if (style & MD_ST_BOLD)
		a |= (unsigned)A_BOLD;
	if (style & MD_ST_ITALIC)
		a |= (unsigned)A_ITALIC;
	if (style & MD_ST_UNDERLINE)
		a |= (unsigned)A_UNDERLINE;
	if (style & MD_ST_STRIKE)
		a |= (unsigned)A_DIM;
	return a;
}

static void
md_emit(const struct md_run *run, void *userdata)
{
	struct md_sink *s = userdata;
	unsigned attr = s->base_attr | md_style_to_attr(run->style);

	/* Code carries its own colour, overriding the stream's default pair. */
	if (run->style & MD_ST_CODE)
		attr = (attr & ~(unsigned)A_COLOR) | (unsigned)COLOR_PAIR(7);

	if (s->no_bold)
		attr &= ~(unsigned)A_BOLD;

	rseg_push(s->u, (int)attr, run->text, run->len);
}

/*
 * Render one markdown span into rendered runs, OR-ing base_attr into every run
 * (e.g. the reasoning colour for the think channel). no_bold drops emphasis
 * bold (used by the reasoning stream, where bold would promote its muted
 * colour to a grey bright slot). Width feeds table layout; soft-wrapping of the
 * emitted runs is still done later by wrap_walk.
 */
static void
render_markdown(struct ui *u, const char *md, int w, int base_attr, bool no_bold)
{
	struct md_sink sink = {
		.u = u,
		.base_attr = (unsigned)base_attr,
		.no_bold = no_bold,
	};
	struct md_opts opts = {.width = w, .tables = MD_TABLE_AUTO};

	md_render(md, strlen(md), &opts, md_emit, &sink);
}

/* Number of newline-delimited lines in text (a trailing partial line counts).
 */
static int
count_lines(const char *s, size_t len)
{
	int lines = 0;

	for (size_t i = 0; i < len; i++)
		if (s[i] == '\n')
			lines++;
	if (len > 0 && s[len - 1] != '\n')
		lines++;
	return lines;
}

/* Emit a tool-output body: full when expanded, else the first few lines plus a
 * dim "+N lines" hint the user can toggle open with ^O. */
static void
push_tool_output(struct ui *u, const char *text)
{
	enum { PREVIEW = 6 };
	size_t len = strlen(text);
	int total = count_lines(text, len);
	size_t cut;
	int seen;
	char hint[64];

	if (u->expand_output || total <= PREVIEW) {
		rseg_push(u, seg_attr(ST_TOOL_OUT), text, len);
		return;
	}
	cut = 0;
	seen = 0;
	while (cut < len && seen < PREVIEW) {
		if (text[cut] == '\n')
			seen++;
		cut++;
	}
	rseg_push(u, seg_attr(ST_TOOL_OUT), text, cut);
	snprintf(hint, sizeof(hint), "  ... (+%d lines, ^O to expand)\n",
	         total - PREVIEW);
	rseg_push(u, seg_attr(ST_TOOL_OUT), hint, strlen(hint));
}

/* Resolve the source span list into the rendered run cache for width w. */
static void
rebuild_render(struct ui *u, int w)
{
	for (size_t i = 0; i < u->nrsegs; i++)
		free(u->rsegs[i].text);
	u->nrsegs = 0;

	for (size_t i = 0; i < u->nsegs; i++) {
		const struct seg *g = &u->segs[i];
		if (g->style == ST_REASON && !u->show_reasoning)
			continue; /* think channel hidden */
		if (g->style == ST_ASSIST && w >= 4)
			render_markdown(u, g->text, w, A_NORMAL, false);
		else if (g->style == ST_REASON && w >= 4)
			render_markdown(u, g->text, w, COLOR_PAIR(6), true);
		else if (g->style == ST_TOOL_OUT)
			push_tool_output(u, g->text);
		else
			rseg_push(u, seg_attr(g->style), g->text,
			          strlen(g->text));
	}
	u->built_gen = u->gen;
	u->built_width = w;
	u->built_expand = u->expand_output;
	u->built_reasoning = u->show_reasoning;
}

/*
 * Walk the rendered runs, soft-wrapping every run to width w. Returns the total
 * number of wrapped rows. When draw is true, cells whose wrapped-row index
 * falls in [start, start+h) are painted into u->txt at row (index - start).
 * The single walk is used both to measure (draw=false) and to paint, so the
 * two passes can never disagree about where lines wrap.
 */
static int
wrap_walk(struct ui *u, int w, int h, int start, bool draw)
{
	int row = 0, col = 0;

	if (w < 1)
		w = 1;

	for (size_t i = 0; i < u->nrsegs; i++) {
		const char *s = u->rsegs[i].text;
		size_t len = strlen(s);
		size_t j = 0;
		mbstate_t ps;

		if (draw)
			wattrset(u->txt, u->rsegs[i].attr);
		memset(&ps, 0, sizeof(ps));

		while (j < len) {
			wchar_t wc;
			size_t k = mbrtowc(&wc, s + j, len - j, &ps);
			int cw;

			if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
				k = 1; /* invalid/incomplete byte */
				wc = 0;
			}
			if (wc == L'\n') {
				row++;
				col = 0;
				j += k;
				continue;
			}
			cw = wcwidth(wc);
			if (cw < 0)
				cw = 1;
			if (cw > 0 && col + cw > w) {
				row++;
				col = 0;
			}
			if (draw && row >= start && row < start + h) {
				wmove(u->txt, row - start, col);
				waddnstr(u->txt, s + j, (int)k);
			}
			col += cw;
			j += k;
		}
	}
	if (draw)
		wattrset(u->txt, A_NORMAL);
	return row + 1;
}

static void
draw_transcript(struct ui *u)
{
	int w = getmaxx(u->txt);
	int h = getmaxy(u->txt);
	int total, maxscroll, start;

	if (u->gen != u->built_gen || w != u->built_width ||
	    u->expand_output != u->built_expand ||
	    u->show_reasoning != u->built_reasoning)
		rebuild_render(u, w);

	total = wrap_walk(u, w, h, 0, false);

	maxscroll = total - h;
	if (maxscroll < 0)
		maxscroll = 0;
	if (u->scroll > (size_t)maxscroll)
		u->scroll = (size_t)maxscroll;

	start = total - h - (int)u->scroll;
	if (start < 0)
		start = 0;

	werase(u->txt);
	wrap_walk(u, w, h, start, true);
	wnoutrefresh(u->txt);
}

static void handle_keys(struct ui *u); /* defined below; shared with on_stdin */

static void
on_repaint(uv_timer_t *t)
{
	struct ui *u = t->data;

	/* Also drain input here so a lone Escape (held by ncurses waiting for a
	 * possible escape sequence) is flushed within a tick, even if no further
	 * keystroke arrives to trigger the stdin poll. */
	handle_keys(u);

	if (u->busy)
		u->spinner++;

	if (!u->dirty && !u->busy)
		return;

	/* Grow/shrink the input box to fit what's typed before drawing. */
	if (relayout(u, false))
		u->full_redraw = true;

	if (u->full_redraw) {
		/* The three windows tile the screen; redrawing them all from
		 * scratch repaints every cell, clearing resize artifacts. */
		redrawwin(u->txt);
		redrawwin(u->stat);
		redrawwin(u->in);
		u->full_redraw = false;
	}

	draw_transcript(u);
	draw_status(u);
	draw_input(u);
	doupdate();
	u->dirty = false;
}

/* ---- input ---- */

/* Echo a prompt and hand it to the agent. Returns true if the turn started. */
static bool
do_submit(struct ui *u, const char *prompt, bool echo)
{
	int r;

	u->scroll = 0; /* snap to the bottom so the reply is visible */
	if (echo) {
		ui_push(u, ST_USER, "\nyou> ");
		ui_push(u, ST_USER, prompt);
		ui_push(u, ST_USER, "\n");
	}
	r = clm_agent_submit(u->agent, prompt);
	if (r < 0) {
		ui_push(u, ST_ERROR, "\nerror: ");
		ui_push(u, ST_ERROR, clm_agent_get_last_error(u->agent));
		return false;
	}
	u->busy = true;
	u->started_assist = false;
	u->usage[0] = '\0';
	return true;
}

/* Queue a prompt typed while a turn is running; it echoes as "(queued)". */
static void
enqueue_prompt(struct ui *u, const char *prompt)
{
	char *dup = strdup(prompt);
	if (dup == NULL)
		return;
	if (u->nqueue == u->cap_queue) {
		size_t ncap = u->cap_queue ? u->cap_queue * 2 : 8;
		char **p = realloc(u->queue, ncap * sizeof(*p));
		if (p == NULL) {
			free(dup);
			return;
		}
		u->queue = p;
		u->cap_queue = ncap;
	}
	u->queue[u->nqueue++] = dup;
	ui_push(u, ST_USER, "\nyou> ");
	ui_push(u, ST_USER, prompt);
	ui_push(u, ST_META, "  (queued)\n");
}

static void
drain_queue(struct ui *u)
{
	char *prompt;

	if (u->busy || u->nqueue == 0)
		return;
	prompt = u->queue[0];
	memmove(u->queue, u->queue + 1, (u->nqueue - 1) * sizeof(*u->queue));
	u->nqueue--;
	do_submit(u, prompt, false); /* already echoed when queued */
	free(prompt);
}

#ifdef CLM_LUA
static char *xdg_config_path(const char *suffix);
#endif

/* A '/word ...' line: run a UI command instead of prompting the model. */
static void
run_command(struct ui *u, const char *line)
{
	const char *arg;
	size_t wlen;

	line++; /* skip '/' */
	arg = line;
	while (*arg != '\0' && *arg != ' ')
		arg++;
	wlen = (size_t)(arg - line);
	while (*arg == ' ')
		arg++;

#define CMD(s) (wlen == strlen(s) && strncmp(line, s, wlen) == 0)
	if (CMD("help") || CMD("h") || CMD("?")) {
		ui_push(
		    u, ST_META,
		    "\ncommands:\n"
		    "  /help              this help\n"
		    "  /clear             clear the transcript\n"
		    "  /agent <name>      switch agent profile\n"
		    "  /reasoning [on|off] show/hide the think channel (^R)\n"
		    "  /output [full|short] tool output detail (^O)\n"
		    "  /compact           summarize old turns to reclaim context\n"
		    "  /quit              exit\n"
		    "keys: ^R reasoning  ^O output  ^L redraw  "
		    "PgUp/PgDn scroll\n");
	} else if (CMD("clear") || CMD("cls")) {
		for (size_t i = 0; i < u->nsegs; i++)
			free(u->segs[i].text);
		u->nsegs = 0;
		u->scroll = 0;
		u->gen++;
	} else if (CMD("reasoning") || CMD("think")) {
		if (strcmp(arg, "on") == 0)
			u->show_reasoning = true;
		else if (strcmp(arg, "off") == 0)
			u->show_reasoning = false;
		else
			u->show_reasoning = !u->show_reasoning;
		ui_push(u, ST_META,
		        u->show_reasoning ? "\n[reasoning shown]\n"
		                          : "\n[reasoning hidden]\n");
	} else if (CMD("output")) {
		if (strcmp(arg, "full") == 0)
			u->expand_output = true;
		else if (strcmp(arg, "short") == 0)
			u->expand_output = false;
		else
			u->expand_output = !u->expand_output;
		ui_push(u, ST_META,
		        u->expand_output ? "\n[output: full]\n"
		                         : "\n[output: short]\n");
	} else if (CMD("compact")) {
		int rc = clm_agent_compact(u->agent);
		if (rc == 0) {
			u->busy = true;
			ui_push(u, ST_META, "\n[compacting the conversation...]\n");
		} else if (rc == -EBUSY) {
			ui_push(u, ST_ERROR, "\nbusy; try again when idle\n");
		} else {
			ui_push(u, ST_ERROR, "\ncompaction failed to start\n");
		}
	} else if (CMD("agent") || CMD("a")) {
		/* /agent <name>
		 * Switch agent: reload profile, swap provider+prompt, clear history. */
#ifdef CLM_LUA
		if (arg[0] == '\0') {
			ui_push(u, ST_META, "\nusage: /agent <name>\n");
		} else if (u->lcfg == NULL) {
			ui_push(u, ST_ERROR, "\nno config loaded\n");
		} else {
			char *adir = xdg_config_path("clm/agents");
			if (clm_lua_cfg_load_agent(u->lcfg, adir, arg) < 0) {
				char msg[128];
				(void)snprintf(msg, sizeof(msg),
				    "\nagent '%s' not found\n", arg);
				ui_push(u, ST_ERROR, msg);
				free(adir);
			} else {
				free(adir);
				/* Resolve new provider. */
				const char *prov = clm_lua_cfg_get_str(u->lcfg, "provider");
				const char *purl = prov ? clm_lua_cfg_provider_str(u->lcfg, prov, "url") : NULL;
				const char *pmodel = prov ? clm_lua_cfg_provider_str(u->lcfg, prov, "model") : NULL;
				const char *pkey = prov ? clm_lua_cfg_provider_str(u->lcfg, prov, "api_key") : NULL;
				const char *sprompt = clm_lua_cfg_get_str(u->lcfg, "system_prompt");

				/* Tear down plugins + agent. */
				clm_lua_env_free(u->lua_env);
				u->lua_env = NULL;
				clm_agent_free(u->agent);

				/* Rebuild cfg. */
				int r;
				struct clm_cfg newcfg = {0};
				char url_buf[512];
				if (purl != NULL) {
					size_t ulen = strlen(purl);
					while (ulen > 0 && purl[ulen-1] == '/')
						ulen--;
					(void)snprintf(url_buf, sizeof(url_buf),
					    "%.*s/chat/completions", (int)ulen, purl);
					newcfg.base_url = url_buf;
				} else {
					newcfg.base_url = "http://127.0.0.1:8081/v1/chat/completions";
				}
				newcfg.model = pmodel ? pmodel : "local-model";
				newcfg.api_key = pkey ? pkey : (getenv("CLM_API_KEY") ? getenv("CLM_API_KEY") : "sk-no-key-required");
				newcfg.system_prompt = sprompt;
				newcfg.provider = CLM_PROVIDER_OPENAI;
				newcfg.stream = 1;

				r = clm_agent_new(&newcfg, u->host, &tui_callbacks, u, &u->agent);
				if (r < 0) {
					ui_push(u, ST_ERROR, "\nfailed to create agent\n");
				} else {
					clm_tools_register_shell(u->agent);
					/* Reload plugins. */
					if (clm_lua_env_new(u->agent, &u->lua_env) == 0) {
						clm_lua_env_set_config_from(u->lua_env, u->lcfg);
						if (u->plugin_dir != NULL) {
							clm_lua_load_plugins(u->lua_env, u->plugin_dir);
						} else {
							char *pp = xdg_config_path("clm/plugins");
							if (pp != NULL) {
								clm_lua_load_plugins(u->lua_env, pp);
								free(pp);
							}
						}
						/* Load agent-specific plugins. */
						char *apdir = xdg_config_path("clm/agents");
						if (apdir != NULL) {
							char apbuf[512];
							(void)snprintf(apbuf, sizeof(apbuf), "%s/%s", apdir, arg);
							clm_lua_load_plugins(u->lua_env, apbuf);
							free(apdir);
						}
					}
					free(u->agent_name);
					u->agent_name = strdup(arg);
					u->model = newcfg.model;
					char msg[128];
					(void)snprintf(msg, sizeof(msg),
					    "\n[switched to agent: %s]\n", arg);
					ui_push(u, ST_META, msg);
					clm_agent_check_connection(u->agent);
				}
			}
		}
#else
		ui_push(u, ST_ERROR, "\nagents require Lua support\n");
#endif
	} else if (CMD("quit") || CMD("exit") || CMD("q")) {
		u->quit = true;
	} else {
		ui_push(u, ST_ERROR, "\nunknown command (try /help)\n");
	}
#undef CMD
}

/*
 * Record a submitted line in the recall history. Skips empties and immediate
 * duplicates of the most recent entry. The browse cursor is reset to the live
 * line by the caller after submit.
 */
static void
hist_add(struct ui *u, const char *line)
{
	char *dup;

	if (line[0] == '\0')
		return;
	if (u->nhist > 0 && strcmp(u->hist[u->nhist - 1], line) == 0)
		return;

	if (u->nhist == u->cap_hist) {
		size_t ncap = u->cap_hist ? u->cap_hist * 2 : 16;
		char **h = realloc(u->hist, ncap * sizeof(*h));
		if (h == NULL)
			return; /* history is best-effort */
		u->hist = h;
		u->cap_hist = ncap;
	}
	dup = strdup(line);
	if (dup == NULL)
		return;
	u->hist[u->nhist++] = dup;
}

/* Load a history entry (or a saved live line) into the input buffer. */
static void
hist_load(struct ui *u, const char *text)
{
	size_t n = strlen(text);

	if (n >= sizeof(u->input))
		n = sizeof(u->input) - 1;
	memcpy(u->input, text, n);
	u->input[n] = '\0';
	u->input_len = n;
	u->input_pos = n;
	u->dirty = true;
}

/*
 * Move through the prompt history. dir < 0 goes to older entries, dir > 0 to
 * newer. Stepping up from the live line stashes it so stepping back down
 * restores it; stepping down past the newest entry returns to that live line.
 */
static void
hist_recall(struct ui *u, int dir)
{
	if (u->nhist == 0)
		return;

	if (dir < 0) { /* older */
		if (u->hist_pos == 0)
			return; /* already at the oldest */
		if (u->hist_pos == u->nhist) {
			/* Leaving the live line: save it for the trip back. */
			u->input[u->input_len] = '\0';
			(void)snprintf(u->hist_saved, sizeof(u->hist_saved),
			    "%s", u->input);
		}
		u->hist_pos--;
		hist_load(u, u->hist[u->hist_pos]);
	} else { /* newer */
		if (u->hist_pos >= u->nhist)
			return; /* already at the live line */
		u->hist_pos++;
		if (u->hist_pos == u->nhist)
			hist_load(u, u->hist_saved); /* back to the live line */
		else
			hist_load(u, u->hist[u->hist_pos]);
	}
}

static void
submit_line(struct ui *u)
{
	u->input[u->input_len] = '\0';
	if (u->input_len == 0)
		return;

	hist_add(u, u->input);

	if (u->input[0] == '/')
		run_command(u, u->input);
	else if (strcmp(u->input, "quit") == 0 || strcmp(u->input, "exit") == 0)
		u->quit = true;
	else if (u->busy)
		enqueue_prompt(u, u->input);
	else
		do_submit(u, u->input, true);

	u->input_len = 0;
	u->input_pos = 0;
	u->hist_pos = u->nhist; /* reset recall to the live line */
	u->hist_saved[0] = '\0';
	u->dirty = true;
}

/* Byte offset of the UTF-8 char boundary before/after pos. */
static size_t
prev_boundary(const char *s, size_t pos)
{
	if (pos == 0)
		return 0;
	do {
		pos--;
	} while (pos > 0 && (s[pos] & 0xc0) == 0x80);
	return pos;
}

static size_t
next_boundary(const char *s, size_t pos, size_t len)
{
	if (pos >= len)
		return len;
	do {
		pos++;
	} while (pos < len && (s[pos] & 0xc0) == 0x80);
	return pos;
}

static void
input_char(struct ui *u, wint_t wch)
{
	char mb[MB_LEN_MAX];
	int n = wctomb(mb, (wchar_t)wch);
	if (n <= 0)
		return;
	if (u->input_len + (size_t)n >= sizeof(u->input) - 1)
		return;
	/* Insert at the cursor, shifting the tail right. */
	memmove(u->input + u->input_pos + (size_t)n, u->input + u->input_pos,
	        u->input_len - u->input_pos);
	memcpy(u->input + u->input_pos, mb, (size_t)n);
	u->input_len += (size_t)n;
	u->input_pos += (size_t)n;
	u->dirty = true;
}

static void
input_backspace(struct ui *u)
{
	if (u->input_pos == 0)
		return;
	size_t start = prev_boundary(u->input, u->input_pos);
	memmove(u->input + start, u->input + u->input_pos,
	        u->input_len - u->input_pos);
	u->input_len -= u->input_pos - start;
	u->input_pos = start;
	u->dirty = true;
}

/* Save [from,to) to the kill buffer and splice it out of the input. */
static void
input_kill(struct ui *u, size_t from, size_t to)
{
	if (to <= from)
		return;
	size_t n = to - from;
	if (n >= sizeof(u->kill))
		n = sizeof(u->kill) - 1;
	memcpy(u->kill, u->input + from, n);
	u->kill_len = n;
	memmove(u->input + from, u->input + to, u->input_len - to);
	u->input_len -= to - from;
	if (u->input_pos > to)
		u->input_pos -= to - from;
	else if (u->input_pos > from)
		u->input_pos = from;
	u->dirty = true;
}

static void
input_yank(struct ui *u)
{
	if (u->kill_len == 0)
		return;
	if (u->input_len + u->kill_len >= sizeof(u->input) - 1)
		return;
	memmove(u->input + u->input_pos + u->kill_len, u->input + u->input_pos,
	        u->input_len - u->input_pos);
	memcpy(u->input + u->input_pos, u->kill, u->kill_len);
	u->input_len += u->kill_len;
	u->input_pos += u->kill_len;
	u->dirty = true;
}

/* ---- file-path tab completion ---- */

/*
 * Free any cached completion candidates. (Reserved for future multi-tab
 * cycling support.)  Called as no-op from input_complete() to reset state.
 */
static void
complete_free(struct ui *u)
{
	for (size_t i = 0; i < u->complete_n; i++)
		free(u->complete_candidates[i]);
	u->complete_n = 0;
	u->complete_idx = 0;
}

/*
 * Extract the "word" at the cursor position for file-path completion.
 *
 * A word is the maximal span of non-space characters ending at input_pos
 * (or input_len if the cursor is at the end).  Returns the start byte
 * offset in *wstart and the length in *wlen.  If there is no word,
 * returns false.
 */
static bool
extract_word(const char *input, size_t input_len, size_t input_pos,
             size_t *wstart, size_t *wlen)
{
	/* end of the word: input_pos if cursor is mid-word, else input_len. */
	size_t wend = input_pos;

	/* Walk left to find the word boundary. */
	while (wend > 0 && input[wend - 1] != ' ')
		wend--;

	/* cursor is after spaces — nothing to complete. */
	if (wend == input_pos)
		return false;

	size_t ws = wend;

	/* Walk right from cursor to find the end of the word. */
	size_t we = input_pos;
	while (we < input_len && input[we] != ' ')
		we++;

	*wstart = ws;
	*wlen = we - ws;
	return true;
}

/*
 * Check if a string looks like a file path: contains '/' or starts with '~'.
 */
static bool
looks_like_path(const char *s, size_t len)
{
	if (len == 0)
		return false;
	if (s[0] == '~')
		return true;
	for (size_t i = 0; i < len; i++)
		if (s[i] == '/')
			return true;
	return false;
}

/*
 * Expand leading ~ to $HOME.  Returns a malloc'd string or NULL.
 */
static char *
expand_tilde(const char *s, size_t len)
{
	if (len > 0 && s[0] == '~') {
		const char *home = getenv("HOME");
		if (home) {
			size_t hlen = strlen(home);
			size_t n = hlen + len - 1;
			char *out = malloc(n + 1);
			if (out) {
				memcpy(out, home, hlen);
				memcpy(out + hlen, s + 1, len - 1);
				out[n] = '\0';
			}
			return out;
		}
	}
	char *out = malloc(len + 1);
	if (out) {
		memcpy(out, s, len);
		out[len] = '\0';
	}
	return out;
}

/*
 * Return true if path exists and is a directory.
 */
static bool
is_directory(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/*
 * Tab completion for file paths.
 *
 * If the word under the cursor looks like a file path:
 *   1. Gather matching entries from the parent directory.
 *   2. If there is exactly one match, insert it.  If it is a directory,
 *      append '/' so the next TAB can complete inside it.
 *   3. If ambiguous but there is a common extension, insert that prefix.
 *      If the prefix itself is a directory, append '/'.
 *   4. Otherwise just print the candidate list (no change to the input).
 */
static void
input_complete(struct ui *u)
{
	complete_free(u);

	size_t wstart, wlen;
	if (!extract_word(u->input, u->input_len, u->input_pos, &wstart, &wlen))
		return;

	const char *word = u->input + wstart;
	if (!looks_like_path(word, wlen))
		return;

	/* Expand ~ in the word for filesystem access. */
	char *expanded = expand_tilde(word, wlen);
	if (!expanded)
		return;

	/* Determine the directory and basename prefix. */
	char *slash = strrchr(expanded, '/');

	char *dir, *prefix;
	size_t dir_offset; /* how much of the original word is the dir part */
	if (slash) {
		*slash = '\0';
		dir = expanded[0] != '\0' ? expanded : "/";
		prefix = slash + 1;
		/* Find the corresponding slash in the original word. */
		const char *orig_slash = NULL;
		for (size_t i = wlen; i > 0; i--) {
			if (word[i - 1] == '/') {
				orig_slash = word + i;
				break;
			}
		}
		dir_offset = orig_slash ? (size_t)(orig_slash - word) : 0;
	} else {
		dir = ".";
		prefix = expanded;
		dir_offset = 0;
	}

	DIR *d = opendir(dir);
	if (!d) {
		free(expanded);
		return;
	}

	/* Collect matching entries (cap at 32). */
	char *candidates[32];
	size_t ncandidates = 0;

	struct dirent *ent;
	size_t plen = strlen(prefix);
	while (ncandidates < 32 && (ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (strncmp(ent->d_name, prefix, plen) == 0) {
			char *c = strdup(ent->d_name);
			if (c)
				candidates[ncandidates++] = c;
		}
	}
	closedir(d);

	if (ncandidates == 0) {
		free(expanded);
		return;
	}

	/* Sort candidates. */
	for (size_t i = 1; i < ncandidates; i++) {
		char *key = candidates[i];
		size_t j = i;
		while (j > 0 && strcmp(candidates[j - 1], key) > 0) {
			candidates[j] = candidates[j - 1];
			j--;
		}
		candidates[j] = key;
	}

	/* Find longest common prefix among candidates. */
	size_t common = strlen(candidates[0]);
	for (size_t i = 1; i < ncandidates; i++) {
		size_t k = 0;
		while (k < common && candidates[i][k] == candidates[0][k])
			k++;
		common = k;
	}

	/* Build the replacement: original dir prefix + completed basename. */
	const char *basename_insert = NULL;
	size_t basename_len = 0;
	bool append_slash = false;

	if (ncandidates == 1) {
		basename_insert = candidates[0];
		basename_len = strlen(candidates[0]);
		autofree char *full = malloc(1024);
		if (full) {
			(void)snprintf(full, 1024, "%s/%s", dir, candidates[0]);
			if (is_directory(full))
				append_slash = true;
		}
	} else if (common > plen) {
		basename_insert = candidates[0];
		basename_len = common;
	}

	/* Show candidates if ambiguous, columnated. */
	if (ncandidates > 1) {
		autofree char *buf = malloc(1024);
		if (buf) {
			int blen = 0;

			/* Find widest name for column padding. */
			size_t maxw = 0;
			for (size_t i = 0; i < ncandidates; i++) {
				size_t n = strlen(candidates[i]);
				if (n > maxw)
					maxw = n;
			}
			maxw += 2; /* inter-column gap */
			size_t cols = 60 / (maxw > 0 ? maxw : 1);
			if (cols < 1) cols = 1;

			blen += snprintf(buf, 1024, "\n");
			for (size_t i = 0; i < ncandidates && (size_t)blen < 1024 - maxw - 4; i++) {
				size_t remaining = 1024 - (size_t)blen;
				blen += snprintf(buf + blen, remaining, "%-*s",
				    (int)maxw, candidates[i]);
				if ((i + 1) % cols == 0 || i + 1 == ncandidates)
					blen += snprintf(buf + blen,
					    1024 - (size_t)blen, "\n");
			}
			ui_push(u, ST_META, buf);
		}
	}

	/* Replace the word from dir_offset onward with the completion. */
	if (basename_insert) {
		size_t insert_len = basename_len + (append_slash ? 1 : 0);
		size_t replace_start = wstart + dir_offset;
		size_t replace_end = wstart + wlen;
		size_t tail_len = u->input_len - replace_end;
		size_t new_len = replace_start + insert_len + tail_len;

		if (new_len < sizeof(u->input)) {
			memmove(u->input + replace_start + insert_len,
			    u->input + replace_end, tail_len + 1);
			memcpy(u->input + replace_start, basename_insert, basename_len);
			if (append_slash)
				u->input[replace_start + basename_len] = '/';
			u->input_len = new_len;
			u->input_pos = replace_start + insert_len;
			u->dirty = true;
		}
	}

	for (size_t i = 0; i < ncandidates; i++)
		free(candidates[i]);
	free(expanded);
}

/*
 * Answer a pending permission prompt from a keypress. Returns true if the key
 * was a recognised answer (y/n/a/d) or Escape. Escape denies and cancels the
 * turn; the others map to the four allow/deny x once/always decisions.
 */
static bool
answer_permission(struct ui *u, int ch)
{
	const struct clm_permission_req *req;
	enum clm_permission_decision d;
	const char *label;

	if (u->perm_count == 0)
		return false;
	req = u->perm_queue[0];

	switch (ch) {
	case 'y': case 'Y': d = CLM_PERM_ALLOW_ONCE;   label = "allowed"; break;
	case 'a': case 'A': d = CLM_PERM_ALLOW_ALWAYS; label = "always allowed"; break;
	case 'n': case 'N': d = CLM_PERM_DENY_ONCE;    label = "denied"; break;
	case 'd': case 'D': d = CLM_PERM_DENY_ALWAYS;  label = "always denied"; break;
	case 27:            d = CLM_PERM_DENY_ONCE;    label = "denied (cancelled)"; break;
	default:
		return false; /* not an answer key */
	}

	/* Pop the front of the queue. */
	u->perm_count--;
	for (size_t i = 0; i < u->perm_count; i++)
		u->perm_queue[i] = u->perm_queue[i + 1];
	u->perm_showing = false;

	ui_push(u, ST_PERM, "-> ");
	ui_push(u, ST_PERM, label);
	ui_push(u, ST_PERM, "\n");
	clm_tool_permission_respond(u->agent, req, d);

	/* Escape also cancels the rest of the turn (drain remaining). */
	if (ch == 27) {
		for (size_t i = 0; i < u->perm_count; i++)
			clm_tool_permission_respond(u->agent, u->perm_queue[i],
			    CLM_PERM_DENY_ONCE);
		u->perm_count = 0;
		clm_agent_cancel(u->agent);
	}

	/* For "always" decisions, auto-resolve remaining queued requests for
	 * the same tool without prompting. */
	if (d == CLM_PERM_ALLOW_ALWAYS || d == CLM_PERM_DENY_ALWAYS) {
		const char *answered_name = clm_permission_req_name(req);
		size_t i = 0;
		while (i < u->perm_count) {
			const char *qname = clm_permission_req_name(u->perm_queue[i]);
			if (answered_name != NULL && qname != NULL &&
			    strcmp(answered_name, qname) == 0) {
				clm_tool_permission_respond(u->agent,
				    u->perm_queue[i], d);
				u->perm_count--;
				for (size_t j = i; j < u->perm_count; j++)
					u->perm_queue[j] = u->perm_queue[j + 1];
			} else {
				i++;
			}
		}
	}

	/* Show next prompt if any remain. */
	if (u->perm_count > 0)
		show_next_perm(u);

	u->dirty = true;
	return true;
}

static void
handle_keys(struct ui *u)
{
	wint_t wch;
	int r;

	while ((r = wget_wch(u->in, &wch)) != ERR) {
		/*
		 * Permission-input mode: while a tool authorization is pending,
		 * the next key answers it and nothing else is processed.
		 */
		if (u->perm_showing && r != KEY_CODE_YES) {
			if (answer_permission(u, (int)wch))
				continue;
			/* Not a recognised answer key: ignore it, stay pending. */
			continue;
		}

		if (r == KEY_CODE_YES) {
			switch (wch) {
			case KEY_BACKSPACE:
				input_backspace(u);
				break;
			case KEY_ENTER:
				submit_line(u);
				break;
			case KEY_LEFT:
				u->input_pos =
				    prev_boundary(u->input, u->input_pos);
				u->dirty = true;
				break;
			case KEY_RIGHT:
				u->input_pos = next_boundary(
				    u->input, u->input_pos, u->input_len);
				u->dirty = true;
				break;
			case KEY_UP: /* recall an older prompt */
				hist_recall(u, -1);
				break;
			case KEY_DOWN: /* recall a newer prompt */
				hist_recall(u, +1);
				break;
			case KEY_HOME:
				u->input_pos = 0;
				u->dirty = true;
				break;
			case KEY_END:
				u->input_pos = u->input_len;
				u->dirty = true;
				break;
			case KEY_DC: /* Delete */
				if (u->input_pos < u->input_len)
					input_kill(u, u->input_pos,
					           next_boundary(u->input,
					                         u->input_pos,
					                         u->input_len));
				u->kill_len = 0; /* Delete doesn't feed yank */
				break;
			case KEY_PPAGE: { /* PageUp: scroll transcript back */
				int page = getmaxy(u->txt) - 1;
				if (page < 1)
					page = 1;
				u->scroll += (size_t)page;
				u->dirty = true;
				break;
			}
			case KEY_NPAGE: { /* PageDown: scroll toward the bottom
				           */
				int page = getmaxy(u->txt) - 1;
				if (page < 1)
					page = 1;
				if (u->scroll > (size_t)page)
					u->scroll -= (size_t)page;
				else
					u->scroll = 0;
				u->dirty = true;
				break;
			}
			case KEY_RESIZE:
				break; /* handled via SIGWINCH */
			default:
				break;
			}
			continue;
		}
		switch (wch) {
		case 4: /* Ctrl-D */
			if (u->input_len == 0)
				u->quit = true;
			break;
		case 3: /* Ctrl-C */
			u->quit = true;
			break;
		case 1: /* Ctrl-A: start of line */
			u->input_pos = 0;
			u->dirty = true;
			break;
		case 5: /* Ctrl-E: end of line */
			u->input_pos = u->input_len;
			u->dirty = true;
			break;
		case 2: /* Ctrl-B: back one char */
			u->input_pos = prev_boundary(u->input, u->input_pos);
			u->dirty = true;
			break;
		case 6: /* Ctrl-F: forward one char */
			u->input_pos =
			    next_boundary(u->input, u->input_pos, u->input_len);
			u->dirty = true;
			break;
		case 21: /* Ctrl-U: kill to start of line */
			input_kill(u, 0, u->input_pos);
			break;
		case 11: /* Ctrl-K: kill to end of line */
			input_kill(u, u->input_pos, u->input_len);
			break;
		case 25: /* Ctrl-Y: yank */
			input_yank(u);
			break;
		case 12: /* Ctrl-L: redraw the screen */
			u->full_redraw = true;
			u->dirty = true;
			break;
		case 15: /* Ctrl-O: toggle full vs. collapsed tool output */
			u->expand_output = !u->expand_output;
			u->dirty = true;
			break;
		case 18: /* Ctrl-R: toggle the reasoning (think) channel */
			u->show_reasoning = !u->show_reasoning;
			u->dirty = true;
			break;
		case 27: /* Escape: cancel a running turn, else clear the input */
			if (clm_agent_cancel(u->agent) == 0) {
				ui_push(u, ST_META, "\n[cancelled]\n");
			} else {
				u->input_len = 0;
				u->input_pos = 0;
			}
			u->dirty = true;
			break;
		case 9: /* TAB: file-path completion */
			input_complete(u);
			break;
		case 8:
		case 127: /* Backspace / DEL */
			input_backspace(u);
			break;
		case '\r':
		case '\n':
			submit_line(u);
			break;
		default:
			if (wch >= 32)
				input_char(u, wch);
			break;
		}
	}

	if (u->quit)
		uv_stop(u->loop);
}

static void
on_stdin(uv_poll_t *p, int status, int events)
{
	struct ui *u = p->data;

	(void)events;
	if (status < 0)
		return;
	handle_keys(u);
}

/* ---- window lifecycle ---- */

static void
make_windows(struct ui *u)
{
	int h = LINES, w = COLS;
	if (h < 3)
		h = 3;
	u->txt = newwin(h - 2, w, 0, 0);
	u->stat = newwin(1, w, h - 2, 0);
	u->in = newwin(1, w, h - 1, 0);
	u->in_h = 1;
	scrollok(u->in, TRUE);
	keypad(u->in, TRUE);
	nodelay(u->in, TRUE);
}

static void
on_winch(uv_signal_t *s, int signum)
{
	struct ui *u = s->data;
	struct winsize ws;
	(void)signum;

	/*
	 * ncurses can't learn the new size on its own here (its own SIGWINCH
	 * path is bypassed by our uv_signal handler), so query the tty and tell
	 * it via resizeterm(), which updates LINES/COLS and stdscr. Then re-lay
	 * out our windows and force a full, artifact-free repaint.
	 */
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
	    ws.ws_col > 0)
		resizeterm(ws.ws_row, ws.ws_col);

	relayout(u, true);
	u->full_redraw = true;
	u->dirty = true;
}

/*
 * Colour is deliberately confined to the 16-colour ANSI palette: the 8 base
 * colours plus A_BOLD (bright). No init_color(), no 256/truecolour indices --
 * this keeps us readable on plain terminals and under any user theme (the -1
 * background is the terminal default). Markdown is rendered NOCOLOUR, so it
 * only contributes attributes, never pair colours. Keep it this way.
 */
static void
init_colors(void)
{
	if (!has_colors())
		return;
	start_color();
	use_default_colors();
	/*
	 * Colours are chosen to read well on a stock Solarized 16-colour palette
	 * where A_BOLD promotes 0-7 accents to the grey 8-15 base tones. So we
	 * distinguish by colour, not bold, and reserve the grey base tones for
	 * genuinely secondary text (the reasoning channel). TODO: themeable.
	 */
	init_pair(1, COLOR_BLUE, -1);          /* user prompt (blue) */
	init_pair(2, COLOR_MAGENTA, -1);       /* tool call summary (magenta) */
	init_pair(3, COLOR_RED, -1);           /* error */
	init_pair(4, COLOR_YELLOW, -1);        /* timeout / warning */
	init_pair(5, COLOR_BLACK, COLOR_BLUE); /* status bar */
	init_pair(6, 10, -1);                  /* reasoning: Solarized base01 */
	init_pair(7, COLOR_GREEN, -1);         /* code (inline and block) */
	init_pair(8, COLOR_CYAN, -1);          /* assistant "clm>" label */
	/*
	 * Reserved for attention, not decoration: red = error; yellow = warning /
	 * timeout; and orange (ANSI 9 / brred) for tool-call permission prompts --
	 * the "about to do something, confirm?" caution colour. TODO: expose the
	 * whole role->colour map via the theme layer.
	 */
	init_pair(9, 9, -1);                   /* permission prompt (orange) */
}

/* Periodic (and startup) connectivity probe. */
static void
on_health(uv_timer_t *t)
{
	struct ui *u = t->data;
	clm_agent_check_connection(u->agent);
}

#ifdef CLM_LUA
static char *
xdg_config_path(const char *suffix)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *out = NULL;

	if (xdg != NULL && xdg[0] != '\0') {
		size_t n = strlen(xdg) + 1 + strlen(suffix) + 1;
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/%s", xdg, suffix);
	} else if (home != NULL && home[0] != '\0') {
		size_t n = strlen(home) + sizeof("/.config/") + strlen(suffix);
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/.config/%s", home, suffix);
	}
	return out;
}
#endif

int
tui_run(const struct clm_cfg *cfg, const char *plugin_dir,
    struct clm_lua_cfg *lcfg)
{
	struct ui *u;
	uv_loop_t *loop;
	int r;

	setlocale(LC_ALL, "");

	u = calloc(1, sizeof(*u)); /* too large for the stack frame limit */
	if (u == NULL) {
		fprintf(stderr, "error: out of memory\n");
		return 1;
	}

	loop = uv_default_loop();
	u->loop = loop;
	u->model = cfg->model;
#ifdef CLM_LUA
	if (lcfg != NULL)
		u->agent_name = strdup(clm_lua_cfg_get_str(lcfg, "agent"));
	u->lcfg = lcfg;
	u->plugin_dir = plugin_dir;
#endif
	u->state = CLM_STATE_IDLE;
	/* Default to the clean view: reasoning hidden and tool output collapsed.
	 * Opt in with ^R / ^O (see the status-bar hints). */
	u->show_reasoning = false;
	u->expand_output = false;

	r = clm_host_uv_new(loop, &u->host);
	if (r < 0) {
		fprintf(stderr, "error: failed to create host (%d)\n", r);
		free(u);
		return 1;
	}

	r = clm_agent_new(cfg, u->host, &tui_callbacks, u, &u->agent);
	if (r < 0) {
		fprintf(stderr, "error: failed to create agent (%d)\n", r);
		clm_host_uv_free(u->host);
		free(u);
		return 1;
	}
	/* Desktop uv layer: add the shell_exec tool (not in the portable core). */
	clm_tools_register_shell(u->agent);

#ifdef CLM_LUA
	if (clm_lua_env_new(u->agent, &u->lua_env) == 0) {
		if (lcfg != NULL)
			clm_lua_env_set_config_from(u->lua_env, lcfg);
		if (plugin_dir != NULL) {
			clm_lua_load_plugins(u->lua_env, plugin_dir);
		} else {
			char *ppath = xdg_config_path("clm/plugins");
			if (ppath != NULL) {
				clm_lua_load_plugins(u->lua_env, ppath);
				free(ppath);
			}
		}
	}
#endif

	initscr();
	cbreak();
	noecho();
	nonl();
	set_escdelay(25); /* make a lone Escape (cancel) responsive */
	init_colors();
	make_windows(u);

	ui_push(u, ST_META,
	        "clm -- type a prompt, /help for commands, /quit to exit.\n");

	uv_poll_init(loop, &u->stdin_poll, fileno(stdin));
	u->stdin_poll.data = u;
	uv_poll_start(&u->stdin_poll, UV_READABLE, on_stdin);

	uv_timer_init(loop, &u->repaint);
	u->repaint.data = u;
	uv_timer_start(&u->repaint, on_repaint, 0, 80);

	uv_signal_init(loop, &u->winch);
	u->winch.data = u;
	uv_signal_start(&u->winch, on_winch, SIGWINCH);

	/* Probe connectivity now (0ms) and every 20s, so a wrong URL or a down
	 * server shows in the status bar before the first prompt. */
	u->conn = CLM_CONN_CHECKING;
	uv_timer_init(loop, &u->health);
	u->health.data = u;
	uv_timer_start(&u->health, on_health, 0, 20000);

	uv_run(loop, UV_RUN_DEFAULT);

	endwin();
#ifdef CLM_LUA
	clm_lua_env_free(u->lua_env);
#endif
	clm_agent_free(u->agent);
	clm_host_uv_free(u->host);
	for (size_t i = 0; i < u->nsegs; i++)
		free(u->segs[i].text);
	free(u->segs);
	free(u->perm_queue);
	for (size_t i = 0; i < u->nrsegs; i++)
		free(u->rsegs[i].text);
	free(u->rsegs);
	for (size_t i = 0; i < u->nqueue; i++)
		free(u->queue[i]);
	free(u->queue);
	for (size_t i = 0; i < u->nhist; i++)
		free(u->hist[i]);
	free(u->hist);
	free(u);

	return 0;
}
