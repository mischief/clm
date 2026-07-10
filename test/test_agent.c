// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
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
	struct clm_host *host;
	struct clm_agent *agent;
	int stream;
	enum clm_provider provider;
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
	/* Permission-gate testing. */
	int perm_prompts;                     /* times on_permission fired */
	enum clm_permission_decision perm_decision; /* what to answer */
	int notices;
	char notice[256];
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
 * Queue a tool-call assistant reply, built with cJSON so the nested
 * "arguments" string (JSON encoded inside JSON) is escaped correctly.
 * args_json is the tool's raw arguments object, e.g. {"path":"/x"}.
 */
static void
canned_tool_call(struct canned_server *srv, const char *name, const char *args_json)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON *calls = cJSON_CreateArray();
	cJSON *call = cJSON_CreateObject();
	cJSON *func = cJSON_CreateObject();
	char *printed;

	cJSON_AddItemToObject(func, "name", cJSON_CreateString(name));
	cJSON_AddItemToObject(func, "arguments", cJSON_CreateString(args_json));
	cJSON_AddItemToObject(call, "id", cJSON_CreateString("c1"));
	cJSON_AddItemToObject(call, "type", cJSON_CreateString("function"));
	cJSON_AddItemToObject(call, "function", func);
	cJSON_AddItemToArray(calls, call);
	cJSON_AddItemToObject(message, "role", cJSON_CreateString("assistant"));
	cJSON_AddItemToObject(message, "content", cJSON_CreateString(""));
	cJSON_AddItemToObject(message, "tool_calls", calls);
	cJSON_AddItemToObject(choice, "finish_reason", cJSON_CreateString("tool_calls"));
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddItemToObject(root, "choices", choices);

	printed = cJSON_PrintUnformatted(root);
	canned_reply(srv, printed != NULL ? printed : "{}");
	free(printed);
	cJSON_Delete(root);
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
on_notice(const char *text, void *user)
{
	struct tstate *st = user;
	st->notices++;
	(void)snprintf(st->notice, sizeof(st->notice), "%s", text ? text : "");
}

static void
on_turn_done(int status, void *user)
{
	struct tstate *st = user;
	st->turn_done = 1;
	st->turn_status = status;
}

static void
on_permission(const struct clm_permission_req *req, void *user)
{
	struct tstate *st = user;
	st->perm_prompts++;
	clm_tool_permission_respond(st->agent, req, st->perm_decision);
}

static const struct clm_callbacks callbacks = {
	.on_assistant_text = on_assistant_text,
	.on_reasoning = on_reasoning,
	.on_permission = on_permission,
	.on_tool_result = on_tool_result,
	.on_finish_reason = on_finish_reason,
	.on_usage = on_usage,
	.on_turn_done = on_turn_done,
	.on_notice = on_notice,
};

/* A mock tool that completes synchronously with a fixed string. */
static void
echo_hello(struct clm_tool_invocation *inv, void *user)
{
	(void)user;
	clm_tool_complete(inv, "hello");
}

/* A mock tool that returns raw binary output (embedded NUL, non-UTF-8 bytes)
 * via clm_tool_complete_buf, to exercise inv_finalize's binary-output
 * detector (see find_binary_offset in tools.c). */
