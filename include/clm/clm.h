// SPDX-License-Identifier: ISC
#ifndef CLM_H
#define CLM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clm/clm_export.h"
#include "clm/compress.h"
#include "clm/host.h"

struct clm_agent;
struct clm_cfg;

/* LLM provider types. */
enum clm_provider {
	CLM_PROVIDER_OPENAI,
	CLM_PROVIDER_OLLAMA,
	CLM_PROVIDER_ANTHROPIC,
};

/*
 * Parse a provider "kind" string (as used in config.lua's providers[*].kind;
 * see clm-config(5)) into the enum. Recognizes "openai", "ollama", and
 * "anthropic"; anything else, including NULL, yields CLM_PROVIDER_OPENAI
 * (the OpenAI-compatible dialect almost every local server also speaks).
 */
CLM_API enum clm_provider clm_provider_from_str(const char *kind);

/*
 * The server implementation behind the API, orthogonal to clm_provider (which
 * is the wire dialect). This gates implementation-specific behaviour: e.g.
 * llama.cpp exposes GET /props with n_ctx and slot counts. GENERIC assumes
 * nothing beyond the dialect. Auto-detected at connect when possible.
 *
 * As backend-specific quirks accumulate, this enum is the seam to grow into a
 * per-backend ops vtable; keep new quirks gated on it rather than scattered.
 */
enum clm_backend {
	CLM_BACKEND_GENERIC = 0,
	CLM_BACKEND_LLAMACPP,
};

/* Outcome of a tool call, reported to on_tool_result. */
enum clm_tool_outcome {
	CLM_TOOL_OK,
	CLM_TOOL_FAILED,
	CLM_TOOL_TIMEDOUT,
};

/*
 * A frontend's answer to a permission request. The _ALWAYS variants are
 * remembered for the rest of the session (per tool), so that tool is not
 * prompted again; the _ONCE variants apply only to this invocation.
 */
enum clm_permission_decision {
	CLM_PERM_ALLOW_ONCE,
	CLM_PERM_ALLOW_ALWAYS,
	CLM_PERM_DENY_ONCE,
	CLM_PERM_DENY_ALWAYS,
};

/*
 * An opaque authorization request handed to the on_permission callback. It
 * references a parked tool invocation and is valid only until answered with
 * clm_tool_permission_respond. Read its fields with the accessors below.
 */
struct clm_permission_req;

/* The tool name being authorized. */
CLM_API const char *clm_permission_req_name(const struct clm_permission_req *req);
/* The raw JSON arguments of the call (for the frontend to display). */
CLM_API const char *clm_permission_req_args(const struct clm_permission_req *req);

/* Result of a server connectivity probe (see clm_agent_check_connection). */
enum clm_conn_status {
	CLM_CONN_CHECKING, /* a probe is in flight */
	CLM_CONN_ONLINE,   /* endpoint reachable (HTTP 2xx) */
	CLM_CONN_OFFLINE,  /* unreachable, or a non-2xx/auth error */
};

/* Why a model response ended (choices[0].finish_reason). */
enum clm_finish_reason {
	CLM_FINISH_STOP,           /* natural end */
	CLM_FINISH_LENGTH,         /* hit the token limit (truncated) */
	CLM_FINISH_TOOL_CALLS,     /* stopped to call tools */
	CLM_FINISH_CONTENT_FILTER, /* filtered */
	CLM_FINISH_OTHER,
};

/* Token accounting for a model response, when the server reports it. */
struct clm_usage {
	int prompt_tokens;
	int completion_tokens;
	int total_tokens;
	double tokens_per_sec; /* generation rate, 0 if unknown */
};

/* Per-tool behaviour flags (clm_tool_def.flags). */
enum clm_tool_flags {
	/* Advertise timeout_ms / output_cap as call parameters the model may
	 * set per invocation, overriding the tool's defaults. */
	CLM_TOOL_TIMEOUT_OVERRIDABLE = 1 << 0,
	CLM_TOOL_OUTPUT_CAP_OVERRIDABLE = 1 << 1,

	/* Skip the permission gate: the tool is safe enough to run unprompted
	 * (e.g. read-only). Without this, a tool is gated (default-deny). */
	CLM_TOOL_NO_PROMPT = 1 << 2,

