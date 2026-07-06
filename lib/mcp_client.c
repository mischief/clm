// SPDX-License-Identifier: ISC
/*
 * MCP client -- see include/clm/mcp.h for the design note. Two transports
 * share one struct: CLM_MCP_HTTP posts one JSON-RPC message per call through
 * the existing http_async engine (no persistent connection); CLM_MCP_STDIO
 * spawns the server once and speaks newline-delimited JSON-RPC over its
 * stdin/stdout for the life of the client, so replies must be routed back to
 * the right in-flight call by "id".
 *
 * Prototype scope: HTTP transport assumes a plain JSON response body (no SSE
 * framing). Stdio transport does not currently fail in-flight tool
 * invocations when the client is freed early (see clm_mcp_client_free).
 */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/tools.h"
#include "clm/mcp.h"
#include "clm/http_async.h"
#include "clm/cleanup.h"
#include "version.h"
#include "banned.h"

#define MCP_DEFAULT_TIMEOUT_MS 30000u
#define MCP_PROTOCOL_VERSION "2024-11-05"

/*
 * Auto-restart budget for a stdio server that dies (crash, killed, etc): a
 * small token bucket, same idea as clm_ratelimit (not reused directly -- it's
 * core-internal and hidden across the libclm.so boundary; this one is scoped
 * to restart attempts only, so a tiny local copy is simpler than exporting
 * that API). BURST tokens available up front so a couple of quick crashes
 * still recover instantly; after that, one more restart per RATE_SECONDS, so
 * a genuine crash loop backs off instead of hammering fork/exec forever.
 */
#define MCP_RESTART_BURST 3.0
#define MCP_RESTART_RATE_SECONDS 30.0 /* 1 token refilled per this many seconds */

/* What to do once a stdio client's handles finish closing; see
 * mcp_begin_close's comment further down for why this is needed. */
enum mcp_close_intent { MCP_CLOSE_THEN_IDLE, MCP_CLOSE_THEN_RESPAWN, MCP_CLOSE_THEN_FREE };

struct mcp_tool_ctx; /* full definition further down, near mcp_register_tools */

struct mcp_pending {
	int id;
	enum { MCP_PEND_INIT, MCP_PEND_LIST, MCP_PEND_CALL } kind;
	struct clm_tool_invocation *inv; /* MCP_PEND_CALL only */
	struct mcp_pending *next;
};

struct clm_mcp_client {
	struct clm_agent *agent;
	uv_loop_t *loop;
	enum clm_mcp_transport transport;
	char *name;
	char *url;
	char *api_key;
	uint64_t timeout_ms;

	void (*on_ready)(int status, size_t tool_count, void *user);
	void *user;

	int next_id;
	struct mcp_pending *pending;

	/* Full ("<server>__<tool>") names of every tool this client has
	 * registered on the agent, so a disconnect can unregister them all. */
	char **registered_names;
	struct mcp_tool_ctx **registered_ctxs; /* parallel to registered_names */
	size_t registered_count;

	/* stdio only */
	uv_process_t proc;
	uv_pipe_t in, out;
	bool proc_spawned; /* handles are live (spawned, not yet closed) */
	bool dead; /* the subprocess exited or the pipe broke; see mcp_go_dead */
	char *linebuf;
	size_t linelen, linecap;

	/* Close-in-progress bookkeeping; see mcp_begin_close's comment. */
	bool closing;
	int close_remaining;
	enum mcp_close_intent close_intent;

	/* Deep copy of server_cfg->argv, kept alive so a dead client can
	 * re-exec the same command; see mcp_go_dead / mcp_do_respawn. NULL for
	 * the HTTP transport (nothing to restart -- each call is independent). */
	char **argv_copy;

	/* Restart token bucket; see MCP_RESTART_BURST/RATE_SECONDS above. */
	double restart_tokens;
	uint64_t last_refill_usec;
};

/* Per-tool user data for a registered MCP tool. */
struct mcp_tool_ctx {
	struct clm_mcp_client *client;
	char *remote_name;
};

/* Per-HTTP-call context (HTTP transport has no persistent connection, so each
 * call carries its own context instead of using the pending list). */
enum mcp_http_kind { MCP_HTTP_INIT, MCP_HTTP_LIST, MCP_HTTP_CALL };

struct mcp_http_ctx {
	struct clm_mcp_client *client;
	struct clm_tool_invocation *inv; /* NULL for init/list */
	enum mcp_http_kind kind;
};

static void mcp_send_tools_list(struct clm_mcp_client *client);
static void mcp_handle_result(struct clm_mcp_client *client,
    struct mcp_pending *pend, struct json_object *result, const char *err);