static void
emit_binary(struct clm_tool_invocation *inv, void *user)
{
	static const uint8_t data[] = { 'h', 'i', 0x00, 0x89, 'P', 'N', 'G' };
	struct clm_buffer buf;

	(void)user;
	buf.data = data;
	buf.len = sizeof(data);
	clm_tool_complete_buf(inv, buf);
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
	cfg.provider = st->provider;
	cfg.model = "test-model";
	cfg.stream = st->stream;
	cfg.system_prompt = st->system_prompt;

	r = clm_host_uv_new(st->loop, &st->host);
	CHECK(r == 0, "clm_host_uv_new");
	r = clm_agent_new(&cfg, st->host, &callbacks, st, &agent);
	CHECK(r == 0, "clm_agent_new");
	CHECK(clm_tools_register_shell(agent) == 0, "register shell");
	CHECK(clm_tools_register_bg(agent) == 0, "register bg");
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
	clm_host_uv_free(st->host);
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

/*
 * (b2) A tool that reports raw binary output via clm_tool_complete_buf: the
 * history/request-building path must never emit invalid-UTF-8 bytes into the
 * JSON request (cJSON_CreateString on non-UTF-8 data is a landmine some
 * cJSON builds silently mishandle), so inv_finalize replaces it with a
 * human-readable warning before it ever reaches history or the wire.
 */
static void
test_binary_tool_output(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_tool_call(srv, "emit_binary", "{}");
	canned_reply(srv, final_reply);

	st.agent = make_agent(&st, canned_port(srv));

	def.name = "emit_binary";
	def.description = "emit binary output";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = emit_binary;
	CHECK(clm_tool_add(st.agent, &def) == 0, "clm_tool_add");

	CHECK(clm_agent_submit(st.agent, "use the tool") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "binary tool turn ok");
	CHECK(st.tool_results == 1, "one tool result");
	CHECK(st.last_outcome == CLM_TOOL_OK, "tool outcome ok");
	CHECK(strstr(st.tool_content, "binary data at byte offset 2 of 7") != NULL,
	    "on_tool_result sees the offset warning, not raw bytes");
	CHECK(canned_request_count(srv) == 2, "two requests (call + result)");
	CHECK(canned_last_request(srv) != NULL &&
	    strstr(canned_last_request(srv), "binary data at byte offset 2 of 7") != NULL,
	    "warning (not raw binary) is what actually gets sent to the API");

	teardown(&st, srv);
}

/*
 * (b3) bg_exec: the model starts a real background job via bg_exec, gets
 * back an immediate "started" result (turn 1 ends normally, no waiting on
 * the child process), and once the process actually exits, its output must
 * arrive as an AUTOMATIC follow-up turn via clm_agent_notify -- a third
 * canned request the test never explicitly triggers, proving the
 * job-exit -> notify -> clm_agent_submit chain in agent.c/tool_bg.c actually
 * fires end to end (not just that bg_exec's own invocation completes).
 */
static void
test_bg_exec(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	/* Turn 1: the model starts a background job. Turn 2 (automatic, once
	 * the job exits): the model acknowledges the job's result. */
	canned_tool_call(srv, "bg_exec",
	    "{\"command\":\"printf bgoutput123\",\"label\":\"probe\"}");
	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"started it\"}}]}");
	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"job acknowledged\"}}]}");

	st.agent = make_agent(&st, canned_port(srv));

	CHECK(clm_agent_submit(st.agent, "run a background job") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "turn 1 ok");
	CHECK(st.tool_results == 1, "bg_exec's own invocation completed once");
	CHECK(strstr(st.tool_content, "started background job") != NULL,
	    "bg_exec's immediate result is a start acknowledgement, not the job's output");
	CHECK(strstr(st.assistant, "started it") != NULL, "turn 1 final answer delivered");
	CHECK(canned_request_count(srv) == 2, "two requests for turn 1 (call + start ack)");

	/* Turn 1 is done, but the background job (a real subprocess) may not
	 * have exited yet. Wait for the automatic follow-up turn its exit
	 * triggers via clm_agent_notify -- run_until_done again, watching for
	 * a second turn_done. */
	st.turn_done = 0;
	run_until_done(&st);

	CHECK(st.turn_status == 0, "turn 2 (automatic) ok");
	CHECK(strstr(st.assistant, "job acknowledged") != NULL,
	    "turn 2's final answer delivered");
	CHECK(canned_request_count(srv) == 3,
	    "job exit triggered a third request automatically");
	CHECK(canned_last_request(srv) != NULL &&
	    strstr(canned_last_request(srv), "bgoutput123") != NULL,
	    "the job's real output reached the model in the auto-triggered turn");
	CHECK(canned_last_request(srv) != NULL &&
	    strstr(canned_last_request(srv), "probe") != NULL,
	    "the job's label reached the model in the auto-triggered turn");

	teardown(&st, srv);
}

/*
 * (b2) Mid-chain autocompact: if usage crosses the threshold on a response
 * that itself requests another tool call (i.e. the turn is NOT done yet),
 * clm_agent_tools_done() must trigger clm_agent_compact() itself right then
 * -- not wait for the whole turn to finish first, which is what tui.c's own
 * end-of-turn check does and why this needed a second, earlier check inside
 * agent.c (see clm_agent_tools_done). ctx_max is never probed against the
 * canned server (no /props), so this exercises the absolute-token fallback
 * path, not the percentage one -- both share the same
 * clm_agent_over_autocompact_threshold(), so covering one covers the calc
 * for both.
 *
 * Reply sequence: (1) tool call, with usage already over
 * CLM_AUTOCOMPACT_FALLBACK_TOKENS -- clm_agent_tools_done sees this once the
 * tool finishes and, instead of proceeding straight to the next LLM call,
 * fires off (2) a compaction request. (3) is the resumed turn's real next
 * LLM call, which must still happen automatically -- if the mid-chain
 * compact swallowed the turn instead of resuming it, turn_done would never
 * fire and run_until_done would hang instead of completing.
 */
static void
test_autocompact_mid_chain(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	/* (1) Tool call, usage already past the 100000-token fallback. */
	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":"
	    "[{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":"
	    "\"echo_hello\",\"arguments\":\"{}\"}}]}}],"
	    "\"usage\":{\"prompt_tokens\":99000,\"completion_tokens\":2000,"
	    "\"total_tokens\":101000}}");
	/* (2) Compaction's own summarization request. */
	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"SUMMARY\"}}]}");
	/* (3) Resumed turn's real next call -- must still happen automatically. */
	canned_reply(srv, final_reply);

	st.agent = make_agent(&st, canned_port(srv));

	def.name = "echo_hello";
	def.description = "echo hello";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "clm_tool_add");

	CHECK(clm_agent_submit(st.agent, "use the tool") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "mid-chain compact: turn still completes");
	CHECK(strstr(st.assistant, "done") != NULL,
	    "mid-chain compact: resumed chain delivers final answer");
	CHECK(canned_request_count(srv) == 3,
	    "mid-chain compact: tool call + compaction + resumed call");
	/* After a successful compact, usage is reset to 0 (see
	 * clm_agent_tools_done) until the next real LLM call reports a fresh
	 * figure -- confirm it's no longer sitting over threshold from the
	 * pre-compaction reading. */
	CHECK(!clm_agent_over_autocompact_threshold(st.agent),
	    "mid-chain compact: threshold cleared after compacting");

	teardown(&st, srv);
}

