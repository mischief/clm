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
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <unistd.h>
#include <wchar.h>

#include <curses.h>
#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/log.h"
#include "clm/host_uv.h"
#include "clm/peer.h"
#include "clm/lua_plugin.h"
#include "clm/provider.h"
#include "complete.h"
#include "emoji.h"
#include "frontend.h"
#include "md_render.h"
#include "mcp_setup.h"
#include "plugin_setup.h"
#include "cfg_tuning.h"
#include "loop_drain.h"
#include "model_spec.h"
#include "tui_internal.h"
#include "sysinfo.h"
#include "xdg.h"

/*
 * Custom key codes for bracketed paste (xterm/most terminal emulators,
 * enabled via the "\033[?2004h" mode below): a paste is wrapped in
 * "\033[200~" ... "\033[201~" so we can tell "the user hit Enter" apart from
 * "the pasted text happened to contain a newline" -- without this, pasting
 * multi-line text submits one line at a time (each embedded newline reaching
 * the input loop looks exactly like a real Enter keypress) instead of
 * landing as a single multi-line prompt. Values are well above KEY_MAX
 * (0777) so they can't collide with any real curses key code.
 */
#define CLM_TUI_EVENT_LINES 4 /* lines of a background event to show */
#define UI_KEY_PASTE_START 0x1000
#define UI_KEY_PASTE_END 0x1001

/* ---- UI model mutation (called from libclm callbacks; keep cheap) ---- */

