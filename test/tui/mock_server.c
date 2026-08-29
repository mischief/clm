// SPDX-License-Identifier: ISC
#include <sys/stat.h>
#include <sys/types.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <dirent.h>

#include <arpa/inet.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "mock_server.h"

/* Enough calls to outrun the agent's tool rate limit, so the tail of a batch
 * is still parked when a test cancels the turn. */
#define MANY_CALLS 16

/* Ordinary replies complete quickly. A prompt naming "slowtest" streams
 * slowly enough to leave a real busy window to act in. */
#define CHUNK_DELAY_MS 20
#define SLOW_DELAY_MS 120

/* The canned assistant reply: small, but covering the interesting markdown
 * -- a heading, bold and italic runs, a bullet list, and a table. */
static const char REPLY_MD[] = "## Fruit\n"
                               "\n"
                               "A **bold** word and an *italic* word.\n"
                               "\n"
                               "- Apple\n"
                               "- Banana\n"
                               "- Orange\n"
                               "\n"
                               "| Fruit | Colour |\n"
                               "| --- | --- |\n"
                               "| Apple | Red |\n"
                               "| Banana | Yellow |\n";

static const char SSE_HEAD[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/event-stream\r\n"
                               "Cache-Control: no-cache\r\n"
                               "Connection: close\r\n\r\n";

struct frame {
	char *data;
	uint64_t delay_ms;
};

struct conn {
	struct mock_server *srv;
	struct conn *next_conn;
	uv_tcp_t handle;
	uv_timer_t timer;
	char *buf;
	size_t len;
	size_t cap;
	bool served;
	struct frame *frames;
	size_t nframes;
	size_t next;
	size_t inflight;
	bool finishing;
	int to_close;
};

struct mock_server {
	uv_loop_t loop;
	uv_tcp_t listener;
	uv_async_t stopper;
	uv_thread_t thread;
	struct conn *conns;
	bool running;
	char url[64];
	char scratch[64];
	uv_mutex_t lock;
	char log_path[1024];
};

struct wreq {
	uv_write_t req;
	struct conn *conn;
	char *data;
};

int
mock_many_calls(void)
{
	return MANY_CALLS;
}

/* Case-insensitive substring search, so the scenario keywords match however
 * the prompt was typed. */
static bool
has_ci(const char *hay, const char *needle)
{
	size_t n;

	if (hay == NULL || needle == NULL)
		return false;
	n = strlen(needle);
	for (; *hay != '\0'; hay++) {
		if (strncasecmp(hay, needle, n) == 0)
			return true;
	}
	return false;
}

static const char *
msg_role(const cJSON *m)
{
	const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");

	return cJSON_IsString(r) ? r->valuestring : "";
}

static const char *
msg_content(const cJSON *m)
{
	const cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "content");

	return cJSON_IsString(c) ? c->valuestring : "";
}

/* ---- response frames ---- */

static void
frame_add(struct conn *c, char *data, uint64_t delay_ms)
{
	struct frame *f;

	if (data == NULL)
		return;
	f = realloc(c->frames, (c->nframes + 1) * sizeof(*f));
	if (f == NULL) {
		free(data);
		return;
	}
	c->frames = f;
	c->frames[c->nframes].data = data;
	c->frames[c->nframes].delay_ms = delay_ms;
	c->nframes++;
}

static char *
sse_frame(cJSON *obj)
{
	char *json = cJSON_PrintUnformatted(obj);
	char *out;
	size_t n;

	cJSON_Delete(obj);
	if (json == NULL)
		return NULL;
	n = strlen(json) + sizeof("data: \n\n");
	out = malloc(n);
	if (out != NULL)
		(void)snprintf(out, n, "data: %s\n\n", json);
	free(json);
	return out;
}

static char *
dup_str(const char *s)
{
	char *p = malloc(strlen(s) + 1);

	if (p != NULL)
		memcpy(p, s, strlen(s) + 1);
	return p;
}