/*
 * (b3) Compaction summary from the reasoning channel: a "thinking" model can
 * spend its whole completion budget on chain-of-thought and hit
 * finish_reason "length" with an empty "content" but a non-empty
 * "reasoning" -- seen live against a local ollama reasoning model summarizing
 * a near-full context (exactly when compaction fires). extract_message_content
 * must fall back to reasoning/reasoning_content instead of reporting
 * "compaction produced no summary", or compaction can never succeed at all
 * for such a model.
 */
static void
test_compact_reasoning_fallback(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	int i;

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	/* A few ordinary turns so clm_history_compact has a real cut point. */
	for (i = 0; i < 3; i++) {
		canned_reply(srv, final_reply);
		if (i == 0)
			st.agent = make_agent(&st, canned_port(srv));
		CHECK(clm_agent_submit(st.agent, "hi") == 0, "submit");
		run_until_done(&st);
		st.turn_done = 0;
	}

	/* Compaction's summarization call: empty content, reasoning only. */
	canned_reply(srv,
	    "{\"choices\":[{\"finish_reason\":\"length\",\"index\":0,"
	    "\"message\":{\"role\":\"assistant\",\"content\":\"\","
	    "\"reasoning\":\"REASONING_ONLY_SUMMARY\"}}]}");

	CHECK(clm_agent_compact(st.agent) == 0, "compact: accepted");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "compact: reasoning fallback succeeds");
	CHECK(clm_agent_get_state(st.agent) == CLM_STATE_COMPLETE,
	    "compact: left in complete state, not error");

	teardown(&st, srv);
}

/*
 * (b4) A model with no tool-calling support: the first turn (tools attached,
 * since clm always registers at least the shell/bg builtins) gets ollama's
 * real-world plain-text 400 "does not support tools" back. The turn must
 * retry with no "tools" field and succeed, firing on_notice once, and a
 * second turn must not attach "tools" again or re-fire the notice --
 * confirming tools_unsupported sticks for the session.
 */
static void
test_tools_unsupported_retry(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_reply_status(srv, 400,
	    "registry.ollama.ai/some/model:8b does not support tools");
	canned_reply(srv, final_reply);
	canned_reply(srv, final_reply);

	st.agent = make_agent(&st, canned_port(srv));

	CHECK(clm_agent_submit(st.agent, "hi") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "tools-unsupported: retry without tools succeeds");
	CHECK(st.notices == 1, "tools-unsupported: notice fired once");
	CHECK(canned_request_count(srv) == 2,
	    "tools-unsupported: failed attempt + retry");
	CHECK(strstr(canned_last_request(srv), "\"tools\"") == NULL,
	    "tools-unsupported: retry omits tools field");

	st.turn_done = 0;
	CHECK(clm_agent_submit(st.agent, "hi again") == 0, "submit 2");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "tools-unsupported: second turn succeeds");
	CHECK(st.notices == 1, "tools-unsupported: no repeat notice");
	CHECK(canned_request_count(srv) == 3,
	    "tools-unsupported: second turn is a single request, no retry");
	CHECK(strstr(canned_last_request(srv), "\"tools\"") == NULL,
	    "tools-unsupported: still omits tools field on later turns");

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

/*
 * (d2) Regression for the use-after-free described in clm issue #3: freeing
 * the agent while a shell_exec child process is still running must not let
 * that process's later exit callback touch the freed agent.
 *
 * shell_exec's own uv_process_t keeps running after clm_agent_free() returns
 * -- cancellation (clm_tools_detach(), invoked from clm_agent_free()) only
 * sends SIGTERM; the process's real exit_cb (shell_on_exit -> ... ->
 * inv_finalize) fires later, asynchronously, on the same loop. Before the
 * clm_tools_detach() fix this dereferenced the freed agent (and its already-
 * freed tool registry/history) from that later callback; run under ASan this
 * reliably aborted the test binary. The fix severs the batch from the agent
 * up front, so the delayed completion just retires quietly.
 *
 * The command sleeps long enough that clm_agent_free() below is guaranteed
 * to run while the child is still alive: submit() and dispatch (loopback
 * HTTP + uv_spawn) complete in microseconds, nowhere close to this.
 */
static void
test_agent_free_during_shell_exec(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	int i;

	st.loop = loop;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_tool_call(srv, "shell_exec", "{\"command\":\"sleep 0.2\"}");

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "sleep a bit") == 0, "submit");

	/* Pump the loop just enough for the canned HTTP round trip and the
	 * uv_spawn() to happen (both effectively instantaneous), but nowhere
	 * near the 0.2s the child needs to exit. */
	for (i = 0; i < 50; i++)
		uv_run(loop, UV_RUN_NOWAIT);
	CHECK(!st.turn_done, "turn must still be waiting on the shell command");

	/* Simulate /agent switching mid-flight: free the agent while
	 * shell_exec's child process is still running. */
	clm_agent_free(st.agent);
	st.agent = NULL;

	/* Nothing will talk to the canned server again; stop it now so its
	 * listening socket doesn't keep UV_RUN_DEFAULT below from ever going
	 * idle. */
	canned_stop(srv);

	/* Drain until the child actually exits and its delayed completion
	 * runs through the detached path. Reaching here without a crash
	 * (or an ASan abort) is the test. */
	uv_run(loop, UV_RUN_DEFAULT);

	clm_host_uv_free(st.host);
	uv_run(loop, UV_RUN_DEFAULT); /* drain pending closes */
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

