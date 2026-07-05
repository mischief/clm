// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

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
};

static void
on_mcp_ready(int status, size_t tool_count, void *user)
{
	struct mcp_ready_ctx *ctx = user;
	if (status == 0)
		fprintf(stderr, "mcp: %s: %zu tool%s registered\n", ctx->name,
		    tool_count, tool_count == 1 ? "" : "s");
	else
		fprintf(stderr, "mcp: %s: connect failed (%d)\n", ctx->name, status);
	free(ctx->name);
	free(ctx);
}

static const char *
json_str(struct json_object *obj, const char *key)
{
	struct json_object *v;
	if (!json_object_object_get_ex(obj, key, &v))
		return NULL;
	if (json_object_get_type(v) != json_type_string)
		return NULL;
	return json_object_get_string(v);
}

/* Returns the connected client handle, or NULL if this entry was skipped or
 * failed to start (already reported to stderr either way). */
static struct clm_mcp_client *
connect_one(struct clm_agent *agent, uv_loop_t *loop, struct json_object *srv)
{
	const char *name = json_str(srv, "name");
	const char *transport = json_str(srv, "transport");
	struct clm_mcp_server_cfg server_cfg = {0};
	struct mcp_ready_ctx *ready_ctx;
	struct json_object *jtimeout = NULL, *jcmd = NULL;
	autofree char **argv = NULL;
	struct clm_mcp_client *client = NULL;

	if (name == NULL) {
		fprintf(stderr, "mcp: skipping server with no 'name'\n");
		return NULL;
	}

	server_cfg.name = name;
	if (json_object_object_get_ex(srv, "timeout_ms", &jtimeout))
		server_cfg.timeout_ms = (uint64_t)json_object_get_int64(jtimeout);

	if (transport != NULL && strcmp(transport, "http") == 0) {
		server_cfg.transport = CLM_MCP_HTTP;
		server_cfg.url = json_str(srv, "url");
		server_cfg.api_key = json_str(srv, "api_key");
		if (server_cfg.url == NULL) {
			fprintf(stderr, "mcp: %s: http transport needs 'url'\n", name);
			return NULL;
		}
	} else {
		size_t n, i;

		server_cfg.transport = CLM_MCP_STDIO;
		if (!json_object_object_get_ex(srv, "command", &jcmd) ||
		    json_object_get_type(jcmd) != json_type_array ||
		    json_object_array_length(jcmd) == 0) {
			fprintf(stderr, "mcp: %s: stdio transport needs a non-empty 'command' array\n",
			    name);
			return NULL;
		}
		n = json_object_array_length(jcmd);
		argv = calloc(n + 1, sizeof(*argv));
		if (argv == NULL)
			return NULL;
		for (i = 0; i < n; i++) {
			struct json_object *e = json_object_array_get_idx(jcmd, i);
			argv[i] = (char *)json_object_get_string(e);
		}
		server_cfg.argv = argv;
	}

	ready_ctx = malloc(sizeof(*ready_ctx));
	if (ready_ctx == NULL)
		return NULL;
	ready_ctx->name = strdup(name);

	if (clm_mcp_connect(agent, loop, &server_cfg, on_mcp_ready, ready_ctx, &client) != 0) {
		fprintf(stderr, "mcp: %s: failed to start\n", name);
		free(ready_ctx->name);
		free(ready_ctx);
		return NULL;
	}
	return client;
}

struct clm_mcp_client **
clm_cli_connect_mcp_servers(struct clm_agent *agent, uv_loop_t *loop,
    struct clm_lua_cfg *lcfg, size_t *out_count)
{
	autofree char *json = NULL;
	json_cleanup struct json_object *arr = NULL;
	struct clm_mcp_client **clients;
	size_t n, i;

	if (out_count != NULL)
		*out_count = 0;
	if (lcfg == NULL)
		return NULL;

	json = clm_lua_cfg_mcp_servers_json(lcfg);
	if (json == NULL)
		return NULL;

	arr = json_tokener_parse(json);
	if (arr == NULL || json_object_get_type(arr) != json_type_array)
		return NULL;

	n = json_object_array_length(arr);
	if (n == 0)
		return NULL;

	clients = calloc(n, sizeof(*clients));
	if (clients == NULL) {
		fprintf(stderr, "mcp: out of memory starting configured servers\n");
		return NULL;
	}

	for (i = 0; i < n; i++)
		clients[i] = connect_one(agent, loop, json_object_array_get_idx(arr, i));

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