	/* Do not advertise this tool to the model. It stays in the registry and
	 * is invocable internally (e.g. by plugins), but is filtered out of the
	 * schema sent to the model. */
	CLM_TOOL_HIDDEN = 1 << 3,
};

/*
 * Opaque handle for one in-flight tool call, passed to a tool's invoke fn.
 * The tool reads its args/limits from it and reports completion through it.
 */
struct clm_tool_invocation;

/*
 * A tool implementation. Invoked once per tool call. MUST NOT block: start
 * the work and return. Report completion exactly once via clm_tool_complete()
 * (success) or clm_tool_fail() (error); completing synchronously inside this
 * call is allowed. tool_user is the def's user pointer.
 */
typedef void (*clm_tool_fn)(struct clm_tool_invocation *inv, void *tool_user);

/*
 * A tool definition. Registered with clm_tool_add; the agent copies it (and
 * the strings it points to), so it need not outlive the call.
 */
struct clm_tool_def {
	const char *name;          /* unique; matches the model's function name */
	const char *description;
	const char *params_schema; /* JSON "parameters" object, or NULL */
	clm_tool_fn  invoke;
	void        *user;

	size_t   output_cap;       /* result clamp in bytes; 0 => library default */
	uint64_t timeout_ms;       /* per-call deadline; 0 => no timeout */
	unsigned flags;            /* enum clm_tool_flags */
};

/* Agent state. */
enum clm_agent_state {
	CLM_STATE_IDLE,
	CLM_STATE_THINKING,
	CLM_STATE_CALLING_TOOL,
	CLM_STATE_RATE_LIMITED,
	CLM_STATE_COMPLETE,
	CLM_STATE_ERROR,
};

/*
 * Agent configuration. All string fields are borrowed: the caller retains
 * ownership and must keep them valid for the lifetime of the agent.
 */
struct clm_cfg {
	const char *api_key;
	const char *base_url;
	enum clm_provider provider;
	enum clm_backend backend;  /* server impl; GENERIC (0) auto-detects */
	const char *model;
	size_t max_iterations;
	bool stream;              /* request streamed (SSE) responses */
	const char *system_prompt; /* system message; NULL uses a default */

	/* Provider-specific overrides (0 = use defaults) */
	int64_t context_size;     /* override ctx_max (tokens) */
	int autocompact_pct;      /* override CLM_AUTOCOMPACT_PCT (1-99) */
	int64_t rate_tokens_per_sec; /* token-bucket refill rate */
	int64_t rate_burst;       /* token-bucket burst size */

	/*
	 * Agent policy: NULL-terminated list of fnmatch(3) patterns naming
	 * tools whose results go stale as soon as a newer one exists (state
	 * snapshots like a map read). When a tool matching a pattern
	 * completes, every prior result from that same tool is stubbed in
	 * place ("[superseded by newer <tool>]") before the new result is
	 * recorded -- see clm_history_supersede_tool(). NULL disables.
	 * Borrowed like the string fields above.
	 */
	const char *const *volatile_tools;
};

/*
 * ============================================================================
 * Event callbacks: how libclm talks to the outside world.
 * ============================================================================
 *
 * The library is frontend-agnostic. It emits typed events through this vtable;
 * the frontend (cli, curses ui, test harness) renders them however it likes.
 *
 * CONTRACT -- read before implementing callbacks:
 *
 *   - Every callback is OPTIONAL. Leave any field NULL; the library checks
 *     before calling. A test harness might only set on_turn_done.
 *
 *   - Callbacks fire from WITHIN uv_run(), on the loop thread. They MUST NOT
 *     block (no synchronous network, no blocking reads, no sleeping). Do your
 *     work and return promptly so the loop stays responsive. For a ui, mutate
 *     state here and repaint from a uv_timer -- never draw from inside a
 *     callback that could be mid-turn.
 *
 *   - All string arguments are BORROWED and only valid for the duration of the
 *     callback. strdup anything you need to keep.
 *
 *   - The opaque `user` pointer (set in clm_agent_new) is passed to every
 *     callback. Use it to reach your frontend state; do not use globals.
 *
 *   - The callbacks struct itself is copied by value at clm_agent_new, so it
 *     need not outlive the call.
 */