/* Queue an SSE tool-call reply with the name and arguments in separate
 * deltas. Heap, not a stack buffer: at -O2 this otherwise gets inlined into
 * its one call site (test_stream_tool), and the two functions' combined
 * frame trips -Wframe-larger-than=2048 even though neither is a problem on
 * its own -- test code, so a malloc/free here costs nothing that matters. */
static void
canned_stream_tool(struct canned_server *srv, const char *name)
{
	enum { CAP = 1024 };
	char *body = malloc(CAP);
	if (body == NULL)
		return;
	(void)snprintf(body, CAP,
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
	    "\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"%s\","
	    "\"arguments\":\"\"}}]}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,"
	    "\"function\":{\"arguments\":\"{}\"}}]}}]}\n\n"
	    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
	    "data: [DONE]\n\n", name);
	canned_reply(srv, body);
	free(body);
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
 * (g2) Anthropic Messages API: request shape (auth headers, system pulled
 * out of messages[], max_tokens) and a non-streaming text reply translated
 * back from Anthropic's content-block response shape.
 */
static void
test_anthropic_text_reply(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	const char *req;

	st.loop = loop;
	st.provider = CLM_PROVIDER_ANTHROPIC;
	st.system_prompt = "SENTINEL_SYS_PROMPT";
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_reply(srv,
	    "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
	    "\"content\":[{\"type\":\"text\",\"text\":\"hi there\"}],"
	    "\"stop_reason\":\"end_turn\","
	    "\"usage\":{\"input_tokens\":11,\"output_tokens\":4}}");

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "hello") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "anthropic text turn ok");
	CHECK(strstr(st.assistant, "hi there") != NULL, "assistant text delivered");
	CHECK(st.got_finish && st.finish == CLM_FINISH_STOP, "end_turn mapped to stop");
	CHECK(st.got_usage && st.usage.prompt_tokens == 11 &&
	    st.usage.completion_tokens == 4, "anthropic usage translated");

	req = canned_last_request(srv);
	CHECK(req != NULL && strstr(req, "x-api-key: test") != NULL,
	    "x-api-key header sent");
	CHECK(req != NULL && strstr(req, "anthropic-version:") != NULL,
	    "anthropic-version header sent");
	CHECK(req != NULL && strstr(req, "Authorization:") == NULL,
	    "no bearer header for anthropic");
	CHECK(req != NULL && strstr(req, "\"system\":") != NULL,
	    "system sent as top-level field");
	CHECK(req != NULL && strstr(req, "SENTINEL_SYS_PROMPT") != NULL,
	    "system prompt text sent");
	CHECK(req != NULL && strstr(req, "\"max_tokens\":") != NULL,
	    "max_tokens sent");
	CHECK(req != NULL && strstr(req, "\"role\":\"system\"") == NULL,
	    "no system role left in messages[]");

	teardown(&st, srv);
}

/* (g3) Anthropic tool_use in a non-streaming reply, then a tool_result fed
 * back on the next turn. */
static void
test_anthropic_tool_call(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};
	const char *req;

	st.loop = loop;
	st.provider = CLM_PROVIDER_ANTHROPIC;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_reply(srv,
	    "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
	    "\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_1\","
	    "\"name\":\"echo_hello\",\"input\":{}}],"
	    "\"stop_reason\":\"tool_use\","
	    "\"usage\":{\"input_tokens\":5,\"output_tokens\":2}}");
	canned_reply(srv,
	    "{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\","
	    "\"content\":[{\"type\":\"text\",\"text\":\"done\"}],"
	    "\"stop_reason\":\"end_turn\","
	    "\"usage\":{\"input_tokens\":8,\"output_tokens\":2}}");

	st.agent = make_agent(&st, canned_port(srv));
	def.name = "echo_hello";
	def.description = "echo hello";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "clm_tool_add");

	CHECK(clm_agent_submit(st.agent, "use the tool") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "anthropic tool turn ok");
	CHECK(st.tool_results == 1, "one tool result");
	CHECK(st.last_outcome == CLM_TOOL_OK, "tool outcome ok");
	CHECK(strstr(st.assistant, "done") != NULL, "final answer delivered");
	CHECK(canned_request_count(srv) == 2, "two requests");

	req = canned_last_request(srv);
	CHECK(req != NULL && strstr(req, "\"tool_result\"") != NULL,
	    "tool result sent as tool_result block");
	CHECK(req != NULL && strstr(req, "\"tool_use_id\":\"toolu_1\"") != NULL,
	    "tool_use_id round-tripped");

	teardown(&st, srv);
}

/* (g4) Anthropic streaming: text deltas, a tool_use block assembled across
 * content_block_start/content_block_delta events, and message_delta usage. */