static void mcp_go_dead(struct clm_mcp_client *client, int status);
static int mcp_start_handshake(struct clm_mcp_client *client);
static int mcp_spawn(struct clm_mcp_client *client, char *const *argv);

static uint64_t
mcp_now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* Refill and try to spend one restart token. Returns false (no token spent)
 * if the budget is exhausted -- the caller stays dead rather than retrying. */
static bool
mcp_restart_allowed(struct clm_mcp_client *client)
{
	uint64_t now = mcp_now_usec();
	double elapsed_sec = (double)(now - client->last_refill_usec) / 1000000.0;

	client->last_refill_usec = now;
	client->restart_tokens += elapsed_sec / MCP_RESTART_RATE_SECONDS;
	if (client->restart_tokens > MCP_RESTART_BURST)
		client->restart_tokens = MCP_RESTART_BURST;

	if (client->restart_tokens < 1.0)
		return false;
	client->restart_tokens -= 1.0;
	return true;
}

static char **
mcp_dup_argv(char *const *argv)
{
	size_t n = 0, i;
	char **copy;

	while (argv[n] != NULL)
		n++;
	copy = calloc(n + 1, sizeof(*copy));
	if (copy == NULL)
		return NULL;
	for (i = 0; i < n; i++) {
		copy[i] = strdup(argv[i]);
		if (copy[i] == NULL) {
			size_t j;
			for (j = 0; j < i; j++)
				free(copy[j]);
			free(copy);
			return NULL;
		}
	}
	return copy;
}

static void
mcp_free_argv(char **argv)
{
	size_t i;
	if (argv == NULL)
		return;
	for (i = 0; argv[i] != NULL; i++)
		free(argv[i]);
	free(argv);
}

/*
 * Close bookkeeping. IMPORTANT libuv constraint this exists to respect: a
 * uv_process_t must only ever be uv_close()'d from within its own exit_cb (or
 * after it has already fired) -- never speculatively before the child is
 * confirmed to have exited. That is why in/out (pipes -- safe to close
 * anytime) and proc (only from mcp_on_exit) are torn down through two
 * different call sites but share one completion counter: whichever of
 * {mcp_stdio_read's EOF path, mcp_on_exit} notices death first starts the
 * pipe half via mcp_begin_close; mcp_on_exit always closes proc exactly once,
 * whenever the child actually exits, and that decrement is what may finally
 * be the one to reach zero. This also avoids ever repurposing a handle's
 * `.data` to anything but `client` (set once, in mcp_spawn) -- so mcp_on_exit
 * reading proc->data is always safe, never a stale/wrong-typed pointer.
 */
static void mcp_client_free_now(struct clm_mcp_client *client);

static void
mcp_do_respawn(struct clm_mcp_client *client)
{
	int r;

	client->linelen = 0;
	if (client->linebuf != NULL)
		client->linebuf[0] = '\0';
	client->next_id = 1;

	r = mcp_spawn(client, client->argv_copy);
	if (r != 0) {
		/* The exec itself failed (e.g. binary missing/removed) -- retrying
		 * later won't help, so stay dead rather than looping. */
		if (client->on_ready != NULL)
			client->on_ready(r, 0, client->user);
		return;
	}

	client->dead = false;
	r = mcp_start_handshake(client);
	if (r != 0 && client->on_ready != NULL)
		client->on_ready(r, 0, client->user);
}

/* Shared close_cb for in, out, AND proc (the latter only ever passed to
 * uv_close from mcp_on_exit). h->data is always `client` -- never repurposed. */
static void
mcp_handle_closed(uv_handle_t *h)
{
	struct clm_mcp_client *client = h->data;
	if (--client->close_remaining == 0) {
		enum mcp_close_intent intent = client->close_intent;
		client->closing = false;
		switch (intent) {
		case MCP_CLOSE_THEN_RESPAWN: mcp_do_respawn(client); break;
		case MCP_CLOSE_THEN_IDLE: break;
		case MCP_CLOSE_THEN_FREE: mcp_client_free_now(client); break;
		}
	}
}

/*
 * Start (or fold into an already-running) teardown of this client's stdio
 * handles, resolving to `intent` once fully closed. Idempotent: if a close is
 * already in flight (from the other trigger path racing in) or already
 * finished, this only ever *upgrades* the eventual outcome -- MCP_CLOSE_THEN_FREE
 * always wins, since an explicit clm_mcp_client_free must never lose to a
 * concurrent auto-restart decision.
 */