/* One streamed tool call, as the wire format delivers it. */
static char *
tool_call_frame(int index, const char *id, const char *name, const char *args)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *delta = cJSON_CreateObject();
	cJSON *calls = cJSON_CreateArray();
	cJSON *call = cJSON_CreateObject();
	cJSON *fn = cJSON_CreateObject();

	cJSON_AddItemToObject(root, "choices", choices);
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_AddItemToObject(choice, "delta", delta);
	cJSON_AddItemToObject(delta, "tool_calls", calls);
	cJSON_AddItemToArray(calls, call);
	cJSON_AddNumberToObject(call, "index", index);
	cJSON_AddStringToObject(call, "id", id);
	cJSON_AddStringToObject(call, "type", "function");
	cJSON_AddItemToObject(call, "function", fn);
	cJSON_AddStringToObject(fn, "name", name);
	cJSON_AddStringToObject(fn, "arguments", args);
	return sse_frame(root);
}

static char *
finish_frame(const char *reason)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();

	cJSON_AddItemToObject(root, "choices", choices);
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_AddItemToObject(choice, "delta", cJSON_CreateObject());
	cJSON_AddStringToObject(choice, "finish_reason", reason);
	return sse_frame(root);
}

static char *
content_frame(const char *text, size_t len)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *delta = cJSON_CreateObject();
	char *piece = malloc(len + 1);

	if (piece == NULL) {
		cJSON_Delete(root);
		cJSON_Delete(choices);
		cJSON_Delete(choice);
		cJSON_Delete(delta);
		return NULL;
	}
	memcpy(piece, text, len);
	piece[len] = '\0';
	cJSON_AddItemToObject(root, "choices", choices);
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_AddItemToObject(choice, "delta", delta);
	cJSON_AddStringToObject(delta, "content", piece);
	free(piece);
	return sse_frame(root);
}

/* llama.cpp-style final frame: usage plus timings, no choices. */
static char *
usage_frame(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *usage = cJSON_CreateObject();
	cJSON *timings = cJSON_CreateObject();

	cJSON_AddItemToObject(root, "choices", cJSON_CreateArray());
	cJSON_AddItemToObject(root, "usage", usage);
	cJSON_AddNumberToObject(usage, "prompt_tokens", 11);
	cJSON_AddNumberToObject(usage, "completion_tokens", 42);
	cJSON_AddNumberToObject(usage, "total_tokens", 53);
	cJSON_AddItemToObject(root, "timings", timings);
	cJSON_AddNumberToObject(timings, "predicted_per_second", 20.0);
	return sse_frame(root);
}

static char *
http_json(const char *body)
{
	size_t n = strlen(body) + 256;
	char *out = malloc(n);

	if (out != NULL) {
		(void)snprintf(out, n,
		    "HTTP/1.1 200 OK\r\n"
		    "Content-Type: application/json\r\n"
		    "Content-Length: %zu\r\n"
		    "Connection: close\r\n\r\n%s",
		    strlen(body), body);
	}
	return out;
}

/* ---- scripted scenarios ---- */

static void
script_tool_call(struct conn *c, const char *text)
{
	cJSON *args = cJSON_CreateObject();
	const char *name = "shell_exec";
	char *json;

	if (has_ci(text, "escapetest")) {
		/* nroff bold ("c\bc"), an underline pair, and a colour
		 * escape: what captured terminal output really looks like. */
		cJSON_AddStringToObject(args, "command",
		    "printf 'N\\bNA\\bAM\\bME\\bE _\\bi_\\bn_\\bt "
		    "\\033[31mred\\033[0m\\n'");
	} else if (has_ci(text, "edittest")) {
		/* Key order a model is free to pick, and the one that reads
		 * backwards if the tui renders keys as they arrive. */
		name = "edit_file";
		cJSON_AddStringToObject(args, "new_str", "after text");
		cJSON_AddBoolToObject(args, "replace_all", 0);
		cJSON_AddStringToObject(args, "old_str", "before text");
		cJSON_AddStringToObject(args, "path", "/tmp/x");
	} else if (has_ci(text, "multilinetest")) {
		cJSON_AddStringToObject(
		    args, "command", "printf one\ncat /tmp/x\ndoas true");
		cJSON_AddStringToObject(
		    args, "stdin", "first line\nsecond line");
		cJSON_AddNumberToObject(args, "timeout_ms", 10000);
	} else {
		cJSON_AddStringToObject(args, "command", "echo hi");
	}
	json = cJSON_PrintUnformatted(args);
	cJSON_Delete(args);
	if (json == NULL)
		return;
	frame_add(c, dup_str(SSE_HEAD), 0);
	frame_add(c, tool_call_frame(0, "call_1", name, json), 0);
	free(json);
	frame_add(c, finish_frame("tool_calls"), 0);
	frame_add(c, dup_str("data: [DONE]\n\n"), 0);
}