static void
test_anthropic_stream(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;

	st.loop = loop;
	st.stream = 1;
	st.provider = CLM_PROVIDER_ANTHROPIC;
	srv = canned_start(loop);
	CHECK(srv != NULL, "canned_start");

	canned_reply(srv,
	    "event: message_start\n"
	    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
	    "\"usage\":{\"input_tokens\":9}}}\n\n"
	    "event: content_block_start\n"
	    "data: {\"type\":\"content_block_start\",\"index\":0,"
	    "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
	    "event: content_block_delta\n"
	    "data: {\"type\":\"content_block_delta\",\"index\":0,"
	    "\"delta\":{\"type\":\"text_delta\",\"text\":\"hi \"}}\n\n"
	    "event: content_block_delta\n"
	    "data: {\"type\":\"content_block_delta\",\"index\":0,"
	    "\"delta\":{\"type\":\"text_delta\",\"text\":\"there\"}}\n\n"
	    "event: content_block_stop\n"
	    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
	    "event: message_delta\n"
	    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
	    "\"usage\":{\"output_tokens\":3}}\n\n"
	    "event: message_stop\n"
	    "data: {\"type\":\"message_stop\"}\n\n");

	st.agent = make_agent(&st, canned_port(srv));
	CHECK(clm_agent_submit(st.agent, "hello") == 0, "submit");
	run_until_done(&st);

	CHECK(st.turn_status == 0, "anthropic stream ok");
	CHECK(strstr(st.assistant, "hi there") != NULL, "streamed deltas assembled");
	CHECK(st.got_finish && st.finish == CLM_FINISH_STOP, "end_turn mapped to stop");
	CHECK(st.got_usage && st.usage.prompt_tokens == 9 &&
	    st.usage.completion_tokens == 3, "input/output tokens combined");

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
	clm_host_uv_free(st.host);
	uv_run(loop, UV_RUN_DEFAULT); /* drain pending closes */
}

/*
 * (h2) Same bug as (h) above, but for clm_agent_set_provider instead of
 * clm_agent_submit: a turn that ends in CLM_STATE_ERROR must not
 * permanently lock out reconfiguring the agent. Concretely, this is what
 * let a TUI user get stuck after e.g. switching to a model their plan
 * doesn't allow -- the turn 403's into CLM_STATE_ERROR, and every
 * subsequent /model or /provider (both call clm_agent_set_provider) then
 * failed with -EBUSY forever, with no way to switch to a working model
 * short of restarting.
 */
