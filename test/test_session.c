// SPDX-License-Identifier: ISC
/*
 * Unit tests for libclmsession + the lossless message round-trip in
 * libclm/history.c. Pure: no network, no event loop -- a mkdtemp'd
 * session dir stands in for $XDG_STATE_HOME.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "clm/history.h"
#include "clm/session.h"
#include "tap.h"

#define CHECK(cond, msg) TAP_CHECK(cond, msg)

/* Build the canonical 5-message conversation used by several tests. */
static void
fill_history(struct clm_history *h)
{
	struct clm_message *m;

	clm_history_add_system(h, "you are test", NULL);
	clm_history_add_user(
	    h, "hello\nworld \"quoted\" \xf0\x9f\xa6\xb4", NULL);
	m = clm_history_add_assistant_tool_calls(h);
	clm_message_add_tool_call(
	    m, "call_1", "shell_exec", "{\"cmd\":\"ls\"}");
	clm_message_add_tool_call(
	    m, "call_2", "read_file", "{\"path\":\"/etc/motd\"}");
	clm_history_add_tool_result(h, "call_1", "shell_exec", "file1\nfile2",
	    strlen("file1\nfile2"), NULL);
	clm_history_add_assistant_text(h, "there are two files", NULL);
}

static size_t
history_len(const struct clm_history *h)
{
	const struct clm_message *m;
	size_t n = 0;

	TAILQ_FOREACH(m, h, entries)
	n++;
	return n;
}

static void
test_message_roundtrip(void)
{
	struct clm_history in, out;
	const struct clm_message *m, *r;

	clm_history_init(&in);
	clm_history_init(&out);
	fill_history(&in);

	TAILQ_FOREACH(m, &in, entries)
	{
		cJSON *obj = clm_message_to_json_full(m, NULL);
		CHECK(obj != NULL, "serialize message");
		CHECK(clm_message_from_json(&out, obj, NULL) == 0,
		    "deserialize message");
		cJSON_Delete(obj);
	}

	CHECK(history_len(&out) == history_len(&in), "same message count");

	r = TAILQ_FIRST(&out);
	CHECK(r->role == CLM_ROLE_SYSTEM, "system role survives");
	CHECK(strcmp(r->content, "you are test") == 0, "system content");

	r = TAILQ_NEXT(r, entries);
	CHECK(r->role == CLM_ROLE_USER, "user role survives");
	CHECK(
	    strcmp(r->content, "hello\nworld \"quoted\" \xf0\x9f\xa6\xb4") == 0,
	    "newlines/quotes/utf-8 survive");

	r = TAILQ_NEXT(r, entries);
	CHECK(r->role == CLM_ROLE_ASSISTANT, "assistant tool-call role");
	CHECK(r->content == NULL, "tool-call message has no content");
	{
		const struct clm_tool_call *tc = TAILQ_FIRST(&r->tool_calls);
		CHECK(tc != NULL && strcmp(tc->id, "call_1") == 0 &&
		        strcmp(tc->name, "shell_exec") == 0 &&
		        strcmp(tc->args, "{\"cmd\":\"ls\"}") == 0,
		    "first tool call survives");
		tc = tc != NULL ? TAILQ_NEXT(tc, entries) : NULL;
		CHECK(tc != NULL && strcmp(tc->id, "call_2") == 0,
		    "second tool call survives");
	}

	r = TAILQ_NEXT(r, entries);
	CHECK(r->role == CLM_ROLE_TOOL, "tool result role");
	CHECK(r->tool_call_id != NULL && strcmp(r->tool_call_id, "call_1") == 0,
	    "tool_call_id survives");
	CHECK(r->tool_name != NULL && strcmp(r->tool_name, "shell_exec") == 0,
	    "tool_name survives (the field the wire format drops)");

	r = TAILQ_NEXT(r, entries);
	CHECK(r->role == CLM_ROLE_ASSISTANT, "assistant text role");
	CHECK(strcmp(r->content, "there are two files") == 0,
	    "assistant text survives");

	clm_history_free(&in);
	clm_history_free(&out);
}