static void
mcp_begin_close(struct clm_mcp_client *client, enum mcp_close_intent intent)
{
	if (client->closing || !client->proc_spawned) {
		if (intent == MCP_CLOSE_THEN_FREE) {
			if (client->closing)
				client->close_intent = MCP_CLOSE_THEN_FREE;
			else /* nothing left open at all; resolve right now */
				mcp_client_free_now(client);
		}
		return;
	}

	client->closing = true;
	client->proc_spawned = false; /* handles are on their way down */
	client->close_intent = intent;
	client->close_remaining = 3; /* in, out (closed here) + proc (mcp_on_exit) */

	uv_read_stop((uv_stream_t *)&client->out);
	uv_close((uv_handle_t *)&client->in, mcp_handle_closed);
	uv_close((uv_handle_t *)&client->out, mcp_handle_closed);
	/* proc is deliberately NOT closed here -- see the comment above. */
}

static char *
mcp_build_request(const char *method, struct json_object *params, int id)
{
	json_cleanup struct json_object *req = json_object_new_object();
	char *out;

	if (req == NULL)
		return NULL;
	json_object_object_add(req, "jsonrpc", json_object_new_string("2.0"));
	json_object_object_add(req, "id", json_object_new_int(id));
	json_object_object_add(req, "method", json_object_new_string(method));
	if (params != NULL)
		json_object_object_add(req, "params", params);

	out = strdup(json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN));
	return out;
}

static char *
mcp_build_notification(const char *method)
{
	json_cleanup struct json_object *req = json_object_new_object();

	if (req == NULL)
		return NULL;
	json_object_object_add(req, "jsonrpc", json_object_new_string("2.0"));
	json_object_object_add(req, "method", json_object_new_string(method));
	return strdup(json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN));
}

static struct json_object *
mcp_init_params(void)
{
	struct json_object *params = json_object_new_object();
	struct json_object *caps = json_object_new_object();
	struct json_object *info = json_object_new_object();

	json_object_object_add(params, "protocolVersion",
	    json_object_new_string(MCP_PROTOCOL_VERSION));
	json_object_object_add(params, "capabilities", caps);
	json_object_object_add(info, "name", json_object_new_string("clm"));
	json_object_object_add(info, "version", json_object_new_string(CLM_VERSION));
	json_object_object_add(params, "clientInfo", info);
	return params;
}

/* --- tool registration from a tools/list result ------------------------- */

static void
mcp_tool_ctx_free(void *user)
{
	struct mcp_tool_ctx *ctx = user;
	if (ctx == NULL)
		return;
	free(ctx->remote_name);
	free(ctx);
}

static void mcp_tool_invoke(struct clm_tool_invocation *inv, void *user);

/*
 * Record a tool name (for a later clm_tool_remove) and its mcp_tool_ctx (for
 * a later mcp_tool_ctx_free -- clm_tool_remove only frees the core registry
 * node's own strings; the tool_user pointer we handed it in clm_tool_def is
 * ours to free, since the core has no way to know how it was allocated).
 * Best effort: if this fails (OOM), the tool just won't be auto-removed or
 * its ctx freed on disconnect -- a lesser problem than failing registration
 * outright.
 */
static void
mcp_track_registered(struct clm_mcp_client *client, const char *full_name,
    struct mcp_tool_ctx *ctx)
{
	char **names = realloc(client->registered_names,
	    (client->registered_count + 1) * sizeof(*names));
	struct mcp_tool_ctx **ctxs;
	char *copy;

	if (names == NULL)
		return;
	client->registered_names = names;

	ctxs = realloc(client->registered_ctxs,
	    (client->registered_count + 1) * sizeof(*ctxs));
	if (ctxs == NULL)
		return;
	client->registered_ctxs = ctxs;

	copy = strdup(full_name);
	if (copy == NULL)
		return;
	client->registered_names[client->registered_count] = copy;
	client->registered_ctxs[client->registered_count] = ctx;
	client->registered_count++;
}