static void
test_set_provider_after_error(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct clm_cfg cfg = {0};

	st.loop = loop;
	/* Port 1 has nothing listening, so the turn fails to connect. */
	st.agent = make_agent(&st, 1);

	CHECK(clm_agent_submit(st.agent, "first") == 0, "submit accepted");
	run_until_done(&st);
	CHECK(st.turn_status != 0, "turn errored (no server)");
	CHECK(clm_agent_get_state(st.agent) == CLM_STATE_ERROR, "left in error state");

	/* The real assertion: reconfiguring is not wedged by the error. */
	cfg.base_url = "http://127.0.0.1:1/v1/chat/completions";
	cfg.model = "some-other-model";
	CHECK(clm_agent_set_provider(st.agent, &cfg) == 0,
	    "set_provider accepted after error (not -EBUSY)");

	clm_agent_free(st.agent);
	clm_host_uv_free(st.host);
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

/*
 * (i2) clm_parse_models_list: extract ids from an OpenAI-compatible GET
 * /v1/models body, in order, skipping malformed entries; reject garbage.
 * Pure function, no server needed.
 */
static void
test_parse_models_list(void)
{
	char **ids;

	ids = clm_parse_models_list(
	    "{\"object\":\"list\",\"data\":["
	    "{\"id\":\"auto\",\"object\":\"model\"},"
	    "{\"id\":\"kimi-k2.6\",\"object\":\"model\"}]}");
	CHECK(ids != NULL, "models_list: parses a two-entry catalog");
	if (ids != NULL) {
		CHECK(strcmp(ids[0], "auto") == 0, "models_list: first id in order");
		CHECK(strcmp(ids[1], "kimi-k2.6") == 0, "models_list: second id in order");
		CHECK(ids[2] == NULL, "models_list: NULL-terminated");
		clm_free_models_list(ids);
	}

	/* An entry missing "id" (or with a non-string id) is skipped, not
	 * fatal to the rest of the list. */
	ids = clm_parse_models_list(
	    "{\"data\":[{\"object\":\"model\"},{\"id\":\"ok\"},{\"id\":123}]}");
	CHECK(ids != NULL, "models_list: malformed entries skipped, not fatal");
	if (ids != NULL) {
		CHECK(strcmp(ids[0], "ok") == 0, "models_list: only the valid entry kept");
		CHECK(ids[1] == NULL, "models_list: skipped entries don't appear");
		clm_free_models_list(ids);
	}

	/* Wrong shape, garbage, empty, and NULL are all rejected. */
	CHECK(clm_parse_models_list("{\"data\":[]}") == NULL,
	    "models_list: empty data array rejected");
	CHECK(clm_parse_models_list("{\"object\":\"list\"}") == NULL,
	    "models_list: missing data field rejected");
	CHECK(clm_parse_models_list("{\"data\":\"not-an-array\"}") == NULL,
	    "models_list: non-array data rejected");
	CHECK(clm_parse_models_list("not json") == NULL,
	    "models_list: garbage rejected");
	CHECK(clm_parse_models_list(NULL) == NULL, "models_list: NULL rejected");
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
	clm_history_add_system(&h, "SYSPROMPT", NULL);

	clm_history_add_user(&h, "turn1", NULL);
	clm_history_add_assistant_text(&h, "reply1", NULL);

	clm_history_add_user(&h, "turn2", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	tc = clm_message_add_tool_call(m, "call1", "shell_exec", "{}");
	CHECK(tc != NULL, "compact: seed tool call");
	clm_history_add_tool_result(&h, "call1", "shell_exec", "tool output", strlen("tool output"), NULL);
	clm_history_add_assistant_text(&h, "reply2", NULL);

	clm_history_add_user(&h, "turn3", NULL);
	clm_history_add_assistant_text(&h, "reply3", NULL);

	clm_history_add_user(&h, "turn4", NULL);
	clm_history_add_assistant_text(&h, "reply4", NULL);

	/* Keep the last 2 user turns; summarize turns 1-2 (incl. the tool pair).
	 * Returns the number of messages folded (> 0 here). */
	CHECK(clm_history_compact(&h, "SUMMARY", 2, NULL) > 0, "compact: ok");

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
		clm_history_add_system(&h2, "S", NULL);
		clm_history_add_user(&h2, "only", NULL);
		clm_history_add_assistant_text(&h2, "r", NULL);
		CHECK(clm_history_compact(&h2, "SUMMARY", 2, NULL) == 0,
		    "compact: too-short is ok");
		{
			int users = count_role(&h2, CLM_ROLE_USER);
			CHECK(users == 1, "compact: too-short unchanged (no summary)");
		}
		clm_history_free(&h2);
	}

	clm_history_free(&h);
}

/*
 * (j2) clm_history_compact fallback: a forever-mode agentic history has one
 * user message ever (the mission) followed by a long autonomous tool-exchange
 * chain -- no user boundary to cut at. Compaction must fall back to cutting
 * at exchange heads, keeping the mission verbatim, instead of no-oping
 * forever (the "compact forever" loop this guards against: over threshold ->
 * summarize -> fold nothing -> still over threshold -> repeat).
 */
static void
test_history_compact_agentic(void)
{
	struct clm_history h;
	struct clm_message *m;

	clm_history_init(&h);
	clm_history_add_system(&h, "SYSPROMPT", NULL);
	clm_history_add_user(&h, "MISSION", NULL);

	/* 4 tool exchanges, no further user messages. */
	for (int i = 0; i < 4; i++) {
		char id[16], out[16];
		snprintf(id, sizeof(id), "call%d", i);
		snprintf(out, sizeof(out), "out%d", i);
		m = clm_history_add_assistant_tool_calls(&h);
		CHECK(clm_message_add_tool_call(m, id, "local_map", "{}") != NULL,
		    "agentic compact: seed call");
		clm_history_add_tool_result(&h, id, "local_map", out, strlen(out), NULL);
	}

	/* Keep the last 2 exchanges; exchanges 0-1 fold into the summary. */
	CHECK(clm_history_compact(&h, "SUMMARY", 2, NULL) > 0,
	    "agentic compact: folded");

	/* Shape: system, MISSION kept verbatim, then the summary. */
	m = TAILQ_FIRST(&h);
	CHECK(m != NULL && m->role == CLM_ROLE_SYSTEM,
	    "agentic compact: system first");
	m = TAILQ_NEXT(m, entries);
	CHECK(m != NULL && m->role == CLM_ROLE_USER &&
	    strcmp(m->content, "MISSION") == 0, "agentic compact: mission kept");
	m = TAILQ_NEXT(m, entries);
	CHECK(m != NULL && m->role == CLM_ROLE_USER &&
	    strcmp(m->content, "SUMMARY") == 0,
	    "agentic compact: summary after mission");

	/* Last 2 exchanges intact and still paired; older results folded. */
	CHECK(count_role(&h, CLM_ROLE_TOOL) == 2,
	    "agentic compact: 2 results kept");
	{
		int found_old = 0, found_recent = 0;
		TAILQ_FOREACH(m, &h, entries) {
			if (m->role != CLM_ROLE_TOOL || m->content == NULL)
				continue;
			if (strcmp(m->content, "out0") == 0 ||
			    strcmp(m->content, "out1") == 0)
				found_old = 1;
			if (strcmp(m->content, "out2") == 0 ||
			    strcmp(m->content, "out3") == 0)
				found_recent = 1;
		}
		CHECK(!found_old && found_recent,
		    "agentic compact: oldest exchanges folded");
	}

	/* <= keep_recent exchanges total: no valid cut, must report no
	 * progress (0) with the history untouched -- the caller uses that to
	 * distinguish a real fold from a futile summarize call. */
	{
		struct clm_history h2;
		clm_history_init(&h2);
		clm_history_add_system(&h2, "S", NULL);
		clm_history_add_user(&h2, "M", NULL);
		m = clm_history_add_assistant_tool_calls(&h2);
		CHECK(clm_message_add_tool_call(m, "c1", "t", "{}") != NULL,
		    "agentic compact: seed short call1");
		clm_history_add_tool_result(&h2, "c1", "t", "o1", strlen("o1"), NULL);
		m = clm_history_add_assistant_tool_calls(&h2);
		CHECK(clm_message_add_tool_call(m, "c2", "t", "{}") != NULL,
		    "agentic compact: seed short call2");
		clm_history_add_tool_result(&h2, "c2", "t", "o2", strlen("o2"), NULL);
		CHECK(clm_history_compact(&h2, "SUMMARY", 2, NULL) == 0,
		    "agentic compact: too-short no-op");
		CHECK(count_role(&h2, CLM_ROLE_TOOL) == 2,
		    "agentic compact: too-short unchanged");
		clm_history_free(&h2);
	}

	clm_history_free(&h);
}

static void
test_history_supersede(void)
{
	struct clm_history h;
	struct clm_message *m;
	const char *stub = "[superseded by newer local_map]";

	clm_history_init(&h);
	clm_history_add_system(&h, "S", NULL);

	/* Turn 1: local_map + examine results. */
	clm_history_add_user(&h, "turn1", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "c1", "local_map", "{}");
	clm_message_add_tool_call(m, "c2", "examine", "{}");
	clm_history_add_tool_result(&h, "c1", "local_map", "MAP v1", strlen("MAP v1"), NULL);
	clm_history_add_tool_result(&h, "c2", "examine", "a door", strlen("a door"), NULL);
	clm_history_add_assistant_text(&h, "r1", NULL);

	/* Turn 2: a fresh local_map batch (its result not yet recorded). */
	clm_history_add_user(&h, "turn2", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "c3", "local_map", "{}");

	CHECK(clm_history_supersede_tool(&h, "local_map", stub) == 1,
	    "supersede: one prior result stubbed");
	clm_history_add_tool_result(&h, "c3", "local_map", "MAP v2", strlen("MAP v2"), NULL);

	{
		int v1_stubbed = 0, v2_live = 0, examine_kept = 0;
		TAILQ_FOREACH(m, &h, entries) {
			if (m->role != CLM_ROLE_TOOL || m->content == NULL)
				continue;
			if (strcmp(m->tool_call_id, "c1") == 0 &&
			    strcmp(m->content, stub) == 0)
				v1_stubbed = 1;
			if (strcmp(m->content, "MAP v2") == 0)
				v2_live = 1;
			if (strcmp(m->content, "a door") == 0)
				examine_kept = 1;
		}
		CHECK(v1_stubbed, "supersede: old result stubbed in place");
		CHECK(v2_live, "supersede: fresh result untouched");
		CHECK(examine_kept, "supersede: other tools untouched");
	}
	/* Message count unchanged: stubbed, never removed, so every
	 * tool_call keeps its paired result. */
	CHECK(count_role(&h, CLM_ROLE_TOOL) == 3, "supersede: no orphans");

	/* Second pass with the same stub: idempotent, nothing re-stubbed. */
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "c4", "local_map", "{}");
	CHECK(clm_history_supersede_tool(&h, "local_map", stub) == 1,
	    "supersede: v2 stubbed once v3 batch opens");
	CHECK(clm_history_supersede_tool(&h, "local_map", stub) == 0,
	    "supersede: already-stubbed entries left alone");

	clm_history_free(&h);
}

