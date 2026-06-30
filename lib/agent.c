// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "clm/agent.h"
#include "clm/http.h"
#include "clm/llm.h"
#include "clm/tools.h"
#include "clm/history.h"
#include "clm/internal.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

static const char *system_prompt =
    "You are a coding agent that can read files, write files, and execute shell commands. "
    "Use the provided tools when you need to act. When you have the answer, reply directly.";

void
clm_agent_set_error(struct clm_agent *agent, const char *msg)
{
	char *dup = msg ? strdup(msg) : NULL;
	free(agent->last_error);
	agent->last_error = dup;
}

int
clm_tool_register(struct clm_agent *agent, enum clm_tool_type type, const char *name)
{
	struct clm_tool *tools;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(name != NULL, -EINVAL);

	tools = realloc(agent->tools, (agent->tool_count + 1) * sizeof(*tools));
	if (tools == NULL)
		return -ENOMEM;
	agent->tools = tools;

	agent->tools[agent->tool_count].type = type;
	agent->tools[agent->tool_count].name = strdup(name);
	if (agent->tools[agent->tool_count].name == NULL)
		return -ENOMEM;

	agent->tool_count++;
	return 0;
}

int
clm_agent_new(const struct clm_cfg *cfg, uv_loop_t *uv, const struct clm_callbacks *cb, void *user, struct clm_agent **out)
{
	struct clm_agent *agent;
	int r;

	ASSERT_RETURN(out != NULL, -EINVAL);
	ASSERT_RETURN(cfg != NULL, -EINVAL);
	ASSERT_RETURN(cfg->api_key != NULL, -EINVAL);
	ASSERT_RETURN(cfg->base_url != NULL, -EINVAL);
	ASSERT_RETURN(uv != NULL, -EINVAL);

	agent = calloc(1, sizeof(*agent));
	if (agent == NULL)
		return -ENOMEM;

	agent->uv = uv;
	agent->state = CLM_STATE_IDLE;
	agent->max_iterations = cfg->max_iterations ? cfg->max_iterations : CLM_DEFAULT_MAX_ITERATIONS;
	clm_history_init(&agent->history);

	if (cb != NULL) {
		agent->cb_on_assistant_text = cb->on_assistant_text;
		agent->cb_on_reasoning = cb->on_reasoning;
		agent->cb_on_tool_begin = cb->on_tool_begin;
		agent->cb_on_tool_result = cb->on_tool_result;
		agent->cb_on_state = cb->on_state;
		agent->cb_on_turn_done = cb->on_turn_done;
	}
	agent->cb_user = user;

	r = clm_llm_new(&agent->llm, cfg->provider, cfg->api_key, cfg->base_url,
	    cfg->model ? cfg->model : "local-model");
	if (r < 0) {
		free(agent);
		return r;
	}

	if (clm_history_add_system(&agent->history, system_prompt) == NULL) {
		clm_agent_free(agent);
		return -ENOMEM;
	}

	clm_tool_register(agent, CLM_TOOL_FILE_READ, "read_file");
	clm_tool_register(agent, CLM_TOOL_FILE_WRITE, "write_file");
	clm_tool_register(agent, CLM_TOOL_SHELL_EXEC, "shell_exec");

	*out = agent;
	return 0;
}

void
clm_agent_free(struct clm_agent *agent)
{
	size_t i;

	if (agent == NULL)
		return;

	clm_llm_free(agent->llm);
	clm_history_free(&agent->history);
	free(agent->last_error);
	for (i = 0; i < agent->tool_count; i++)
		free(agent->tools[i].name);
	free(agent->tools);
	free(agent);
}

void
clm_agent_free_ptr(struct clm_agent **agent)
{
	if (agent && *agent) {
		clm_agent_free(*agent);
		*agent = NULL;
	}
}

enum clm_agent_state
clm_agent_get_state(const struct clm_agent *agent)
{
	return agent ? agent->state : CLM_STATE_ERROR;
}

const char *
clm_agent_get_last_error(const struct clm_agent *agent)
{
	if (agent == NULL || agent->last_error == NULL)
		return "";
	return agent->last_error;
}

/*
 * Reach into a parsed completion response and return choices[0].message,
 * or NULL. The returned object is borrowed from parsed.
 */
static struct json_object *
response_message(struct json_object *parsed)
{
	struct json_object *choices = NULL, *choice0 = NULL, *message = NULL;

	if (!json_object_object_get_ex(parsed, "choices", &choices))
		return NULL;
	if (json_object_get_type(choices) != json_type_array)
		return NULL;
	choice0 = json_object_array_get_idx(choices, 0);
	if (choice0 == NULL)
		return NULL;
	if (!json_object_object_get_ex(choice0, "message", &message))
		return NULL;
	return message;
}

/*
 * Record the assistant's tool-call message into history and dispatch each
 * call, appending one tool-result message per call id. Returns the number of
 * tool calls handled, or negative errno.
 */
