// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <json-c/json.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/internal.h"
#include "clm/history.h"
#include "canned.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                         \
			failures++;                                            \
		}                                                              \
	} while (0)

struct tstate {
	uv_loop_t *loop;
	struct clm_agent *agent;
	int stream;
	const char *system_prompt;
	int turn_done;
	int turn_status;
	size_t asst_len;
	char assistant[256];
	int tool_results;
	enum clm_tool_outcome last_outcome;
	char tool_content[256];
	size_t reason_len;
	char reasoning[128];
	int got_finish;
	enum clm_finish_reason finish;
	int got_usage;
	struct clm_usage usage;
};

static void
on_assistant_text(const char *text, void *user)
{
	struct tstate *st = user;
	int n = snprintf(st->assistant + st->asst_len,
	    sizeof(st->assistant) - st->asst_len, "%s", text ? text : "");
	if (n > 0)
		st->asst_len += (size_t)n;
}

static void
on_tool_result(const char *name, const char *content, enum clm_tool_outcome outcome, void *user)
{
	struct tstate *st = user;
	(void)name;
	st->tool_results++;
	st->last_outcome = outcome;
	(void)snprintf(st->tool_content, sizeof(st->tool_content), "%s",
	    content ? content : "");
}

/*
 * Queue a tool-call assistant reply, built with json-c so the nested
 * "arguments" string (JSON encoded inside JSON) is escaped correctly.
 * args_json is the tool's raw arguments object, e.g. {"path":"/x"}.
 */
static void
canned_tool_call(struct canned_server *srv, const char *name, const char *args_json)
{
	struct json_object *root = json_object_new_object();
	struct json_object *choices = json_object_new_array();
	struct json_object *choice = json_object_new_object();
	struct json_object *message = json_object_new_object();
	struct json_object *calls = json_object_new_array();
	struct json_object *call = json_object_new_object();
	struct json_object *func = json_object_new_object();

	json_object_object_add(func, "name", json_object_new_string(name));
	json_object_object_add(func, "arguments", json_object_new_string(args_json));
	json_object_object_add(call, "id", json_object_new_string("c1"));
	json_object_object_add(call, "type", json_object_new_string("function"));
	json_object_object_add(call, "function", func);
	json_object_array_add(calls, call);
	json_object_object_add(message, "role", json_object_new_string("assistant"));
	json_object_object_add(message, "content", json_object_new_string(""));
	json_object_object_add(message, "tool_calls", calls);
	json_object_object_add(choice, "finish_reason", json_object_new_string("tool_calls"));
	json_object_object_add(choice, "message", message);
	json_object_array_add(choices, choice);
	json_object_object_add(root, "choices", choices);

	canned_reply(srv, json_object_to_json_string(root));
	json_object_put(root);
}

static const char *final_reply =
    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
    "\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}";

static void
on_reasoning(const char *text, void *user)
{
	struct tstate *st = user;
	int n = snprintf(st->reasoning + st->reason_len,
	    sizeof(st->reasoning) - st->reason_len, "%s", text ? text : "");
	if (n > 0)
		st->reason_len += (size_t)n;
}

static void
on_finish_reason(enum clm_finish_reason reason, void *user)
{
	struct tstate *st = user;
	st->got_finish = 1;
	st->finish = reason;
}

static void
on_usage(const struct clm_usage *usage, void *user)
{
	struct tstate *st = user;
	st->got_usage = 1;
	st->usage = *usage;
}

static void
on_turn_done(int status, void *user)
{
	struct tstate *st = user;
	st->turn_done = 1;
	st->turn_status = status;
}

static const struct clm_callbacks callbacks = {
	.on_assistant_text = on_assistant_text,
	.on_reasoning = on_reasoning,
	.on_tool_result = on_tool_result,
	.on_finish_reason = on_finish_reason,
	.on_usage = on_usage,
	.on_turn_done = on_turn_done,
};

/* A mock tool that completes synchronously with a fixed string. */
static void
echo_hello(struct clm_tool_invocation *inv, void *user)
{
	(void)user;
	clm_tool_complete(inv, "hello");
}

static struct clm_agent *
make_agent(struct tstate *st, int port)
{
	char url[128];
	struct clm_agent *agent = NULL;
	struct clm_cfg cfg = {0};
	int r;

	(void)snprintf(url, sizeof(url),
	    "http://127.0.0.1:%d/v1/chat/completions", port);
	cfg.api_key = "test";
	cfg.base_url = url;
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = "test-model";
	cfg.stream = st->stream;
	cfg.system_prompt = st->system_prompt;

	r = clm_agent_new(&cfg, st->loop, &callbacks, st, &agent);
	CHECK(r == 0, "clm_agent_new");
	return agent;
}

