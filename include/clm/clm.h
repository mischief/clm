// SPDX-License-Identifier: ISC
#ifndef CLM_H
#define CLM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "clm/clm_export.h"

struct clm_agent;
struct clm_cfg;

/* LLM provider types. */
enum clm_provider {
	CLM_PROVIDER_OPENAI,
	CLM_PROVIDER_OLLAMA,
	CLM_PROVIDER_ANTHROPIC,
};

/* Outcome of a tool call, reported to on_tool_result. */
enum clm_tool_outcome {
	CLM_TOOL_OK,
	CLM_TOOL_FAILED,
	CLM_TOOL_TIMEDOUT,
};

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
	const char *model;
	size_t max_iterations;
	bool stream;              /* request streamed (SSE) responses */
	const char *system_prompt; /* system message; NULL uses a default */
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
 * Create an agent bound to the caller's event loop.
 *
 * The caller OWNS `loop`: the library registers handles on it but never calls
 * uv_run() or uv_loop_close(). cb may be NULL (no events). user is passed back
 * to every callback. cfg and cb are copied/consumed at the call; loop and user
 * must outlive the agent.
 */
CLM_API int clm_agent_new(const struct clm_cfg *cfg, uv_loop_t *loop,
    const struct clm_callbacks *cb, void *user, struct clm_agent **out);

CLM_API void clm_agent_free(struct clm_agent *agent);

/*
 * Submit a user turn. Returns immediately (0 on accepted, negative errno on
 * failure to enqueue). The turn runs as the loop is driven, emitting events
 * via the callbacks and ending with on_turn_done. Do not submit a new turn
 * until on_turn_done for the previous one has fired.
 */
CLM_API int clm_agent_submit(struct clm_agent *agent, const char *prompt);

CLM_API enum clm_agent_state clm_agent_get_state(const struct clm_agent *agent);
CLM_API const char *clm_agent_get_last_error(const struct clm_agent *agent);

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
 * Register a tool. The agent copies def and its strings. The same name may
 * not be registered twice. Returns 0, or negative errno (-EEXIST on a
 * duplicate name, -EINVAL on a malformed def).
 */
CLM_API int clm_tool_add(struct clm_agent *agent, const struct clm_tool_def *def);

/* --- accessors usable from inside a tool's invoke fn --- */
CLM_API const char *clm_tool_invocation_name(const struct clm_tool_invocation *inv);
CLM_API const char *clm_tool_invocation_args(const struct clm_tool_invocation *inv);
CLM_API uv_loop_t *clm_tool_invocation_loop(const struct clm_tool_invocation *inv);
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
