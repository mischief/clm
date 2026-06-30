// SPDX-License-Identifier: ISC
#ifndef CLM_H
#define CLM_H

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

/* Tool types. */
enum clm_tool_type {
	CLM_TOOL_FILE_READ,
	CLM_TOOL_FILE_WRITE,
	CLM_TOOL_SHELL_EXEC,
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

	/* A tool finished; content is its (possibly truncated) output. */
	void (*on_tool_result)(const char *name, const char *content, void *user);

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

/*
 * Blocking convenience wrapper for simple/oneshot use. Runs its own private
 * loop until the turn completes and returns the final assistant text in
 * *result (caller frees). Do NOT mix with clm_agent_submit on the same agent.
 */
CLM_API int clm_agent_run(struct clm_agent *agent, const char *prompt, char **result);

CLM_API enum clm_agent_state clm_agent_get_state(const struct clm_agent *agent);
CLM_API const char *clm_agent_get_last_error(const struct clm_agent *agent);

CLM_API int clm_tool_register(struct clm_agent *agent, enum clm_tool_type type, const char *name);

CLM_API void clm_agent_free_ptr(struct clm_agent **agent);
#define _cleanup_clm_ __attribute__((cleanup(clm_agent_free_ptr)))

#endif /* CLM_H */