static void
script_many_calls(struct conn *c)
{
	int i;

	frame_add(c, dup_str(SSE_HEAD), 0);
	for (i = 0; i < MANY_CALLS; i++) {
		cJSON *args = cJSON_CreateObject();
		char id[32];
		char cmd[256];
		char *json;

		(void)snprintf(
		    cmd, sizeof(cmd), "touch %s/ran%d", c->srv->scratch, i);
		(void)snprintf(id, sizeof(id), "call_%d", i);
		cJSON_AddStringToObject(args, "command", cmd);
		json = cJSON_PrintUnformatted(args);
		cJSON_Delete(args);
		if (json == NULL)
			continue;
		frame_add(c, tool_call_frame(i, id, "shell_exec", json), 0);
		free(json);
	}
	frame_add(c, finish_frame("tool_calls"), 0);
	frame_add(c, dup_str("data: [DONE]\n\n"), 0);
}

static void
script_peer_send(struct conn *c, const char *target)
{
	cJSON *args = cJSON_CreateObject();
	char *json;

	cJSON_AddStringToObject(args, "to", target);
	cJSON_AddStringToObject(args, "text", "hello from the other agent");
	json = cJSON_PrintUnformatted(args);
	cJSON_Delete(args);
	if (json == NULL)
		return;
	frame_add(c, dup_str(SSE_HEAD), 0);
	frame_add(c, tool_call_frame(0, "call_p", "agent_send", json), 0);
	free(json);
	frame_add(c, finish_frame("tool_calls"), 0);
	frame_add(c, dup_str("data: [DONE]\n\n"), 0);
}

static void
script_markdown(struct conn *c, uint64_t delay)
{
	size_t len = sizeof(REPLY_MD) - 1;
	size_t step = len / 8;
	size_t i;

	if (step == 0)
		step = 1;
	frame_add(c, dup_str(SSE_HEAD), 0);
	for (i = 0; i < len; i += step) {
		size_t n = len - i < step ? len - i : step;
		frame_add(
		    c, content_frame(REPLY_MD + i, n), i == 0 ? 0 : delay);
	}
	frame_add(c, finish_frame("stop"), delay);
	frame_add(c, usage_frame(), 0);
	frame_add(c, dup_str("data: [DONE]\n\n"), 0);
}

static void
script_whole(struct conn *c)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *choices = cJSON_CreateArray();
	cJSON *choice = cJSON_CreateObject();
	cJSON *message = cJSON_CreateObject();
	cJSON *usage = cJSON_CreateObject();
	char *json;

	cJSON_AddItemToObject(root, "choices", choices);
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_AddItemToObject(choice, "message", message);
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(message, "content", REPLY_MD);
	cJSON_AddStringToObject(choice, "finish_reason", "stop");
	cJSON_AddItemToObject(root, "usage", usage);
	cJSON_AddNumberToObject(usage, "prompt_tokens", 11);
	cJSON_AddNumberToObject(usage, "completion_tokens", 42);
	cJSON_AddNumberToObject(usage, "total_tokens", 53);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (json == NULL)
		return;
	frame_add(c, http_json(json), 0);
	free(json);
}

/* ---- request inspection ---- */

/* True when the latest user turn asks for a tool and its own result is not
 * back yet, so each such turn emits the call exactly once. */