static void
test_from_json_rejects_garbage(void)
{
	struct clm_history h;
	cJSON *obj;

	clm_history_init(&h);

	CHECK(clm_message_from_json(&h, NULL, NULL) == -EINVAL, "NULL obj");

	obj = cJSON_Parse("{\"role\":\"emperor\",\"content\":\"hi\"}");
	CHECK(clm_message_from_json(&h, obj, NULL) == -EINVAL, "bad role");
	cJSON_Delete(obj);

	obj = cJSON_Parse("{\"content\":\"hi\"}");
	CHECK(clm_message_from_json(&h, obj, NULL) == -EINVAL, "no role");
	cJSON_Delete(obj);

	obj = cJSON_Parse("{\"role\":\"assistant\",\"tool_calls\":\"nope\"}");
	CHECK(clm_message_from_json(&h, obj, NULL) == -EINVAL,
	    "non-array tool_calls");
	cJSON_Delete(obj);

	CHECK(history_len(&h) == 0, "nothing appended on rejects");
	clm_history_free(&h);
}

static void
test_session_file_roundtrip(const char *dir)
{
	struct clm_session *s = NULL;
	struct clm_history in, out;
	struct clm_message *m;
	cJSON *meta = NULL;
	char id[128];

	clm_history_init(&in);
	clm_history_init(&out);
	fill_history(&in);

	CHECK(clm_session_create(
	          dir, "test-model", "test-provider", "test-agent", &s) == 0,
	    "create session");
	CHECK(clm_session_is_empty(s), "fresh session is empty");

	TAILQ_FOREACH(m, &in, entries)
	{
		if (m->role == CLM_ROLE_SYSTEM)
			continue;
		CHECK(clm_session_append(s, m, NULL) == 0, "append message");
	}
	CHECK(!clm_session_is_empty(s), "appended session not empty");

	(void)snprintf(id, sizeof(id), "%s", clm_session_id(s));
	clm_session_free(s);

	CHECK(clm_session_load(dir, id, &out, &meta) == 0, "load session");
	CHECK(history_len(&out) == history_len(&in) - 1,
	    "all non-system messages loaded");
	CHECK(meta != NULL, "meta line parsed");
	if (meta != NULL) {
		CHECK(
		    strcmp(cJSON_GetStringValue(
		               cJSON_GetObjectItemCaseSensitive(meta, "model")),
		        "test-model") == 0,
		    "meta model");
		cJSON_Delete(meta);
	}

	/* Reopen for appending: keeps counting as non-empty. */
	s = NULL;
	CHECK(clm_session_open(dir, id, &s) == 0, "reopen session");
	CHECK(!clm_session_is_empty(s), "reopened session not empty");
	CHECK(clm_session_discard(s) == 0, "discard deletes");
	CHECK(clm_session_load(dir, id, &out, NULL) == -ENOENT,
	    "discarded session gone");

	clm_history_free(&in);
	clm_history_free(&out);
}

static void
append_raw(const char *dir, const char *id, const char *bytes)
{
	char path[512];
	FILE *f;

	(void)snprintf(path, sizeof(path), "%s/%s.jsonl", dir, id);
	f = fopen(path, "a");
	if (f != NULL) {
		fputs(bytes, f);
		fclose(f);
	}
}

static void
test_crash_tolerance(const char *dir)
{
	struct clm_session *s = NULL;
	struct clm_history h;
	char id[128];

	clm_history_init(&h);

	CHECK(clm_session_create(dir, NULL, NULL, NULL, &s) == 0,
	    "create session");
	(void)snprintf(id, sizeof(id), "%s", clm_session_id(s));
	{
		struct clm_history tmp;
		struct clm_message *m;
		clm_history_init(&tmp);
		m = clm_history_add_user(&tmp, "hi", NULL);
		CHECK(clm_session_append(s, m, NULL) == 0, "append");
		clm_history_free(&tmp);
	}
	clm_session_free(s);

	/* Garbage middle line + truncated (no newline) final line: both
	 * must be skipped, the good message kept. */
	append_raw(dir, id, "this is not json\n");
	append_raw(dir, id, "{\"type\":\"msg\",\"role\":\"assistant\",\"con");

	CHECK(
	    clm_session_load(dir, id, &h, NULL) == 0, "load survives garbage");
	CHECK(history_len(&h) == 1, "only the good message loaded");
	clm_history_free(&h);

	/* A too-new meta version must refuse to load. */
	clm_history_init(&h);
	append_raw(dir, id, "\n{\"type\":\"meta\",\"v\":99}\n");
	CHECK(clm_session_load(dir, id, &h, NULL) == -EPROTONOSUPPORT,
	    "newer format version rejected");
	clm_history_free(&h);
}

/* clm_history_repair_dangling_tool_calls: the crash-recovery repair applied
 * to a resumed session's in-memory history before it ever reaches a
 * provider (see clm/history.h's doc comment). */