void
ui_push(struct ui *u, enum ui_style style, const char *text)
{
	if (text == NULL || *text == '\0')
		return;

	/* A markdown reparse (gen bump) only makes sense at a line boundary --
	 * a partial trailing line ("**bol", an unfinished table row) must not
	 * be rendered until it completes. So bump gen (triggering
	 * rebuild_render) only when this chunk closes a line; otherwise just
	 * append and mark dirty, and the last complete render stays on screen.
	 * Turn-end pushes a trailing newline, which flushes the final line. */
	bool has_nl = strchr(text, '\n') != NULL;

	/* Coalesce consecutive same-style pushes into one span. This keeps the
	 * span count small under streaming (many tiny chunks) and, crucially,
	 * rejoins a multibyte char that was split across two stream chunks. */
	if (u->nsegs > 0 && u->segs[u->nsegs - 1].style == style &&
	    style != ST_EVENT && style != ST_TOOL) {
		char *old = u->segs[u->nsegs - 1].text;
		size_t oldlen = strlen(old);
		size_t addlen = strlen(text);
		char *p = realloc(old, oldlen + addlen + 1);
		if (p == NULL)
			return;
		memcpy(p + oldlen, text, addlen + 1);
		u->segs[u->nsegs - 1].text = p;
		/* A moved span leaves the rendered runs that borrow it
		 * pointing at freed memory, so force a rebuild. */
		if (has_nl || p != old)
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
	memset(u->segs[u->nsegs].cnt, 0, sizeof(u->segs[u->nsegs].cnt));
	u->nsegs++;
	/* A new span of a different style closes the previous line's context;
	 * bump gen so the switch is rendered even without an embedded newline.
	 */
	u->gen++;
	u->dirty = true;
}

static void
cb_mcp_status(const char *msg, bool error, void *user)
{
	struct ui *u = user;
	char line[512];

	if (msg == NULL)
		return;
	(void)snprintf(line, sizeof(line), "%s\n", msg);
	ui_push(u, error ? ST_ERROR : ST_META, line);
}

/*
 * The agent is the authority on whether a turn is running: it can start one
 * on its own (a notification queued mid-turn, submitted once the previous
 * one lands), so a flag set only where this file calls submit goes stale and
 * input typed in that window is rejected as "turn already in progress".
 */
/* Grow a byte buffer to hold `need` bytes plus a terminator. */
static bool
buf_reserve(char **buf, size_t *cap, size_t need)
{
	size_t want = *cap ? *cap : 256;
	char *p;

	if (need == SIZE_MAX)
		return false;
	if (need + 1 <= *cap)
		return true;
	while (want < need + 1) {
		if (want > SIZE_MAX / 2)
			want = need + 1;
		else
			want *= 2;
	}
	p = realloc(*buf, want);
	if (p == NULL)
		return false;
	*buf = p;
	*cap = want;
	return true;
}

/* What the input buffer keeps after a submit; anything larger is returned. */
#define INPUT_KEEP_BYTES 4096

bool
ui_input_reserve(struct ui *u, size_t need)
{
	return buf_reserve(&u->input, &u->input_cap, need);
}

static void
ui_set_state(struct ui *u, enum clm_agent_state st)
{
	u->state = st;
	u->dirty = true;
	u->busy = st == CLM_STATE_THINKING || st == CLM_STATE_CALLING_TOOL ||
	    st == CLM_STATE_RATE_LIMITED;
}

static void drain_steering_queue(struct ui *u); /* injects at decision point */
static void enqueue_steering(struct ui *u, const char *prompt);
static bool do_submit(struct ui *u, const char *prompt, bool echo);
static void session_after_compact(struct ui *u);

/* ---- libclm callbacks ---- */

static void
cb_assistant_text(const char *text, void *user)
{
	struct ui *u = user;
	clm_debug("[content] %.*s",
	    (int)(strlen(text) > 200 ? 200 : strlen(text)), text);
	if (!u->started_assist) {
		ui_push(u, ST_LABEL, "\nclm>\n");
		u->started_assist = true;
	}
	ui_push(u, ST_ASSIST, text);
}

static void
cb_reasoning(const char *text, void *user)
{
	clm_debug("[reason] %.*s",
	    (int)(strlen(text) > 200 ? 200 : strlen(text)), text);
	ui_push(user, ST_REASON, text);
}

/* Extract a string field from a JSON args object, or NULL. Caller must not
 * free the result; it is owned by (and valid for the life of) obj. */
static const char *
json_field(cJSON *obj, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

	if (v == NULL || !cJSON_IsString(v))
		return NULL;
	return cJSON_GetStringValue(v);
}

/* Push a one-line, human-readable summary of a tool call -- never raw JSON.
 * The detail is flattened (no newlines/tabs) and clamped so one call is one
 * row; an over-long detail gets an ellipsis. */
/* Lines of a multi-line tool argument shown before the rest is elided. */
#define TOOL_SUMMARY_MAX_LINES 8

/* Field separator between the packed verb and detail of an ST_TOOL segment. */
#define TOOL_SUMMARY_SEP '\x1f'

/*
 * Record a tool call. The verb and full detail are packed into one ST_TOOL
 * segment (verb, separator, detail); emit_tool_summary re-formats it at
 * render time, so ^O can expand the whole command, not the stored preview.
 */
static void
push_tool_summary(struct ui *u, const char *verb, const char *detail)
{
	size_t vn = strlen(verb);
	size_t dn = detail != NULL ? strlen(detail) : 0;
	char *packed = malloc(vn + 2 + dn);

	if (packed == NULL)
		return;
	memcpy(packed, verb, vn);
	packed[vn] = TOOL_SUMMARY_SEP;
	memcpy(packed + vn + 1, detail != NULL ? detail : "", dn);
	packed[vn + 1 + dn] = '\0';
	ui_push(u, ST_TOOL, packed);
	free(packed);
}

/* Render every field of a tool-call args object (except skip_key, if
 * non-NULL -- used to omit a field already shown elsewhere, e.g.
 * cdda_action's own "action" name shown as the verb) as
 * "key=value key2=value2 ...", clamped to a max length with a trailing
 * "..." if it would run longer -- a generic fallback so any tool
 * (including ones with no hand-written case in cb_tool_begin below)
 * still shows what it was actually called with, instead of a bare tool
 * name with no arguments visible at all. Values use
 * cJSON_PrintUnformatted for anything that isn't a plain string
 * (numbers, bools, nested objects/arrays all render reasonably that
 * way); string values are shown unquoted since the key= already makes
 * the field clear without JSON's quoting noise. Writes into buf
 * (caller-provided, of size bufsz) and returns it.
 */
static char *
render_tool_args(cJSON *obj, char *buf, size_t bufsz, const char *skip_key)
{
	size_t n = 0;
	bool first = true;

	buf[0] = '\0';
	if (obj == NULL || bufsz == 0)
		return buf;

	for (cJSON *val = obj->child; val != NULL; val = val->next) {
		const char *key = val->string;
		autofree char *printed = NULL;
		const char *vstr;
		int written;

		if (key == NULL)
			continue;
		if (skip_key != NULL && strcmp(key, skip_key) == 0)
			continue;
		if (cJSON_IsString(val)) {
			vstr = cJSON_GetStringValue(val);
		} else {
			printed = cJSON_PrintUnformatted(val);
			vstr = printed != NULL ? printed : "";
		}
		written = snprintf(
		    buf + n, bufsz - n, "%s%s=%s", first ? "" : " ", key, vstr);
		if (written < 0 || (size_t)written >= bufsz - n) {
			/* Would overflow -- truncate with an ellipsis rather
			 * than a partially-written garbled field. */
			const char *ell = "...";
			size_t ell_len = strlen(ell);
			if (bufsz > ell_len)
				memcpy(buf + bufsz - 1 - ell_len, ell,
				    ell_len + 1);
			return buf;
		}
		n += (size_t)written;
		first = false;
	}
	return buf;
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	struct ui *u = user;
	cJSON *obj = args ? cJSON_Parse(args) : NULL;

	if (strcmp(name, "shell_exec") == 0) {
		push_tool_summary(
		    u, "executing shell command", json_field(obj, "command"));
		u->n_cmd++;
	} else if (strcmp(name, "read_file") == 0) {
		push_tool_summary(u, "reading file", json_field(obj, "path"));
		u->n_read++;
	} else if (strcmp(name, "write_file") == 0) {
		push_tool_summary(u, "writing file", json_field(obj, "path"));
		u->n_write++;
	} else if (strcmp(name, "cdda_think") == 0) {
		push_tool_summary(u, "thinking", json_field(obj, "thought"));
		u->n_other++;
	} else if (strcmp(name, "cdda_action") == 0) {
		char buf[36]; /* 32 chars of content + room for "..." */
		render_tool_args(obj, buf, sizeof(buf), "action");
		char label[64];
		snprintf(label, sizeof(label), "%s%s%s",
		    json_field(obj, "action") ? json_field(obj, "action") : "?",
		    strlen(buf) > 0 ? " " : "", buf);
		push_tool_summary(u, "action", label);
		u->n_other++;
	} else if (strcmp(name, "cdda_state") == 0) {
		push_tool_summary(u, "state", "");
		u->n_other++;
		/* just a marker that the call happened -- the raw JSON
		 * result itself is still not shown (see cb_tool_result
		 * below), it's noisy and adds nothing a human wants to
		 * watch scroll by. */
	} else if (strcmp(name, "cdda_overmap") == 0) {
		char buf[32];
		cJSON *jr = obj != NULL
		    ? cJSON_GetObjectItemCaseSensitive(obj, "radius")
		    : NULL;
		snprintf(buf, sizeof(buf), "radius=%d",
		    jr != NULL ? (int)cJSON_GetNumberValue(jr) : 10);
		push_tool_summary(u, "overmap", buf);
		u->n_other++;
	} else if (strcmp(name, "cdda_examine") == 0) {
		char buf[48];
		cJSON *jx = NULL, *jy = NULL, *jz = NULL;
		if (obj != NULL) {
			jx = cJSON_GetObjectItemCaseSensitive(obj, "x");
			jy = cJSON_GetObjectItemCaseSensitive(obj, "y");
			jz = cJSON_GetObjectItemCaseSensitive(obj, "z");
		}
		snprintf(buf, sizeof(buf), "x=%d y=%d z=%d",
		    jx != NULL ? (int)cJSON_GetNumberValue(jx) : 0,
		    jy != NULL ? (int)cJSON_GetNumberValue(jy) : 0,
		    jz != NULL ? (int)cJSON_GetNumberValue(jz) : 0);
		push_tool_summary(u, "examine", buf);
		u->n_other++;
	} else {
		char buf[36]; /* 32 chars of content + room for "..." */
		render_tool_args(obj, buf, sizeof(buf), NULL);
		push_tool_summary(u, name, buf);
		u->n_other++;
	}
	cJSON_Delete(obj);
}

static void
cb_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	struct ui *u = user;
	size_t clen;
	switch (outcome) {
	case CLM_TOOL_OK:
		/* cdda_* tool results are raw JSON state dumps -- the begin
		 * summary already showed what matters (thought/action), so
		 * skip echoing the body. Failures still show below. */
		if (strcmp(name, "cdda_state") == 0 ||
		    strcmp(name, "cdda_action") == 0 ||
		    strcmp(name, "cdda_think") == 0 ||
		    strcmp(name, "cdda_examine") == 0)
			break;
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

/* Shared "ran N commands, read N files, wrote N files, called N tools" tally
 * formatter behind both the per-batch summary line (push_batch_summary) and
 * the collapsed-older-clusters aggregate (push_collapsed_summary). Returns
 * false (line untouched) if every count is zero. */
static bool
format_tool_tally(
    char *line, size_t cap, const int cnt[CLM_TALLY_N], const char *suffix)
{
	size_t n;
	int parts = 0;
	static const struct {
		const char *one, *many;
	} cats[] = {
	    {"ran 1 command", "commands"},
	    {"read 1 file", "files"},
	    {"wrote 1 file", "files"},
	    {"called 1 tool", "tools"},
	    {"received 1 event", "events"},
	};
	static const char *verb[] = {
	    "ran", "read", "wrote", "called", "received"};
	int sum = 0;

	for (size_t i = 0; i < CLM_TALLY_N; i++)
		sum += cnt[i];
	if (sum == 0)
		return false;

	n = (size_t)snprintf(line, cap, "  -- ");
	for (size_t i = 0; i < CLM_TALLY_N; i++) {
		if (cnt[i] == 0)
			continue;
		if (parts++ > 0 && n < cap - 4)
			n += (size_t)snprintf(line + n, cap - n, ", ");
		if (cnt[i] == 1)
			n += (size_t)snprintf(
			    line + n, cap - n, "%s", cats[i].one);
		else
			n += (size_t)snprintf(line + n, cap - n, "%s %d %s",
			    verb[i], cnt[i], cats[i].many);
	}
	if (n < cap - 1)
		snprintf(line + n, cap - n, "%s", suffix);
	return true;
}

/* Emit "ran 2 commands, read 3 files, ..." from the batch tally, tagged
 * ST_BATCH (not ST_META) and carrying the raw counts on the segment so a
 * later, no-longer-latest cluster can still be folded into a combined
 * aggregate once a newer one takes its place (see rebuild_render). */
static void
push_batch_counts(struct ui *u, const int cnt[CLM_TALLY_N])
{
	char line[128];

	if (!format_tool_tally(line, sizeof(line), cnt, "\n"))
		return;
	ui_push(u, ST_BATCH, line);
	if (u->nsegs > 0 && u->segs[u->nsegs - 1].style == ST_BATCH) {
		memcpy(u->segs[u->nsegs - 1].cnt, cnt,
		    sizeof(u->segs[u->nsegs - 1].cnt));
	}
}

static void
push_batch_summary(struct ui *u)
{
	int cnt[CLM_TALLY_N] = {u->n_cmd, u->n_read, u->n_write, u->n_other};

	push_batch_counts(u, cnt);
}

/* Which of the four tally slots a tool name belongs to. */
static void
tally_tool(int cnt[CLM_TALLY_N], const char *name)
{
	if (name == NULL)
		name = ""; /* unnamed call falls to the "other" slot below */
	if (strcmp(name, "shell_exec") == 0)
		cnt[0]++;
	else if (strcmp(name, "read_file") == 0)
		cnt[1]++;
	else if (strcmp(name, "write_file") == 0)
		cnt[2]++;
	else
		cnt[3]++;
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
	else if (reason == CLM_FINISH_EMPTY)
		ui_push(u, ST_META, "\n[empty response]\n");
}

/*
 * Autocompact threshold checking itself lives in agent.c now
 * (clm_agent_over_autocompact_threshold), shared with
 * clm_agent_tools_done()'s own mid-chain check so both use the exact same
 * calc. tui.c's cb_usage below still needs to feed the agent a live usage
 * number for that shared check to have anything current to look at.
 */

static void
cb_usage(const struct clm_usage *usage, void *user)
{
	struct ui *u = user;

	/* Tokens carried into the next turn's prompt; drives the context gauge
	 * drawn in the status bar. */
	u->ctx_used = (int64_t)usage->prompt_tokens + usage->completion_tokens;

	/* Cache reads say the prompt prefix stayed byte-stable; a run of turns
	 * with 0 there means every resend is being paid for in full. */
	if (usage->tokens_per_sec > 0 && usage->cache_read_tokens > 0)
		snprintf(u->usage, sizeof(u->usage), "%.0f tok/s, %d cached",
		    usage->tokens_per_sec, usage->cache_read_tokens);
	else if (usage->tokens_per_sec > 0)
		snprintf(u->usage, sizeof(u->usage), "%.0f tok/s",
		    usage->tokens_per_sec);
	else if (usage->cache_read_tokens > 0)
		snprintf(u->usage, sizeof(u->usage), "%d cached",
		    usage->cache_read_tokens);
	else
		u->usage[0] = '\0';
	u->dirty = true;
}

static void
cb_state(enum clm_agent_state st, void *user)
{
	struct ui *u = user;

	/* A mid-chain autocompact (triggered internally between tool
	 * batches, not by tui.c's own end-of-turn check further down) has
	 * no on_turn_done to piggyback a message on -- the whole point is
	 * that the interrupted tool chain resumes silently rather than
	 * ending the turn. Every state transition passes through here
	 * though, including the THINKING transition right after a mid-chain
	 * resume, so poll for a just-failed attempt on every callback. */
	if (clm_agent_take_mid_chain_compact_started(u->agent)) {
		ui_push(u, ST_META,
		    "\n[context over threshold, auto-compacting...]\n");
		/* Stale: it counts a prompt prefix compaction is discarding. */
		u->usage[0] = '\0';
		u->dirty = true;
	}
	if (clm_agent_take_mid_chain_compact_succeeded(u->agent)) {
		ui_push(u, ST_META, "[autocompact complete, resuming]\n");
		session_after_compact(u);
		u->dirty = true;
	}
	if (clm_agent_take_mid_chain_compact_error(u->agent)) {
		ui_push(
		    u, ST_META, "\n[autocompact failed, continuing anyway: ");
		ui_push(u, ST_META, clm_agent_get_last_error(u->agent));
		ui_push(u, ST_META, "]\n");
		u->dirty = true;
	}

	/* Check for mid-turn steering at decision points: after tool batch
	 * completes, before/after LLM response, before tool calls. */
	if (u->steering_nqueue > 0) {
		enum clm_agent_state prev = u->prev_state;
		/* Inject after tools finish (CALLING_TOOL→THINKING), after LLM
		 * finishes thinking (THINKING→CALLING_TOOL or
		 * THINKING→COMPLETE). */
		if ((prev == CLM_STATE_CALLING_TOOL &&
		        st == CLM_STATE_THINKING) ||
		    (prev == CLM_STATE_THINKING &&
		        (st == CLM_STATE_CALLING_TOOL ||
		            st == CLM_STATE_COMPLETE)))
			drain_steering_queue(u);
	}

	u->prev_state = st;
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
	bool was_compacting = u->autocompacting;
	u->autocompacting = false;

	/* -ECANCELED is a user Escape; we already showed "[cancelled]". A
	 * failed autocompact attempt (was_compacting, any other status) is
	 * handled below -- don't show the generic error banner for it or
	 * re-probe the connection, since a compaction failure (e.g. the
	 * compact request itself timing/erroring out, plausible given it
	 * ships the entire pre-compaction history as one extra LLM call)
	 * isn't a sign the backend is actually down, just that this one
	 * shrink attempt didn't work. */
	if (status != 0 && status != -ECANCELED && !was_compacting) {
		ui_push(u, ST_ERROR, "\nerror: ");
		ui_push(u, ST_ERROR, clm_agent_get_last_error(u->agent));
		/* A failed turn may mean the server went away; re-probe. */
		clm_agent_check_connection(u->agent);
	} else if (was_compacting && status != 0 && status != -ECANCELED) {
		ui_push(
		    u, ST_META, "\n[autocompact failed, continuing anyway: ");
		ui_push(u, ST_META, clm_agent_get_last_error(u->agent));
		ui_push(u, ST_META, "]\n");
	}
	ui_push(u, ST_NORMAL, "\n");
	if (u->compacting) {
		u->compacting = false;
		if (status == 0)
			session_after_compact(u);
	}
	u->busy = false;
	u->perm_count = 0; /* no prompt outlives its turn */
	u->perm_showing = false;
	u->batch[0] = '\0';
	u->dirty = true;

	/*
	 * Force a compaction here, before draining the queue or resubmitting
	 * --forever, if usage crossed the threshold on the turn that just
	 * finished.
	 *
	 * This is NOT truly mid-turn -- clm_agent_compact()/clm_agent_cancel()
	 * only have anything to act on while an HTTP request or a tool batch
	 * is actually in flight (checked: emit_usage() fires synchronously
	 * right after a response lands and before the next tool dispatch or
	 * LLM call in the same chain, so there's no live "inflight"/
	 * "active_batch" at that instant for cancel to grab). A single
	 * cdda-style prompt can still chain many tool-calling round-trips
	 * before ever reaching this callback, so an agent stuck in a long
	 * chain can overshoot the threshold before this ever runs. Catching
	 * that earlier would need a hook inside clm_agent_tools_done() (see
	 * agent.c) between a tool batch finishing and the next LLM call
	 * firing -- a real library change, not a tui.c-only hack -- so this
	 * is the honest, working version for now; the harder mid-chain
	 * version is a real follow-up, not implemented here.
	 *
	 * Skipped entirely if this call is itself compaction's own
	 * completion (was_compacting) -- whether it succeeded or failed,
	 * don't immediately try to compact again from in here; either the
	 * context already shrank (success) or a retry belongs on some later
	 * turn once real usage is known again (failure), not stacked
	 * directly on top of the failure in the same callback.
	 */
	if (!was_compacting && (status == 0 || status == -ECANCELED) &&
	    clm_agent_over_autocompact_threshold(u->agent)) {
		int rc = clm_agent_compact(u->agent);
		if (rc == 0) {
			u->busy = true;
			u->autocompacting = true;
			u->compacting = true;
			/* Compaction's own completion re-enters this same
			 * callback (see compact_success_cb/compact_error_cb
			 * in agent.c, both of which call cb_on_turn_done)
			 * before any real usage figure for the shrunk (or
			 * still-oversized, on failure) context is known --
			 * clear the stale pre-compaction ctx_used now so
			 * that re-entry doesn't see the same over-threshold
			 * reading and try to compact again immediately. It'll
			 * be replaced with a real number on the next actual
			 * LLM call either way. The cache-read/rate line goes
			 * with it: it describes the prompt that just got
			 * thrown away. */
			u->ctx_used = 0;
			u->usage[0] = '\0';
			ui_push(u, ST_META,
			    "\n[context over threshold, auto-compacting...]\n");
			return; /* re-enters this function (see above) with
			         * was_compacting true, which then falls
			         * through below to drain the queue / resubmit
			         * regardless of whether compaction itself
			         * succeeded or failed. */
		}
		/* -EBUSY shouldn't happen here (state is idle), and any other
		 * failure just means we fall through and keep going without
		 * having shrunk the context -- not fatal, just try again
		 * next turn. */
	}

	/* Anything typed while the turn ran goes in now, ahead of --forever:
	 * a human waiting on their own prompt outranks the loop's. */
	if (!u->busy && u->steering_nqueue > 0)
		drain_steering_queue(u);

	/* --forever: keep the agent going instead of idling for human input.
	 * Skip on a real error so a broken backend/tool doesn't spin forever
	 * -- UNLESS that "error" was just a failed autocompact attempt
	 * (was_compacting), in which case the original turn before it already
	 * completed successfully and --forever should keep going regardless;
	 * only a failure of the agent's actual conversational turn should stop
	 * the loop. */
	if (!u->busy && u->forever_prompt != NULL &&
	    (status == 0 || status == -ECANCELED || was_compacting))
		do_submit(u, u->forever_prompt, true);
}

/* Push value on one or more indented lines.  The caller has already emitted
 * the parameter name and its separating newline.  Prefix every physical line
 * so embedded source/code indentation remains visibly inside this argument's
 * block rather than being mistaken for a new prompt field. */
static void
push_perm_multiline_value(struct ui *u, const char *value)
{
	const char *line = value;
	const char *nl;

	do {
		nl = strchr(line, '\n');
		ui_push(u, ST_PERM, "    ");
		if (nl != NULL) {
			char *copy = strndup(line, (size_t)(nl - line));
			if (copy != NULL) {
				ui_push(u, ST_PERM, copy);
				free(copy);
			}
			ui_push(u, ST_PERM, "\n");
			line = nl + 1;
		} else {
			ui_push(u, ST_PERM, line);
		}
	} while (nl != NULL);
}

/* Render one argument: name and value on a line, or a named indented block
 * when the value spans lines. */
static void
push_perm_arg(struct ui *u, cJSON *v, bool *first)
{
	autofree char *printed = NULL;
	const char *value;
	bool multiline;

	if (!*first)
		ui_push(u, ST_PERM, "\n");
	*first = false;
	if (cJSON_IsString(v))
		value = cJSON_GetStringValue(v);
	else {
		printed = cJSON_PrintUnformatted(v);
		value = printed != NULL ? printed : "";
	}
	multiline = strchr(value, '\n') != NULL;

	ui_push(u, ST_PERM, "\n  ");
	ui_push(u, ST_PERM, v->string != NULL ? v->string : "?");
	if (multiline) {
		ui_push(u, ST_PERM, ":\n");
		push_perm_multiline_value(u, value);
	} else {
		ui_push(u, ST_PERM, ": ");
		ui_push(u, ST_PERM, value);
	}
}

/* True if names, a JSON array of strings, holds key. */
static bool
json_array_has(cJSON *names, const char *key)
{
	cJSON *n;

	if (key == NULL || !cJSON_IsArray(names))
		return false;
	cJSON_ArrayForEach(n, names) if (cJSON_IsString(n) &&
	    n->valuestring != NULL &&
	    strcmp(n->valuestring, key) == 0) return true;
	return false;
}

/*
 * The order a tool declares its required parameters in, read from the
 * "parameters" object the tool registered. A model emits argument keys in
 * any order it likes, so an edit can print its replacement before the text
 * being replaced. Borrowed from schema; NULL when the tool declared none.
 */
static cJSON *
perm_arg_order(cJSON *schema)
{
	cJSON *req;

	if (!cJSON_IsObject(schema))
		return NULL;
	req = cJSON_GetObjectItemCaseSensitive(schema, "required");
	return cJSON_IsArray(req) ? req : NULL;
}

/*
 * Render permission arguments one per line.  A one-line value stays beside
 * its name.  Multi-line values get a named, indented block and a blank line
 * before the following parameter, making commands and replacement text safe
 * to inspect without confusing their contents with adjacent arguments.
 * Falls back to the raw string if it is not a JSON object.
 */
static void
push_perm_args(struct ui *u, const char *args, const char *schema)
{
	cJSON *obj, *sch, *order, *name;
	bool first = true;

	if (args == NULL || args[0] == '\0' || strcmp(args, "{}") == 0)
		return;

	obj = cJSON_Parse(args);
	if (obj == NULL || !cJSON_IsObject(obj)) {
		ui_push(u, ST_PERM, ":\n");
		push_perm_multiline_value(u, args);
		if (obj != NULL)
			cJSON_Delete(obj);
		return;
	}

	sch = schema != NULL ? cJSON_Parse(schema) : NULL;
	order = perm_arg_order(sch);

	ui_push(u, ST_PERM, ":");
	cJSON_ArrayForEach(name, order)
	{
		cJSON *v;

		if (!cJSON_IsString(name) || name->valuestring == NULL)
			continue;
		v = cJSON_GetObjectItemCaseSensitive(obj, name->valuestring);
		if (v != NULL)
			push_perm_arg(u, v, &first);
	}
	for (cJSON *v = obj->child; v != NULL; v = v->next)
		if (!json_array_has(order, v->string))
			push_perm_arg(u, v, &first);
	cJSON_Delete(sch);
	cJSON_Delete(obj);
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
	const char *name, *args, *schema;

	if (u->perm_count == 0) {
		u->perm_showing = false;
		return;
	}
	req = u->perm_queue[0];
	name = clm_permission_req_name(req);
	args = clm_permission_req_args(req);
	schema = clm_permission_req_schema(req);

	u->perm_showing = true;
	ui_push(u, ST_PERM, "\nallow tool ");
	ui_push(u, ST_PERM, name ? name : "?");
	push_perm_args(u, args, schema);
	if (clm_permission_req_reason(req) != NULL) {
		ui_push(u, ST_PERM, "\nhook: ");
		ui_push(u, ST_PERM, clm_permission_req_reason(req));
	}
	ui_push(u, ST_PERM,
	    "\n(y) once  (n) deny  (a) always  (d) never  [esc = "
	    "deny+cancel]\n");
	u->dirty = true;
}

static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct ui *u = user;

	/* Nobody is watching this one: answer for them. The status bar says
	 * the session is running this way, so it cannot pass for a normal
	 * one. */
	if ((u->allow_all || u->auto_mode) &&
	    clm_permission_req_reason(req) == NULL) {
		clm_tool_permission_respond(u->agent, req, CLM_PERM_ALLOW_ONCE);
		return;
	}

	/* Enqueue the request. */
	if (u->perm_count >= u->perm_cap) {
		size_t ncap = u->perm_cap ? u->perm_cap * 2 : 16;
		const struct clm_permission_req **nq =
		    (const struct clm_permission_req **)realloc(
		        (void *)u->perm_queue, ncap * sizeof(*nq));
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

static void
cb_notice(const char *text, void *user)
{
	struct ui *u = user;

	ui_push(u, ST_META, "\n[");
	ui_push(u, ST_META, text ? text : "notice");
	ui_push(u, ST_META, "]\n");
	u->dirty = true;
}

/*
 * Mirror every appended history message into the session log. A system
 * message becomes a prompt record, kept for reference: the prologue is
 * rebuilt from config on every start and never replayed.
 */
/* Remember a prompt the transcript shows; the oldest goes when full. */
static void
echo_note(struct ui *u, const char *text)
{
	char *copy = strdup(text);

	if (copy == NULL)
		return;
	if (u->n_echoed == sizeof(u->echoed) / sizeof(u->echoed[0])) {
		free(u->echoed[0]);
		memmove((void *)u->echoed, (void *)(u->echoed + 1),
		    (u->n_echoed - 1) * sizeof(u->echoed[0]));
		u->n_echoed--;
	}
	u->echoed[u->n_echoed++] = copy;
}

/* True, and forgets it, if the transcript already shows this text. */
static bool
echo_take(struct ui *u, const char *text)
{
	for (size_t i = 0; i < u->n_echoed; i++) {
		if (strcmp(u->echoed[i], text) != 0)
			continue;
		free(u->echoed[i]);
		memmove((void *)(u->echoed + i), (void *)(u->echoed + i + 1),
		    (u->n_echoed - i - 1) * sizeof(u->echoed[0]));
		u->n_echoed--;
		return true;
	}
	return false;
}

/* Show a user message nobody typed here: an irc event, a monitor line, a
 * finished background job. rebuild_render collapses the body. */
static void
show_event(struct ui *u, const char *text)
{
	ui_push(u, ST_EVENT, text);
}

/* A cut tool result goes whole to the current session's spool. */
static int
cb_spool(const char *call_id, const char *tool, const void *data, size_t len,
    char *path, size_t pathsz, void *user)
{
	struct ui *u = user;

	(void)tool;
	if (u->session == NULL)
		return -ENOENT;
	return clm_session_spool(u->session, call_id, data, len, path, pathsz);
}

static void
cb_message(const struct clm_message *msg, void *user)
{
	struct ui *u = user;
	int r;

	if (msg->role == CLM_ROLE_USER && msg->content != NULL &&
	    strncmp(msg->content, "[context update]", 16) != 0 &&
	    strncmp(msg->content, "[message from agent ", 20) != 0 &&
	    !echo_take(u, msg->content))
		show_event(u, msg->content);

	if (u->session == NULL)
		return;

	r = clm_session_append(u->session, msg, NULL);
	if (r < 0) {
		/* Warn once and stop logging rather than spamming a warning
		 * per message on e.g. a full disk. */
		ui_push(u, ST_META, "\n[session logging failed; disabled]\n");
		clm_session_free(u->session);
		u->session = NULL;
		u->dirty = true;
	}
}

/*
 * Persist a history rewrite. on_message never fires for one, so without
 * this the log would still hold everything compaction just folded away and
 * a resume would replay it all.
 */
/* Cache the tail of the session id for the status bar. */
static void
set_session_short(struct ui *u)
{
	const char *id = u->session != NULL ? clm_session_id(u->session) : NULL;
	size_t len;

	u->session_short[0] = '\0';
	if (id == NULL)
		return;
	len = strlen(id);
	(void)snprintf(u->session_short, sizeof(u->session_short), "%s",
	    len > 8 ? id + len - 8 : id);
}

static void
session_after_compact(struct ui *u)
{
	const struct clm_history *h;

	if (u->session == NULL)
		return;
	h = clm_agent_get_history(u->agent);
	if (h == NULL)
		return;
	if (clm_session_rewrite(u->session, h, NULL) < 0) {
		ui_push(u, ST_META, "\n[session rewrite failed; disabled]\n");
		clm_session_free(u->session);
		u->session = NULL;
		u->dirty = true;
	}
}

/* Show what another agent said. The agent itself gets the same text via
 * clm_agent_notify, framed with the sender's id. */
static void
cb_peer_message(
    const char *from, const char *name, const char *text, void *user)
{
	struct ui *u = user;
	char head[128];
	size_t n = strlen(from);

	(void)snprintf(head, sizeof(head), "\npeer %s (%s)> ",
	    n > 8 ? from + n - 8 : from, name != NULL ? name : "?");
	ui_push(u, ST_META, head);
	ui_push(u, ST_USER, text);
	ui_push(u, ST_USER, "\n");
	u->dirty = true;
}

/*
 * Show a framed peer message from a resumed history the way it showed live.
 * Returns false when the content is not one, so the caller shows it as is.
 */
static bool
replay_peer_message(struct ui *u, const char *content)
{
	static const char pre[] = "[message from agent ";
	autofree char *copy = NULL;
	char *head, *name, *text, *tail;

	if (strncmp(content, pre, sizeof(pre) - 1) != 0)
		return false;
	copy = strdup(content + sizeof(pre) - 1);
	if (copy == NULL)
		return false;
	text = strstr(copy, "]\n");
	if (text == NULL)
		return false;
	*text = '\0';
	text += 2;
	head = copy;
	name = strstr(head, " (");
	if (name == NULL || name[strlen(name) - 1] != ')')
		return false;
	*name = '\0';
	name += 2;
	name[strlen(name) - 1] = '\0';
	/* Drop the reply hint that deliver appends for the model. */
	tail = strrchr(text, '\n');
	if (tail != NULL && tail[1] == '(')
		*tail = '\0';
	cb_peer_message(head, name, text, u);
	return true;
}

/* Move the peer socket onto the current session id. Other agents address
 * this one by that id, so a new session needs a new socket. */
static void
peer_rebind(struct ui *u)
{
	if (u->peer == NULL || u->session == NULL)
		return;
	clm_peer_free(u->peer);
	u->peer = NULL;
	/* The tools stay registered from startup; only the socket moves. */
	(void)clm_peer_start(u->agent, u->loop, clm_session_id(u->session),
	    u->agent_name != NULL ? u->agent_name : "clm", u->model,
	    cb_peer_message, u, &u->peer);
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
    .on_notice = cb_notice,
    .on_message = cb_message,
};

/* ---- rendering (all drawing happens here, from the repaint timer) ---- */

static int
seg_attr(struct ui *u, enum ui_style style)
{
	if (u->color) {
		switch (style) {
		case ST_USER:
			return COLOR_PAIR(1);
		case ST_LABEL:
			return COLOR_PAIR(8);
		case ST_PERM:
			return COLOR_PAIR(9);
		case ST_REASON:
			return COLOR_PAIR(6);
		case ST_TOOL:
			return COLOR_PAIR(2);
		case ST_ERROR:
			return COLOR_PAIR(3);
		case ST_TIMEOUT:
			return COLOR_PAIR(4);
		default:
			break;
		}
	} else {
		switch (style) {
		case ST_USER:
		case ST_LABEL:
			return A_BOLD;
		case ST_PERM:
			return A_BOLD | A_UNDERLINE;
		case ST_REASON:
			return A_DIM;
		case ST_TOOL:
			return A_UNDERLINE;
		case ST_ERROR:
			return A_BOLD | A_REVERSE;
		case ST_TIMEOUT:
			return A_REVERSE;
		default:
			break;
		}
	}
	switch (style) {
	case ST_ASSIST:
		return A_NORMAL;
	case ST_TOOL_OUT:
	case ST_EVENT:
	case ST_META:
	case ST_BATCH:
		return A_DIM;
	default:
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
	case CLM_STATE_RATE_LIMITED:
		return "waiting"; /* animated with dots in draw_status directly
		                   */
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

/* ---- help popup ---- */

/* Keys for the popup; the slash commands come from ui_commands. */
static const char *const help_keys[][2] = {
    {"^G", "open or close this help"},
    {"Enter", "send the prompt"},
    {"Esc", "cancel the running turn, else clear the input"},
    {"^C", "quit"},
    {"^D", "quit, on an empty line"},
    {"^R", "show or hide reasoning"},
    {"^O", "full or short tool output and commands"},
    {"^L", "redraw the screen"},
    {"PgUp PgDn wheel", "scroll the transcript"},
    {"End", "scroll to the bottom, on an empty line"},
    {"Up Down", "recall earlier prompts"},
    {"Tab", "complete commands, names and paths"},
    {"^A ^E ^B ^F", "start, end, back, forward"},
    {"^U ^K ^Y", "kill to start, kill to end, yank"},
};

struct help_text {
	char **line;
	bool *bold;
	int n, cap;
};

static void
help_add(struct help_text *h, const char *s, int len, bool bold)
{
	if (h->n == h->cap) {
		int cap = h->cap ? h->cap * 2 : 64;
		char **l =
		    (char **)realloc((void *)h->line, (size_t)cap * sizeof(*l));
		bool *b;

		if (l == NULL)
			return;
		h->line = l;
		b = realloc(h->bold, (size_t)cap * sizeof(*b));
		if (b == NULL)
			return;
		h->bold = b;
		h->cap = cap;
	}
	h->line[h->n] = strndup(s, (size_t)len);
	if (h->line[h->n] == NULL)
		return;
	h->bold[h->n++] = bold;
}

/*
 * One entry: left in a column of lw cells, right wrapped beside it. When
 * the popup is too narrow for two columns, right goes below, indented.
 */
static void
help_entry(
    struct help_text *h, const char *left, const char *right, int w, int lw)
{
	char buf[256];
	int indent, cw;
	bool first = true;

	if (w < lw + 20) {
		help_add(h, left, (int)strlen(left), false);
		indent = 4;
		first = false;
	} else {
		indent = lw;
	}
	cw = w - indent;
	if (cw < 8)
		cw = 8;
	while (*right != '\0') {
		int n = (int)strlen(right);

		if (n > cw) {
			n = cw;
			while (n > 0 && right[n] != ' ')
				n--;
			if (n == 0)
				n = cw;
		}
		(void)snprintf(buf, sizeof(buf), "%-*.*s%.*s", indent, indent,
		    first ? left : "", n, right);
		help_add(h, buf, (int)strlen(buf), false);
		first = false;
		right += n;
		while (*right == ' ')
			right++;
	}
}

static void help_free(struct help_text *h);

/* Command entries for the popup: the left and right column of each. */
struct help_cmds {
	struct help_text left, right;
};

static void
help_cmd_add(
    struct help_cmds *c, const char *name, const char *args, const char *right)
{
	char left[64];

	(void)snprintf(left, sizeof(left), "/%s%s%s", name,
	    args[0] != '\0' ? " " : "", args);
	help_add(&c->left, left, (int)strlen(left), false);
	help_add(&c->right, right, (int)strlen(right), false);
	/* Keep the two columns paired if one add ran out of memory. */
	if (c->left.n != c->right.n) {
		int n = c->left.n < c->right.n ? c->left.n : c->right.n;

		while (c->left.n > n)
			free(c->left.line[--c->left.n]);
		while (c->right.n > n)
			free(c->right.line[--c->right.n]);
	}
}

/* A plugin command; one a built-in name hides is left out. */
static void
help_cmd_plugin(const char *name, const char *args, const char *help,
    const char *plugin, void *user)
{
	char right[192];

	if (ui_command_find(name, strlen(name)) != NULL)
		return;
	(void)snprintf(right, sizeof(right), "%s%s(%s)", help,
	    help[0] != '\0' ? " " : "", plugin);
	help_cmd_add(user, name, args, right);
}

static void
help_build(struct ui *u, struct help_text *h, int w)
{
	struct help_cmds c = {0};
	char right[192];
	size_t off;
	int lw = 0, nbuiltin;

	for (size_t i = 0; i < ui_ncommands; i++) {
		const struct ui_command *cmd = &ui_commands[i];

		off = (size_t)snprintf(right, sizeof(right), "%s", cmd->help);
		for (const char *a = cmd->aliases; a != NULL && *a != '\0';) {
			size_t n = strcspn(a, " ");

			if (off < sizeof(right))
				off += (size_t)snprintf(right + off,
				    sizeof(right) - off, "%s/%.*s",
				    a == cmd->aliases ? "; also " : ", ",
				    (int)n, a);
			a += n;
			a += strspn(a, " ");
		}
		help_cmd_add(&c, cmd->name, cmd->args, right);
	}
	nbuiltin = c.left.n;
	clm_lua_commands(u->lua_env, help_cmd_plugin, &c);

	/* The left column fits the longest key or command, plus a gap. */
	for (size_t i = 0; i < sizeof(help_keys) / sizeof(help_keys[0]); i++)
		if ((int)strlen(help_keys[i][0]) > lw)
			lw = (int)strlen(help_keys[i][0]);
	for (int i = 0; i < c.left.n; i++)
		if ((int)strlen(c.left.line[i]) > lw)
			lw = (int)strlen(c.left.line[i]);
	lw += 2;

	help_add(h, "keys", 4, true);
	for (size_t i = 0; i < sizeof(help_keys) / sizeof(help_keys[0]); i++)
		help_entry(h, help_keys[i][0], help_keys[i][1], w, lw);
	help_add(h, "", 0, false);
	help_add(h, "commands", 8, true);
	for (int i = 0; i < c.left.n; i++) {
		if (i == nbuiltin) {
			help_add(h, "", 0, false);
			help_add(h, "plugin commands", 15, true);
		}
		help_entry(h, c.left.line[i], c.right.line[i], w, lw);
	}
	help_free(&c.left);
	help_free(&c.right);
}

static void
help_free(struct help_text *h)
{
	for (int i = 0; i < h->n; i++)
		free(h->line[i]);
	free((void *)h->line);
	free(h->bold);
}

static void
help_open(struct ui *u)
{
	u->help_shown = true;
	u->help_top = 0;
	u->dirty = true;
}

static void
help_close(struct ui *u)
{
	u->help_shown = false;
	if (u->help != NULL) {
		delwin(u->help);
		u->help = NULL;
	}
	u->full_redraw = true;
	u->dirty = true;
}

/* Rows of text the popup shows at once, for paging. */
static int
help_rows(struct ui *u)
{
	return u->help != NULL ? getmaxy(u->help) - 2 : 1;
}

/* Draw the popup over the transcript. Rebuilt each time: it is small. */
static void
draw_help(struct ui *u)
{
	struct help_text h = {0};
	int ty, tx, th, tw, w, hh, rows;
	char foot[48];

	if (!u->help_shown)
		return;
	getbegyx(u->txt, ty, tx);
	getmaxyx(u->txt, th, tw);
	(void)tx;
	w = tw < 80 ? tw : 80;
	if (w < 20 || th < 5)
		return;
	help_build(u, &h, w - 4);
	hh = h.n + 2 < th ? h.n + 2 : th;
	rows = hh - 2;
	if (u->help_top > h.n - rows)
		u->help_top = h.n - rows;
	if (u->help_top < 0)
		u->help_top = 0;

	if (u->help != NULL)
		delwin(u->help);
	u->help = newwin(hh, w, ty + (th - hh) / 2, (COLS - w) / 2);
	if (u->help == NULL) {
		help_free(&h);
		return;
	}
	werase(u->help);
	box(u->help, 0, 0);
	mvwprintw(u->help, 0, 2, " help ");
	for (int i = 0; i < rows && u->help_top + i < h.n; i++) {
		int k = u->help_top + i;

		if (h.bold[k])
			wattron(u->help, A_BOLD);
		mvwaddnstr(u->help, i + 1, 2, h.line[k], w - 4);
		if (h.bold[k])
			wattroff(u->help, A_BOLD);
	}
	if (h.n > rows)
		(void)snprintf(foot, sizeof(foot), " %d-%d of %d, ^G close ",
		    u->help_top + 1, u->help_top + rows, h.n);
	else
		(void)snprintf(foot, sizeof(foot), " ^G close ");
	if ((int)strlen(foot) < w - 2)
		mvwprintw(
		    u->help, hh - 1, w - 1 - (int)strlen(foot), "%s", foot);
	wnoutrefresh(u->help);
	help_free(&h);
}

/*
 * A key while the popup is open. Scroll keys move it; ^G and Esc close
 * it. Any other key closes it and is handled as usual, so typing goes on
 * without a separate close. Returns true when the key is used up.
 */
static bool
help_key(struct ui *u, int r, wint_t wch)
{
	int page = help_rows(u);

	if (r == KEY_CODE_YES) {
		switch (wch) {
		case KEY_UP:
			u->help_top--;
			break;
		case KEY_DOWN:
			u->help_top++;
			break;
		case KEY_PPAGE:
			u->help_top -= page;
			break;
		case KEY_NPAGE:
			u->help_top += page;
			break;
		case KEY_HOME:
			u->help_top = 0;
			break;
		case KEY_END:
			u->help_top = INT_MAX / 2;
			break;
		case KEY_MOUSE: {
			MEVENT ev;

			if (getmouse(&ev) != OK)
				return true;
			if (ev.bstate & BUTTON4_PRESSED)
				u->help_top -= 3;
			else if (ev.bstate & BUTTON5_PRESSED)
				u->help_top += 3;
			break;
		}
		case KEY_RESIZE:
			return false;
		default:
			help_close(u);
			return false;
		}
		u->dirty = true;
		return true;
	}
	help_close(u);
	return wch == 7 || wch == 27;
}

/*
 * One piece of the status bar. When the bar is too wide, the piece with
 * the highest shrink or drop rank goes first: a shrink swaps in the short
 * text, a drop removes the piece. Rank 0 never goes.
 */
struct stat_piece {
	char text[112];
	char shrt[80]; /* "" = no short form */
	int shrink, drop;
	bool short_on, gone;
};

/* Cells a UTF-8 string takes; every character in the bar is one cell. */
static int
stat_cells(const char *s)
{
	int n = 0;

	for (; *s != '\0'; s++)
		if (((unsigned char)*s & 0xC0) != 0x80)
			n++;
	return n;
}

static int
stat_width(const struct stat_piece *p, int np)
{
	int w = 0;

	for (int i = 0; i < np; i++)
		if (!p[i].gone)
			w += 1 +
			    stat_cells(p[i].short_on ? p[i].shrt : p[i].text);
	return w;
}

/* Shrink or drop pieces, highest rank first, until the bar fits in w. */
static void
stat_fit(struct stat_piece *p, int np, int w)
{
	while (stat_width(p, np) > w) {
		int best = -1, rank = 0;
		bool shrink = false;

		for (int i = 0; i < np; i++) {
			if (p[i].gone)
				continue;
			if (!p[i].short_on && p[i].shrt[0] != '\0' &&
			    p[i].shrink > rank) {
				best = i;
				rank = p[i].shrink;
				shrink = true;
			}
			if (p[i].drop > rank) {
				best = i;
				rank = p[i].drop;
				shrink = false;
			}
		}
		if (best < 0)
			return;
		if (shrink)
			p[best].short_on = true;
		else
			p[best].gone = true;
	}
}

enum {
	SP_SESSION,
	SP_MODE,
	SP_TAG,
	SP_CONN,
	SP_INFO,
	SP_GAUGE,
	SP_HINT,
	SP_N,
};

static void
draw_status(struct ui *u)
{
	static const char spin[] = "|/-\\";
	struct stat_piece p[SP_N];
	const char *info;
	char sp = u->busy || u->cmd_live > 0 ? spin[u->spinner & 3] : ' ';
	int w = getmaxx(u->stat);

	werase(u->stat);
	if (u->color) {
		wbkgd(u->stat, COLOR_PAIR(5));
		wattron(u->stat, COLOR_PAIR(5));
	} else {
		wbkgd(u->stat, A_REVERSE);
		wattron(u->stat, A_REVERSE);
	}

	/* Rate-limit wait wins over the leftover batch/usage strings: while
	 * the turn is parked nothing else is in flight, and a stale "N tok/s"
	 * would hide the only signal that explains the silence. The usage
	 * string only shows between turns for the same reason: it reports the
	 * turn that just ended, so it must not sit where "thinking" belongs. */
	if (u->state == CLM_STATE_RATE_LIMITED) {
		static const char *dots[] = {
		    "waiting.", "waiting..", "waiting..."};
		info = dots[((unsigned)u->spinner / 4) % 3];
	} else if (u->batch[0] != '\0')
		info = u->batch;
	else if (!u->busy && u->cmd_live > 0)
		info = u->cmd_name;
	else if (!u->busy && u->usage[0] != '\0')
		info = u->usage;
	else
		info = state_label(u->state);

	memset(p, 0, sizeof(p));

	/* Ranks, first to go: hint, [online], provider and agent, session
	 * id, offline detail, model, gauge, mode and offline. The live
	 * status never goes. */
	(void)snprintf(p[SP_SESSION].text, sizeof(p[SP_SESSION].text), "%s",
	    u->session_short);
	p[SP_SESSION].drop = 6;
	p[SP_SESSION].gone = u->session_short[0] == '\0';

	if (u->allow_all)
		(void)snprintf(
		    p[SP_MODE].text, sizeof(p[SP_MODE].text), "[allow-all]");
	else if (u->auto_mode)
		(void)snprintf(
		    p[SP_MODE].text, sizeof(p[SP_MODE].text), "[auto]");
	else
		p[SP_MODE].gone = true;
	p[SP_MODE].drop = 2;

	/* "[provider/model:agent]": '/' as in the provider/model-id spec
	 * that -m and /model take, ':' before the agent profile, which is a
	 * separate axis. The short form is the model alone. */
	{
		char tag[96];
		size_t off = 0;

		tag[0] = '\0';
		if (u->provider_name != NULL)
			off += (size_t)snprintf(tag + off, sizeof(tag) - off,
			    "%s", u->provider_name);
		if (u->model != NULL && off < sizeof(tag))
			off += (size_t)snprintf(tag + off, sizeof(tag) - off,
			    "%s%s", off > 0 ? "/" : "", u->model);
		if (u->agent_name != NULL && off < sizeof(tag))
			(void)snprintf(tag + off, sizeof(tag) - off, "%s%s",
			    off > 0 ? ":" : "", u->agent_name);
		(void)snprintf(
		    p[SP_TAG].text, sizeof(p[SP_TAG].text), "[%s]", tag);
		if (u->model != NULL)
			(void)snprintf(p[SP_TAG].shrt, sizeof(p[SP_TAG].shrt),
			    "[%.*s]", (int)sizeof(p[SP_TAG].shrt) - 3,
			    u->model);
		p[SP_TAG].gone = tag[0] == '\0';
		p[SP_TAG].shrink = 7;
		p[SP_TAG].drop = 4;
	}

	switch (u->conn) {
	case CLM_CONN_CHECKING:
		(void)snprintf(
		    p[SP_CONN].text, sizeof(p[SP_CONN].text), "[connecting]");
		p[SP_CONN].drop = 8;
		break;
	case CLM_CONN_ONLINE:
		(void)snprintf(
		    p[SP_CONN].text, sizeof(p[SP_CONN].text), "[online]");
		p[SP_CONN].drop = 8;
		break;
	case CLM_CONN_OFFLINE:
		(void)snprintf(
		    p[SP_CONN].shrt, sizeof(p[SP_CONN].shrt), "[offline]");
		if (u->conn_detail[0] != '\0')
			(void)snprintf(p[SP_CONN].text, sizeof(p[SP_CONN].text),
			    "[offline: %s]", u->conn_detail);
		else
			(void)snprintf(p[SP_CONN].text, sizeof(p[SP_CONN].text),
			    "[offline]");
		p[SP_CONN].shrink = 5;
		p[SP_CONN].drop = 2;
		break;
	}

	/* Fixed width so the animated waiting dots don't shift the gauge. */
	(void)snprintf(
	    p[SP_INFO].text, sizeof(p[SP_INFO].text), " %c %-14s", sp, info);

	fmt_ctx_gauge(u, p[SP_GAUGE].text, sizeof(p[SP_GAUGE].text));
	p[SP_GAUGE].gone = p[SP_GAUGE].text[0] == '\0';
	p[SP_GAUGE].drop = 3;

	(void)snprintf(p[SP_HINT].text, sizeof(p[SP_HINT].text), "^G help");
	p[SP_HINT].drop = 9;

	stat_fit(p, SP_N, w - 1);

	wmove(u->stat, 0, 0);
	for (int i = 0; i < SP_HINT; i++) {
		if (p[i].gone)
			continue;
		wprintw(u->stat, " %s", p[i].short_on ? p[i].shrt : p[i].text);
	}
	/* The hint sits at the right edge, apart from the live status. */
	if (!p[SP_HINT].gone)
		mvwprintw(u->stat, 0, w - 1 - stat_cells(p[SP_HINT].text), "%s",
		    p[SP_HINT].text);

	wattroff(u->stat, u->color ? COLOR_PAIR(5) : A_REVERSE);
	wnoutrefresh(u->stat);
}

/*
 * Walk the input text the way curses lays it out in the box: '\n' is a hard
 * line break, not a near-zero-width character. A bracketed paste can land
 * literal newlines in u->input (see UI_KEY_PASTE_START), and a short
 * multi-line paste's total display width can be less than one row.
 *
 * Returns the rows the text needs. When cy/cx are non-NULL they receive the
 * row and column the byte at `upto` sits at, so the cursor lands where its
 * character is drawn.
 */
static int
input_walk(struct ui *u, int w, size_t upto, int *cy, int *cx)
{
	mbstate_t ps;
	size_t i = 0;
	int rows = 1;
	int row = 0;
	int col = 2; /* "> " prefix */

	if (w < 1)
		w = 1;
	if (cy != NULL) {
		*cy = row;
		*cx = col;
	}

	memset(&ps, 0, sizeof(ps));
	while (i < u->input_len) {
		wchar_t wc;
		size_t k;
		int cw = 1;

		if (u->input[i] == '\n') {
			if (i == upto && cy != NULL) {
				*cy = row;
				*cx = col;
			}
			rows++;
			row++;
			col = 0;
			i++;
			continue;
		}

		k = mbrtowc(&wc, u->input + i, u->input_len - i, &ps);
		if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
			k = 1; /* invalid/incomplete byte: one column, advance
			          one byte */
		} else {
			cw = wcwidth(wc);
			if (cw < 0)
				cw = 1;
		}

		/* A character that does not fit is drawn on the next row. */
		if (col + cw > w) {
			rows++;
			row++;
			col = 0;
		}
		if (i == upto && cy != NULL) {
			*cy = row;
			*cx = col;
		}
		col += cw;
		i += k;
		/* Filling the last column moves the curses cursor to the next
		 * row at once, so the text needs that row too. */
		if (col >= w) {
			rows++;
			row++;
			col = 0;
		}
	}
	if (upto >= u->input_len && cy != NULL) {
		*cy = row;
		*cx = col;
	}
	return rows;
}

static int
input_rows(struct ui *u, int w)
{
	return input_walk(u, w, u->input_len, NULL, NULL);
}

static void
draw_input(struct ui *u)
{
	int w = getmaxx(u->in);
	int h = getmaxy(u->in);
	int cy, cx, rows;

	werase(u->in);
	wmove(u->in, 0, 0);
	waddstr(u->in, "> ");
	/* Let curses wrap the text across the box's rows. */
	waddnstr(u->in, u->input, (int)u->input_len);

	rows = input_walk(u, w, u->input_pos, &cy, &cx);
	/* A capped box scrolled its first rows away: move the cursor up with
	 * the text. A cursor inside the lost rows sits at the top. */
	if (rows > h)
		cy -= rows - h;
	if (cy < 0) {
		cy = 0;
		cx = 0;
	}
	wmove(u->in, cy, cx);
	wnoutrefresh(u->in);
}

/*
 * Blank columns kept between the transcript text and both screen edges, so
 * lines do not run into the terminal border. Dropped on a screen too narrow
 * to spare them.
 */
#define CLM_TXT_MARGIN 2

/* Left origin and width of the transcript window on a screen w columns wide. */
static void
txt_geom(int w, int *x, int *tw)
{
	int m = w >= CLM_TXT_MARGIN * 4 ? CLM_TXT_MARGIN : 0;

	*x = m;
	*tw = w - 2 * m;
	if (*tw < 1)
		*tw = 1;
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
	int need, maxih, txt_h, txt_x, txt_w;

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

	/* The transcript window holds scrollback content, so only move and
	 * resize it in place. The status and input windows carry no state,
	 * so recreate them where they now belong. */
	txt_geom(w, &txt_x, &txt_w);
	wresize(u->txt, txt_h, txt_w);
	mvwin(u->txt, 0, txt_x);
	delwin(u->stat);
	delwin(u->in);
	u->stat = newwin(1, w, txt_h, 0);
	u->in = newwin(need, w, txt_h + 1, 0);
	/* Rows change hands here, and curses' idea of the physical screen
	 * no longer matches what the old windows left in the gutters. */
	clearok(curscr, TRUE);
	scrollok(u->in, TRUE);
	keypad(u->in, TRUE);
	nodelay(u->in, TRUE);
	return true;
}

/* Make room for one more rendered run. Returns false when it cannot. */
static bool
rseg_reserve(struct ui *u)
{
	size_t ncap;
	struct rseg *p;

	if (u->nrsegs < u->cap_rsegs)
		return true;
	ncap = u->cap_rsegs ? u->cap_rsegs * 2 : 32;
	p = realloc(u->rsegs, ncap * sizeof(*p));
	if (p == NULL)
		return false;
	u->rsegs = p;
	u->cap_rsegs = ncap;
	return true;
}

/* Append a rendered run to the cache, copying len bytes of text. */
static void
rseg_push(struct ui *u, int attr, const char *text, size_t len)
{
	char *dup;

	if (len == 0 || !rseg_reserve(u))
		return;
	dup = malloc(len + 1);
	if (dup == NULL)
		return;
	memcpy(dup, text, len);
	dup[len] = '\0';
	u->rsegs[u->nrsegs].attr = attr;
	u->rsegs[u->nrsegs].text = dup;
	u->rsegs[u->nrsegs].owned = true;
	u->nrsegs++;
}

/*
 * Append a run that points straight at a source span, for text that renders
 * verbatim. The span outlives the run: ui_push bumps gen whenever a span
 * moves or goes away, which rebuilds the cache before the next draw.
 */
static void
rseg_push_ref(struct ui *u, int attr, char *text)
{
	if (text == NULL || *text == '\0' || !rseg_reserve(u))
		return;
	u->rsegs[u->nrsegs].attr = attr;
	u->rsegs[u->nrsegs].text = text;
	u->rsegs[u->nrsegs].owned = false;
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
	if (run->style & MD_ST_CODE) {
		if (s->u->color)
			attr = (attr & ~(unsigned)A_COLOR) |
			    (unsigned)COLOR_PAIR(7);
		else
			attr = (attr & ~(unsigned)A_COLOR) | (unsigned)A_BOLD;
	}

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
render_markdown(
    struct ui *u, const char *md, int w, int base_attr, bool no_bold)
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

/* Emit a single-line tool detail: tabs escaped, clamped to one row with a
 * trailing "..." unless expanded, where it is emitted whole in chunks. */
static void
emit_tool_detail_inline(struct ui *u, int attr, const char *detail, bool expand)
{
	char buf[512];
	size_t n = 0, cap = sizeof(buf) - 5; /* room for "...\n" + a tab pair */
	const char *p = detail;

	for (; *p != '\0'; p++) {
		if (!expand && n >= cap)
			break;
		if (expand && n >= cap) {
			rseg_push(u, attr, buf, n);
			n = 0;
		}
		if (*p == '\t') {
			buf[n++] = '\\';
			buf[n++] = 't';
		} else {
			buf[n++] = *p;
		}
	}
	if (!expand && *p != '\0') {
		buf[n++] = '.';
		buf[n++] = '.';
		buf[n++] = '.';
	}
	buf[n++] = '\n';
	rseg_push(u, attr, buf, n);
}

/* Emit a multi-line tool detail as an indented block, elided after
 * TOOL_SUMMARY_MAX_LINES unless expanded. */
static void
emit_tool_detail_block(struct ui *u, int attr, const char *detail, bool expand)
{
	const char *p = detail, *nl;
	size_t shown = 0;

	for (; *p != '\0'; p = nl + 1) {
		char buf[512];
		size_t len;

		nl = strchr(p, '\n');
		len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		if (expand) {
			rseg_push(u, attr, "    ", 4);
			rseg_push(u, attr, p, len);
			rseg_push(u, attr, "\n", 1);
		} else if (shown == TOOL_SUMMARY_MAX_LINES) {
			rseg_push(u, attr, "      ...\n", 10);
			return;
		} else {
			if (len >= sizeof(buf) - 8)
				len = sizeof(buf) - 8;
			int w = snprintf(
			    buf, sizeof(buf), "    %.*s\n", (int)len, p);
			rseg_push(u, attr, buf, (size_t)w);
		}
		shown++;
		if (nl == NULL)
			return;
	}
}

/* Render a packed "verb<sep>detail" ST_TOOL segment. A single-line detail
 * follows the verb on one row; a multi-line one becomes an indented block.
 * ^O (expand) keeps the whole detail instead of the elided preview. */
static void
emit_tool_summary(struct ui *u, const char *packed, bool expand)
{
	const char *sep = strchr(packed, TOOL_SUMMARY_SEP);
	size_t verblen = sep != NULL ? (size_t)(sep - packed) : strlen(packed);
	const char *detail = sep != NULL ? sep + 1 : "";
	int attr = seg_attr(u, ST_TOOL);
	bool multiline = strchr(detail, '\n') != NULL;
	bool has_detail = *detail != '\0';
	char head[128];

	(void)snprintf(head, sizeof(head), "  %.*s%s", (int)verblen, packed,
	    has_detail && !multiline ? ": " : "\n");
	rseg_push(u, attr, head, strlen(head));
	if (!has_detail)
		return;
	if (multiline)
		emit_tool_detail_block(u, attr, detail, expand);
	else
		emit_tool_detail_inline(u, attr, detail, expand);
}

/* Emit a tool-output body: full when expanded, else the first few lines plus a
 * dim "+N lines" hint the user can toggle open with ^O. Older tool output
 * (is_latest false) collapses further still, down to a single hint line with
 * no body at all -- once a call is no longer the most recent one, its output
 * is rarely what the user is looking at, and letting every call in a long
 * session leave ~8 lines behind makes the transcript unscannable. ^O
 * (u->expand_output) overrides this for every call, latest or not. */
static void
push_tool_output(struct ui *u, const char *text, bool is_latest)
{
	enum { PREVIEW = 6 };
	size_t len = strlen(text);
	int total = count_lines(text, len);
	size_t cut;
	int seen;
	char hint[64];

	if (u->expand_output || (is_latest && total <= PREVIEW)) {
		rseg_push(u, seg_attr(u, ST_TOOL_OUT), text, len);
		return;
	}
	if (!is_latest) {
		snprintf(hint, sizeof(hint), "  ... (%d lines, ^O to expand)\n",
		    total);
		rseg_push(u, seg_attr(u, ST_TOOL_OUT), hint, strlen(hint));
		return;
	}
	cut = 0;
	seen = 0;
	while (cut < len && seen < PREVIEW) {
		if (text[cut] == '\n')
			seen++;
		cut++;
	}
	rseg_push(u, seg_attr(u, ST_TOOL_OUT), text, cut);
	snprintf(hint, sizeof(hint), "  ... (+%d lines, ^O to expand)\n",
	    total - PREVIEW);
	rseg_push(u, seg_attr(u, ST_TOOL_OUT), hint, strlen(hint));
}

/* Emit an event body: the latest shows its first lines, older ones their
 * first line, each with a hint when there is more. ^O shows all. */
static void
push_event(struct ui *u, const char *text, bool is_latest)
{
	int keep = is_latest ? CLM_TUI_EVENT_LINES : 1;
	size_t len = strlen(text), cut = 0;
	int total = count_lines(text, len), seen = 0;
	char hint[64];

	rseg_push(u, seg_attr(u, ST_META), "\nevent> ", 8);
	if (u->expand_output || total <= keep) {
		rseg_push(u, seg_attr(u, ST_EVENT), text, len);
	} else {
		while (cut < len && seen < keep) {
			if (text[cut] == '\n')
				seen++;
			cut++;
		}
		rseg_push(u, seg_attr(u, ST_EVENT), text, cut);
		snprintf(hint, sizeof(hint), "  ... (+%d lines, ^O to expand)",
		    total - keep);
		rseg_push(u, seg_attr(u, ST_EVENT), hint, strlen(hint));
	}
	rseg_push(u, seg_attr(u, ST_META), "\n", 1);
}

/* Render one combined "-- ran N commands, read N files, ..." line summarizing
 * every tool-call cluster older than the most recent one, in place of their
 * individual "executing ..."/output/tally lines. ^O (u->expand_output)
 * bypasses this in rebuild_render, so this is only ever reached collapsed. */
static void
push_collapsed_summary(struct ui *u, const int cnt[CLM_TALLY_N])
{
	char line[160];

	if (format_tool_tally(line, sizeof(line), cnt, " (^O to expand)\n"))
		rseg_push(u, seg_attr(u, ST_META), line, strlen(line));
}

/* True if the event at i led to tool calls within [i, end]: only those fold
 * into the tally, so an event answered in words stays in view. */
static bool
event_starts_tools(const struct ui *u, size_t i, size_t end)
{
	for (size_t j = i + 1; j <= end && j < u->nsegs; j++) {
		enum ui_style st = u->segs[j].style;

		if (st == ST_TOOL)
			return true;
		if (st != ST_EVENT && st != ST_REASON && st != ST_META)
			return false;
	}
	return false;
}

/* Resolve the source span list into the rendered run cache for width w. */
static void
rebuild_render(struct ui *u, int w)
{
	size_t last_tool_out = (size_t)-1, last_event = (size_t)-1;
	size_t last_batch = (size_t)-1, collapse_end = (size_t)-1;
	int agg[CLM_TALLY_N] = {0};
	bool aggregating = false;

	for (size_t i = 0; i < u->nrsegs; i++) {
		if (u->rsegs[i].owned)
			free(u->rsegs[i].text);
	}
	u->nrsegs = 0;
	u->wrap_valid = false; /* rsegs are about to change shape */

	/* Find the most recent ST_TOOL_OUT segment so push_tool_output can
	 * leave only it at full preview depth and collapse the rest. */
	for (size_t i = u->nsegs; i > 0; i--) {
		if (u->segs[i - 1].style == ST_TOOL_OUT) {
			last_tool_out = i - 1;
			break;
		}
	}
	for (size_t i = u->nsegs; i > 0; i--) {
		if (u->segs[i - 1].style == ST_EVENT) {
			last_event = i - 1;
			break;
		}
	}

	/* Find the two most recent ST_BATCH segments. Everything up to and
	 * including the second-to-last one (collapse_end) belongs to a
	 * completed tool-call cluster older than the latest one, and gets
	 * folded into a single aggregate below instead of rendered
	 * individually. Skipped entirely (collapse_end left at -1) when ^O
	 * is on, or when there's at most one completed cluster so far --
	 * nothing older to collapse yet. */
	if (!u->expand_output) {
		for (size_t i = u->nsegs; i > 0; i--) {
			if (u->segs[i - 1].style != ST_BATCH)
				continue;
			if (last_batch == (size_t)-1) {
				last_batch = i - 1;
				continue;
			}
			collapse_end = i - 1;
			break;
		}
	}

	for (size_t i = 0; i < u->nsegs; i++) {
		const struct seg *g = &u->segs[i];

		/* Checked before the collapse logic below: a hidden think-
		 * channel segment is invisible either way and must not count
		 * as an "interruption" that breaks a run of collapsed tool
		 * clusters -- models routinely emit one of these between every
		 * tool call, which used to flush (and restart) the aggregate
		 * once per call, defeating the whole point of collapsing. */
		if (g->style == ST_REASON && !u->show_reasoning)
			continue; /* think channel hidden */

		/* Mute tool-cluster segments in the collapsed range, tallying
		 * ST_BATCH's counts as we go; anything else in that range
		 * (e.g. assistant text interleaved between tool calls) still
		 * renders normally, flushing any pending aggregate first so
		 * ordering stays correct. */
		if (collapse_end != (size_t)-1 && i <= collapse_end &&
		    (g->style == ST_TOOL || g->style == ST_TOOL_OUT ||
		        g->style == ST_BATCH ||
		        (g->style == ST_EVENT &&
		            event_starts_tools(u, i, collapse_end)))) {
			if (g->style == ST_BATCH)
				for (size_t k = 0; k < CLM_TALLY_N; k++)
					agg[k] += g->cnt[k];
			else if (g->style == ST_EVENT)
				agg[4]++;
			aggregating = true;
			continue;
		}
		if (aggregating) {
			push_collapsed_summary(u, agg);
			memset(agg, 0, sizeof(agg));
			aggregating = false;
		}

		if (g->style == ST_ASSIST && w >= 4)
			render_markdown(u, g->text, w, A_NORMAL, false);
		else if (g->style == ST_REASON && w >= 4)
			render_markdown(u, g->text, w,
			    u->color ? COLOR_PAIR(6) : A_DIM, true);
		else if (g->style == ST_TOOL)
			emit_tool_summary(u, g->text, u->expand_output);
		else if (g->style == ST_TOOL_OUT)
			push_tool_output(u, g->text, i == last_tool_out);
		else if (g->style == ST_EVENT)
			push_event(u, g->text, i == last_event);
		else
			rseg_push_ref(u, seg_attr(u, g->style), g->text);
	}
	if (aggregating)
		push_collapsed_summary(u, agg);

	u->built_gen = u->gen;
	u->built_width = w;
	u->built_expand = u->expand_output;
	u->built_reasoning = u->show_reasoning;
}

static bool
wrap_is_space(wchar_t wc)
{
	return wc == L' ' || wc == L'\t' || wc == L'\n';
}

/* True if the text before rsegs[i] ends in whitespace, or nothing precedes
 * it. Lets a resumed walk know whether rsegs[i] starts a new word. */
static bool
wrap_space_before(const struct ui *u, size_t i)
{
	while (i > 0) {
		const char *s = u->rsegs[--i].text;
		size_t len = strlen(s);

		if (len > 0)
			return s[len - 1] == ' ' || s[len - 1] == '\t' ||
			    s[len - 1] == '\n';
	}
	return true;
}

/* Display width of the word that starts at byte j of rsegs[i]. The word
 * can span later rsegs. The scan stops at whitespace or once the width
 * exceeds cap. */
static int
wrap_word_width(const struct ui *u, size_t i, size_t j, int cap)
{
	int width = 0;

	for (; i < u->nrsegs; i++, j = 0) {
		const char *s = u->rsegs[i].text;
		size_t len = strlen(s);
		mbstate_t ps;

		memset(&ps, 0, sizeof(ps));
		while (j < len) {
			wchar_t wc;
			size_t k = mbrtowc(&wc, s + j, len - j, &ps);
			int cw;

			if (k == (size_t)-1 || k == (size_t)-2 || k == 0) {
				k = 1;
				wc = 0;
			}
			if (wrap_is_space(wc))
				return width;
			cw = wcwidth(wc);
			width += cw < 0 ? 1 : cw;
			if (width > cap)
				return width;
			j += k;
		}
	}
	return width;
}

/*
 * Soft-wrap rsegs[i0, iN) to width w, starting at cursor (*row, *col), and
 * advance *row and *col past the last character walked. If ckpt_row and
 * ckpt_col are non-NULL, record the (row, col) at the start of each rseg
 * into them -- this is how wrap_walk below builds its resume cache. If draw
 * is true,
 * cells whose wrapped-row index falls in [start, start+h) are painted into
 * u->txt at row (index - start). Both the checkpoint-building pass and the
 * paint pass call this same core loop, so they can never disagree about
 * where lines wrap.
 *
 * A word that does not fit on the current row moves to the next row whole.
 * A word wider than w breaks at the right edge. A space that overflows the
 * row is dropped, so a wrapped row does not start with it.
 */
static void
wrap_span(struct ui *u, size_t i0, size_t iN, int *row, int *col, int w, int h,
    int start, bool draw, int *ckpt_row, int *ckpt_col)
{
	bool space_before = wrap_space_before(u, i0);

	for (size_t i = i0; i < iN; i++) {
		const char *s = u->rsegs[i].text;
		size_t len = strlen(s);
		size_t j = 0;
		mbstate_t ps;

		if (ckpt_row != NULL) {
			ckpt_row[i] = *row;
			ckpt_col[i] = *col;
		}
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
				(*row)++;
				*col = 0;
				space_before = true;
				j += k;
				continue;
			}
			cw = wcwidth(wc);
			if (cw < 0)
				cw = 1;
			if (wrap_is_space(wc) && *col + cw > w) {
				(*row)++;
				*col = 0;
				space_before = true;
				j += k;
				continue;
			}
			if (space_before && *col > 0) {
				int ww = wrap_word_width(u, i, j, w);

				if (*col + ww > w && ww <= w) {
					(*row)++;
					*col = 0;
				}
			}
			space_before = wrap_is_space(wc);
			if (cw > 0 && *col + cw > w) {
				(*row)++;
				*col = 0;
			}
			if (draw && *row >= start && *row < start + h) {
				wmove(u->txt, *row - start, *col);
				waddnstr(u->txt, s + j, (int)k);
			}
			*col += cw;
			j += k;
		}
	}
	if (draw)
		wattrset(u->txt, A_NORMAL);
}

/* Binary-search wrap_row[0..n] (sorted ascending) for the rightmost index
 * whose checkpoint row is <= start, so a paint pass can resume closest to
 * its visible window instead of walking from rseg 0. */
static size_t
wrap_checkpoint_before(const int *rows, size_t n, int start)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (rows[mid] <= start)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo > 0 ? lo - 1 : 0;
}

/* Ensure wrap_row/wrap_col hold valid checkpoints for rsegs[0..base] at
 * width w (index `base` = the position just after the last real rseg,
 * where the transient steering tail begins -- see draw_transcript). If the
 * cache is already valid (rebuild_render hasn't run and w hasn't changed
 * since), this is a no-op: no mbrtowc/wcwidth work at all. Otherwise it
 * does one full wrap_span pass over the real rsegs to rebuild it.
 */
static void
wrap_ensure_checkpoints(struct ui *u, int w, size_t base)
{
	int row = 0, col = 0;
	int *r, *c;

	if (u->wrap_valid && u->wrap_width == w)
		return;

	if (u->cap_wrap < base + 1) {
		size_t ncap = u->cap_wrap ? u->cap_wrap * 2 : 64;

		while (ncap < base + 1)
			ncap *= 2;
		r = malloc(ncap * sizeof(*r));
		c = malloc(ncap * sizeof(*c));
		if (r == NULL || c == NULL) {
			free(r);
			free(c);
			return; /* leave wrap_valid false; retry next tick */
		}
		if (u->cap_wrap > 0) {
			memcpy(r, u->wrap_row, u->cap_wrap * sizeof(*r));
			memcpy(c, u->wrap_col, u->cap_wrap * sizeof(*c));
		}
		free(u->wrap_row);
		free(u->wrap_col);
		u->wrap_row = r;
		u->wrap_col = c;
		u->cap_wrap = ncap;
	}

	wrap_span(
	    u, 0, base, &row, &col, w, 0, 0, false, u->wrap_row, u->wrap_col);
	u->wrap_row[base] = row;
	u->wrap_col[base] = col;
	u->wrap_width = w;
	u->wrap_valid = true;
}

/*
 * Walk the rendered runs (real rsegs plus the transient tail appended past
 * `base` by draw_transcript), soft-wrapped to width w. Returns the total
 * number of wrapped rows. When draw is true, cells whose wrapped-row index
 * falls in [start, start+h) are painted into u->txt at row (index - start).
 *
 * The real-rseg portion is resumed from a cached checkpoint rather than
 * walked from scratch whenever possible: wrap_ensure_checkpoints only
 * redoes that work when rebuild_render has changed rsegs or w has changed;
 * a paint pass additionally jumps to the closest checkpoint at or before
 * `start` instead of resuming at row 0. Only the checkpoint-building pass
 * and the tail (0-3 short lines) ever get walked character-by-character on
 * a steady-state repaint tick.
 */
static int
wrap_walk(struct ui *u, int w, int h, int start, bool draw, size_t base)
{
	int row, col;
	size_t i0;

	if (w < 1)
		w = 1;

	wrap_ensure_checkpoints(u, w, base);

	if (draw) {
		i0 = wrap_checkpoint_before(u->wrap_row, base + 1, start);
		row = u->wrap_row[i0];
		col = u->wrap_col[i0];
	} else {
		i0 = base;
		row = u->wrap_row[base];
		col = u->wrap_col[base];
	}

	wrap_span(u, i0, u->nrsegs, &row, &col, w, h, start, draw, NULL, NULL);
	return row + 1;
}

static void
draw_transcript(struct ui *u)
{
	int w = getmaxx(u->txt);
	int h = getmaxy(u->txt);
	int total, maxscroll, start;
	size_t base;

	if (u->gen != u->built_gen || w != u->built_width ||
	    u->expand_output != u->built_expand ||
	    u->show_reasoning != u->built_reasoning)
		rebuild_render(u, w);

	/* Pending steering entries aren't part of the transcript source (see
	 * enqueue_steering): they haven't happened yet, so rather than mixing
	 * them into segs/rsegs (and the gen-keyed rebuild cache) they're
	 * appended here as a transient tail, pinned below whatever the real
	 * cache holds, then trimmed back off below so next frame starts clean.
	 */
	base = u->nrsegs;
	for (size_t i = 0; i < u->steering_nqueue; i++) {
		char line[600];

		rseg_push(u, seg_attr(u, ST_USER), "\nyou> ", 6);
		rseg_push(u, seg_attr(u, ST_USER), u->steering_queue[i],
		    strlen(u->steering_queue[i]));
		(void)snprintf(line, sizeof(line), "  (queued)\n");
		rseg_push(u, seg_attr(u, ST_META), line, strlen(line));
	}

	total = wrap_walk(u, w, h, 0, false, base);

	/* scroll is a distance from the bottom, so on its own it re-anchors
	 * to the new bottom whenever total grows -- which drags the viewport
	 * forward through content that just streamed in, exactly while the
	 * user is trying to read something further up. While actually
	 * scrolled up (not following), grow scroll by however much total
	 * grew since the last paint, holding the viewport's absolute
	 * position (start, below) steady instead. Following (scroll == 0)
	 * is deliberately left alone -- that case is supposed to track the
	 * bottom. */
	if (u->scroll > 0 && u->last_total >= 0 && total > u->last_total)
		u->scroll += (size_t)(total - u->last_total);
	u->last_total = total;

	maxscroll = total - h;
	if (maxscroll < 0)
		maxscroll = 0;
	if (u->scroll > (size_t)maxscroll)
		u->scroll = (size_t)maxscroll;

	start = total - h - (int)u->scroll;
	if (start < 0)
		start = 0;

	werase(u->txt);
	wrap_walk(u, w, h, start, true, base);
	wnoutrefresh(u->txt);

	/* Drop the transient queue tail appended above -- it must not survive
	 * into the next frame's rsegs count (rebuild_render's free loop, or
	 * this same overlay run again, would otherwise double up on it). */
	for (size_t i = base; i < u->nrsegs; i++) {
		if (u->rsegs[i].owned)
			free(u->rsegs[i].text);
	}
	u->nrsegs = base;
}

static void handle_keys(struct ui *u); /* defined below; shared with on_stdin */

static void
on_repaint(uv_timer_t *t)
{
	struct ui *u = t->data;

	/* Also drain input here so a lone Escape (held by ncurses waiting for a
	 * possible escape sequence) is flushed within a tick, even if no
	 * further keystroke arrives to trigger the stdin poll. */
	handle_keys(u);

	if (u->busy || u->cmd_live > 0)
		u->spinner++;

	if (!u->dirty && !u->busy && u->cmd_live == 0)
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

	/* The transcript window is inset, so the gutter columns belong to no
	 * window and no erase covers them. Blank the screen underneath first;
	 * the three windows paint over it before the single doupdate below. */
	werase(stdscr);
	wnoutrefresh(stdscr);

	draw_transcript(u);
	draw_status(u);
	draw_input(u);
	draw_help(u);
	doupdate();
	u->dirty = false;
}

/* ---- input ---- */

/* Echo a prompt and hand it to the agent. Returns true if the turn started. */
static bool
do_submit(struct ui *u, const char *prompt, bool echo)
{
	int r;

	/* No forced "scroll = 0, snap to the bottom" here on purpose.
	 * Someone already at the bottom (scroll == 0, the common case: typed
	 * a prompt, hit enter) is unaffected either way. But submitting a
	 * prompt while scrolled up reading history used to yank the
	 * viewport back to the bottom regardless -- exactly the complaint
	 * this fixes, same root issue as draw_transcript's own scroll-vs-
	 * growing-transcript fix, just triggered by a new turn starting
	 * instead of an existing one streaming. */
	if (echo) {
		ui_push(u, ST_USER, "\nyou> ");
		ui_push(u, ST_USER, prompt);
		ui_push(u, ST_USER, "\n");
		echo_note(u, prompt);
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

static void
enqueue_steering(struct ui *u, const char *prompt)
{
	char *dup = strdup(prompt);
	if (dup == NULL)
		return;
	if (u->steering_nqueue == u->steering_cap) {
		size_t ncap = u->steering_cap ? u->steering_cap * 2 : 8;
		char **p = (char **)realloc(
		    (void *)u->steering_queue, ncap * sizeof(*p));
		if (p == NULL) {
			free(dup);
			return;
		}
		u->steering_queue = p;
		u->steering_cap = ncap;
	}
	u->steering_queue[u->steering_nqueue++] = dup;
	/* Don't push into the transcript source here: a turn is in flight and
	 * streaming into the same segs array, so anything appended now would
	 * land wherever the stream happens to be, not at the end of the
	 * conversation -- draw_transcript renders pending queue entries as a
	 * transient tail instead, pinned below the real content until each one
	 * drains. */
	u->dirty = true;
}

static void drain_steering_one(struct ui *u);

/* Hand every queued message to the agent at once: notify() coalesces what
 * it cannot start right away, so a burst lands as one injection instead of
 * one per decision point. */
static void
drain_steering_queue(struct ui *u)
{
	while (u->steering_nqueue > 0)
		drain_steering_one(u);
}

static void
drain_steering_one(struct ui *u)
{
	char *prompt;
	enum clm_agent_state prev_state;
	int r;

	if (u->steering_nqueue == 0)
		return;
	/* Idle or mid-turn: clm_agent_notify below handles both. */
	prompt = u->steering_queue[0];
	memmove((void *)u->steering_queue, (void *)(u->steering_queue + 1),
	    (u->steering_nqueue - 1) * sizeof(*u->steering_queue));
	u->steering_nqueue--;
	/* Render exactly like any other submitted prompt: no separate
	 * "[steering: ...]" treatment, so the transcript doesn't distinguish
	 * a mid-turn nudge from a normal turn-start prompt once it lands. */
	ui_push(u, ST_USER, "\nyou> ");
	ui_push(u, ST_USER, prompt);
	ui_push(u, ST_USER, "\n");
	echo_note(u, prompt);

	/*
	 * clm_agent_notify(), not clm_agent_submit(): two of the three
	 * decision points this fires at (the CALLING_TOOL<->THINKING
	 * transitions) still have the agent's state set to THINKING or
	 * CALLING_TOOL at this exact instant, and clm_agent_submit()
	 * unconditionally rejects those with "turn already in progress".
	 * notify() queues the text in that case and puts it in history at
	 * the turn's next LLM call, so the model reads it on the next round
	 * trip of the chain already running; it only submits right away --
	 * starting a genuinely new turn -- at the third decision point
	 * (THINKING->COMPLETE), where the agent is actually free.
	 */
	prev_state = clm_agent_get_state(u->agent);
	r = clm_agent_notify(u->agent, prompt);
	if (r < 0) {
		ui_push(u, ST_ERROR, "\nerror: ");
		ui_push(u, ST_ERROR, clm_agent_get_last_error(u->agent));
	} else if (prev_state != CLM_STATE_THINKING &&
	    prev_state != CLM_STATE_CALLING_TOOL) {
		/* Actually started a fresh turn, same bookkeeping do_submit()
		 * does on a successful submit. */
		u->started_assist = false;
		u->usage[0] = '\0';
		u->busy = true;
	}
	free(prompt);
	u->dirty = true;
}

/*
 * Set u->model (status bar display + /provider's "keep the current model"
 * lookup), replacing any prior value. Always makes an owned copy: unlike
 * most of this file's config-derived strings, u->model can now also come
 * from live/ad hoc input (a model id typed straight into /model that
 * isn't in config.lua's models{} table, see CMD("model")'s literal
 * fallback) backed by a transient command-line buffer, not the
 * long-lived Lua config table -- so it can never safely be borrowed.
 * NULL is accepted (clears the display) and safe to pass.
 */
static void
set_current_model(struct ui *u, const char *model)
{
	char *dup = model != NULL ? strdup(model) : NULL;
	free(u->model);
	u->model = dup;
	clm_peer_set_model(u->peer, u->model);
}

/*
 * Set u->provider_name (status bar display), replacing any prior value.
 * Same owned-copy contract as set_current_model. NULL clears the display --
 * used whenever a switch lands on a connection with no config.lua providers[]
 * entry behind it (a literal model id on the currently active connection, see
 * cmd_model's fallback path), since that connection's provider is unnamed.
 */
static void
set_current_provider(struct ui *u, const char *provider)
{
	char *dup = provider != NULL ? strdup(provider) : NULL;
	free(u->provider_name);
	u->provider_name = dup;
}

static int
strp_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Push a sorted, comma-joined listing of config.<table>'s entry names
 * (e.g. "providers" or "models") for /model and /provider run with no
 * argument -- discoverability, since otherwise the only way to see what's
 * configured is to go read config.lua. label is the human word used in
 * the message ("model"/"provider"); cmd is what to type ("/model
 * <name>"/"/provider <name>").
 */
static void
list_config_names(
    struct ui *u, const char *table, const char *label, const char *cmd)
{
	char **names;
	char msg[1024];
	size_t off = 0;

	if (u->lcfg == NULL) {
		ui_push(u, ST_ERROR, "\nno config loaded\n");
		return;
	}

	names = clm_lua_cfg_list_names(u->lcfg, table);
	if (names == NULL) {
		char nomsg[128];
		(void)snprintf(
		    nomsg, sizeof(nomsg), "\nno %ss configured\n", label);
		ui_push(u, ST_META, nomsg);
		return;
	}

	size_t n = 0;
	while (names[n] != NULL)
		n++;
	qsort((void *)names, n, sizeof(*names), strp_cmp);

	/* snprintf's return value is how much it WOULD have written, which
	 * can exceed the remaining space on truncation -- clamp off after
	 * every call or "sizeof(msg) - off" underflows (size_t) on the next
	 * one and hands snprintf a huge bogus size. */
	int wrote =
	    snprintf(msg + off, sizeof(msg) - off, "\navailable %ss:\n", label);
	off += (wrote > 0) ? (size_t)wrote : 0;
	if (off > sizeof(msg))
		off = sizeof(msg);
	for (size_t i = 0; i < n && off < sizeof(msg); i++) {
		wrote =
		    snprintf(msg + off, sizeof(msg) - off, "  %s\n", names[i]);
		off += (wrote > 0) ? (size_t)wrote : 0;
		if (off > sizeof(msg))
			off = sizeof(msg);
	}
	if (off < sizeof(msg)) {
		(void)snprintf(
		    msg + off, sizeof(msg) - off, "usage: %s <name>\n", cmd);
	}
	ui_push(u, ST_META, msg);
	clm_lua_cfg_free_str_list(names);
}

/*
 * /model's live half: async GET {provider}/models against whatever
 * connection is currently active, appended below list_config_names'
 * "from config:" section once it resolves (config.lua's models{} table
 * only ever tells you what YOU declared; this is what the server actually
 * has right now -- the two can and do diverge, e.g. a multi-model router
 * like freellmapi vs. a single-model local llama.cpp instance). ids is
 * only valid for the duration of this call (see clm_agent_list_models);
 * copy what's needed into msg before returning.
 */
static void
on_live_models_result(char **ids, void *user)
{
	struct ui *u = user;
	size_t cap = sizeof("\nfrom server:\n");
	size_t off;
	int wrote;
	autofree char *msg = NULL;

	/* Heap, not a stack buffer: a router like freellmapi can report 60+
	 * ids, and sizing up front (rather than a fixed-size stack array)
	 * also sidesteps -Wframe-larger-than on a plain char msg[N]. */
	for (size_t i = 0; ids[i] != NULL; i++)
		cap += strlen(ids[i]) + sizeof("  \n") - 1;

	msg = malloc(cap);
	if (msg == NULL)
		return;

	wrote = snprintf(msg, cap, "\nfrom server:\n");
	off = (wrote > 0) ? (size_t)wrote : 0;
	for (size_t i = 0; ids[i] != NULL && off < cap; i++) {
		wrote = snprintf(msg + off, cap - off, "  %s\n", ids[i]);
		off += (wrote > 0) ? (size_t)wrote : 0;
	}
	ui_push(u, ST_META, msg);
}

static void
on_live_models_error(const char *emsg, void *user)
{
	struct ui *u = user;
	char msg[256];

	(void)snprintf(msg, sizeof(msg),
	    "\n(couldn't query live models from the current provider: %s)\n",
	    emsg);
	ui_push(u, ST_META, msg);
}

/* A config key of the model entry, else of the provider entry, or NULL. */
static const char *
model_cfg_str(
    struct ui *u, const char *prov, const char *model, const char *key)
{
	const char *e = NULL;

	if (u->lcfg == NULL || prov == NULL)
		return NULL;
	e = clm_lua_cfg_provider_str(u->lcfg, prov, key);
	if (model != NULL) {
		const char *me =
		    clm_lua_cfg_provider_model_str(u->lcfg, prov, model, key);
		if (me != NULL)
			e = me;
	}
	return e;
}

/*
 * Effort for this connection: a /effort override if one is in force, else
 * the model entry, else the provider entry. NULL means "send nothing and
 * take the backend's default".
 */
static const char *
resolve_effort(struct ui *u, const char *prov, const char *model)
{
	if (u->effort_override[0] != '\0')
		return u->effort_override;
	return model_cfg_str(u, prov, model, "effort");
}

/* Hand the freshly built agent whatever effort applies to it. */
static void
apply_effort(struct ui *u, const char *prov, const char *model)
{
	const char *e = resolve_effort(u, prov, model);

	if (e != NULL)
		(void)clm_agent_set_effort(u->agent, e);
	(void)clm_agent_set_compact_effort(
	    u->agent, model_cfg_str(u, prov, model, "compact_effort"));
}

/* Where this session lives: id for addressing it, path for reading it. */
static void
cmd_session(struct ui *u)
{
	/* PATH_MAX twice over would blow the frame budget; the session dir
	 * and id are both short in practice. */
	char dir[256];
	char line[512];
	const char *id;

	if (u->session == NULL) {
		ui_push(u, ST_META, "\nsession: not being logged\n");
		return;
	}
	id = clm_session_id(u->session);
	(void)snprintf(line, sizeof(line), "\nsession: %s\n", id);
	ui_push(u, ST_META, line);
	(void)snprintf(line, sizeof(line), "  short:  %s\n", u->session_short);
	ui_push(u, ST_META, line);
	if (clm_session_state_dir(dir, sizeof(dir)) == 0) {
		(void)snprintf(
		    line, sizeof(line), "  log:    %s/%s.jsonl\n", dir, id);
		ui_push(u, ST_META, line);
	}
	(void)snprintf(line, sizeof(line), "  resume: clm --resume %s\n",
	    u->session_short);
	ui_push(u, ST_META, line);
}

/* Auto mode without a pre_tool hook would be plain allow-all. */
static void
auto_check_hooks(struct ui *u)
{
	if (!u->auto_mode || clm_agent_pre_tool_hook_count(u->agent) > 0)
		return;
	u->auto_mode = false;
	ui_push(u, ST_ERROR,
	    "\n[auto mode off: no plugin with a pre_tool hook, such as "
	    "jev_guard]\n");
}

static void
cmd_auto(struct ui *u, const char *arg)
{
	if (arg == NULL)
		arg = "";
	if (strcmp(arg, "off") == 0 || (arg[0] == '\0' && u->auto_mode)) {
		u->auto_mode = false;
		ui_push(u, ST_META, "\n[auto mode off: tool calls ask]\n");
		return;
	}
	u->auto_mode = true;
	auto_check_hooks(u);
	if (u->auto_mode)
		ui_push(u, ST_META,
		    "\n[auto mode on: tool calls run unless a hook blocks "
		    "or asks]\n");
}

static void
cmd_effort(struct ui *u, const char *arg)
{
	char msg[160];

	if (arg == NULL || arg[0] == '\0') {
		const char *cur = clm_agent_get_effort(u->agent);

		(void)snprintf(msg, sizeof(msg), "\neffort: %s\n",
		    cur != NULL ? cur : "(backend default)");
		ui_push(u, ST_META, msg);
		return;
	}
	if (strcmp(arg, "default") == 0) {
		u->effort_override[0] = '\0';
		(void)clm_agent_set_effort(u->agent, NULL);
		ui_push(u, ST_META, "\neffort: backend default\n");
		return;
	}
	if (clm_agent_set_effort(u->agent, arg) < 0) {
		ui_push(u, ST_ERROR, "\ncould not set effort\n");
		return;
	}
	(void)snprintf(
	    u->effort_override, sizeof(u->effort_override), "%s", arg);
	(void)snprintf(msg, sizeof(msg),
	    "\neffort: %s (server rejects a level its model does not take)\n",
	    arg);
	ui_push(u, ST_META, msg);
}

static void
cmd_agent(struct ui *u, const char *arg)
{
	/* /agent <name>
	 * Switch agent: reload profile, swap provider+prompt, clear history.
	 * Safe even mid-turn: clm_agent_free() (via clm_tools_detach(),
	 * lib/tools.c) severs any in-flight tool batch from the agent
	 * before freeing it, so a shell/bg child process that is still
	 * running gets to finish asynchronously without touching freed
	 * memory. */
	if (arg[0] == '\0') {
		ui_push(u, ST_META, "\nusage: /agent <name>\n");
	} else if (u->lcfg == NULL) {
		ui_push(u, ST_ERROR, "\nno config loaded\n");
	} else {
		char *adir = xdg_config_path("clm/agents");
		if (clm_lua_cfg_load_agent(u->lcfg, adir, arg) < 0) {
			char msg[128];
			(void)snprintf(
			    msg, sizeof(msg), "\nagent '%s' not found\n", arg);
			ui_push(u, ST_ERROR, msg);
			free(adir);
		} else {
			free(adir);
			/* Resolve the agent's model (or the top-level
			 * default, if the profile doesn't set its own) --
			 * a "provider/model-id" spec, see src/model_spec.h --
			 * down to a provider connection, same indirection
			 * as the startup path in main.c. */
			const char *model_name =
			    clm_lua_cfg_get_str(u->lcfg, "model");
			autofree char *spec_provider = NULL;
			const char *spec_model = NULL;
			split_provider_model(
			    model_name, &spec_provider, &spec_model);
			const char *prov = spec_provider;
			const char *purl = prov
			    ? clm_lua_cfg_provider_str(u->lcfg, prov, "url")
			    : NULL;
			const char *pkind = prov
			    ? clm_lua_cfg_provider_str(u->lcfg, prov, "kind")
			    : NULL;
			const char *pkey = prov
			    ? clm_lua_cfg_provider_str(u->lcfg, prov, "api_key")
			    : NULL;
			const char *pmodel = spec_model;
			const char *sprompt =
			    clm_lua_cfg_get_str(u->lcfg, "system_prompt");

			/* Tear down plugins + MCP clients (must happen while
			 * the old agent is still alive: unregistering their
			 * tools touches it) + agent. */
			clm_lua_env_free(u->lua_env);
			u->lua_env = NULL;
			clm_cli_free_mcp_servers(
			    u->mcp_clients, u->mcp_client_count);
			u->mcp_clients = NULL;
			u->mcp_client_count = 0;
			clm_agent_free(u->agent);

			/* Rebuild cfg. */
			int r;
			struct clm_cfg newcfg = {
			    .disable_parallel_tool_calls = true,
			};
			char url_buf[512];
			if (purl != NULL) {
				size_t ulen = strlen(purl);
				while (ulen > 0 && purl[ulen - 1] == '/')
					ulen--;
				(void)snprintf(url_buf, sizeof(url_buf),
				    "%.*s/chat/completions", (int)ulen, purl);
				newcfg.base_url = url_buf;
			} else {
				newcfg.base_url =
				    "http://127.0.0.1:8081/v1/chat/completions";
			}
			newcfg.model = pmodel ? pmodel : "local-model";
			/* Empty string is a valid, explicit "no key needed"
			 * (see http_async.c: Content-Type no longer depends
			 * on a non-empty key, so there's no need for a
			 * non-empty placeholder here either). */
			newcfg.api_key = (pkey != NULL && pkey[0] != '\0')
			    ? pkey
			    : (getenv("CLM_API_KEY") ? getenv("CLM_API_KEY")
			                             : "");
			newcfg.system_prompt = sprompt;
			/* A profile switch builds a whole new agent, so the
			 * host block has to be handed over again. The agent
			 * copies it, so this frees at the end of the switch. */
			autofree char *sysinfo = clm_cli_sysinfo();

			newcfg.system_prompt_suffix = sysinfo;
			newcfg.provider = clm_provider_from_str(pkind);
			newcfg.stream = 1;
			clm_cfg_apply_tuning(
			    u->lcfg, prov, spec_model, &newcfg);
			if (prov) {
				newcfg.rate_tokens_per_sec =
				    clm_lua_cfg_provider_int(u->lcfg, prov,
				        "rate_tokens_per_sec", 0);
				newcfg.rate_burst = clm_lua_cfg_provider_int(
				    u->lcfg, prov, "rate_burst", 0);
				newcfg.disable_parallel_tool_calls =
				    clm_lua_cfg_provider_int(u->lcfg, prov,
				        "disable_parallel_tool_calls", 1) != 0;
			}
			/* Volatile-tool policy from the new agent profile.
			 * Not freed on the next switch (borrowed by the
			 * agent for its lifetime) -- same intentional
			 * process-lifetime leak as lcfg itself. */
			newcfg.volatile_tools =
			    (const char *const *)clm_lua_cfg_get_str_list(
			        u->lcfg, "volatile_tools");

			r = clm_agent_new(
			    &newcfg, u->host, &tui_callbacks, u, &u->agent);
			if (r < 0) {
				ui_push(
				    u, ST_ERROR, "\nfailed to create agent\n");
			} else {
				apply_effort(u, prov, spec_model);
				clm_agent_set_spool(u->agent, cb_spool, u);
				clm_tools_register_shell(u->agent);
				clm_tools_register_bg(u->agent);
				clm_tools_register_monitor(u->agent);
				/* Reload plugins. */
				if (clm_lua_env_new(u->agent, &u->lua_env) ==
				    0) {
					clm_lua_env_set_config_from(
					    u->lua_env, u->lcfg);
					char *pp = u->plugin_dir == NULL
					    ? xdg_config_path("clm/plugins")
					    : NULL;

					clm_cli_load_plugins(u->lua_env,
					    u->plugin_dir != NULL
					        ? u->plugin_dir
					        : pp,
					    u->lcfg, u->opt_plugins,
					    cb_mcp_status, u);
					free(pp);
					/* Load agent-specific plugins. */
					char *apdir =
					    xdg_config_path("clm/agents");
					if (apdir != NULL) {
						char apbuf[512];
						(void)snprintf(apbuf,
						    sizeof(apbuf), "%s/%s",
						    apdir, arg);
						clm_lua_load_plugins(
						    u->lua_env, apbuf);
						free(apdir);
					}
				}
				auto_check_hooks(u);
				u->mcp_clients = clm_cli_connect_mcp_servers(
				    u->agent, u->loop, u->lcfg, cb_mcp_status,
				    u, &u->mcp_client_count);
				free(u->agent_name);
				u->agent_name = strdup(arg);
				set_current_model(u, newcfg.model);
				set_current_provider(u, prov);
				/* New agent, empty history: the old
				 * reading describes a conversation this
				 * agent never saw. */
				u->ctx_used = 0;
				u->usage[0] = '\0';
				char msg[128];
				(void)snprintf(msg, sizeof(msg),
				    "\n[switched to agent: %s]\n", arg);
				ui_push(u, ST_META, msg);
				clm_agent_check_connection(u->agent);
			}
		}
	}
}

static void
cmd_model(struct ui *u, const char *arg)
{
	/* /model <provider/model-id>: resolve
	 * config.providers[provider].models[model-id] to a connection +
	 * wire model id and reconfigure the live agent in place via
	 * clm_agent_set_provider() -- history, tools, and MCP clients are
	 * untouched. Contrast /agent, which also changes the system
	 * prompt/tool set and needs a full teardown + rebuild.
	 *
	 * A bare id with no "/" is a literal wire model id requested on
	 * whatever connection is currently active (no context_size/
	 * autocompact_pct override applies) -- this is what makes the live
	 * listing below actually useful: an id the server reports but that
	 * was never added to any provider's models{} can still be switched
	 * to, same "config spec, or literal fallback" contract as
	 * -m/--model on the CLI in main.c. */
	if (arg[0] == '\0') {
		list_config_names(u, "models", "model", "/model");
		(void)clm_agent_list_models(
		    u->agent, on_live_models_result, on_live_models_error, u);
	} else if (u->lcfg == NULL) {
		ui_push(u, ST_ERROR, "\nno config loaded\n");
	} else {
		autofree char *spec_provider = NULL;
		const char *spec_model = NULL;
		split_provider_model(arg, &spec_provider, &spec_model);

		const char *purl = spec_provider != NULL
		    ? clm_lua_cfg_provider_str(u->lcfg, spec_provider, "url")
		    : NULL;

		if (spec_provider != NULL && purl == NULL) {
			/* A "provider/..." was typed but that provider name
			 * doesn't exist -- an error, not a literal fallback:
			 * silently sending "typo/model-id" itself as a wire
			 * id would be actively misleading. */
			char msg[128];
			(void)snprintf(msg, sizeof(msg),
			    "\nprovider '%s' not found\n", spec_provider);
			ui_push(u, ST_ERROR, msg);
		} else if (spec_provider == NULL) {
			/* No "/" at all -- literal wire id on the current
			 * connection. */
			struct clm_cfg litcfg = {0};
			const char *cur_url = clm_agent_get_base_url(u->agent);

			if (cur_url == NULL) {
				ui_push(u, ST_ERROR,
				    "\nno active connection to switch on\n");
			} else {
				char msg[160];

				litcfg.base_url = cur_url;
				litcfg.api_key =
				    clm_agent_get_api_key(u->agent);
				litcfg.provider =
				    clm_agent_get_provider(u->agent);
				litcfg.model = arg;

				int rc =
				    clm_agent_set_provider(u->agent, &litcfg);
				if (rc == 0) {
					set_current_model(u, litcfg.model);
					(void)snprintf(msg, sizeof(msg),
					    "\n[switched to model: %s "
					    "(literal, not in config.lua)]\n",
					    arg);
					ui_push(u, ST_META, msg);
					clm_agent_check_connection(u->agent);
				} else if (rc == -EBUSY) {
					ui_push(u, ST_ERROR,
					    "\nbusy; try again when idle\n");
				} else {
					ui_push(u, ST_ERROR,
					    "\nfailed to switch model\n");
				}
			}
		} else {
			struct clm_cfg newcfg = {
			    .disable_parallel_tool_calls = true,
			};
			char url_buf[512];
			enum clm_provider provider =
			    clm_provider_from_str(clm_lua_cfg_provider_str(
			        u->lcfg, spec_provider, "kind"));

			/* clm_provider_build_url() is the one place the
			 * base-url + endpoint-path logic lives -- see
			 * clm/provider.h's endpoint_path for why that matters
			 * (this call site used to hardcode "/chat/completions"
			 * independently of main.c's CLI-startup path, and
			 * drifted out of sync with it). */
			clm_provider_build_url(
			    url_buf, sizeof(url_buf), purl, provider);
			newcfg.base_url = url_buf;
			newcfg.api_key = clm_lua_cfg_provider_str(
			    u->lcfg, spec_provider, "api_key");
			newcfg.provider = provider;
			newcfg.model = spec_model;
			clm_cfg_apply_tuning(
			    u->lcfg, spec_provider, spec_model, &newcfg);
			newcfg.rate_tokens_per_sec = clm_lua_cfg_provider_int(
			    u->lcfg, spec_provider, "rate_tokens_per_sec", 0);
			newcfg.rate_burst = clm_lua_cfg_provider_int(
			    u->lcfg, spec_provider, "rate_burst", 0);
			newcfg.disable_parallel_tool_calls =
			    clm_lua_cfg_provider_int(u->lcfg, spec_provider,
			        "disable_parallel_tool_calls", 1) != 0;

			int rc = clm_agent_set_provider(u->agent, &newcfg);
			if (rc == 0) {
				set_current_model(u, newcfg.model);
				set_current_provider(u, spec_provider);
				char msg[128];
				(void)snprintf(msg, sizeof(msg),
				    "\n[switched to model: %s]\n", arg);
				ui_push(u, ST_META, msg);
				clm_agent_check_connection(u->agent);
			} else if (rc == -EBUSY) {
				ui_push(u, ST_ERROR,
				    "\nbusy; try again when idle\n");
			} else {
				ui_push(
				    u, ST_ERROR, "\nfailed to switch model\n");
			}
		}
	}
}

static void
cmd_provider(struct ui *u, const char *arg)
{
	/* /provider <name>: like /model, but overrides only the
	 * provider connection (endpoint, key, wire dialect, rate
	 * limits) backing whichever model is currently active, e.g.
	 * to fail over to a mirror endpoint without changing which
	 * model is requested. */
	if (arg[0] == '\0') {
		list_config_names(u, "providers", "provider", "/provider");
	} else if (u->lcfg == NULL) {
		ui_push(u, ST_ERROR, "\nno config loaded\n");
	} else {
		const char *purl =
		    clm_lua_cfg_provider_str(u->lcfg, arg, "url");
		if (purl == NULL) {
			char msg[128];
			(void)snprintf(msg, sizeof(msg),
			    "\nprovider '%s' not found\n", arg);
			ui_push(u, ST_ERROR, msg);
		} else {
			struct clm_cfg newcfg = {
			    .disable_parallel_tool_calls = true,
			};
			char url_buf[512];
			size_t ulen = strlen(purl);
			while (ulen > 0 && purl[ulen - 1] == '/')
				ulen--;
			(void)snprintf(url_buf, sizeof(url_buf),
			    "%.*s/chat/completions", (int)ulen, purl);
			newcfg.base_url = url_buf;
			newcfg.api_key =
			    clm_lua_cfg_provider_str(u->lcfg, arg, "api_key");
			newcfg.provider = clm_provider_from_str(
			    clm_lua_cfg_provider_str(u->lcfg, arg, "kind"));
			newcfg.model =
			    u->model; /* keep the currently active model */
			clm_cfg_apply_tuning(u->lcfg, arg, u->model, &newcfg);
			newcfg.rate_tokens_per_sec = clm_lua_cfg_provider_int(
			    u->lcfg, arg, "rate_tokens_per_sec", 0);
			newcfg.rate_burst = clm_lua_cfg_provider_int(
			    u->lcfg, arg, "rate_burst", 0);
			newcfg.disable_parallel_tool_calls =
			    clm_lua_cfg_provider_int(u->lcfg, arg,
			        "disable_parallel_tool_calls", 1) != 0;

			int rc = clm_agent_set_provider(u->agent, &newcfg);
			if (rc == 0) {
				set_current_provider(u, arg);
				char msg[128];
				(void)snprintf(msg, sizeof(msg),
				    "\n[switched to provider: %s]\n", arg);
				ui_push(u, ST_META, msg);
				clm_agent_check_connection(u->agent);
			} else if (rc == -EBUSY) {
				ui_push(u, ST_ERROR,
				    "\nbusy; try again when idle\n");
			} else {
				ui_push(u, ST_ERROR,
				    "\nfailed to switch provider\n");
			}
		}
	}
}

/*
 * /cd [path]: change the process's working directory. No argument goes to
 * $HOME (same default as a bare shell "cd"). A leading "~" or "~/..." is
 * expanded against $HOME by hand -- this doesn't go through a shell, so
 * there's no other tilde expansion happening for it.
 *
 * This exists for the human at the keyboard, not the model (it isn't a
 * tool call): shell_exec/bg_exec children don't inherit a chdir from here
 * or from each other (each is a fresh $SHELL -c with the process's cwd at
 * spawn time), but a human watching the transcript can still lose track of
 * where clm itself is running -- e.g. after a background job's spawned
 * child writes something odd to the tty and the terminal state looks
 * confusing -- and "just chdir the whole terminal" isn't a thing a model
 * mid-turn can safely reach for. This is the human's own manual override.
 */
static void
cmd_cd(struct ui *u, const char *arg)
{
	autofree char *expanded = NULL;
	autofree char *cwd = NULL;
	const char *target;

	if (arg[0] == '\0') {
		target = getenv("HOME");
		if (target == NULL) {
			ui_push(u, ST_ERROR, "\ncd: $HOME not set\n");
			return;
		}
	} else if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
		const char *home = getenv("HOME");
		if (home == NULL) {
			ui_push(u, ST_ERROR, "\ncd: $HOME not set\n");
			return;
		}
		size_t n = strlen(home) + strlen(arg + 1) + 1;
		expanded = malloc(n);
		if (expanded == NULL) {
			ui_push(u, ST_ERROR, "\nout of memory\n");
			return;
		}
		(void)snprintf(expanded, n, "%s%s", home, arg + 1);
		target = expanded;
	} else {
		target = arg;
	}

	if (chdir(target) != 0) {
		autofree char *msg = NULL;
		if (asprintf(&msg, "\ncd: %s: %s\n", target, strerror(errno)) <
		    0)
			msg = NULL;
		ui_push(u, ST_ERROR, msg != NULL ? msg : "\ncd: failed\n");
		return;
	}

	/* getcwd(NULL, 0) heap-allocates exactly as large a buffer as the
	 * path needs (a glibc/BSD extension, not POSIX, but both host
	 * platforms this runs on support it) -- avoids a fixed-size
	 * PATH_MAX stack buffer blowing submit_line's frame-size budget
	 * (-Wframe-larger-than=2048) merely by being reachable from its
	 * call chain. */
	cwd = getcwd(NULL, 0);
	if (cwd != NULL) {
		autofree char *msg = NULL;
		if (asprintf(&msg, "\n[cwd: %s]\n", cwd) < 0)
			msg = NULL;
		ui_push(u, ST_META, msg != NULL ? msg : "\n[cwd changed]\n");
	} else {
		ui_push(u, ST_META, "\n[cwd changed]\n");
	}
}

static void
cmd_help(struct ui *u, const char *arg)
{
	(void)arg;
	help_open(u);
}

static void
cmd_clear(struct ui *u, const char *arg)
{
	/* Reset the actual conversation history first -- an agent busy
	 * mid-turn refuses (-EBUSY) and the on-screen transcript is
	 * left alone in that case too, rather than looking cleared
	 * while the backend history it was displaying still isn't. */
	int rc = clm_agent_clear_history(u->agent);

	(void)arg;
	if (rc == -EBUSY) {
		ui_push(u, ST_ERROR, "\nbusy; try again when idle\n");
		return;
	}
	for (size_t i = 0; i < u->nsegs; i++)
		free(u->segs[i].text);
	u->nsegs = 0;
	u->scroll = 0;
	u->usage[0] = '\0';
	u->ctx_used = 0;
	u->gen++;
	/* Rotate the session log too: the cleared transcript stays
	 * resumable under its old id, and what follows is a new
	 * conversation, so it gets a new file. An empty old session is
	 * just deleted. */
	if (u->session != NULL) {
		if (clm_session_is_empty(u->session))
			(void)clm_session_discard(u->session);
		else
			clm_session_free(u->session);
		u->session = NULL;
		if (clm_session_create(NULL, u->model, u->provider_name,
		        u->agent_name, &u->session) == 0) {
			set_session_short(u);
			peer_rebind(u);
			ui_push(u, ST_META, "[new session ");
			ui_push(u, ST_META, clm_session_id(u->session));
			ui_push(u, ST_META, "]\n");
		}
	}
}

static void
cmd_reasoning(struct ui *u, const char *arg)
{
	if (strcmp(arg, "on") == 0)
		u->show_reasoning = true;
	else if (strcmp(arg, "off") == 0)
		u->show_reasoning = false;
	else
		u->show_reasoning = !u->show_reasoning;
	ui_push(u, ST_META,
	    u->show_reasoning ? "\n[reasoning shown]\n"
	                      : "\n[reasoning hidden]\n");
}

static void
cmd_output(struct ui *u, const char *arg)
{
	if (strcmp(arg, "full") == 0)
		u->expand_output = true;
	else if (strcmp(arg, "short") == 0)
		u->expand_output = false;
	else
		u->expand_output = !u->expand_output;
	ui_push(u, ST_META,
	    u->expand_output ? "\n[output: full]\n" : "\n[output: short]\n");
}

static void
cmd_compact(struct ui *u, const char *arg)
{
	int rc = clm_agent_compact(u->agent);

	(void)arg;
	if (rc == 0) {
		u->busy = true;
		u->compacting = true;
		u->usage[0] = '\0';
		ui_push(u, ST_META, "\n[compacting the conversation...]\n");
	} else if (rc == -EBUSY) {
		ui_push(u, ST_ERROR, "\nbusy; try again when idle\n");
	} else {
		ui_push(u, ST_ERROR, "\ncompaction failed to start\n");
	}
}

static void
cmd_session_line(struct ui *u, const char *arg)
{
	(void)arg;
	cmd_session(u);
}

static void
cmd_quit(struct ui *u, const char *arg)
{
	(void)arg;
	u->quit = true;
}

/*
 * The slash commands. Dispatch, TAB completion and the help popup all
 * read this table, so a command added here shows up in all three.
 */
const struct ui_command ui_commands[] = {
    {"help", "h ?", "", "this help; ^G opens it too", cmd_help},
    {"clear", "cls", "", "clear the transcript; starts a new session log",
        cmd_clear},
    {"cd", NULL, "[path]", "change the working directory (no arg: $HOME)",
        cmd_cd},
    {"agent", "a", "<name>", "switch agent profile (system prompt + tools)",
        cmd_agent},
    {"model", NULL, "[name]",
        "switch model; no arg lists the models in config.lua", cmd_model},
    {"provider", NULL, "[name]",
        "switch provider connection; no arg lists them", cmd_provider},
    {"effort", NULL, "[level]",
        "reasoning effort: low, medium, high, xhigh, max, or default; "
        "no arg shows it",
        cmd_effort},
    {"auto", NULL, "[on|off]",
        "run tool calls without asking, unless a pre_tool hook blocks or "
        "asks",
        cmd_auto},
    {"session", NULL, "", "this session's id, log path and resume command",
        cmd_session_line},
    {"reasoning", "think", "[on|off]", "show or hide reasoning (^R)",
        cmd_reasoning},
    {"output", NULL, "[full|short]", "tool output/command detail (^O)",
        cmd_output},
    {"compact", NULL, "", "summarize old turns to free context", cmd_compact},
    {"quit", "exit q", "", "exit", cmd_quit},
};
const size_t ui_ncommands = sizeof(ui_commands) / sizeof(ui_commands[0]);

/* True when the word of length len names c or one of its aliases. */
static bool
command_matches(const struct ui_command *c, const char *word, size_t len)
{
	const char *a = c->aliases;

	if (strlen(c->name) == len && strncmp(c->name, word, len) == 0)
		return true;
	while (a != NULL && *a != '\0') {
		size_t n = strcspn(a, " ");

		if (n == len && strncmp(a, word, len) == 0)
			return true;
		a += n;
		a += strspn(a, " ");
	}
	return false;
}

const struct ui_command *
ui_command_find(const char *word, size_t len)
{
	for (size_t i = 0; i < ui_ncommands; i++)
		if (command_matches(&ui_commands[i], word, len))
			return &ui_commands[i];
	return NULL;
}

/* A plugin command in flight. A result from before an Escape is dropped. */
struct cmd_wait {
	struct ui *u;
	unsigned gen;
	char name[40];
};

static void
cmd_done(int status, const char *text, void *user)
{
	struct cmd_wait *w = user;
	struct ui *u = w->u;

	if (w->gen == u->cmd_gen) {
		u->cmd_live--;
		if (status == 0 && text != NULL && text[0] != '\0') {
			ui_push(u, ST_META, "\n");
			ui_push(u, ST_META, text);
			ui_push(u, ST_META, "\n");
		} else if (status < 0) {
			ui_push(u, ST_ERROR, "\n/");
			ui_push(u, ST_ERROR, w->name);
			ui_push(u, ST_ERROR, ": ");
			ui_push(u, ST_ERROR, text != NULL ? text : "failed");
			ui_push(u, ST_ERROR, "\n");
		}
		u->dirty = true;
	}
	free(w);
}

/* Drop the results of the plugin commands still running. */
static bool
cmd_cancel_all(struct ui *u)
{
	if (u->cmd_live == 0)
		return false;
	u->cmd_gen++;
	u->cmd_live = 0;
	ui_push(u, ST_META, "\n[command cancelled]\n");
	return true;
}

/* A '/word ...' line: run a UI command instead of prompting the model. */
static void
run_command(struct ui *u, const char *line)
{
	const struct ui_command *cmd;
	struct cmd_wait *w;
	const char *arg;
	char name[40];
	size_t wlen;
	int r;

	line++; /* skip '/' */
	arg = line;
	while (*arg != '\0' && *arg != ' ')
		arg++;
	wlen = (size_t)(arg - line);
	while (*arg == ' ')
		arg++;

	cmd = ui_command_find(line, wlen);
	if (cmd != NULL) {
		cmd->fn(u, arg);
		return;
	}

	/* Then the commands plugins registered. */
	if (wlen < sizeof(name)) {
		memcpy(name, line, wlen);
		name[wlen] = '\0';
		w = calloc(1, sizeof(*w));
		if (w == NULL) {
			ui_push(u, ST_ERROR, "\nout of memory\n");
			return;
		}
		w->u = u;
		w->gen = u->cmd_gen;
		(void)snprintf(w->name, sizeof(w->name), "%s", name);
		r = clm_lua_command_run(u->lua_env, name, arg, cmd_done, w);
		if (r == 0) {
			u->cmd_live++;
			(void)snprintf(
			    u->cmd_name, sizeof(u->cmd_name), "/%s", name);
			u->dirty = true;
			return;
		}
		free(w);
		if (r != -ENOENT) {
			ui_push(u, ST_ERROR, "\n/");
			ui_push(u, ST_ERROR, name);
			ui_push(u, ST_ERROR, ": could not start\n");
			return;
		}
	}
	ui_push(u, ST_ERROR, "\nunknown command (try /help or ^G)\n");
}

/*
 * Record a submitted line in the recall history. Skips empties and immediate
 * duplicates of the most recent entry. The browse cursor is reset to the live
 * line by the caller after submit.
 */
/* Bounds on what Up/Down recall keeps: total text, and entry count. */
#define HIST_MAX_BYTES (256UL * 1024)
#define HIST_MAX_ENTRIES 500

static void
hist_add(struct ui *u, const char *line)
{
	char *dup;
	size_t len = strlen(line);

	if (line[0] == '\0')
		return;
	if (u->nhist > 0 && strcmp(u->hist[u->nhist - 1], line) == 0)
		return;

	if (u->nhist == u->cap_hist) {
		size_t ncap = u->cap_hist ? u->cap_hist * 2 : 16;
		char **h = (char **)realloc((void *)u->hist, ncap * sizeof(*h));
		if (h == NULL)
			return; /* history is best-effort */
		u->hist = h;
		u->cap_hist = ncap;
	}
	dup = strdup(line);
	if (dup == NULL)
		return;
	u->hist[u->nhist++] = dup;
	u->hist_bytes += len;

	/* Recall is a convenience, not a log: a couple of large pastes must
	 * not pin megabytes for the rest of the session. */
	while (u->nhist > 1 &&
	    (u->hist_bytes > HIST_MAX_BYTES || u->nhist > HIST_MAX_ENTRIES)) {
		u->hist_bytes -= strlen(u->hist[0]);
		free(u->hist[0]);
		memmove((void *)u->hist, (void *)(u->hist + 1),
		    (u->nhist - 1) * sizeof(*u->hist));
		u->nhist--;
	}
}

/* Load a history entry (or a saved live line) into the input buffer. */
static void
hist_load(struct ui *u, const char *text)
{
	size_t n = strlen(text);

	if (!ui_input_reserve(u, n))
		return;
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
			free(u->hist_saved);
			u->hist_saved = strdup(u->input);
		}
		u->hist_pos--;
		hist_load(u, u->hist[u->hist_pos]);
	} else { /* newer */
		if (u->hist_pos >= u->nhist)
			return; /* already at the live line */
		u->hist_pos++;
		if (u->hist_pos == u->nhist)
			hist_load(u,
			    u->hist_saved != NULL ? u->hist_saved
			                          : ""); /* the live line */
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
	else {
		/* Always queue, never submit straight through: the drain
		 * hands it to the agent the moment the agent will take it,
		 * and that keeps one path for idle and mid-turn input. */
		enqueue_steering(u, u->input);
		if (!u->busy)
			drain_steering_queue(u);
	}

	u->input_len = 0;
	u->input_pos = 0;
	/* A pasted line can be large, and the buffer only ever grows. Give
	 * an oversized one back once the line is on its way. */
	if (u->input_cap > INPUT_KEEP_BYTES) {
		char *p = realloc(u->input, INPUT_KEEP_BYTES);

		if (p != NULL) {
			u->input = p;
			u->input_cap = INPUT_KEEP_BYTES;
			u->input[0] = '\0';
		}
	}
	u->hist_pos = u->nhist; /* reset recall to the live line */
	free(u->hist_saved);
	u->hist_saved = NULL;
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

/* A ':' sits at u->input_pos - 1 (just typed, or just inserted by tab
 * completion). If it closes a ":shortcode:" run immediately behind the
 * cursor, replace the whole ":shortcode:" span with the looked-up glyph,
 * Slack/Discord-style. No-op (silently) if there's no opening ':' nearby,
 * the name has characters a shortcode can't, or the name isn't in the
 * table. Declared in tui_internal.h -- complete.c calls this too, after an
 * unambiguous shortcode-name tab completion appends the closing ':'. */
void
tui_expand_emoji_at_cursor(struct ui *u)
{
	size_t close = u->input_pos - 1; /* index of the ':' just inserted */
	size_t i = close;
	while (i > 0) {
		i--;
		char c = u->input[i];
		if (c == ':')
			break;
		if (!(isalnum((unsigned char)c) || c == '_' || c == '+' ||
		        c == '-'))
			return;
		if (close - i > 64) /* no shortcode is this long; bail early */
			return;
	}
	if (u->input[i] != ':')
		return;

	size_t namestart = i + 1;
	size_t namelen = close - namestart;
	if (namelen == 0)
		return;

	char glyph[5];
	size_t glyphlen =
	    clm_emoji_lookup(u->input + namestart, namelen, glyph);
	if (glyphlen == 0)
		return;

	size_t fullspan = close - i + 1; /* ":name:" including both colons */
	if (glyphlen > fullspan &&
	    !ui_input_reserve(u, u->input_len + (glyphlen - fullspan)))
		return;

	size_t tail_len = u->input_len - u->input_pos;
	memmove(u->input + i + glyphlen, u->input + u->input_pos, tail_len);
	memcpy(u->input + i, glyph, glyphlen);
	u->input_len = u->input_len - fullspan + glyphlen;
	u->input_pos = i + glyphlen;
	u->dirty = true;
}

static void
input_char(struct ui *u, wint_t wch)
{
	char mb[MB_LEN_MAX];
	int n = wctomb(mb, (wchar_t)wch);
	if (n <= 0)
		return;
	if (!ui_input_reserve(u, u->input_len + (size_t)n))
		return;
	/* Insert at the cursor, shifting the tail right. */
	memmove(u->input + u->input_pos + (size_t)n, u->input + u->input_pos,
	    u->input_len - u->input_pos);
	memcpy(u->input + u->input_pos, mb, (size_t)n);
	u->input_len += (size_t)n;
	u->input_pos += (size_t)n;
	u->dirty = true;

	if (wch == L':')
		tui_expand_emoji_at_cursor(u);
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
	if (!buf_reserve(&u->kill, &u->kill_cap, n))
		return;
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
	if (!ui_input_reserve(u, u->input_len + u->kill_len))
		return;
	memmove(u->input + u->input_pos + u->kill_len, u->input + u->input_pos,
	    u->input_len - u->input_pos);
	memcpy(u->input + u->input_pos, u->kill, u->kill_len);
	u->input_len += u->kill_len;
	u->input_pos += u->kill_len;
	u->dirty = true;
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
	case 'y':
	case 'Y':
		d = CLM_PERM_ALLOW_ONCE;
		label = "allowed";
		break;
	case 'a':
	case 'A':
		d = CLM_PERM_ALLOW_ALWAYS;
		label = "always allowed";
		break;
	case 'n':
	case 'N':
		d = CLM_PERM_DENY_ONCE;
		label = "denied";
		break;
	case 'd':
	case 'D':
		d = CLM_PERM_DENY_ALWAYS;
		label = "always denied";
		break;
	case 27:
		d = CLM_PERM_DENY_ONCE;
		label = "denied (cancelled)";
		break;
	default:
		return false; /* not an answer key */
	}

	/* Pop the front of the queue. */
	u->perm_count--;
	for (size_t i = 0; i < u->perm_count; i++)
		u->perm_queue[i] = u->perm_queue[i + 1];
	u->perm_showing = false;

	/* For "always" decisions, we need the answered name before we
	 * potentially free the invocation. */
	const char *answered_name = NULL;
	if (d == CLM_PERM_ALLOW_ALWAYS || d == CLM_PERM_DENY_ALWAYS) {
		answered_name = clm_permission_req_name(req);
	}

	ui_push(u, ST_PERM, "-> ");
	ui_push(u, ST_PERM, label);
	ui_push(u, ST_PERM, "\n");
	clm_tool_permission_respond(u->agent, req, d);

	/* Escape also cancels the rest of the turn (drain remaining). */
	if (ch == 27) {
		for (size_t i = 0; i < u->perm_count; i++)
			clm_tool_permission_respond(
			    u->agent, u->perm_queue[i], CLM_PERM_DENY_ONCE);
		u->perm_count = 0;
		clm_agent_cancel(u->agent);
	}

	/* For "always" decisions, auto-resolve remaining queued requests for
	 * the same tool without prompting. */
	if (d == CLM_PERM_ALLOW_ALWAYS || d == CLM_PERM_DENY_ALWAYS) {
		size_t i = 0;
		while (i < u->perm_count) {
			const char *qname =
			    clm_permission_req_name(u->perm_queue[i]);
			if (answered_name != NULL && qname != NULL &&
			    strcmp(answered_name, qname) == 0) {
				clm_tool_permission_respond(
				    u->agent, u->perm_queue[i], d);
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
		/* Every key -- including the TAB that starts a new completion
		 * pass -- invalidates any completion request still in flight
		 * from an earlier key (see complete_generation's doc comment
		 * in tui_internal.h). */
		u->complete_generation++;

		if (u->help_shown && help_key(u, r, wch))
			continue;

		/*
		 * Permission-input mode: while a tool authorization is pending,
		 * the next key answers it and nothing else is processed.
		 */
		if (u->perm_showing && r != KEY_CODE_YES) {
			if (answer_permission(u, (int)wch))
				continue;
			/* Not a recognised answer key: ignore it, stay pending.
			 */
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
			case UI_KEY_PASTE_START: {
				/*
				 * Consume everything up to the matching END
				 * marker ourselves, inserting it straight into
				 * the input buffer (embedded newlines included)
				 * rather than letting it flow back through the
				 * normal key-at-a-time path, where a '\r'/'\n'
				 * means "submit". A CR inside pasted text
				 * (xterm sends line endings as '\r') is
				 * translated to '\n' so it renders/wraps the
				 * same as a typed newline.
				 *
				 * u->in is nodelay(), so wget_wch never blocks
				 * -- ERR just means "nothing buffered right
				 * now", not "the paste is over". A large paste
				 * can arrive split across multiple pty reads,
				 * so on ERR we retry briefly (bounded, so a
				 * real dropped/missing END marker can't wedge
				 * the UI) before giving up and taking whatever
				 * arrived so far.
				 */
				wint_t pwch;
				int pr;
				int idle_retries = 0;
				const int max_idle_retries =
				    200; /* ~200 * 1ms = 200ms */
				for (;;) {
					pr = wget_wch(u->in, &pwch);
					if (pr == ERR) {
						if (++idle_retries >
						    max_idle_retries)
							break; /* END never
							          showed up;
							          stop waiting
							        */
						napms(1);
						continue;
					}
					idle_retries = 0;
					if (pr == KEY_CODE_YES) {
						if (pwch ==
						    (wint_t)UI_KEY_PASTE_END)
							break;
						continue; /* ignore other
						             special keys
						             mid-paste */
					}
					if (pwch == L'\r')
						pwch = L'\n';
					if (pwch >= 32 || pwch == L'\n' ||
					    pwch == L'\t')
						input_char(u, pwch);
				}
				u->dirty = true;
				break;
			}
			case UI_KEY_PASTE_END:
				/* Stray END with no matching START: ignore. */
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
				/* An empty input line has no meaningful
				 * "end of line" to jump to, so repurpose End
				 * there as "scroll the transcript to the
				 * bottom" instead -- the same convention many
				 * pagers/terminals use, and it doesn't
				 * collide with the common case (End while
				 * actually editing a line still just moves
				 * the cursor). */
				if (u->input_len == 0)
					u->scroll = 0;
				else
					u->input_pos = u->input_len;
				u->dirty = true;
				break;
			case KEY_DC: /* Delete */
				if (u->input_pos < u->input_len)
					input_kill(u, u->input_pos,
					    next_boundary(u->input,
					        u->input_pos, u->input_len));
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
			case KEY_MOUSE: {
				/* Wheel notch: a few lines, not a full page
				 * like PgUp/PgDn. Terminals report wheel
				 * motion as button-4/5 press events, no
				 * corresponding release. */
				MEVENT event;
				if (getmouse(&event) != OK)
					break;
				const size_t notch = 3;
				if (event.bstate & BUTTON4_PRESSED) {
					u->scroll += notch;
					u->dirty = true;
				} else if (event.bstate & BUTTON5_PRESSED) {
					if (u->scroll > notch)
						u->scroll -= notch;
					else
						u->scroll = 0;
					u->dirty = true;
				}
				break;
			}
			case KEY_RESIZE: /* handled via SIGWINCH */
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
		case 7: /* Ctrl-G: help popup */
			help_open(u);
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
		case 27: { /* Escape: cancel a running turn, else clear the
			      input */
			int cancel_rc = clm_agent_cancel(u->agent);
			if (cancel_rc == 0) {
				ui_push(u, ST_META, "\n[cancelled]\n");
			} else if (cancel_rc != -EALREADY &&
			    cmd_cancel_all(u)) {
				/* Nothing else to do: the commands were the
				 * only thing in flight. */
			} else if (cancel_rc != -EALREADY) {
				/* Truly nothing in flight -- treat Escape as
				 * clear-input instead. -EALREADY means a cancel
				 * from an earlier Escape is still unwinding
				 * (killed subprocesses take a moment to
				 * actually exit); do nothing rather than
				 * spamming another
				 * "[cancelled]" per repeat keypress or clearing
				 * input the user hasn't finished typing yet. */
				u->input_len = 0;
				u->input_pos = 0;
			}
			u->dirty = true;
			break;
		}
		case 9: /* TAB: completion (commands, config/live names, paths)
		         */
			complete_input(u, u->complete_generation);
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
	int txt_x, txt_w;

	txt_geom(w, &txt_x, &txt_w);
	u->txt = newwin(h - 2, txt_w, 0, txt_x);
	u->stat = newwin(1, w, h - 2, 0);
	u->in = newwin(1, w, h - 1, 0);
	u->in_h = 1;
	scrollok(u->in, TRUE);
	keypad(u->in, TRUE);
	nodelay(u->in, TRUE);
}

static void
on_term(uv_signal_t *s, int signum)
{
	struct ui *u = s->data;

	(void)signum;
	uv_stop(u->loop);
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
init_colors(struct ui *u)
{
	if (getenv("NO_COLOR") != NULL || !has_colors())
		return;
	start_color();
	use_default_colors();
	u->color = true;
	/*
	 * Colours are chosen to read well on a stock Solarized 16-colour
	 * palette where A_BOLD promotes 0-7 accents to the grey 8-15 base
	 * tones. So we distinguish by colour, not bold, and reserve the grey
	 * base tones for genuinely secondary text (the reasoning channel).
	 * TODO: themeable.
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
	 * Reserved for attention, not decoration: red = error; yellow = warning
	 * / timeout / tool-call permission prompts (the "about to do something,
	 * confirm?" caution colour -- shares the base-8 yellow with ST_TIMEOUT
	 * rather than reaching for ANSI 9/brred, which silently vanished on
	 * anything without a 16-colour terminfo entry, e.g. plain "screen" over
	 * ssh in tmux). Plain, not | A_BOLD: some terminal/terminfo
	 * combinations render bold as a fixed bright-white override rather
	 * than intensifying the underlying colour pair, which made this show
	 * up as bold white instead of yellow. TODO: expose the whole
	 * role->colour map via the theme layer.
	 */
	init_pair(9, COLOR_YELLOW, -1); /* permission prompt (yellow) */
}

/* Periodic (and startup) connectivity probe. */
static void
on_health(uv_timer_t *t)
{
	struct ui *u = t->data;
	clm_agent_check_connection(u->agent);
}

/*
 * Re-render a restored session's transcript through the same seg vocabulary
 * the live callbacks use, so a resumed conversation looks like it never
 * ended. Internal context-update messages are replayed into the agent but
 * not shown, same as live (they are never echoed).
 */
static void
replay_transcript(struct ui *u, const struct clm_history *h)
{
	const struct clm_message *m;
	int cnt[CLM_TALLY_N] = {0};

	TAILQ_FOREACH(m, h, entries)
	{
		/* A cluster ends at the first message that is not one of its
		 * results. Its tally is what rebuild_render folds older
		 * clusters into, so a resumed transcript needs it as much as
		 * a live one does. */
		if (m->role != CLM_ROLE_TOOL) {
			push_batch_counts(u, cnt);
			memset(cnt, 0, sizeof(cnt));
		}
		switch (m->role) {
		case CLM_ROLE_SYSTEM:
			break;
		case CLM_ROLE_USER:
			if (m->content == NULL ||
			    strncmp(m->content, "[context update]",
			        strlen("[context update]")) == 0)
				break;
			if (replay_peer_message(u, m->content))
				break;
			ui_push(u, ST_USER, "\nyou> ");
			ui_push(u, ST_USER, m->content);
			ui_push(u, ST_USER, "\n");
			break;
		case CLM_ROLE_ASSISTANT: {
			const struct clm_tool_call *tc;
			TAILQ_FOREACH(tc, &m->tool_calls, entries)
			{
				push_tool_summary(u,
				    tc->name != NULL ? tc->name : "?",
				    tc->args);
				tally_tool(cnt, tc->name);
			}
			if (m->content != NULL && m->content[0] != '\0') {
				ui_push(u, ST_LABEL, "\nclm>\n");
				ui_push(u, ST_ASSIST, m->content);
				ui_push(u, ST_NORMAL, "\n");
			}
			break;
		}
		case CLM_ROLE_TOOL:
			if (m->content != NULL && m->content[0] != '\0') {
				ui_push(u, ST_TOOL_OUT, m->content);
				if (m->content[strlen(m->content) - 1] != '\n')
					ui_push(u, ST_TOOL_OUT, "\n");
			}
			break;
		}
	}
	push_batch_counts(u, cnt); /* a cluster that ends the history */
}

/* The handles this frontend owns. Close them before draining the loop: their
 * callbacks reach into an agent and a ui that teardown has already released.
 */
static void
close_own_handles(struct ui *u)
{
	uv_handle_t *own[] = {(uv_handle_t *)&u->stdin_poll,
	    (uv_handle_t *)&u->repaint, (uv_handle_t *)&u->winch,
	    (uv_handle_t *)&u->term, (uv_handle_t *)&u->hup,
	    (uv_handle_t *)&u->health};

	for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
		if (!uv_is_closing(own[i]))
			uv_close(own[i], NULL);
	}
}

int
tui_run(const struct clm_cfg *cfg, const char *plugin_dir,
    const char *const *opt_plugins, struct clm_lua_cfg *lcfg,
    const char *config_load_err, const char *forever_prompt,
    struct clm_session *session, const struct clm_history *restore,
    int repaired_tool_calls, bool allow_all, bool auto_mode)
{
	struct ui *u;
	uv_loop_t *loop;
	bool drained;
	int r;

	setlocale(LC_ALL, "");

	u = calloc(1, sizeof(*u)); /* too large for the stack frame limit */
	if (u == NULL) {
		fprintf(stderr, "error: out of memory\n");
		return 1;
	}
	u->last_total = -1; /* draw_transcript hasn't painted yet */
	u->allow_all = allow_all;
	u->auto_mode = auto_mode;
	if (!ui_input_reserve(u, 1024)) {
		fprintf(stderr, "error: out of memory\n");
		free(u);
		return 1;
	}
	u->input[0] = '\0';

	loop = uv_default_loop();
	u->loop = loop;
	set_current_model(u, cfg->model);
	set_current_provider(u, cfg->provider_name);
	u->forever_prompt = forever_prompt;
	if (lcfg != NULL) {
		const char *aname = clm_lua_cfg_get_agent_name(lcfg);
		if (aname != NULL)
			u->agent_name = strdup(aname);
	}
	u->lcfg = lcfg;
	u->plugin_dir = plugin_dir;
	u->opt_plugins = opt_plugins;
	u->session = session;
	set_session_short(u);
	u->state = CLM_STATE_IDLE;
	/* Default to the clean view: reasoning hidden and tool output
	 * collapsed. Opt in with ^R / ^O (see the status-bar hints). */
	u->show_reasoning = false;
	u->expand_output = false;
	if (lcfg != NULL) {
		const char *sr = clm_lua_cfg_get_str(lcfg, "show_thinking");
		if (sr != NULL && strcmp(sr, "true") == 0)
			u->show_reasoning = true;
		const char *eo = clm_lua_cfg_get_str(lcfg, "expand_output");
		if (eo != NULL && strcmp(eo, "true") == 0)
			u->expand_output = true;
	}

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
	apply_effort(u, cfg->provider_name, cfg->model);
	clm_agent_set_spool(u->agent, cb_spool, u);

	/* Peer messaging is optional: a bind failure (no runtime dir, a
	 * hostile /tmp) costs the socket, not the session. */
	if (u->session != NULL &&
	    clm_peer_start(u->agent, loop, clm_session_id(u->session),
	        u->agent_name != NULL ? u->agent_name : "clm", cfg->model,
	        cb_peer_message, u, &u->peer) == 0)
		(void)clm_peer_register_tools(u->agent);
	/* Desktop uv layer: add the shell_exec/bg_exec tools (not in the
	 * portable core). */
	clm_tools_register_shell(u->agent);
	clm_tools_register_bg(u->agent);
	clm_tools_register_monitor(u->agent);

	if (clm_lua_env_new(u->agent, &u->lua_env) == 0) {
		if (lcfg != NULL)
			clm_lua_env_set_config_from(u->lua_env, lcfg);
		autofree char *ppath = NULL;

		if (plugin_dir == NULL)
			ppath = xdg_config_path("clm/plugins");
		clm_cli_load_plugins(u->lua_env,
		    plugin_dir != NULL ? plugin_dir : ppath, lcfg, opt_plugins,
		    cb_mcp_status, u);
	}
	auto_check_hooks(u);
	u->mcp_clients = clm_cli_connect_mcp_servers(
	    u->agent, loop, lcfg, cb_mcp_status, u, &u->mcp_client_count);
	clm_cli_wait_mcp_ready(loop, CLM_MCP_READY_WAIT_MS);

	/* newterm, not initscr: it hands back the screen so the exit path can
	 * delscreen it and leave nothing of curses outstanding. */
	u->screen = newterm(NULL, stdout, stdin);
	if (u->screen == NULL) {
		fprintf(stderr, "error: cannot initialize the terminal\n");
		return 1;
	}
	def_prog_mode();
	/* cbreak still leaves terminal flow control enabled: on many terminals
	 * Ctrl-S/Ctrl-Q are intercepted and, more importantly for editing, the
	 * configured delayed-suspend character can swallow Ctrl-Y before curses
	 * sees it.  raw mode gives the input editor every control key it
	 * handles itself (including Ctrl-Y yank). */
	raw();
	noecho();
	nonl();
	set_escdelay(25); /* make a lone Escape (cancel) responsive */
	mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
	init_colors(u);
	make_windows(u);

	/*
	 * Bracketed paste: ask the terminal to wrap pasted text in
	 * \033[200~ ... \033[201~ so handle_keys can tell a paste apart from
	 * real Enter keypresses (see UI_KEY_PASTE_START's comment above).
	 * define_key teaches wget_wch to recognize the two marker sequences
	 * as atomic custom key codes; the \033[?2004h write is what actually
	 * turns the feature on in the terminal. Both are no-ops (harmless
	 * escape bytes swallowed, or an unrecognized sequence typed back
	 * character-by-character) on a terminal that doesn't support it.
	 */
	define_key("\033[200~", UI_KEY_PASTE_START);
	define_key("\033[201~", UI_KEY_PASTE_END);
	fputs("\033[?2004h", stdout);
	fflush(stdout);

	ui_push(u, ST_META,
	    "clm -- type a prompt, /help for commands, /quit to exit.\n");

	/*
	 * config.lua exists but failed to load: a real problem, not the
	 * ordinary "nothing configured yet" case (see frontend.h's
	 * config_load_err doc comment) -- the agent is now running on
	 * hardcoded defaults with none of the user's providers/model/
	 * system_prompt/tools settings applied, and that needs to be
	 * impossible to miss, not a line buried in $CLM_DEBUG_LOG. Pushed as
	 * its own ST_ERROR block (bold+reverse red / reverse video, see
	 * seg_attr) with blank-line padding so it stands out from the ST_META
	 * banner line right above it, rather than blending into ordinary
	 * startup chatter.
	 */
	if (config_load_err != NULL) {
		autofree char *banner = NULL;
		if (asprintf(&banner,
		        "\n"
		        "*** config.lua failed to load; running with "
		        "defaults only ***\n"
		        "%s\n"
		        "fix config.lua and restart, or /quit and re-run "
		        "after fixing it.\n",
		        config_load_err) >= 0) {
			ui_push(u, ST_ERROR, banner);
		} else {
			ui_push(u, ST_ERROR,
			    "\n*** config.lua failed to load; running with "
			    "defaults only ***\n");
		}
	}

	if (restore != NULL) {
		r = clm_agent_restore_history(u->agent, restore);
		if (r < 0) {
			ui_push(u, ST_ERROR,
			    "\nfailed to restore the session; "
			    "starting fresh\n");
		} else {
			replay_transcript(u, restore);
			ui_push(u, ST_META, "\n[resumed session ");
			ui_push(u, ST_META,
			    u->session != NULL ? clm_session_id(u->session)
			                       : "?");
			ui_push(u, ST_META, "]\n");
			/* Say so if a crash-dangling tool call got patched --
			 * never rewrite history under the user silently. */
			if (repaired_tool_calls > 0) {
				autofree char *note = NULL;
				if (asprintf(&note,
				        "[note: %d tool call%s had no "
				        "recorded result (the session ended "
				        "before it finished) -- filled in "
				        "with a placeholder so the "
				        "conversation could resume]\n",
				        repaired_tool_calls,
				        repaired_tool_calls == 1 ? "" : "s") >=
				    0)
					ui_push(u, ST_META, note);
			}
		}
	}

	uv_poll_init(loop, &u->stdin_poll, fileno(stdin));
	u->stdin_poll.data = u;
	uv_poll_start(&u->stdin_poll, UV_READABLE, on_stdin);

	uv_timer_init(loop, &u->repaint);
	u->repaint.data = u;
	uv_timer_start(&u->repaint, on_repaint, 0, 80);

	uv_signal_init(loop, &u->winch);
	u->winch.data = u;
	uv_signal_start(&u->winch, on_winch, SIGWINCH);

	/* Leave by the same path a /quit takes: the terminal is restored and
	 * the peer socket removed instead of being left for the next listing
	 * to reap. */
	uv_signal_init(loop, &u->term);
	u->term.data = u;
	uv_signal_start(&u->term, on_term, SIGTERM);
	uv_signal_init(loop, &u->hup);
	u->hup.data = u;
	uv_signal_start(&u->hup, on_term, SIGHUP);

	/* Probe connectivity now (0ms) and every 20s, so a wrong URL or a down
	 * server shows in the status bar before the first prompt. */
	u->conn = CLM_CONN_CHECKING;
	uv_timer_init(loop, &u->health);
	u->health.data = u;
	uv_timer_start(&u->health, on_health, 0, 20000);

	/* --forever: kick off the first turn ourselves instead of waiting on
	 * a human to type it in. */
	if (u->forever_prompt != NULL)
		do_submit(u, u->forever_prompt, true);

	uv_run(loop, UV_RUN_DEFAULT);

	/* Before curses goes away: a cancelled turn still repaints. */
	clm_settle_turn(u->agent, loop);

	/* Leave the terminal as we found it, and curses with nothing of ours
	 * still allocated: the windows first, then the screen behind them. */
	fputs("\033[?2004l", stdout);
	fflush(stdout);
	delwin(u->txt);
	delwin(u->stat);
	delwin(u->in);
	endwin();
	delscreen(u->screen);
	u->screen = NULL;
	/* Print the resume handle now that the terminal is sane again; a
	 * session nothing was said in is deleted instead. */
	if (u->session != NULL) {
		if (clm_session_is_empty(u->session)) {
			(void)clm_session_discard(u->session);
		} else {
			/* Include the model that was live at exit: a
			 * resumed session replays history into a fresh
			 * agent built from whatever -m/config.lua says right
			 * now, so if the user switched models mid-session
			 * (/model, /provider, /agent) the resume command
			 * would otherwise silently go back to the original
			 * one. Printed, not auto-applied -- --resume doesn't
			 * read it back, this is purely so the user can paste
			 * it onto -m themselves. */
			if (u->model != NULL) {
				/* -m takes "provider/model-id" (see
				 * src/model_spec.h); the bare model id alone
				 * doesn't identify a connection, so prefix it
				 * with provider_name when known. */
				if (u->provider_name != NULL)
					printf("session: %s, resume it with:\n"
					       "  clm --resume %s -m %s/%s\n",
					    clm_session_id(u->session),
					    clm_session_id(u->session),
					    u->provider_name, u->model);
				else
					printf("session: %s, resume it with:\n"
					       "  clm --resume %s -m %s\n",
					    clm_session_id(u->session),
					    clm_session_id(u->session),
					    u->model);
			} else {
				printf("session: %s, resume it with:\n"
				       "  clm --resume %s\n",
				    clm_session_id(u->session),
				    clm_session_id(u->session));
			}
			clm_session_free(u->session);
		}
		u->session = NULL;
	}
	clm_lua_env_free(u->lua_env);
	clm_cli_free_mcp_servers(u->mcp_clients, u->mcp_client_count);
	clm_peer_free(u->peer);
	clm_agent_free(u->agent);
	clm_host_uv_free(u->host);
	close_own_handles(u);
	drained = clm_drain_loop(loop) == 0;
	for (size_t i = 0; i < u->nsegs; i++)
		free(u->segs[i].text);
	free(u->segs);
	free((void *)u->perm_queue);
	for (size_t i = 0; i < u->nrsegs; i++) {
		if (u->rsegs[i].owned)
			free(u->rsegs[i].text);
	}
	free(u->rsegs);
	free(u->wrap_row);
	free(u->wrap_col);
	for (size_t i = 0; i < u->steering_nqueue; i++)
		free(u->steering_queue[i]);
	free((void *)u->steering_queue);
	for (size_t i = 0; i < u->n_echoed; i++)
		free(u->echoed[i]);
	for (size_t i = 0; i < u->nhist; i++)
		free(u->hist[i]);
	free((void *)u->hist);
	free(u->input);
	free(u->kill);
	free(u->hist_saved);
	free(u->model);
	free(u->agent_name);
	free(u->provider_name);
	/*
	 * A drain that timed out left a handle of ours still closing, and
	 * libuv holds a pointer to it inside `u`. Freeing the struct now is
	 * exactly the use-after-free the drain exists to prevent, so strand
	 * the shell of it instead -- this is the last statement before
	 * returning to main, and exit(2) reclaims it either way.
	 */
	if (drained)
		free(u);

	return 0;
}