static void
run_until_done(struct tstate *st)
{
	while (!st->turn_done)
		uv_run(st->loop, UV_RUN_ONCE);
}

static void
teardown(struct tstate *st, struct canned_server *srv)
{
	clm_agent_free(st->agent);
	canned_stop(srv);
	uv_run(st->loop, UV_RUN_DEFAULT); /* drain pending closes */
}

/* (a) A plain text prompt and a canned assistant reply. */
static void
test_text_reply(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	st.system_prompt = "SENTINEL_SYS_PROMPT";
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"hi there\"}}]}");

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "hello") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "text turn ok");
	CHECK(strstr(st.assistant, "hi there") != NULL, "assistant text delivered");
	CHECK(canned_request_count(srv) == 1, "one request");
	CHECK(canned_last_request(srv) != NULL &&
	    strstr(canned_last_request(srv), "SENTINEL_SYS_PROMPT") != NULL,
	    "custom system prompt sent");

	teardown(&st, srv);
}

/* (b) A prompt whose reply is a tool call, then a final answer. */
static void
test_tool_call(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	/* Turn 1: the model requests the echo tool. Turn 2: final answer. */
	canned_tool_call(srv, "echo_hello", "{}");
	canned_reply(srv, final_reply);

	st.agent = make_agent(&st, canned_port(srv));

	def.name = "echo_hello";
	def.description = "echo hello";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "clm_tool_add");

	CHECK(clm_agent_submit(st.agent, "use the tool") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "tool turn ok");
	CHECK(st.tool_results == 1, "one tool result");
	CHECK(st.last_outcome == CLM_TOOL_OK, "tool outcome ok");
	CHECK(strstr(st.assistant, "done") != NULL, "final answer delivered");
	CHECK(canned_request_count(srv) == 2, "two requests (call + result)");
	/* The tool result must be fed back to the model on the second request. */
	CHECK(canned_last_request(srv) != NULL &&
	    strstr(canned_last_request(srv), "hello") != NULL,
	    "tool result echoed back to model");

	teardown(&st, srv);
}

/* (c) Real write_file then read_file builtins against a temp sandbox. */
static void
test_file_tools(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	char args[256];
	char dir[] = "/tmp/clm_test_XXXXXX";
	char path[128];
	char disk[64] = {0};
	FILE *f;

	CHECK(mkdtemp(dir) != NULL, "mkdtemp");
	(void)snprintf(path, sizeof(path), "%s/greeting.txt", dir);

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	(void)snprintf(args, sizeof(args),
	    "{\"path\":\"%s\",\"content\":\"hello\"}", path);
	canned_tool_call(srv, "write_file", args);

	(void)snprintf(args, sizeof(args), "{\"path\":\"%s\"}", path);
	canned_tool_call(srv, "read_file", args);

	canned_reply(srv, final_reply);

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "write then read") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "file turn ok");
	CHECK(st.tool_results == 2, "two tool results");
	CHECK(st.last_outcome == CLM_TOOL_OK, "read outcome ok");

	f = fopen(path, "r");
	CHECK(f != NULL, "written file exists");
	if (f != NULL) {
		size_t n = fread(disk, 1, sizeof(disk) - 1, f);
		disk[n] = '\0';
		fclose(f);
	}
	CHECK(strstr(disk, "hello") != NULL, "file content written to disk");
	CHECK(strstr(st.tool_content, "hello") != NULL, "read_file returned content");

	teardown(&st, srv);
	(void)unlink(path);
	(void)rmdir(dir);
}

/* (d) Real shell_exec builtin via uv_spawn. */
static void
test_shell_exec(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_tool_call(srv, "shell_exec", "{\"command\":\"echo hello\"}");
	canned_reply(srv, final_reply);

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "echo hello") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "shell turn ok");
	CHECK(st.tool_results == 1, "one tool result");
	CHECK(st.last_outcome == CLM_TOOL_OK, "shell outcome ok");
	CHECK(strstr(st.tool_content, "hello") != NULL, "shell output captured");

	teardown(&st, srv);
}

/* Queue an SSE text reply split across two content deltas. */
static void
canned_stream_text(struct canned_server *srv, const char *a, const char *b)
{
	char body[1024];
	(void)snprintf(body, sizeof(body),
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
	    "data: [DONE]\n\n", a, b);
	canned_reply(srv, body);
}

/* Queue an SSE tool-call reply with the name and arguments in separate deltas. */
static void
canned_stream_tool(struct canned_server *srv, const char *name)
{
	char body[1024];
	(void)snprintf(body, sizeof(body),
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
	    "\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"%s\","
	    "\"arguments\":\"\"}}]}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
	    "\"function\":{\"arguments\":\"{}\"}}]}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
	    "data: [DONE]\n\n", name);
	canned_reply(srv, body);
}