static bool
wants_tool(const cJSON *msgs)
{
	static const char *const words[] = {
	    "shelltest", "multilinetest", "manytest", "escapetest", "edittest"};
	const cJSON *m;
	const char *last = NULL;
	int i = 0;
	int last_user = -1;
	int n = 0;
	size_t w;
	bool asked = false;

	cJSON_ArrayForEach(m, msgs)
	{
		if (strcmp(msg_role(m), "user") == 0) {
			last_user = n;
			last = msg_content(m);
		}
		n++;
	}
	if (last_user < 0)
		return false;
	for (w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
		if (has_ci(last, words[w]))
			asked = true;
	}
	if (!asked)
		return false;
	cJSON_ArrayForEach(m, msgs)
	{
		if (i++ < last_user)
			continue;
		if (strcmp(msg_role(m), "tool") == 0)
			return false;
		if (has_ci(msg_content(m), "<tool_response>"))
			return false;
	}
	return true;
}

/* A prompt of the form "peersend to=<id>" asks for one agent_send call.
 * Copies the target into out and returns true. */
static bool
peer_target(const cJSON *msgs, char *out, size_t outlen)
{
	const cJSON *m;

	cJSON_ArrayForEach(m, msgs)
	{
		if (strcmp(msg_role(m), "tool") == 0)
			return false;
	}
	cJSON_ArrayForEach(m, msgs)
	{
		const char *c = msg_content(m);
		const char *p;

		if (strcmp(msg_role(m), "user") != 0 ||
		    strstr(c, "peersend") == NULL)
			continue;
		for (p = c; p != NULL; p = strchr(p, ' ')) {
			size_t n;

			while (*p == ' ')
				p++;
			if (strncmp(p, "to=", 3) != 0)
				continue;
			p += 3;
			n = strcspn(p, " \t\r\n");
			if (n + 1 > outlen)
				n = outlen - 1;
			memcpy(out, p, n);
			out[n] = '\0';
			return true;
		}
	}
	return false;
}

static void
log_request(struct mock_server *srv, const char *body)
{
	char path[sizeof(srv->log_path)];
	cJSON *root;
	char *json;
	FILE *f;

	uv_mutex_lock(&srv->lock);
	(void)snprintf(path, sizeof(path), "%s", srv->log_path);
	uv_mutex_unlock(&srv->lock);
	if (path[0] == '\0')
		return;
	root = cJSON_Parse(body);
	if (root == NULL)
		return;
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (json == NULL)
		return;
	f = fopen(path, "a");
	if (f != NULL) {
		(void)fprintf(f, "%s\n", json);
		(void)fclose(f);
	}
	free(json);
}

static void
build_reply(struct conn *c, const char *body)
{
	cJSON *root = cJSON_Parse(body);
	const cJSON *msgs;
	const cJSON *stream;
	const cJSON *m;
	char target[128];
	uint64_t delay = CHUNK_DELAY_MS;

	log_request(c->srv, body);
	if (root == NULL) {
		script_markdown(c, CHUNK_DELAY_MS);
		return;
	}
	msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
	stream = cJSON_GetObjectItemCaseSensitive(root, "stream");
	if (!cJSON_IsTrue(stream)) {
		script_whole(c);
		cJSON_Delete(root);
		return;
	}
	if (peer_target(msgs, target, sizeof(target))) {
		script_peer_send(c, target);
		cJSON_Delete(root);
		return;
	}
	if (wants_tool(msgs)) {
		const char *last = "";

		cJSON_ArrayForEach(m, msgs)
		{
			if (strcmp(msg_role(m), "user") == 0)
				last = msg_content(m);
		}
		if (has_ci(last, "manytest"))
			script_many_calls(c);
		else
			script_tool_call(c, last);
		cJSON_Delete(root);
		return;
	}
	cJSON_ArrayForEach(m, msgs)
	{
		if (has_ci(msg_content(m), "slowtest"))
			delay = SLOW_DELAY_MS;
	}
	script_markdown(c, delay);
	cJSON_Delete(root);
}

/* ---- connection plumbing ---- */

static void
on_handle_closed(uv_handle_t *handle)
{
	struct conn *c = handle->data;
	struct conn **pp;
	size_t i;

	if (--c->to_close > 0)
		return;
	for (pp = &c->srv->conns; *pp != NULL; pp = &(*pp)->next_conn) {
		if (*pp == c) {
			*pp = c->next_conn;
			break;
		}
	}
	for (i = 0; i < c->nframes; i++)
		free(c->frames[i].data);
	free(c->frames);
	free(c->buf);
	free(c);
}