static void
mcp_register_tools(struct clm_mcp_client *client, struct json_object *tools)
{
	size_t n = json_object_array_length(tools);
	size_t registered = 0;

	for (size_t i = 0; i < n; i++) {
		struct json_object *t = json_object_array_get_idx(tools, i);
		struct json_object *jname = NULL, *jdesc = NULL, *jschema = NULL;
		const char *rname;
		char *full_name, *desc, *schema;
		struct mcp_tool_ctx *ctx;
		struct clm_tool_def def = {0};

		if (!json_object_object_get_ex(t, "name", &jname))
			continue;
		rname = json_object_get_string(jname);

		/* "mcp__<server>__<tool>": matches the scheme Claude Code uses for
		 * MCP-sourced tools (native/Lua tools stay bare -- only MCP can pull
		 * in same-named tools from independent third-party servers, so only
		 * MCP needs the collision guard a prefix buys). */
		{
			size_t full_len = sizeof("mcp__") - 1 + strlen(client->name) +
			    2 + strlen(rname) + 1;
			full_name = malloc(full_len);
			if (full_name == NULL)
				continue;
			(void)snprintf(full_name, full_len, "mcp__%s__%s",
			    client->name, rname);
		}

		json_object_object_get_ex(t, "description", &jdesc);
		desc = strdup(jdesc != NULL ? json_object_get_string(jdesc) : rname);

		if (json_object_object_get_ex(t, "inputSchema", &jschema))
			schema = strdup(json_object_to_json_string_ext(jschema,
			    JSON_C_TO_STRING_PLAIN));
		else
			schema = strdup("{\"type\":\"object\"}");

		ctx = malloc(sizeof(*ctx));
		if (ctx == NULL) {
			free(full_name);
			free(desc);
			free(schema);
			continue;
		}
		ctx->client = client;
		ctx->remote_name = strdup(rname);

		def.name = full_name;
		def.description = desc;
		def.params_schema = schema;
		def.invoke = mcp_tool_invoke;
		def.user = ctx;
		def.timeout_ms = client->timeout_ms;

		if (clm_tool_add(client->agent, &def) == 0) {
			registered++;
			mcp_track_registered(client, full_name, ctx);
		} else {
			mcp_tool_ctx_free(ctx);
		}

		free(full_name);
		free(desc);
		free(schema);
	}

	if (client->on_ready != NULL)
		client->on_ready(0, registered, client->user);
}

/* --- HTTP transport ------------------------------------------------------ */

static void
mcp_http_success(struct clm_http_response *resp, void *user)
{
	struct mcp_http_ctx *hctx = user;
	json_cleanup struct json_object *parsed = NULL;
	struct json_object *result = NULL, *error = NULL;
	const char *err_msg = NULL;

	if (resp->body != NULL)
		parsed = json_tokener_parse(resp->body);

	if (parsed != NULL) {
		json_object_object_get_ex(parsed, "result", &result);
		if (json_object_object_get_ex(parsed, "error", &error))
			err_msg = json_object_get_string(error);
	}

	switch (hctx->kind) {
	case MCP_HTTP_INIT:
		if (err_msg != NULL || result == NULL) {
			if (hctx->client->on_ready != NULL)
				hctx->client->on_ready(-EPROTO, 0, hctx->client->user);
		} else {
			mcp_send_tools_list(hctx->client);
		}
		break;
	case MCP_HTTP_LIST: {
		struct json_object *tools = NULL;
		if (result != NULL &&
		    json_object_object_get_ex(result, "tools", &tools) &&
		    json_object_get_type(tools) == json_type_array)
			mcp_register_tools(hctx->client, tools);
		else if (hctx->client->on_ready != NULL)
			hctx->client->on_ready(-EPROTO, 0, hctx->client->user);
		break;
	}
	case MCP_HTTP_CALL: {
		struct json_object *content = NULL, *first = NULL, *text = NULL;
		if (err_msg != NULL) {
			clm_tool_fail(hctx->inv, err_msg);
		} else if (result != NULL &&
		    json_object_object_get_ex(result, "content", &content) &&
		    json_object_get_type(content) == json_type_array &&
		    json_object_array_length(content) > 0 &&
		    (first = json_object_array_get_idx(content, 0)) != NULL &&
		    json_object_object_get_ex(first, "text", &text)) {
			clm_tool_complete(hctx->inv, json_object_get_string(text));
		} else if (result != NULL) {
			clm_tool_complete(hctx->inv,
			    json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN));
		} else {
			clm_tool_fail(hctx->inv, "malformed MCP response");
		}
		break;
	}
	}

	/* clm_http_response_free is core-internal (hidden across the libclm.so
	 * boundary); free the fields directly instead. */
	free(resp->body);
	free(resp->error_msg);
	free(hctx);
}

static void
mcp_http_error(int error_code, const char *error_msg, void *user)
{
	struct mcp_http_ctx *hctx = user;
	(void)error_code;

	switch (hctx->kind) {
	case MCP_HTTP_INIT:
	case MCP_HTTP_LIST:
		if (hctx->client->on_ready != NULL)
			hctx->client->on_ready(-EIO, 0, hctx->client->user);
		break;
	case MCP_HTTP_CALL:
		clm_tool_fail(hctx->inv, error_msg);
		break;
	}
	free(hctx);
}

