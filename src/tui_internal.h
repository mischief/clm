// SPDX-License-Identifier: ISC
/*
 * Shared internal state between tui.c and complete.c (not installed, not a
 * public API -- just splitting one translation unit's private struct across
 * two files that both need it, the same way the rest of this codebase uses
 * opaque handles + accessors for real module boundaries, except here the
 * "module" is this one binary's TUI and there's no ABI to keep stable).
 */
#ifndef CLM_TUI_INTERNAL_H
#define CLM_TUI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <curses.h>
#include <uv.h>

#include "clm/clm.h"

struct clm_host;
struct clm_lua_env;
struct clm_lua_cfg;
struct clm_mcp_client;

/* Style buckets, mapped to curses attributes in seg_attr() (tui.c). */
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
	ST_BATCH,    /* per-tool-call tally line ("-- ran 1 command"); a
	              * distinct style from ST_META purely so rebuild_render
	              * (tui.c) can find tool-call cluster boundaries without
	              * guessing from other ST_META text */
};

/* One styled run of transcript source text (queued by a callback). */
struct seg {
	enum ui_style style;
	char *text;
	/* Valid only when style == ST_BATCH: the {cmd, read, write, other}
	 * tally this segment's line was formatted from -- kept as numbers
	 * (not just the rendered text) so several older ST_BATCH segments
	 * can be summed into one combined aggregate line at render time (see
	 * rebuild_render/push_collapsed_summary in tui.c). */
	int cnt[4];
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
	struct clm_lua_env *lua_env;
	struct clm_lua_cfg *lcfg;      /* kept alive for /agent switching */
	const char *plugin_dir;        /* NULL = use XDG default */
	struct clm_mcp_client **mcp_clients;
	size_t mcp_client_count;

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
	int last_total; /* wrapped row count as of the last draw_transcript
	                  * call, so a growing transcript can adjust scroll
	                  * to hold the viewport's absolute position steady
	                  * (see draw_transcript) instead of scroll's
	                  * bottom-relative distance silently letting new
	                  * rows drag the view along with them. -1 = not
	                  * yet painted. */

	/* Input line editor (byte buffer, UTF-8; cursor is a byte offset). */
	char input[1024];
	size_t input_len;
	size_t input_pos;
	char kill[1024];
	size_t kill_len;

	/* Bumped on every key handled in handle_keys() (tui.c), including the
	 * TAB that starts a completion pass. An async completion source
	 * (complete.c, e.g. a live /v1/models fetch) snapshots this at
	 * request time and compares it when its result arrives; a mismatch
	 * means the user has since typed something else, pressed TAB again,
	 * or submitted the line, so the result is stale and is dropped
	 * rather than reaching back into an input line that's moved on. */
	uint64_t complete_generation;

	/* Status model. */
	char *model; /* displayed in status bar (owned) */
	char *agent_name; /* displayed in status bar (owned) */
	enum clm_agent_state state;
	enum clm_conn_status conn;
	char conn_detail[64];
	char usage[96];
	int64_t ctx_used; /* tokens carried forward, for the context gauge */
	bool autocompacting; /* true while cb_turn_done re-enters for a compact attempt */
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

	/* --forever: NULL normally; when set, this fixed prompt is
	 * auto-resubmitted every time a turn completes and nothing else is
	 * queued, so the agent keeps going without a human re-prompting it. */
	const char *forever_prompt;

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

/* Append styled text to the transcript source (tui.c). Used by complete.c
 * to show candidate lists / errors the same way any other UI message is
 * shown. */
void ui_push(struct ui *u, enum ui_style style, const char *text);

#endif /* CLM_TUI_INTERNAL_H */
