// SPDX-License-Identifier: ISC
/* Internal definitions -- not installed. */
#ifndef CLM_INTERNAL_H
#define CLM_INTERNAL_H

#include <stddef.h>

#include "clm/clm.h"
#include "clm/history.h"
#include "clm/llm.h"
#include "clm/tools.h"
#include "clm/cleanup.h"
#include "useful.h"

/* Default maximum agent loop iterations when cfg->max_iterations is 0. */
#define CLM_DEFAULT_MAX_ITERATIONS 25

struct clm_agent {
	struct clm_llm *llm;
	uv_loop_t *uv;
	enum clm_agent_state state;
	char *last_error;
	struct clm_history history;
	struct clm_tool *tools;
	size_t tool_count;
	size_t max_iterations;
	size_t iteration;

	/* Event callbacks */
	void (*cb_on_assistant_text)(const char *, void *);
	void (*cb_on_reasoning)(const char *, void *);
	void (*cb_on_tool_begin)(const char *, const char *, void *);
	void (*cb_on_tool_result)(const char *, const char *, void *);
	void (*cb_on_state)(enum clm_agent_state, void *);
	void (*cb_on_turn_done)(int, void *);
	void *cb_user;
};

/* Set agent->last_error to a copy of msg (replacing any previous error). */
void clm_agent_set_error(struct clm_agent *agent, const char *msg);

#endif /* CLM_INTERNAL_H */