static void
test_dangling_tool_call_repair(const char *dir)
{
	struct clm_session *s = NULL;
	struct clm_history h;
	struct clm_message *m;
	const struct clm_message *r;
	char id[128];

	/* Single dangling call at the very end -- the crash-mid-tool-call
	 * case that motivated this. */
	clm_history_init(&h);
	clm_history_add_user(&h, "run something", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(
	    m, "call_1", "shell_exec", "{\"cmd\":\"ls\"}");

	CHECK(clm_history_repair_dangling_tool_calls(&h) == 1,
	    "one dangling call repaired");
	CHECK(history_len(&h) == 3, "synthetic result appended");
	r = TAILQ_LAST(&h, clm_history);
	CHECK(r->role == CLM_ROLE_TOOL, "synthetic message is a tool result");
	CHECK(r->tool_call_id != NULL && strcmp(r->tool_call_id, "call_1") == 0,
	    "synthetic result targets the dangling call id");
	CHECK(r->tool_name != NULL && strcmp(r->tool_name, "shell_exec") == 0,
	    "synthetic result keeps the tool name");
	CHECK(r->content != NULL && strstr(r->content, "missing") != NULL,
	    "synthetic content says the result is missing");

	/* Re-running on an already-repaired history is a no-op: the repair
	 * must not stack placeholders on a session resumed twice. */
	CHECK(clm_history_repair_dangling_tool_calls(&h) == 0,
	    "repair is idempotent");
	CHECK(history_len(&h) == 3, "no extra messages added on re-run");
	clm_history_free(&h);

	/* Partial batch: two calls, only one has a result. Order of the
	 * existing result relative to insertion must be preserved and the
	 * missing one filled in right after. */
	clm_history_init(&h);
	clm_history_add_user(&h, "run two things", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "call_a", "shell_exec", "{}");
	clm_message_add_tool_call(m, "call_b", "shell_exec", "{}");
	clm_history_add_tool_result(
	    &h, "call_a", "shell_exec", "ok", strlen("ok"), NULL);

	CHECK(clm_history_repair_dangling_tool_calls(&h) == 1,
	    "one of two dangling calls repaired");
	CHECK(history_len(&h) == 4, "one synthetic result added");
	r = TAILQ_FIRST(&h);
	r = TAILQ_NEXT(r, entries); /* assistant tool_calls */
	r = TAILQ_NEXT(r, entries); /* first tool result: call_a, real */
	CHECK(r->role == CLM_ROLE_TOOL &&
	        strcmp(r->tool_call_id, "call_a") == 0 &&
	        strcmp(r->content, "ok") == 0,
	    "real result for call_a untouched and stays first");
	r = TAILQ_NEXT(r, entries); /* second tool result: call_b, synthetic */
	CHECK(
	    r->role == CLM_ROLE_TOOL && strcmp(r->tool_call_id, "call_b") == 0,
	    "synthetic result for call_b follows it");
	clm_history_free(&h);

	/* Two back-to-back tool_calls batches with nothing in between:
	 * both calls in the first batch are dangling (the second batch
	 * starting immediately after ends the "immediately following tool
	 * messages" run for the first). */
	clm_history_init(&h);
	clm_history_add_user(&h, "go", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "call_x", "shell_exec", "{}");
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "call_y", "shell_exec", "{}");

	CHECK(clm_history_repair_dangling_tool_calls(&h) == 2,
	    "both batches' calls repaired");
	CHECK(
	    history_len(&h) == 5, "two synthetic results added, one per batch");
	clm_history_free(&h);

	/* Well-formed history: nothing to do. Note fill_history()'s canonical
	 * fixture is not usable here -- it deliberately only resolves
	 * call_1 of its two tool calls (see its comment), so it is itself a
	 * dangling-call fixture, not a clean one. */
	clm_history_init(&h);
	clm_history_add_user(&h, "run one thing", NULL);
	m = clm_history_add_assistant_tool_calls(&h);
	clm_message_add_tool_call(m, "call_1", "shell_exec", "{}");
	clm_history_add_tool_result(
	    &h, "call_1", "shell_exec", "ok", strlen("ok"), NULL);
	clm_history_add_assistant_text(&h, "done", NULL);
	CHECK(clm_history_repair_dangling_tool_calls(&h) == 0,
	    "clean history needs no repair");
	CHECK(history_len(&h) == 4, "clean history unchanged");
	clm_history_free(&h);

	/* End to end: a session file left mid tool-call by a crash loads
	 * and, once repaired, is a well-formed history again. */
	CHECK(clm_session_create(dir, NULL, NULL, NULL, &s) == 0,
	    "create session for crash-mid-tool-call fixture");
	(void)snprintf(id, sizeof(id), "%s", clm_session_id(s));
	{
		struct clm_history tmp;
		struct clm_message *tm;

		clm_history_init(&tmp);
		clm_history_add_user(&tmp, "do a thing", NULL);
		CHECK(clm_session_append(s, TAILQ_FIRST(&tmp), NULL) == 0,
		    "append user message");
		tm = clm_history_add_assistant_tool_calls(&tmp);
		clm_message_add_tool_call(
		    tm, "call_crash", "shell_exec", "{\"cmd\":\"doas ...\"}");
		CHECK(clm_session_append(s, tm, NULL) == 0,
		    "append dangling tool_calls message (session then dies)");
		clm_history_free(&tmp);
	}
	clm_session_free(s);

	clm_history_init(&h);
	CHECK(clm_session_load(dir, id, &h, NULL) == 0,
	    "load crash-mid-tool-call session");
	CHECK(history_len(&h) == 2, "loaded exactly the two logged messages");
	CHECK(clm_history_repair_dangling_tool_calls(&h) == 1,
	    "loaded session repaired on resume");
	CHECK(history_len(&h) == 3, "session now well-formed");
	r = TAILQ_LAST(&h, clm_history);
	CHECK(r->role == CLM_ROLE_TOOL &&
	        strcmp(r->tool_call_id, "call_crash") == 0,
	    "repaired result targets the crashed call");
	clm_history_free(&h);
}

