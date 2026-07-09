// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/mcp.h"
#include "clm/lua_plugin.h"
#include "mcp_setup.h"

/*
 * config.lua shape:
 *   mcp_servers = {
 *     { name = "fs", transport = "stdio", command = {"mcp-server-fs", "/tmp"} },
 *     { name = "search", transport = "http", url = "https://.../mcp", api_key = "..." },
 *   }
 * "transport" defaults to "stdio" if omitted. "timeout_ms" is optional on
 * either kind (per tool-call deadline).
 */

struct mcp_ready_ctx {
	char *name;
	clm_cli_mcp_status_cb status_cb;
	void *status_user;
};

static void
emit_status(clm_cli_mcp_status_cb status_cb, void *user, const char *msg)
{
	if (status_cb != NULL)
		status_cb(msg, user);
	else
		fprintf(stderr, "%s\n", msg);
}

static void
on_mcp_ready(int status, size_t tool_count, void *user)
{
	struct mcp_ready_ctx *ctx = user;
	char msg[256];

	if (status == 0)
		(void)snprintf(msg, sizeof(msg), "mcp: %s: %zu tool%s registered",
		    ctx->name, tool_count, tool_count == 1 ? "" : "s");
	else
		(void)snprintf(msg, sizeof(msg), "mcp: %s: connect failed (%d)",
		    ctx->name, status);
	emit_status(ctx->status_cb, ctx->status_user, msg);
	free(ctx->name);
	free(ctx);
}

static const char *
json_str(cJSON *obj, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (v == NULL || !cJSON_IsString(v))
		return NULL;
	return cJSON_GetStringValue(v);
}

/* returns the connected client handle, or NULL if this entry was skipped or
 * failed to start (already reported through status_cb either way). */
static struct clm_mcp_client *
connect_one(struct clm_agent *agent, uv_loop_t *loop, cJSON *srv,
    clm_cli_mcp_status_cb status_cb, void *status_user)
{
	const char *name = json_str(srv, "name");
	const char *transport = json_str(srv, "transport");
	struct clm_mcp_server_cfg server_cfg = {0};
	struct mcp_ready_ctx *ready_ctx;
	cJSON *jtimeout, *jcmd;
	autofree char **argv = NULL;
	struct clm_mcp_client *client = NULL;

	if (name == NULL) {
		emit_status(status_cb, status_user,
		    "mcp: skipping server with no 'name'");
		return NULL;
	}

	server_cfg.name = name;
	jtimeout = cJSON_GetObjectItemCaseSensitive(srv, "timeout_ms");
	if (jtimeout != NULL && cJSON_IsNumber(jtimeout))
		server_cfg.timeout_ms = (uint64_t)cJSON_GetNumberValue(jtimeout);

	if (transport != NULL && strcmp(transport, "http") == 0) {
		server_cfg.transport = CLM_MCP_HTTP;
		server_cfg.url = json_str(srv, "url");
		server_cfg.api_key = json_str(srv, "api_key");
		if (server_cfg.url == NULL) {
			char msg[256];
			(void)snprintf(msg, sizeof(msg),
			    "mcp: %s: http transport needs 'url'", name);
			emit_status(status_cb, status_user, msg);
			return NULL;
		}
	} else {
		size_t n, i;

		server_cfg.transport = CLM_MCP_STDIO;
		jcmd = cJSON_GetObjectItemCaseSensitive(srv, "command");
		if (jcmd == NULL || !cJSON_IsArray(jcmd) ||
		    cJSON_GetArraySize(jcmd) == 0) {
			char msg[256];
			(void)snprintf(msg, sizeof(msg),
			    "mcp: %s: stdio transport needs a non-empty 'command' array",
			    name);
			emit_status(status_cb, status_user, msg);
			return NULL;
		}
		n = (size_t)cJSON_GetArraySize(jcmd);
		argv = calloc(n + 1, sizeof(*argv));
		if (argv == NULL)
			return NULL;
		for (i = 0; i < n; i++) {
			cJSON *e = cJSON_GetArrayItem(jcmd, i);
			argv[i] = cJSON_GetStringValue(e);
		}
		server_cfg.argv = argv;
	}

	ready_ctx = malloc(sizeof(*ready_ctx));
	if (ready_ctx == NULL)
		return NULL;
	ready_ctx->name = strdup(name);
	ready_ctx->status_cb = status_cb;
	ready_ctx->status_user = status_user;

	if (clm_mcp_connect(agent, loop, &server_cfg, on_mcp_ready, ready_ctx, &client) != 0) {
		char msg[256];
		(void)snprintf(msg, sizeof(msg), "mcp: %s: failed to start", name);
		emit_status(status_cb, status_user, msg);
		free(ready_ctx->name);
		free(ready_ctx);
		return NULL;
	}
	return client;
}

struct clm_mcp_client **
clm_cli_connect_mcp_servers(struct clm_agent *agent, uv_loop_t *loop,
    struct clm_lua_cfg *lcfg, clm_cli_mcp_status_cb status_cb,
    void *status_user, size_t *out_count)
{
	autofree char *json = NULL;
	json_cleanup cJSON *arr = NULL;
	struct clm_mcp_client **clients;
	size_t n, i;

	if (out_count != NULL)
		*out_count = 0;
	if (lcfg == NULL)
		return NULL;

	json = clm_lua_cfg_mcp_servers_json(lcfg);
	if (json == NULL)
		return NULL;

	arr = cJSON_Parse(json);
	if (arr == NULL || !cJSON_IsArray(arr))
		return NULL;

	n = (size_t)cJSON_GetArraySize(arr);
	if (n == 0)
		return NULL;

	clients = calloc(n, sizeof(*clients));
	if (clients == NULL) {
		emit_status(status_cb, status_user,
		    "mcp: out of memory starting configured servers");
		return NULL;
	}

	for (i = 0; i < n; i++)
		clients[i] = connect_one(agent, loop, cJSON_GetArrayItem(arr, i),
		    status_cb, status_user);

	if (out_count != NULL)
		*out_count = n;
	return clients;
}

void
clm_cli_free_mcp_servers(struct clm_mcp_client **clients, size_t count)
{
	size_t i;
	if (clients == NULL)
		return;
	for (i = 0; i < count; i++)
		clm_mcp_client_free(clients[i]); /* NULL entries: no-op */
	free(clients);
}