static int
mcp_http_send(struct clm_mcp_client *client, char *body,
    enum mcp_http_kind kind, struct clm_tool_invocation *inv)
{
	struct mcp_http_ctx *hctx = malloc(sizeof(*hctx));
	int r;

	if (hctx == NULL) {
		free(body);
		return -ENOMEM;
	}
	hctx->client = client;
	hctx->inv = inv;
	hctx->kind = kind;

	r = clm_http_async_post(client->loop, client->url, client->api_key, body,
	    NULL, mcp_http_success, mcp_http_error, NULL, "mcp", hctx, NULL);
	free(body);
	if (r != 0)
		free(hctx);
	return r;
}

/* --- stdio transport ------------------------------------------------------ */

struct mcp_write_req {
	uv_write_t req;
	uv_buf_t buf;
};

static void
mcp_on_write_done(uv_write_t *req, int status)
{
	struct mcp_write_req *wr = (struct mcp_write_req *)req;
	(void)status;
	free(wr->buf.base);
	free(wr);
}

static int
mcp_stdio_send(struct clm_mcp_client *client, char *body)
{
	struct mcp_write_req *wr = malloc(sizeof(*wr));
	size_t len;
	char *framed;

	if (wr == NULL || client->dead || !client->proc_spawned) {
		free(body);
		free(wr);
		return -EIO;
	}
	len = strlen(body);
	framed = malloc(len + 2);
	if (framed == NULL) {
		free(body);
		free(wr);
		return -ENOMEM;
	}
	memcpy(framed, body, len);
	framed[len] = '\n';
	framed[len + 1] = '\0';
	free(body);

	wr->buf = uv_buf_init(framed, (unsigned)(len + 1));
	if (uv_write(&wr->req, (uv_stream_t *)&client->in, &wr->buf, 1,
	    mcp_on_write_done) < 0) {
		free(framed);
		free(wr);
		return -EIO;
	}
	return 0;
}

static void
mcp_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base ? suggested : 0;
}

static struct mcp_pending *
mcp_pending_take(struct clm_mcp_client *client, int id)
{
	struct mcp_pending **pp = &client->pending;
	while (*pp != NULL) {
		if ((*pp)->id == id) {
			struct mcp_pending *p = *pp;
			*pp = p->next;
			return p;
		}
		pp = &(*pp)->next;
	}
	return NULL;
}

static void
mcp_process_line(struct clm_mcp_client *client, char *line)
{
	json_cleanup struct json_object *parsed = json_tokener_parse(line);
	struct json_object *jid = NULL, *result = NULL, *error = NULL;
	struct mcp_pending *pend;
	const char *err_msg = NULL;

	if (parsed == NULL || !json_object_object_get_ex(parsed, "id", &jid))
		return; /* notification from the server; ignore */

	pend = mcp_pending_take(client, json_object_get_int(jid));
	if (pend == NULL)
		return;

	json_object_object_get_ex(parsed, "result", &result);
	if (json_object_object_get_ex(parsed, "error", &error))
		err_msg = json_object_get_string(error);

	mcp_handle_result(client, pend, result, err_msg);
	free(pend);
}

static void
mcp_handle_result(struct clm_mcp_client *client, struct mcp_pending *pend,
    struct json_object *result, const char *err)
{
	switch (pend->kind) {
	case MCP_PEND_INIT:
		if (err != NULL || result == NULL) {
			if (client->on_ready != NULL)
				client->on_ready(-EPROTO, 0, client->user);
		} else {
			char *note = mcp_build_notification("notifications/initialized");
			if (note != NULL)
				mcp_stdio_send(client, note);
			mcp_send_tools_list(client);
		}
		break;
	case MCP_PEND_LIST: {
		struct json_object *tools = NULL;
		if (result != NULL &&
		    json_object_object_get_ex(result, "tools", &tools) &&
		    json_object_get_type(tools) == json_type_array)
			mcp_register_tools(client, tools);
		else if (client->on_ready != NULL)
			client->on_ready(-EPROTO, 0, client->user);
		break;
	}
	case MCP_PEND_CALL: {
		struct json_object *content = NULL, *first = NULL, *text = NULL;
		if (err != NULL) {
			clm_tool_fail(pend->inv, err);
		} else if (result != NULL &&
		    json_object_object_get_ex(result, "content", &content) &&
		    json_object_get_type(content) == json_type_array &&
		    json_object_array_length(content) > 0 &&
		    (first = json_object_array_get_idx(content, 0)) != NULL &&
		    json_object_object_get_ex(first, "text", &text)) {
			clm_tool_complete(pend->inv, json_object_get_string(text));
		} else if (result != NULL) {
			clm_tool_complete(pend->inv,
			    json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN));
		} else {
			clm_tool_fail(pend->inv, "malformed MCP response");
		}
		break;
	}
	}
}