static void
test_id_validation(const char *dir)
{
	struct clm_session *s = NULL;
	struct clm_history h;

	clm_history_init(&h);
	CHECK(clm_session_open(dir, "../evil", &s) == -EINVAL,
	    "path traversal id rejected on open");
	CHECK(clm_session_load(dir, "a/b", &h, NULL) == -EINVAL,
	    "slash id rejected on load");
	CHECK(clm_session_open(dir, "", &s) == -EINVAL, "empty id rejected");
	CHECK(clm_session_open(dir, "no-such-session", &s) == -ENOENT,
	    "missing session is ENOENT");
	clm_history_free(&h);
}

static void
test_listing(const char *dir)
{
	struct clm_session_info *infos = NULL;
	size_t n = 0;

	/* Two fresh sessions with one user message each. */
	for (int i = 0; i < 2; i++) {
		struct clm_session *s = NULL;
		struct clm_history tmp;
		struct clm_message *m;

		CHECK(clm_session_create(dir, "m", NULL, NULL, &s) == 0,
		    "create listed session");
		clm_history_init(&tmp);
		m = clm_history_add_user(&tmp, "list me", NULL);
		CHECK(clm_session_append(s, m, NULL) == 0, "append");
		clm_history_free(&tmp);
		clm_session_free(s);
	}

	CHECK(clm_session_list(dir, &infos, &n) == 0, "list sessions");
	CHECK(n >= 2, "listing sees the sessions created above");
	for (size_t i = 1; i < n; i++)
		CHECK(infos[i - 1].created >= infos[i].created, "newest first");
	for (size_t i = 0; i < n; i++)
		CHECK(infos[i].id != NULL, "every row has an id");
	clm_session_list_free(infos, n);

	CHECK(clm_session_list("/nonexistent-dir-xyzzy", &infos, &n) == 0 &&
	        n == 0,
	    "missing dir is an empty listing");
}

static void
remove_dir(const char *dir)
{
	DIR *d;
	struct dirent *ent;
	char path[512];

	d = opendir(dir);
	if (d == NULL)
		return;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		(void)snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		(void)unlink(path);
	}
	(void)closedir(d);
	(void)rmdir(dir);
}

static int
test_session_suite(void *arg)
{
	(void)arg;
	char dir[] = "/tmp/clm-session-test-XXXXXX";

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	test_message_roundtrip();
	test_from_json_rejects_garbage();
	test_session_file_roundtrip(dir);
	test_crash_tolerance(dir);
	test_dangling_tool_call_repair(dir);
	test_id_validation(dir);
	test_listing(dir);
	remove_dir(dir);

	return 0;
}

int
main(void)
{
	TAP_ADD("session", test_session_suite, NULL);
	return tap_run();
}
