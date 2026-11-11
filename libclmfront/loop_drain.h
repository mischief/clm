// SPDX-License-Identifier: ISC
/*
 * Shared by main.c and tui.c: settle the loop after teardown. Every uv-backed
 * object in clm frees itself from a uv_close callback, so a frontend that
 * stops running the loop at teardown leaks all of them.
 */
#ifndef CLM_LOOP_DRAIN_H
#define CLM_LOOP_DRAIN_H

#include <stdbool.h>

#include <uv.h>

#include "clm/clm.h"

/* How long the drain waits for the last closes to land. Long enough for a
 * SIGKILL'd MCP server to be reaped, short enough to never feel like a hang.
 */
#define CLM_DRAIN_MS 500

/* Loop turns spent waiting for a cancelled turn to unwind. */
#define CLM_SETTLE_ITERATIONS 1000

static inline void
clm_drain_deadline(uv_timer_t *t)
{
	*(bool *)t->data = true;
}

/*
 * Cancel a turn still in flight and let the loop deliver the cancellation:
 * the turn belongs to its completion callback, so an exit that never runs
 * that callback leaks it. Bounded, so a stuck cancel cannot hang exit.
 */
static inline void
clm_settle_turn(struct clm_agent *agent, uv_loop_t *loop)
{
	int i;

	if (agent == NULL || loop == NULL || clm_agent_cancel(agent) != 0)
		return;
	for (i = 0; i < CLM_SETTLE_ITERATIONS; i++) {
		enum clm_agent_state st = clm_agent_get_state(agent);

		if (st != CLM_STATE_THINKING && st != CLM_STATE_CALLING_TOOL)
			break;
		uv_run(loop, UV_RUN_NOWAIT);
	}
}

/*
 * Run loop until nothing is left on it, or the deadline expires, then close
 * it. Call it after the last *_free on an exit path, with the frontend's own
 * handles already closed: a handle still active here keeps firing callbacks
 * into objects the teardown released. Returns 0 if every handle settled,
 * UV_EBUSY if one is still outstanding.
 */
static inline int
clm_drain_loop(uv_loop_t *loop)
{
	uv_timer_t deadline;
	bool expired = false;

	if (loop == NULL)
		return 0;

	uv_timer_init(loop, &deadline);
	deadline.data = &expired;
	uv_timer_start(&deadline, clm_drain_deadline, CLM_DRAIN_MS, 0);
	/* Unreferenced so the deadline alone never counts as "still busy". */
	uv_unref((uv_handle_t *)&deadline);

	while (!expired && uv_run(loop, UV_RUN_ONCE) != 0)
		;

	uv_close((uv_handle_t *)&deadline, NULL);
	(void)uv_run(loop, UV_RUN_NOWAIT);
	return uv_loop_close(loop);
}

#endif /* CLM_LOOP_DRAIN_H */
