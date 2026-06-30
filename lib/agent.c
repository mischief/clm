// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "clm/agent.h"
#include "clm/http.h"
#include "clm/http_async.h"
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

static void clm_agent_start_turn(struct clm_agent *agent);

int
clm_agent_submit(struct clm_agent *agent, const char *prompt)
{
	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(prompt != NULL, -EINVAL);

	if (agent->state != CLM_STATE_IDLE && agent->state != CLM_STATE_COMPLETE) {
		clm_agent_set_error(agent, "turn already in progress");
		return -EBUSY;
	}

	if (clm_history_add_user(&agent->history, prompt) == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		return -ENOMEM;
	}

	agent->state = CLM_STATE_THINKING;

	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);

	clm_agent_start_turn(agent);

	return 0;
}

struct clm_async_turn {
	struct clm_agent *agent;
	struct json_object *messages;
	struct json_object *tools;
	struct json_object *parsed;
};

static void
clm_async_turn_free(struct clm_async_turn *turn)
{
	if (turn) {
		free(turn);
	}
}

static int handle_tool_calls(struct clm_agent *agent, struct json_object *tool_calls);
static struct json_object *response_message(struct json_object *parsed);

static void
clm_http_success_cb_wrapper(struct clm_http_response *resp, void *user)
{
	struct clm_async_turn *turn = (struct clm_async_turn *)user;
	struct clm_agent *agent = turn->agent;
	struct json_object *message, *content = NULL, *tool_calls = NULL;
	struct json_object *finish_reason_obj = NULL;
	const char *finish_reason = NULL;
	int r;

	turn->parsed = resp && resp->body ? json_tokener_parse(resp->body) : NULL;
	if (resp)
		clm_http_response_free(resp);

	if (turn->parsed == NULL) {
		clm_agent_set_error(agent, "could not parse llm response");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		clm_async_turn_free(turn);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-EIO, agent->cb_user);
		return;
	}

	message = response_message(turn->parsed);
	if (message == NULL) {
		clm_agent_set_error(agent, "could not extract message from llm response");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		clm_async_turn_free(turn);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-EIO, agent->cb_user);
		return;
	}

	json_object_object_get_ex(message, "content", &content);
	json_object_object_get_ex(message, "finish_reason", &finish_reason_obj);
	if (finish_reason_obj)
		finish_reason = json_object_get_string(finish_reason_obj);

	if (finish_reason && strcmp(finish_reason, "tool_calls") == 0) {
		json_object_object_get_ex(message, "tool_calls", &tool_calls);
		if (tool_calls) {
			agent->state = CLM_STATE_CALLING_TOOL;
			if (agent->cb_on_state)
				agent->cb_on_state(agent->state, agent->cb_user);

			r = handle_tool_calls(agent, tool_calls);
			if (r < 0) {
				clm_agent_set_error(agent, "tool execution failed");
				agent->state = CLM_STATE_ERROR;
				if (agent->cb_on_state)
					agent->cb_on_state(agent->state, agent->cb_user);
				clm_async_turn_free(turn);
				if (agent->cb_on_turn_done)
					agent->cb_on_turn_done(r, agent->cb_user);
				return;
			}

			agent->state = CLM_STATE_THINKING;
			if (agent->cb_on_state)
				agent->cb_on_state(agent->state, agent->cb_user);

			clm_agent_start_turn(agent);
		} else {
			clm_agent_set_error(agent, "tool_calls not found in response");
			agent->state = CLM_STATE_ERROR;
			if (agent->cb_on_state)
				agent->cb_on_state(agent->state, agent->cb_user);
			clm_async_turn_free(turn);
			if (agent->cb_on_turn_done)
				agent->cb_on_turn_done(-EIO, agent->cb_user);
		}
	} else {
		if (content) {
			const char *text = json_object_get_string(content);
			if (agent->cb_on_assistant_text)
				agent->cb_on_assistant_text(text, agent->cb_user);
		}

		agent->state = CLM_STATE_COMPLETE;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);

		clm_async_turn_free(turn);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(0, agent->cb_user);
	}
}

static void
clm_http_error_cb_wrapper(int error_code, const char *error_msg, void *user)
{
	struct clm_async_turn *turn = (struct clm_async_turn *)user;
	struct clm_agent *agent = turn->agent;

	clm_agent_set_error(agent, error_msg ? error_msg : "http request failed");
	agent->state = CLM_STATE_ERROR;
	if (agent->cb_on_state)
		agent->cb_on_state(agent->state, agent->cb_user);
	clm_async_turn_free(turn);
	if (agent->cb_on_turn_done)
		agent->cb_on_turn_done(error_code, agent->cb_user);
}

static void
clm_agent_start_turn(struct clm_agent *agent)
{
	json_cleanup struct json_object *req = NULL;
	json_cleanup struct json_object *messages = NULL;
	json_cleanup struct json_object *tools = NULL;
	json_cleanup struct json_object *jmodel = NULL;
	json_cleanup struct json_object *jstream = NULL;
	struct clm_async_turn *turn;
	const char *body;

	messages = clm_history_to_json(&agent->history);
	tools = clm_tools_build_schema(agent);
	if (messages == NULL || tools == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	req = json_object_new_object();
	if (req == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	jmodel = json_object_new_string(agent->llm->model);
	if (jmodel == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}
	json_object_object_add(req, "model", jmodel);

	json_object_object_add(req, "messages", json_object_get(messages));

	jstream = json_object_new_boolean(0);
	if (jstream == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}
	json_object_object_add(req, "stream", jstream);

	json_object_object_add(req, "tools", json_object_get(tools));

	body = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
	if (body == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	turn = malloc(sizeof(struct clm_async_turn));
	if (turn == NULL) {
		clm_agent_set_error(agent, "out of memory");
		agent->state = CLM_STATE_ERROR;
		if (agent->cb_on_state)
			agent->cb_on_state(agent->state, agent->cb_user);
		if (agent->cb_on_turn_done)
			agent->cb_on_turn_done(-ENOMEM, agent->cb_user);
		return;
	}

	turn->agent = agent;
	turn->messages = messages;
	turn->tools = tools;
	turn->parsed = NULL;

	clm_http_async_post(agent->uv, agent->llm->base_url, agent->llm->api_key,
			    body, clm_http_success_cb_wrapper, clm_http_error_cb_wrapper, turn);
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