static void
mcp_stdio_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct clm_mcp_client *client = stream->data;
	char *nl;

	if (nread <= 0) {
		free(buf->base);
		/* nread == 0 just means "nothing to read right now"; a negative
		 * value (including UV_EOF) means the pipe is gone -- the child
		 * died or closed stdout. mcp_go_dead is idempotent, so this races
		 * harmlessly against mcp_on_exit picking it up first. */
		if (nread < 0)
			mcp_go_dead(client, -ECONNRESET);
		return;
	}

	if (client->linelen + (size_t)nread + 1 > client->linecap) {
		size_t nc = client->linecap ? client->linecap * 2 : 4096;
		char *p;
		while (nc < client->linelen + (size_t)nread + 1)
			nc *= 2;
		p = realloc(client->linebuf, nc);
		if (p == NULL) {
			free(buf->base);
			return;
		}
		client->linebuf = p;
		client->linecap = nc;
	}
	memcpy(client->linebuf + client->linelen, buf->base, (size_t)nread);
	client->linelen += (size_t)nread;
	client->linebuf[client->linelen] = '\0';
	free(buf->base);

	while ((nl = memchr(client->linebuf, '\n', client->linelen)) != NULL) {
		size_t linesz = (size_t)(nl - client->linebuf);
		*nl = '\0';
		mcp_process_line(client, client->linebuf);
		memmove(client->linebuf, nl + 1, client->linelen - linesz - 1);
		client->linelen -= linesz + 1;
		client->linebuf[client->linelen] = '\0';
	}
}

static void
mcp_on_exit(uv_process_t *proc, int64_t exit_status, int term_signal)
{
	struct clm_mcp_client *client = proc->data;
	(void)exit_status;
	(void)term_signal;

	/* Run the death bookkeeping first (idempotent: a no-op if the pipe-EOF
	 * path already got here first and closed in/out). Then close proc --
	 * always here, and only here, so we never uv_close() a uv_process_t
	 * before its own exit_cb (this one) has actually fired. */
	mcp_go_dead(client, -ECONNRESET);
	uv_close((uv_handle_t *)proc, mcp_handle_closed);
}

static int
mcp_spawn(struct clm_mcp_client *client, char *const *argv)
{
	uv_stdio_container_t stdio[3];
	uv_process_options_t opt;
	int r, argc = 0;

	memset(&opt, 0, sizeof(opt));
	while (argv[argc] != NULL)
		argc++;
	if (argc == 0)
		return -EINVAL;

	uv_pipe_init(client->loop, &client->in, 0);
	client->in.data = client;
	uv_pipe_init(client->loop, &client->out, 0);
	client->out.data = client;

	/* Flags describe the CHILD's end of the pipe: it reads stdin, writes
	 * stdout. We hold the opposite end (write to client->in, read from
	 * client->out). */
	stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
	stdio[0].data.stream = (uv_stream_t *)&client->in;
	stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
	stdio[1].data.stream = (uv_stream_t *)&client->out;
	stdio[2].flags = UV_IGNORE;

	opt.file = argv[0];
	opt.args = (char **)argv;
	opt.stdio = stdio;
	opt.stdio_count = 3;
	opt.exit_cb = mcp_on_exit;

	client->proc.data = client;
	r = uv_spawn(client->loop, &client->proc, &opt);
	if (r < 0)
		return r;

	client->proc_spawned = true;
	uv_read_start((uv_stream_t *)&client->out, mcp_alloc_cb, mcp_stdio_read);
	return 0;
}

/* --- shared: dispatch by transport --------------------------------------- */

static int
mcp_send(struct clm_mcp_client *client, char *body, enum mcp_http_kind kind,
    struct clm_tool_invocation *inv)
{
	if (client->transport == CLM_MCP_HTTP)
		return mcp_http_send(client, body, kind, inv);

	/* stdio: record a pending entry keyed by the id we just embedded. */
	struct mcp_pending *pend = malloc(sizeof(*pend));
	int r;

	if (pend == NULL) {
		free(body);
		return -ENOMEM;
	}
	pend->id = client->next_id - 1; /* caller already bumped next_id */
	pend->inv = inv;
	pend->next = client->pending;