/* A NO_PROMPT variant of the echo tool, to check the gate is bypassed. */
static void
echo_ok(struct clm_tool_invocation *inv, void *user)
{
	(void)user;
	clm_tool_complete(inv, "ok");
}

/* (k) Denying a gated tool completes it as failed, and the prompt fired. */
static void
test_perm_deny(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	st.perm_decision = CLM_PERM_DENY_ONCE;
	srv = canned_start(loop);
	canned_tool_call(srv, "echo_hello", "{}");
	canned_reply(srv, final_reply);
	st.agent = make_agent(&st, canned_port(srv));

	def.name = "echo_hello";
	def.description = "echo";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "perm: add tool");
	CHECK(clm_agent_submit(st.agent, "use it") == 0, "perm: submit");
	run_until_done(&st);

	CHECK(st.perm_prompts == 1, "perm: prompt fired once");
	CHECK(st.last_outcome == CLM_TOOL_FAILED, "perm: deny -> failed");
	teardown(&st, srv);
}

/* (l) A NO_PROMPT tool runs without ever firing the permission prompt. */
static void
test_perm_no_prompt(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	st.perm_decision = CLM_PERM_DENY_ONCE; /* would deny if asked */
	srv = canned_start(loop);
	canned_tool_call(srv, "echo_ok", "{}");
	canned_reply(srv, final_reply);
	st.agent = make_agent(&st, canned_port(srv));

	def.name = "echo_ok";
	def.description = "echo";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_ok;
	def.flags = CLM_TOOL_NO_PROMPT;
	CHECK(clm_tool_add(st.agent, &def) == 0, "perm: add NO_PROMPT tool");
	CHECK(clm_agent_submit(st.agent, "use it") == 0, "perm: submit");
	run_until_done(&st);

	CHECK(st.perm_prompts == 0, "perm: NO_PROMPT never prompts");
	CHECK(st.last_outcome == CLM_TOOL_OK, "perm: NO_PROMPT ran");
	teardown(&st, srv);
}

/* (m) ALLOW_ALWAYS is remembered: a second call of the same tool in the
 * session does not prompt again. */