struct clm_callbacks {
	/* A chunk of assistant text (streaming) or the full final answer. */
	void (*on_assistant_text)(const char *text, void *user);

	/* A chunk of model reasoning, for models that emit a think channel. */
	void (*on_reasoning)(const char *text, void *user);

	/* The agent is about to execute a tool. args is the raw JSON arguments. */
	void (*on_tool_begin)(const char *name, const char *args, void *user);

	/*
	 * A gated tool needs authorization before it runs. The frontend renders
	 * a prompt however it likes and MUST eventually call
	 * clm_tool_permission_respond(agent, req, decision) -- possibly much
	 * later. Until then the invocation is parked and the turn waits. If this
	 * callback is NULL, gated tools are DENIED (secure default: a frontend
	 * that wires no policy runs no gated tools). req is valid only until it
	 * is responded to.
	 */
	void (*on_permission)(const struct clm_permission_req *req, void *user);

	/* A tool finished; content is its (possibly truncated) output, outcome
	 * tells the frontend whether it succeeded, failed, or timed out. */
	void (*on_tool_result)(const char *name, const char *content,
	    enum clm_tool_outcome outcome, void *user);

	/* Tool-batch progress, for a ui to render activity. Fired once when a
	 * batch of tool calls starts (completed == 0) and again after each call
	 * finishes, until completed == total. */
	void (*on_tool_batch)(size_t completed, size_t total, void *user);

	/* Why the most recent model response ended. Fired once per response
	 * (a tool turn reports CLM_FINISH_TOOL_CALLS; the final answer reports
	 * CLM_FINISH_STOP, or CLM_FINISH_LENGTH if truncated). */
	void (*on_finish_reason)(enum clm_finish_reason reason, void *user);

	/* Token usage for a model response, when the server reports it. */
	void (*on_usage)(const struct clm_usage *usage, void *user);

	/* Result of a connectivity probe (clm_agent_check_connection). Fires
	 * with CLM_CONN_CHECKING when a probe starts, then ONLINE or OFFLINE;
	 * detail is a short human string (NULL when ONLINE/CHECKING). */
	void (*on_connection)(enum clm_conn_status status, const char *detail,
	    void *user);

	/* Agent state changed (e.g. for a ui spinner/status line). */
	void (*on_state)(enum clm_agent_state state, void *user);

	/* The turn finished. status is 0 on success, negative errno on failure. */
	void (*on_turn_done)(int status, void *user);
};

/*
 * Create an agent bound to a host (transport + optional timers; see clm/host.h).
 *
 * The caller OWNS `host` and whatever it wraps (e.g. an event loop): the library
 * uses it but never tears it down. cb may be NULL (no events). user is passed
 * back to every callback. cfg and cb are copied/consumed at the call; host and
 * user must outlive the agent.
 */
CLM_API int clm_agent_new(const struct clm_cfg *cfg, struct clm_host *host,
    const struct clm_callbacks *cb, void *user, struct clm_agent **out);

CLM_API void clm_agent_free(struct clm_agent *agent);

/*
 * Install an optional compressor for history content (see clm/compress.h).
 * NULL (the default after clm_agent_new) stores/serializes history content
 * plain, exactly as before this existed. Intended for RAM-constrained
 * embedders (e.g. ESP32); desktop embedders have no reason to call this.
 * cz is not copied: it must outlive the agent, or until replaced/cleared by
 * another call. Call before submitting any turns -- switching compressors
 * mid-history is unsupported (content already stored under the old
 * compressor could not be read back).
 */
CLM_API void clm_agent_set_compressor(struct clm_agent *agent,
    const struct clm_compressor *cz);

/*
 * Submit a user turn. Returns immediately (0 on accepted, negative errno on
 * failure to enqueue). The turn runs as the loop is driven, emitting events
 * via the callbacks and ending with on_turn_done. Do not submit a new turn
 * until on_turn_done for the previous one has fired.
 */
CLM_API int clm_agent_submit(struct clm_agent *agent, const char *prompt);