/* (e) Streamed text reply, assembled from deltas. */
static void
test_stream_text(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	st.stream = 1;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_stream_text(srv, "hi ", "there");

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "hello") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "stream text ok");
	CHECK(strstr(st.assistant, "hi there") != NULL, "deltas assembled");
	CHECK(canned_request_count(srv) == 1, "one request");

	teardown(&st, srv);
}

/* (f) Streamed tool call (name + args in separate deltas), then a final answer. */
static void
test_stream_tool(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	st.stream = 1;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_stream_tool(srv, "echo_hello");
	canned_stream_text(srv, "all ", "done");

	st.agent = make_agent(&st, canned_port(srv));
	def.name = "echo_hello";
	def.description = "echo hello";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "clm_tool_add");

	CHECK(clm_agent_submit(st.agent, "use the tool") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "stream tool ok");
	CHECK(st.tool_results == 1, "one tool result");
	CHECK(st.last_outcome == CLM_TOOL_OK, "tool outcome ok");
	CHECK(strstr(st.assistant, "done") != NULL, "final answer streamed");
	CHECK(canned_request_count(srv) == 2, "two requests");
	CHECK(canned_last_request(srv) != NULL &&
	    strstr(canned_last_request(srv), "hello") != NULL,
	    "tool result fed back");

	teardown(&st, srv);
}

/* (g) Streamed reasoning channel, finish_reason, and usage/timings. */
static void
test_stream_meta(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	st.stream = 1;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_reply(srv,
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"hmm\"}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"answer\"}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
	    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,"
	    "\"total_tokens\":15},\"timings\":{\"predicted_per_second\":42.5}}\n\n"
	    "data: [DONE]\n\n");

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "think") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "meta turn ok");
	CHECK(strstr(st.reasoning, "hmm") != NULL, "reasoning delivered");
	CHECK(strstr(st.assistant, "answer") != NULL, "content delivered");
	CHECK(st.got_finish && st.finish == CLM_FINISH_STOP, "finish_reason stop");
	CHECK(st.got_usage && st.usage.prompt_tokens == 10 &&
	    st.usage.completion_tokens == 5, "usage tokens");
	CHECK(st.usage.total_tokens == 15, "usage total");
	CHECK(st.usage.tokens_per_sec > 42.0 && st.usage.tokens_per_sec < 43.0,
	    "usage tok/s");

	teardown(&st, srv);
}

/*
 * (h) After a turn ends in the error state (here: a dead endpoint), the agent
 * must accept a new prompt rather than rejecting it as "turn already in
 * progress". This regresses a bug where a cancelled/errored turn left the
 * agent stuck: every later submit returned -EBUSY until restart.
 */
static void
test_recover_after_error(uv_loop_t *loop)
{
	struct tstate st = {0};

	st.loop = loop;
	/* Port 1 has nothing listening, so the turn fails to connect. */
	st.agent = make_agent(&st, 1);

	CHECK(clm_agent_submit(st.agent, "first") == 0, "first submit accepted");
	run_until_done(&st);
	CHECK(st.turn_status != 0, "first turn errored (no server)");
	CHECK(clm_agent_get_state(st.agent) == CLM_STATE_ERROR, "left in error state");

	/* The real assertion: a second prompt is not wedged by the error. */
	st.turn_done = 0;
	CHECK(clm_agent_submit(st.agent, "second") == 0,
	    "second submit accepted after error (not -EBUSY)");
	run_until_done(&st);

	clm_agent_free(st.agent);
	uv_run(loop, UV_RUN_DEFAULT); /* drain pending closes */
}

/*
 * (i) clm_parse_props: derive the per-conversation context budget from a
 * llama.cpp /props body, divide across slots, and reject non-llama.cpp or
 * malformed bodies. Pure function, no server needed.
 */
static void
test_parse_props(void)
{
	int64_t ctx = 0;

	/* Single slot: ctx is the full n_ctx. */
	ctx = 0;
	CHECK(clm_parse_props(
	    "{\"build_info\":\"b1\",\"total_slots\":1,"
	    "\"default_generation_settings\":{\"n_ctx\":262144}}", &ctx) == 0,
	    "props: parses n_ctx");
	CHECK(ctx == 262144, "props: single-slot ctx is full n_ctx");

	/* Multiple slots: context is shared, so the budget is divided. */
	ctx = 0;
	CHECK(clm_parse_props(
	    "{\"build_info\":\"b1\",\"total_slots\":4,"
	    "\"default_generation_settings\":{\"n_ctx\":262144}}", &ctx) == 0,
	    "props: parses with slots");
	CHECK(ctx == 65536, "props: ctx divided across slots");

	/* No build_info => not llama.cpp => rejected. */
	ctx = -1;
	CHECK(clm_parse_props(
	    "{\"default_generation_settings\":{\"n_ctx\":262144}}", &ctx) < 0,
	    "props: no build_info rejected");
	CHECK(ctx == -1, "props: ctx untouched on reject");

	/* Missing n_ctx and outright garbage are rejected. */
	CHECK(clm_parse_props("{\"build_info\":\"b1\"}", &ctx) < 0,
	    "props: missing n_ctx rejected");
	CHECK(clm_parse_props("not json", &ctx) < 0, "props: garbage rejected");
	CHECK(clm_parse_props(NULL, &ctx) < 0, "props: NULL rejected");
}