static void
test_perm_remember(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def def = {0};

	st.loop = loop;
	st.perm_decision = CLM_PERM_ALLOW_ALWAYS;
	srv = canned_start(loop);
	/* Two separate turns, each invoking the tool once. */
	canned_tool_call(srv, "echo_hello", "{}");
	canned_reply(srv, final_reply);
	canned_tool_call(srv, "echo_hello", "{}");
	canned_reply(srv, final_reply);
	st.agent = make_agent(&st, canned_port(srv));

	def.name = "echo_hello";
	def.description = "echo";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "perm: add tool");

	CHECK(clm_agent_submit(st.agent, "first") == 0, "perm: submit 1");
	run_until_done(&st);
	st.turn_done = 0;
	CHECK(clm_agent_submit(st.agent, "second") == 0, "perm: submit 2");
	run_until_done(&st);

	CHECK(st.perm_prompts == 1, "perm: ALWAYS remembered (prompted once)");
	teardown(&st, srv);
}

/* (n) With no on_permission handler, gated tools are denied (default-deny),
 * but a NO_PROMPT tool still runs. */
static void
test_perm_no_handler(uv_loop_t *loop)
{
	static const struct clm_callbacks no_perm_cb = {
		.on_assistant_text = on_assistant_text,
		.on_tool_result = on_tool_result,
		.on_turn_done = on_turn_done,
	};
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_cfg cfg = {0};
	struct clm_tool_def def = {0};
	char url[128];

	st.loop = loop;
	srv = canned_start(loop);
	canned_tool_call(srv, "echo_hello", "{}");
	canned_reply(srv, final_reply);

	(void)snprintf(url, sizeof(url),
	    "http://127.0.0.1:%d/v1/chat/completions", canned_port(srv));
	cfg.api_key = "test";
	cfg.base_url = url;
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = "test-model";
	CHECK(clm_host_uv_new(loop, &st.host) == 0, "perm: host");
	CHECK(clm_agent_new(&cfg, st.host, &no_perm_cb, &st, &st.agent) == 0,
	    "perm: agent with no handler");

	def.name = "echo_hello";
	def.description = "echo";
	def.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	def.invoke = echo_hello;
	CHECK(clm_tool_add(st.agent, &def) == 0, "perm: add tool");
	CHECK(clm_agent_submit(st.agent, "use it") == 0, "perm: submit");
	run_until_done(&st);

	CHECK(st.last_outcome == CLM_TOOL_FAILED,
	    "perm: no handler -> denied");
	clm_agent_free(st.agent);
	clm_host_uv_free(st.host);
	canned_stop(srv);
	uv_run(loop, UV_RUN_DEFAULT);
}

/* (o) A HIDDEN tool is not advertised to the model, but a visible one is. */
static void
test_hidden_tool(uv_loop_t *loop)
{
	struct tstate st = {0};
	struct canned_server *srv;
	struct clm_tool_def vis = {0}, hid = {0};
	const char *req;

	st.loop = loop;
	srv = canned_start(loop);
	canned_reply(srv, final_reply); /* plain answer; we inspect the request */
	st.agent = make_agent(&st, canned_port(srv));

	vis.name = "visible_tool";
	vis.description = "seen";
	vis.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	vis.invoke = echo_ok;
	CHECK(clm_tool_add(st.agent, &vis) == 0, "hidden: add visible");

	hid.name = "secret_tool";
	hid.description = "unseen";
	hid.params_schema = "{\"type\":\"object\",\"properties\":{}}";
	hid.invoke = echo_ok;
	hid.flags = CLM_TOOL_HIDDEN;
	CHECK(clm_tool_add(st.agent, &hid) == 0, "hidden: add hidden");

	CHECK(clm_agent_submit(st.agent, "hi") == 0, "hidden: submit");
	run_until_done(&st);

	req = canned_last_request(srv);
	CHECK(req != NULL && strstr(req, "visible_tool") != NULL,
	    "hidden: visible tool advertised");
	CHECK(req != NULL && strstr(req, "secret_tool") == NULL,
	    "hidden: hidden tool NOT advertised");
	teardown(&st, srv);
}

int
main(void)
{
	uv_loop_t loop;

	uv_loop_init(&loop);
	test_text_reply(&loop);
	test_tool_call(&loop);
	test_binary_tool_output(&loop);
	test_bg_exec(&loop);
	test_autocompact_mid_chain(&loop);
	test_compact_reasoning_fallback(&loop);
	test_tools_unsupported_retry(&loop);
	test_file_tools(&loop);
	test_shell_exec(&loop);
	test_agent_free_during_shell_exec(&loop);
	test_stream_text(&loop);
	test_stream_tool(&loop);
	test_stream_meta(&loop);
	test_anthropic_text_reply(&loop);
	test_anthropic_tool_call(&loop);
	test_anthropic_stream(&loop);
	test_recover_after_error(&loop);
	test_set_provider_after_error(&loop);
	test_parse_props();
	test_parse_models_list();
	test_history_compact();
	test_history_compact_agentic();
	test_history_supersede();
	test_perm_deny(&loop);
	test_perm_no_prompt(&loop);
	test_perm_remember(&loop);
	test_perm_no_handler(&loop);
	test_hidden_tool(&loop);
	uv_loop_close(&loop);

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
