// SPDX-License-Identifier: ISC
/* Internal definitions -- not installed. */
#ifndef CLM_INTERNAL_H
#define CLM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "clm/clm.h"
#include "clm/history.h"
#include "clm/llm.h"
#include "clm/tools.h"
#include "clm/ratelimit.h"
#include "clm/cleanup.h"
#include "useful.h"

/* Default maximum agent loop iterations when cfg->max_iterations is 0. */
#define CLM_DEFAULT_MAX_ITERATIONS 25

/* struct clm_host, clm_http_call, clm_timer come from clm/host.h (via clm.h). */

struct clm_agent {
	struct clm_llm *llm;
	struct clm_host *host;
	enum clm_agent_state state;
	char *last_error;
	struct clm_history history;
	struct clm_tool_list tools;
	size_t tool_count; /* live (non-removed) tools; diagnostics only */
	size_t max_iterations;
	size_t iteration;
	bool stream;
	enum clm_backend backend; /* server impl, for gating quirks like /props */
	int64_t ctx_max;          /* per-conversation context tokens, 0 = unknown */
	int64_t ctx_used;         /* last known prompt+completion tokens, 0 = unknown;
	                           * updated in emit_usage(), read by
	                           * clm_agent_over_autocompact_threshold() */
	int autocompact_pct;      /* 0 = use default CLM_AUTOCOMPACT_PCT */
	const char *const *volatile_tools; /* borrowed from cfg; see clm_cfg */
	char *props_url;          /* llama.cpp GET /props, or NULL */
	char *compact_body;       /* POST body for an in-flight /compact, freed on done */
	time_t last_time_stamp; /* wall clock of the last injected time context */
	struct clm_tool_batch *active_batch;

	/* Token-bucket rate limiter for tool dispatch (NULL = unlimited). */
	struct clm_ratelimit *tool_rl;

	/* Token-bucket rate limiter for outgoing LLM requests (NULL =
	 * unlimited). Separate from tool_rl: a single logical turn can chain
	 * several LLM round-trips (tool call -> result -> another LLM call
	 * -> ...) with no tool-dispatch rate limiting in between them at
	 * all, so a fast tool-calling agent can burst well past a backend's
	 * requests-per-minute limit even though tool_rl is perfectly happy.
	 * Added after a real 429 incident against OpenAI during exactly
	 * this pattern. */
	struct clm_ratelimit *llm_rl;
	struct clm_timer *llm_rl_timer; /* non-NULL while a start_turn retry is parked */

	/* The turn's in-flight HTTP request (for cancellation), else NULL. */
	struct clm_http_call *inflight;
	bool cancelling; /* a cancel is unwinding the current turn */
	/* True while a clm_agent_compact() call was triggered internally
	 * from clm_agent_tools_done() (mid-chain, between a tool batch
	 * finishing and the next LLM call) rather than by an explicit
	 * caller. Lets compact_success_cb/compact_error_cb tell the two
	 * cases apart: an explicit caller (e.g. tui.c's own end-of-turn
	 * check) expects the normal cb_on_turn_done to fire on completion,
	 * but a mid-chain trigger is NOT a real turn ending -- the tool
	 * chain that was interrupted to make room still needs to continue,
	 * so completion should resume it via clm_agent_start_turn() instead
	 * of reporting done (which would make a --forever caller think the
	 * conversational turn finished and incorrectly submit a fresh
	 * prompt on top of an unfinished tool chain). */
	bool compact_resume_chain;

	/* Set when a mid-chain autocompact fires (tools_done triggers it)
	 * so the UI can print a "compacting..." message; polled/cleared
	 * via clm_agent_take_mid_chain_compact_started(). */
	bool mid_chain_compact_started;

	/* Set when a mid-chain autocompact completes successfully (the
	 * interrupted chain is about to resume); polled/cleared via
	 * clm_agent_take_mid_chain_compact_succeeded(). */
	bool mid_chain_compact_succeeded;

	/* Set instead of firing any callback when a mid-chain autocompact
	 * (see compact_resume_chain above) fails -- there's no "turn done"
	 * event to piggyback an error message on for that path, since the
	 * whole point is that the interrupted tool chain resumes silently.
	 * A UI polls/checks this via clm_agent_take_mid_chain_compact_error()
	 * on the next state-change callback instead. Cleared on a successful
	 * mid-chain compaction too, so a stale failure from an earlier
	 * attempt never lingers past a later success. */
	bool mid_chain_compact_failed;

	/* Event callbacks */
	void (*cb_on_assistant_text)(const char *, void *);
	void (*cb_on_reasoning)(const char *, void *);
	void (*cb_on_tool_begin)(const char *, const char *, void *);
	void (*cb_on_permission)(const struct clm_permission_req *, void *);
	void (*cb_on_tool_result)(const char *, const char *, enum clm_tool_outcome, void *);
	void (*cb_on_tool_batch)(size_t, size_t, void *);
	void (*cb_on_finish_reason)(enum clm_finish_reason, void *);
	void (*cb_on_usage)(const struct clm_usage *, void *);
	void (*cb_on_connection)(enum clm_conn_status, const char *, void *);
	void (*cb_on_state)(enum clm_agent_state, void *);
	void (*cb_on_turn_done)(int, void *);
	void *cb_user;

	/* Derived from base_url: the /v1/models URL used for health probes. */
	char *models_url;
};

/* Set agent->last_error to a copy of msg (replacing any previous error). */
void clm_agent_set_error(struct clm_agent *agent, const char *msg);

/*
 * Called by the tool framework when a dispatched batch finishes. status 0
 * advances the turn (next model call); negative errno ends the turn in error.
 */
void clm_agent_tools_done(struct clm_agent *agent, int status);

/*
 * Parse a llama.cpp GET /props body. Sets *ctx_out to the per-conversation
 * context budget and returns 0; returns -1 for a non-llama.cpp or malformed
 * body. Declared here so it is unit-testable without a live server.
 */
int clm_parse_props(const char *body, int64_t *ctx_out);

#endif /* CLM_INTERNAL_H */