	switch (kind) {
	case MCP_HTTP_INIT: pend->kind = MCP_PEND_INIT; break;
	case MCP_HTTP_LIST: pend->kind = MCP_PEND_LIST; break;
	default: pend->kind = MCP_PEND_CALL; break;
	}

	r = mcp_stdio_send(client, body);
	if (r != 0) {
		free(pend);
		return r;
	}
	client->pending = pend;
	return 0;
}

static void
mcp_send_tools_list(struct clm_mcp_client *client)
{
	int id = client->next_id++;
	char *body = mcp_build_request("tools/list", NULL, id);
	if (body == NULL) {
		if (client->on_ready != NULL)
			client->on_ready(-ENOMEM, 0, client->user);
		return;
	}
	mcp_send(client, body, MCP_HTTP_LIST, NULL);
}

static void
mcp_tool_invoke(struct clm_tool_invocation *inv, void *user)
{
	struct mcp_tool_ctx *ctx = user;
	json_cleanup struct json_object *args =
	    json_tokener_parse(clm_tool_invocation_args(inv));
	struct json_object *params = json_object_new_object();
	int id;
	char *body;

	json_object_object_add(params, "name",
	    json_object_new_string(ctx->remote_name));
	json_object_object_add(params, "arguments",
	    (args != NULL && json_object_get_type(args) == json_type_object)
	        ? json_object_get(args)
	        : json_object_new_object());

	id = ctx->client->next_id++;
	body = mcp_build_request("tools/call", params, id);
	if (body == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	if (mcp_send(ctx->client, body, MCP_HTTP_CALL, inv) != 0)
		clm_tool_fail(inv, "failed to reach MCP server");
}

/* --- lifecycle ------------------------------------------------------------ */

/* Send "initialize" (the first message of a fresh session). Shared between
 * the initial connect and a post-crash respawn. */
static int
mcp_start_handshake(struct clm_mcp_client *client)
{
	int id = client->next_id++;
	char *body = mcp_build_request("initialize", mcp_init_params(), id);
	if (body == NULL)
		return -ENOMEM;
	return mcp_send(client, body, MCP_HTTP_INIT, NULL);
}

/*
 * The server is gone: a subprocess exited (mcp_on_exit) or its pipe broke
 * (mcp_stdio_read on EOF/error). Idempotent -- both paths can race to call
 * this for the same death.
 *
 * Fails every outstanding call immediately (rather than making the model
 * wait out each call's timeout), unregisters every tool this client had
 * registered so the model stops seeing them, notifies via on_ready (status
 * < 0, tool_count 0 -- reused as a "this server is now down" signal, see
 * clm/mcp.h), then -- for a stdio server with restart budget left -- closes
 * the old process/pipe handles and respawns the same command.
 */
/* Unregister every tool this client has registered on the agent (used both
 * when a server dies and when the caller explicitly frees a live client --
 * without this, a freed client leaves mcp_tool_ctx pointers in the agent's
 * registry that dangle on the next call to that tool). */
static void
mcp_unregister_all(struct clm_mcp_client *client)
{
	size_t i;
	for (i = 0; i < client->registered_count; i++) {
		clm_tool_remove(client->agent, client->registered_names[i]);
		free(client->registered_names[i]);
		/* Safe regardless of the core's internal inflight/removed
		 * bookkeeping for the clm_tool node itself: nothing ever touches
		 * this ctx after mcp_tool_invoke's initial (synchronous) dispatch
		 * -- no async completion path holds onto it. */
		mcp_tool_ctx_free(client->registered_ctxs[i]);
	}
	free(client->registered_names);
	free(client->registered_ctxs);
	client->registered_names = NULL;
	client->registered_ctxs = NULL;
	client->registered_count = 0;
}

/* Fail every outstanding call immediately (used both on death and on an
 * explicit free, so no invocation is ever left pointing at a client that's
 * about to go away). */
static void
mcp_fail_all_pending(struct clm_mcp_client *client, const char *msg)
{
	struct mcp_pending *p;
	while ((p = client->pending) != NULL) {
		client->pending = p->next;
		if (p->kind == MCP_PEND_CALL && p->inv != NULL)
			clm_tool_fail(p->inv, msg);
		free(p);
	}
}

static void
mcp_go_dead(struct clm_mcp_client *client, int status)
{
	if (client->dead)
		return;
	client->dead = true;

	mcp_fail_all_pending(client, "MCP server disconnected");
	mcp_unregister_all(client);

	if (client->on_ready != NULL)
		client->on_ready(status, 0, client->user);

	if (client->transport == CLM_MCP_STDIO) {
		bool restart = client->argv_copy != NULL && mcp_restart_allowed(client);
		/* Always close (releases the fds) even when not restarting -- a
		 * dead client with an exhausted budget should not sit on open
		 * pipe/process handles forever. */
		mcp_begin_close(client, restart ? MCP_CLOSE_THEN_RESPAWN : MCP_CLOSE_THEN_IDLE);
	}
}

int
clm_mcp_connect(struct clm_agent *agent, struct uv_loop_s *loop,
    const struct clm_mcp_server_cfg *server_cfg,
    void (*on_ready)(int status, size_t tool_count, void *user), void *user,
    struct clm_mcp_client **out)
{
	struct clm_mcp_client *client;
	int r = 0;

	if (out != NULL)
		*out = NULL;
	if (agent == NULL || loop == NULL || server_cfg == NULL ||
	    server_cfg->name == NULL)
		return -EINVAL;
	if (server_cfg->transport == CLM_MCP_HTTP && server_cfg->url == NULL)
		return -EINVAL;
	if (server_cfg->transport == CLM_MCP_STDIO && server_cfg->argv == NULL)
		return -EINVAL;

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		return -ENOMEM;

	client->agent = agent;
	client->loop = (uv_loop_t *)loop;
	client->transport = server_cfg->transport;
	client->name = strdup(server_cfg->name);
	client->url = server_cfg->url != NULL ? strdup(server_cfg->url) : NULL;
	client->api_key = server_cfg->api_key != NULL ? strdup(server_cfg->api_key) : NULL;
	client->timeout_ms = server_cfg->timeout_ms != 0 ? server_cfg->timeout_ms
	                                                  : MCP_DEFAULT_TIMEOUT_MS;
	client->on_ready = on_ready;
	client->user = user;
	client->next_id = 1;
	client->restart_tokens = MCP_RESTART_BURST;
	client->last_refill_usec = mcp_now_usec();

	if (client->transport == CLM_MCP_STDIO) {
		client->argv_copy = mcp_dup_argv(server_cfg->argv);
		if (client->argv_copy == NULL) {
			free(client->name);
			free(client->url);
			free(client->api_key);
			free(client);
			return -ENOMEM;
		}
		r = mcp_spawn(client, client->argv_copy);
		if (r != 0) {
			mcp_free_argv(client->argv_copy);
			free(client->name);
			free(client->url);
			free(client->api_key);
			free(client);
			return r;
		}
	}

	if (out != NULL)
		*out = client;

	r = mcp_start_handshake(client);
	if (r != 0 && on_ready != NULL)
		on_ready(r, 0, user);
	return 0;
}

/* Actually release the struct's own memory. Only called once we're certain no
 * further callback (a pipe read/write, or proc's exit_cb) can fire into it --
 * see mcp_begin_close/mcp_handle_closed. Pending calls and registered tools
 * must already be cleared by the caller before this runs. */
static void
mcp_client_free_now(struct clm_mcp_client *client)
{
	mcp_free_argv(client->argv_copy);
	free(client->linebuf);
	free(client->name);
	free(client->url);
	free(client->api_key);
	free(client);
}

void
clm_mcp_client_free(struct clm_mcp_client *client)
{
	if (client == NULL)
		return;

	/* Detach from the agent and fail anything outstanding immediately,
	 * regardless of transport state -- the caller is discarding this handle
	 * right now and must not see any more callbacks through it. */
	mcp_fail_all_pending(client, "MCP client closed");
	mcp_unregister_all(client);
	client->on_ready = NULL;

	if (client->transport != CLM_MCP_STDIO) {
		mcp_client_free_now(client); /* HTTP: no persistent handles at all */
		return;
	}

	if (!client->dead && client->proc_spawned) {
		/* Still alive: kill it now. mcp_begin_close below only closes the
		 * pipes; proc itself is closed from mcp_on_exit once this kill
		 * actually takes effect (see mcp_begin_close's comment) -- mark
		 * dead here so that eventual exit_cb's call into mcp_go_dead is a
		 * pure no-op (we've already done its bookkeeping ourselves). */
		uv_process_kill(&client->proc, SIGKILL);
		client->dead = true;
	}

	/* Resolves immediately if nothing is left open; otherwise finishes
	 * asynchronously (mcp_client_free_now runs once in/out and proc have
	 * all reported closed -- typically within one or two loop iterations,
	 * or as soon as the SIGKILL above actually lands). */
	mcp_begin_close(client, MCP_CLOSE_THEN_FREE);
}