static void
conn_close(struct conn *c)
{
	if (c->to_close > 0)
		return;
	c->to_close = 2;
	uv_close((uv_handle_t *)&c->timer, on_handle_closed);
	uv_close((uv_handle_t *)&c->handle, on_handle_closed);
}

static void
on_write(uv_write_t *req, int status)
{
	struct wreq *w = (struct wreq *)req;
	struct conn *c = w->conn;

	free(w->data);
	free(w);
	c->inflight--;
	/* The client reads until the connection closes, so the close has to
	 * wait for the last frame to reach the kernel. */
	if (status < 0 || (c->finishing && c->inflight == 0))
		conn_close(c);
}

static void
on_tick(uv_timer_t *timer)
{
	struct conn *c = timer->data;
	struct wreq *w;
	uv_buf_t b;
	size_t n;

	w = calloc(1, sizeof(*w));
	if (w == NULL) {
		conn_close(c);
		return;
	}
	w->conn = c;
	w->data = c->frames[c->next].data;
	c->frames[c->next].data = NULL;
	n = strlen(w->data);
	c->next++;
	b = uv_buf_init(w->data, (unsigned)n);
	if (uv_write(&w->req, (uv_stream_t *)&c->handle, &b, 1, on_write) !=
	    0) {
		free(w->data);
		free(w);
		conn_close(c);
		return;
	}
	c->inflight++;
	if (c->next < c->nframes)
		uv_timer_start(
		    &c->timer, on_tick, c->frames[c->next].delay_ms, 0);
	else
		c->finishing = true;
}

static void
alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base != NULL ? suggested : 0;
}

static long
content_length(const char *buf)
{
	const char *p = buf;

	while (*p != '\0') {
		const char *nl;

		if (strncasecmp(p, "content-length:", 15) == 0)
			return strtol(p + 15, NULL, 10);
		nl = strchr(p, '\n');
		if (nl == NULL)
			break;
		p = nl + 1;
	}
	return -1;
}

static bool
request_complete(const struct conn *c, const char **body_out)
{
	const char *end;
	size_t hlen;
	long clen;

	if (c->buf == NULL)
		return false;
	end = strstr(c->buf, "\r\n\r\n");
	if (end == NULL)
		return false;
	hlen = (size_t)(end - c->buf) + 4;
	clen = content_length(c->buf);
	*body_out = c->buf + hlen;
	if (clen < 0)
		return true;
	return c->len >= hlen + (size_t)clen;
}

static void
serve(struct conn *c, const char *body)
{
	static const char models[] = "{\"object\":\"list\",\"data\":"
	                             "[{\"id\":\"mock-model\","
	                             "\"object\":\"model\"}]}";

	if (strncmp(c->buf, "POST", 4) == 0)
		build_reply(c, body);
	else
		frame_add(c, http_json(models), 0);
	uv_read_stop((uv_stream_t *)&c->handle);
	if (c->nframes == 0) {
		conn_close(c);
		return;
	}
	uv_timer_start(&c->timer, on_tick, c->frames[0].delay_ms, 0);
}

static void
on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct conn *c = stream->data;
	const char *body = NULL;

	if (nread > 0) {
		size_t n = (size_t)nread;

		if (c->len + n + 1 > c->cap) {
			size_t nc = c->cap != 0 ? c->cap : 4096;
			char *p;

			while (nc < c->len + n + 1)
				nc *= 2;
			p = realloc(c->buf, nc);
			if (p != NULL) {
				c->buf = p;
				c->cap = nc;
			}
		}
		if (c->buf != NULL && c->len + n + 1 <= c->cap) {
			memcpy(c->buf + c->len, buf->base, n);
			c->len += n;
			c->buf[c->len] = '\0';
		}
		if (!c->served && request_complete(c, &body)) {
			c->served = true;
			serve(c, body);
		}
	} else if (nread < 0) {
		conn_close(c);
	}
	free(buf->base);
}