static int
handle_tool_calls(struct clm_agent *agent, struct json_object *tool_calls)
{
	struct clm_message *assistant_msg;
	size_t n, i;

	assistant_msg = clm_history_add_assistant_tool_calls(&agent->history);
	if (assistant_msg == NULL)
		return -ENOMEM;

	n = json_object_array_length(tool_calls);

	/* First record every call onto the assistant message. */
	for (i = 0; i < n; i++) {
		struct json_object *tc = json_object_array_get_idx(tool_calls, i);
		struct json_object *id = NULL, *func = NULL, *name = NULL, *args = NULL;
		const char *args_str;

		if (tc == NULL)
			continue;
		json_object_object_get_ex(tc, "id", &id);
		json_object_object_get_ex(tc, "function", &func);
		if (func == NULL)
			continue;
		json_object_object_get_ex(func, "name", &name);
		json_object_object_get_ex(func, "arguments", &args);
		if (name == NULL)
			continue;

		/* arguments may be a JSON string or an object; normalize to string. */
		if (args != NULL && json_object_get_type(args) == json_type_string)
			args_str = json_object_get_string(args);
		else if (args != NULL)
			args_str = json_object_to_json_string_ext(args, JSON_C_TO_STRING_PLAIN);
		else
			args_str = "{}";

		if (clm_message_add_tool_call(assistant_msg,
		    id ? json_object_get_string(id) : "",
		    json_object_get_string(name), args_str) == NULL)
			return -ENOMEM;
	}

	/* Then execute each recorded call, appending one tool result per id. */
	{
		struct clm_tool_call *tc;
		TAILQ_FOREACH(tc, &assistant_msg->tool_calls, entries) {
			struct clm_tool_result *res = NULL;
			int r;

			printf("[tool: %s %s]\n", tc->name, tc->args ? tc->args : "");
			fflush(stdout);

			r = clm_tool_execute(agent, tc, &res);
			if (r < 0)
				return r;

			if (clm_history_add_tool_result(&agent->history, tc->id, res->content) == NULL) {
				clm_tool_result_free(res);
				return -ENOMEM;
			}
			clm_tool_result_free(res);
		}
	}

	return (int)n;
}

int
clm_agent_run(struct clm_agent *agent, const char *prompt, char **result)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(prompt != NULL, -EINVAL);
	ASSERT_RETURN(result != NULL, -EINVAL);

	*result = NULL;
	agent->state = CLM_STATE_THINKING;

	if (clm_history_add_user(&agent->history, prompt) == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		return -ENOMEM;
	}

	for (agent->iteration = 0; agent->iteration < agent->max_iterations; agent->iteration++) {
		struct clm_http_response resp = {0};
		json_cleanup struct json_object *messages = NULL;
		json_cleanup struct json_object *tools = NULL;
		json_cleanup struct json_object *parsed = NULL;
		struct json_object *message, *content = NULL, *tool_calls = NULL;
		int r;

		messages = clm_history_to_json(&agent->history);
		tools = clm_tools_build_schema(agent);
		if (messages == NULL || tools == NULL) {
			clm_agent_set_error(agent, "out of memory");
			agent->state = CLM_STATE_ERROR;
			return -ENOMEM;
		}

		r = clm_llm_chat(agent->llm, messages, tools, &resp);
		if (r < 0) {
			clm_agent_set_error(agent, resp.error_msg ? resp.error_msg : "llm request failed");
			clm_http_response_free(&resp);
			agent->state = CLM_STATE_ERROR;
			return r;
		}

		parsed = resp.body ? json_tokener_parse(resp.body) : NULL;
		clm_http_response_free(&resp);
		if (parsed == NULL) {
			clm_agent_set_error(agent, "could not parse llm response");
			agent->state = CLM_STATE_ERROR;
			return -EIO;
		}

		message = response_message(parsed);
		if (message == NULL) {
			clm_agent_set_error(agent, "llm response had no message");
			agent->state = CLM_STATE_ERROR;
			return -EIO;
		}

		json_object_object_get_ex(message, "tool_calls", &tool_calls);
		if (tool_calls != NULL && json_object_get_type(tool_calls) == json_type_array
		    && json_object_array_length(tool_calls) > 0) {
			agent->state = CLM_STATE_CALLING_TOOL;
			r = handle_tool_calls(agent, tool_calls);
			if (r < 0) {
				clm_agent_set_error(agent, "out of memory handling tool calls");
				agent->state = CLM_STATE_ERROR;
				return r;
			}
			continue; /* feed results back to the model */
		}

		/* No tool calls: this is the final answer. */
		json_object_object_get_ex(message, "content", &content);
		{
			const char *text = content ? json_object_get_string(content) : "";
			if (clm_history_add_assistant_text(&agent->history, text) == NULL) {
				clm_agent_set_error(agent, "out of memory");
				agent->state = CLM_STATE_ERROR;
				return -ENOMEM;
			}
			*result = strdup(text ? text : "");
			if (*result == NULL) {
				agent->state = CLM_STATE_ERROR;
				return -ENOMEM;
			}
		}
		agent->state = CLM_STATE_COMPLETE;
		return 0;
	}

	clm_agent_set_error(agent, "max iterations reached");
	agent->state = CLM_STATE_ERROR;
	return -E2BIG;
}