/*
 * Deliver text from outside the normal turn flow -- e.g. a background job
 * (see clm_tools_register_bg in clm/host_uv.h) reporting its result well
 * after the tool call that started it already completed and returned. If the
 * agent is idle, this behaves exactly like clm_agent_submit(): text is added
 * as a user turn and a new turn starts immediately. If a turn is already in
 * flight (CLM_STATE_THINKING/CLM_STATE_CALLING_TOOL), text is queued instead
 * and folded into a single follow-up turn once the in-flight one lands via
 * on_turn_done -- multiple notifications arriving before that point are
 * coalesced into one turn (blank-line separated), not one turn each, so a
 * burst of background completions does not cascade into a burst of turns.
 * Returns 0 on success (submitted or queued), negative errno on failure
 * (e.g. -ENOMEM; the notification is dropped in that case).
 */
CLM_API int clm_agent_notify(struct clm_agent *agent, const char *text);

CLM_API enum clm_agent_state clm_agent_get_state(const struct clm_agent *agent);
CLM_API int64_t clm_agent_get_ctx_max(const struct clm_agent *agent);
CLM_API const char *clm_agent_get_last_error(const struct clm_agent *agent);

/*
 * True and clears the flag if a mid-chain autocompact (triggered internally
 * by clm_agent_tools_done() between tool batches, not by an explicit
 * clm_agent_compact() call) failed since the last time this was called.
 * There is no on_turn_done/on_state event dedicated to this -- the whole
 * point of a mid-chain compaction is that the tool chain it interrupted
 * resumes silently rather than ending the turn -- so a UI that wants to
 * surface "autocompact just failed, continuing anyway" (e.g. on the next
 * cb_on_state callback) should poll this rather than expecting a callback.
 * clm_agent_get_last_error() holds the actual error message when this
 * returns true.
 */
CLM_API bool clm_agent_take_mid_chain_compact_error(struct clm_agent *agent);

/*
 * True (once) when a mid-chain autocompact has just started. The UI should
 * print a "compacting..." message so the user knows the pause is intentional.
 * Consuming (clears on read, same pattern as _error above).
 */
CLM_API bool clm_agent_take_mid_chain_compact_started(struct clm_agent *agent);

/*
 * True (once) when a mid-chain autocompact has just completed successfully
 * and the interrupted chain is about to resume. Lets the UI print a
 * completion message. Consuming (clears on read).
 */
CLM_API bool clm_agent_take_mid_chain_compact_succeeded(struct clm_agent *agent);

/*
 * True if the agent's last known context usage is at/above the autocompact
 * threshold (a fixed percentage of ctx_max when known, else a fixed
 * absolute token count -- see the definition in agent.c for the exact calc
 * and rationale). clm_agent_tools_done() already checks this itself
 * in-between tool batches to trigger compaction mid-chain; exposed here too
 * so a frontend (e.g. tui.c's status bar) can reflect the same threshold
 * without keeping its own separate copy of the calc.
 */
CLM_API bool clm_agent_over_autocompact_threshold(const struct clm_agent *agent);

/*
 * Probe the API endpoint for reachability (an async GET to its /v1/models).
 * Returns 0 once the probe is enqueued, negative errno on failure to start.
 * The result arrives via the on_connection callback. Safe to call anytime,
 * including while a turn is in flight.
 */
CLM_API int clm_agent_check_connection(struct clm_agent *agent);

/*
 * Cancel the turn in flight: aborts the model request (or running tools) and
 * ends the turn via on_turn_done with status -ECANCELED. Returns 0 if a turn
 * was cancelled, negative errno if nothing was in flight. Safe to call from a
 * callback (e.g. a key handler).
 */
CLM_API int clm_agent_cancel(struct clm_agent *agent);