static void
on_connection(uv_stream_t *server, int status)
{
	struct mock_server *srv = server->data;
	struct conn *c;

	if (status < 0)
		return;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return;
	c->srv = srv;
	c->next_conn = srv->conns;
	srv->conns = c;
	uv_tcp_init(&srv->loop, &c->handle);
	uv_timer_init(&srv->loop, &c->timer);
	c->handle.data = c;
	c->timer.data = c;
	if (uv_accept(server, (uv_stream_t *)&c->handle) == 0)
		uv_read_start((uv_stream_t *)&c->handle, alloc_cb, on_read);
	else
		conn_close(c);
}

/* ---- lifecycle ---- */

static void
on_stop(uv_async_t *async)
{
	struct mock_server *srv = async->loop->data;
	struct conn *c;

	for (c = srv->conns; c != NULL; c = c->next_conn)
		conn_close(c);
	uv_close((uv_handle_t *)&srv->listener, NULL);
	uv_close((uv_handle_t *)&srv->stopper, NULL);
}

static void
run_loop(void *arg)
{
	struct mock_server *srv = arg;

	uv_run(&srv->loop, UV_RUN_DEFAULT);
}

struct mock_server *
mock_start(void)
{
	struct mock_server *srv;
	struct sockaddr_in addr;
	struct sockaddr_storage ss;
	int namelen = (int)sizeof(ss);
	char tmpl[] = "/tmp/clm-tui-tools-XXXXXX";

	srv = calloc(1, sizeof(*srv));
	if (srv == NULL)
		return NULL;
	if (uv_loop_init(&srv->loop) != 0) {
		free(srv);
		return NULL;
	}
	srv->loop.data = srv;
	uv_mutex_init(&srv->lock);
	uv_async_init(&srv->loop, &srv->stopper, on_stop);
	if (uv_ip4_addr("127.0.0.1", 0, &addr) != 0 ||
	    uv_tcp_init(&srv->loop, &srv->listener) != 0) {
		free(srv);
		return NULL;
	}
	srv->listener.data = srv;
	if (uv_tcp_bind(&srv->listener, (const struct sockaddr *)&addr, 0) !=
	        0 ||
	    uv_listen((uv_stream_t *)&srv->listener, 32, on_connection) != 0 ||
	    uv_tcp_getsockname(
	        &srv->listener, (struct sockaddr *)&ss, &namelen) != 0) {
		free(srv);
		return NULL;
	}
	(void)snprintf(srv->url, sizeof(srv->url),
	    "http://127.0.0.1:%d/v1/chat/completions",
	    ntohs(((struct sockaddr_in *)&ss)->sin_port));
	if (mkdtemp(tmpl) == NULL) {
		free(srv);
		return NULL;
	}
	(void)snprintf(srv->scratch, sizeof(srv->scratch), "%s", tmpl);
	if (uv_thread_create(&srv->thread, run_loop, srv) != 0) {
		free(srv);
		return NULL;
	}
	srv->running = true;
	return srv;
}

const char *
mock_url(const struct mock_server *s)
{
	return s->url;
}

const char *
mock_scratch(const struct mock_server *s)
{
	return s->scratch;
}

void
mock_request_log(struct mock_server *s, const char *path)
{
	uv_mutex_lock(&s->lock);
	(void)snprintf(
	    s->log_path, sizeof(s->log_path), "%s", path != NULL ? path : "");
	uv_mutex_unlock(&s->lock);
}

/* The scratch directory only ever holds the marker files the "manytest"
 * calls touch, so one pass over it is enough. */
static void
scratch_clean(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		char child[512];

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		(void)snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		(void)unlink(child);
	}
	(void)closedir(d);
	(void)rmdir(path);
}

void
mock_stop(struct mock_server *s)
{
	if (s == NULL)
		return;
	if (s->running) {
		uv_async_send(&s->stopper);
		uv_thread_join(&s->thread);
	}
	uv_loop_close(&s->loop);
	uv_mutex_destroy(&s->lock);
	scratch_clean(s->scratch);
	free(s);
}