/* Count messages of a given role in a history. */
static int
count_role(struct clm_history *h, enum clm_role role)
{
	struct clm_message *m;
	int n = 0;
	TAILQ_FOREACH(m, h, entries)
		if (m->role == role)
			n++;
	return n;
}

/*
 * (j) clm_history_compact: collapse old turns into one summary while keeping
 * the system prologue and recent user turns, without splitting a tool
 * exchange. Pure history surgery, no server.
 */
static void
test_history_compact(void)
{
	struct clm_history h;
	struct clm_message *m;
	struct clm_tool_call *tc;

	clm_history_init(&h);

	/* system + 4 user turns; turn 2 includes a tool call/result pair. */
	clm_history_add_system(&h, "SYSPROMPT");

	clm_history_add_user(&h, "turn1");
	clm_history_add_assistant_text(&h, "reply1");

	clm_history_add_user(&h, "turn2");
	m = clm_history_add_assistant_tool_calls(&h);
	tc = clm_message_add_tool_call(m, "call1", "shell_exec", "{}");
	CHECK(tc != NULL, "compact: seed tool call");
	clm_history_add_tool_result(&h, "call1", "tool output");
	clm_history_add_assistant_text(&h, "reply2");

	clm_history_add_user(&h, "turn3");
	clm_history_add_assistant_text(&h, "reply3");

	clm_history_add_user(&h, "turn4");
	clm_history_add_assistant_text(&h, "reply4");

	/* Keep the last 2 user turns; summarize turns 1-2 (incl. the tool pair). */
	CHECK(clm_history_compact(&h, "SUMMARY", 2) == 0, "compact: ok");

	/* System message preserved exactly once, still first and unchanged. */
	m = TAILQ_FIRST(&h);
	CHECK(m != NULL && m->role == CLM_ROLE_SYSTEM &&
	    strcmp(m->content, "SYSPROMPT") == 0, "compact: system preserved");
	CHECK(count_role(&h, CLM_ROLE_SYSTEM) == 1, "compact: one system msg");

	/* The summary replaced the old turns: it sits right after system. */
	m = TAILQ_NEXT(m, entries);
	CHECK(m != NULL && m->role == CLM_ROLE_USER &&
	    strcmp(m->content, "SUMMARY") == 0, "compact: summary after system");

	/* Recent turns kept verbatim; old ones (turn1/turn2 + tool pair) gone. */
	CHECK(count_role(&h, CLM_ROLE_TOOL) == 0,
	    "compact: old tool result removed (not orphaned)");
	{
		int found_turn3 = 0, found_turn4 = 0, found_old = 0;
		TAILQ_FOREACH(m, &h, entries) {
			if (m->content == NULL)
				continue;
			if (strcmp(m->content, "turn3") == 0) found_turn3 = 1;
			if (strcmp(m->content, "turn4") == 0) found_turn4 = 1;
			if (strcmp(m->content, "turn1") == 0 ||
			    strcmp(m->content, "turn2") == 0) found_old = 1;
		}
		CHECK(found_turn3 && found_turn4, "compact: recent turns kept");
		CHECK(!found_old, "compact: old turns dropped");
	}

	/* Not enough turns to compact => no-op. */
	{
		struct clm_history h2;
		clm_history_init(&h2);
		clm_history_add_system(&h2, "S");
		clm_history_add_user(&h2, "only");
		clm_history_add_assistant_text(&h2, "r");
		CHECK(clm_history_compact(&h2, "SUMMARY", 2) == 0,
		    "compact: too-short is ok");
		{
			int users = count_role(&h2, CLM_ROLE_USER);
			CHECK(users == 1, "compact: too-short unchanged (no summary)");
		}
		clm_history_free(&h2);
	}

	clm_history_free(&h);
}

int
main(void)
{
	uv_loop_t loop;

	uv_loop_init(&loop);
	test_text_reply(&loop);
	test_tool_call(&loop);
	test_file_tools(&loop);
	test_shell_exec(&loop);
	test_stream_text(&loop);
	test_stream_tool(&loop);
	test_stream_meta(&loop);
	test_recover_after_error(&loop);
	test_parse_props();
	test_history_compact();
	uv_loop_close(&loop);

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