/*
 * Reconfigure the LLM provider/model on a live agent: swaps the endpoint,
 * API key, wire dialect, model, and per-model context/rate-limit overrides,
 * without tearing down history, tools, or MCP clients (contrast a full
 * clm_agent_free + clm_agent_new, needed only when the system prompt or
 * tool set also changes -- see clm-config(5)'s agents vs. models split).
 * Safe to call between turns; returns -EBUSY if one is in flight.
 *
 * Only cfg->base_url, api_key, provider, model, context_size,
 * autocompact_pct, rate_tokens_per_sec, and rate_burst are read; the rest
 * of *cfg (system_prompt, tools, max_iterations, ...) is ignored, since
 * this only ever changes the connection, not the agent's behaviour.
 * cfg->base_url is the full chat completions URL (e.g.
 * "http://host/v1/chat/completions"). cfg->api_key may be NULL for
 * no-auth servers. context_size/autocompact_pct/rate_tokens_per_sec/
 * rate_burst of 0 leave the library default in place, same as at
 * clm_agent_new time.
 */
CLM_API int clm_agent_set_provider(struct clm_agent *agent,
    const struct clm_cfg *cfg);

/*
 * Summarize the conversation and fold old turns into that summary, keeping the
 * system prologue and recent turns. Asynchronous (one model round-trip): fires
 * on_turn_done when finished (unless triggered mid-chain from inside
 * clm_agent_tools_done(), which resumes the interrupted tool chain instead).
 */
CLM_API int clm_agent_compact(struct clm_agent *agent);

/*
 * Answer a permission request from the on_permission callback. Resumes the
 * parked tool invocation: an ALLOW decision runs it, a DENY completes it with
 * a "denied" result the model can see. The _ALWAYS variants also remember the
 * choice for this tool for the rest of the session. May be called from within
 * on_permission (synchronous) or later (async). Returns 0, or negative errno.
 */
CLM_API int clm_tool_permission_respond(struct clm_agent *agent,
    const struct clm_permission_req *req, enum clm_permission_decision decision);

/*
 * Register a tool. The agent copies def and its strings. The same name may
 * not be registered twice. Returns 0, or negative errno (-EEXIST on a
 * duplicate name, -EINVAL on a malformed def).
 */
CLM_API int clm_tool_add(struct clm_agent *agent, const struct clm_tool_def *def);

/*
 * Unregister a tool by name. Safe to call at any time, including with an
 * invocation of that tool still in flight (e.g. awaiting a permission
 * decision, or a slow async call): the tool stops being dispatched to and
 * disappears from the schema advertised to the model immediately, but its
 * bookkeeping is only freed once any in-flight invocation finishes. Returns
 * 0, or -ENOENT if no tool is registered under that name.
 */
CLM_API int clm_tool_remove(struct clm_agent *agent, const char *name);

/* --- accessors usable from inside a tool's invoke fn --- */
CLM_API const char *clm_tool_invocation_name(const struct clm_tool_invocation *inv);
CLM_API const char *clm_tool_invocation_args(const struct clm_tool_invocation *inv);
/*
 * The host's native loop/context (clm_host.ctx) for tools that need to reach
 * the underlying platform — e.g. a subprocess tool casting it back to its
 * uv_loop_t*. Returns NULL if the host exposes no context. Most tools ignore it.
 */
CLM_API void *clm_tool_invocation_loop(const struct clm_tool_invocation *inv);
CLM_API size_t clm_tool_invocation_output_cap(const struct clm_tool_invocation *inv);
CLM_API uint64_t clm_tool_invocation_timeout_ms(const struct clm_tool_invocation *inv);

/*
 * Optional: register a cancel callback the framework calls if the invocation
 * times out, so the tool can abort in-flight work (e.g. kill a subprocess).
 * The tool must still report completion afterwards. Call from invoke().
 */
CLM_API void clm_tool_invocation_set_cancel(struct clm_tool_invocation *inv,
    void (*cancel)(struct clm_tool_invocation *inv, void *user), void *user);

/*
 * Report a tool result. Exactly one of these per invocation. content/msg are
 * copied. clm_tool_fail records the result as "[tool failed: <msg>]" so the
 * model sees the error. content is clamped to the invocation's output cap.
 */
CLM_API void clm_tool_complete(struct clm_tool_invocation *inv, const char *content);
CLM_API void clm_tool_fail(struct clm_tool_invocation *inv, const char *msg);

CLM_API void clm_agent_free_ptr(struct clm_agent **agent);
#define _cleanup_clm_ __attribute__((cleanup(clm_agent_free_ptr)))

#endif /* CLM_H */
