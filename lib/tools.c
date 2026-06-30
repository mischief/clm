// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "clm/tools.h"
#include "clm/internal.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

#define CLM_TOOL_OUTPUT_CAP (64 * 1024) /* bytes returned to the model */
#define CLM_READ_DEFAULT_LIMIT 200      /* lines */

/* Build a successful tool_result carrying content (copied). */
static int
make_result(const char *tool_call_id, const char *content, struct clm_tool_result **out)
{
	struct clm_tool_result *r = calloc(1, sizeof(*r));
	if (r == NULL)
		return -ENOMEM;

	r->tool_call_id = strdup(tool_call_id ? tool_call_id : "");
	r->content = strdup(content ? content : "");
	if (r->tool_call_id == NULL || r->content == NULL) {
		clm_tool_result_free(r);
		return -ENOMEM;
	}

	*out = r;
	return 0;
}

/*
 * Parse call->args (raw JSON) and fetch a string field. Returns a strdup'd
 * value or NULL if absent. *err is set non-zero if args is not valid JSON.
 */
static char *
arg_string(struct json_object *args, const char *key)
{
	struct json_object *v = NULL;
	if (!json_object_object_get_ex(args, key, &v))
		return NULL;
	if (json_object_get_type(v) != json_type_string)
		return NULL;
	return strdup(json_object_get_string(v));
}

static int
arg_int(struct json_object *args, const char *key, int dflt)
{
	struct json_object *v = NULL;
	if (!json_object_object_get_ex(args, key, &v))
		return dflt;
	if (json_object_get_type(v) != json_type_int)
		return dflt;
	return json_object_get_int(v);
}

static int
tool_file_read(const struct clm_tool_call *call, struct json_object *args, struct clm_tool_result **result)
{
	autofree char *path = arg_string(args, "path");
	autoclosefile FILE *fp = NULL;
	autofree char *line = NULL;
	autofree char *out = NULL;
	size_t out_len = 0, out_cap = 0, line_cap = 0;
	int offset, limit, cur = 0, shown = 0, total = 0;
	char footer[128];

	if (path == NULL)
		return make_result(call->id, "Error: missing required string argument 'path'", result);

	offset = arg_int(args, "offset", 1);
	limit = arg_int(args, "limit", CLM_READ_DEFAULT_LIMIT);
	if (offset < 1)
		offset = 1;
	if (limit < 1)
		limit = CLM_READ_DEFAULT_LIMIT;

	fp = fopen(path, "r");
	if (fp == NULL) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "Error: cannot open '%s': %s", path, strerror(errno));
		return make_result(call->id, buf, result);
	}

	/* Accumulate the requested line window, capped at CLM_TOOL_OUTPUT_CAP. */
	for (;;) {
		ssize_t n = getline(&line, &line_cap, fp);
		if (n < 0)
			break;
		cur++;
		total = cur;
		if (cur < offset || shown >= limit)
			continue;

		if (out_len + (size_t)n + 1 > out_cap) {
			size_t ncap = out_cap ? out_cap * 2 : 4096;
			while (ncap < out_len + (size_t)n + 1)
				ncap *= 2;
			if (ncap > CLM_TOOL_OUTPUT_CAP)
				ncap = CLM_TOOL_OUTPUT_CAP;
			char *p = realloc(out, ncap);
			if (p == NULL)
				return -ENOMEM;
			out = p;
			out_cap = ncap;
		}
		if (out_len + (size_t)n + 1 > CLM_TOOL_OUTPUT_CAP)
			break;
		memcpy(out + out_len, line, (size_t)n);
		out_len += (size_t)n;
		out[out_len] = '\0';
		shown++;
	}

	if (shown == 0) {
		char buf[128];
		(void)snprintf(buf, sizeof(buf), "(file has %d lines; offset %d is past end)", total, offset);
		return make_result(call->id, buf, result);
	}

	(void)snprintf(footer, sizeof(footer),
	    "\n[lines %d-%d of %d%s]", offset, offset + shown - 1, total,
	    (offset + shown - 1 < total) ? "; call read_file with a higher offset to continue" : "");

	/* Append footer (out has room: cap leaves slack, but guard anyway). */
	{
		size_t flen = strlen(footer);
		char *p = realloc(out, out_len + flen + 1);
		if (p == NULL)
			return -ENOMEM;
		out = p;
		memcpy(out + out_len, footer, flen + 1);
	}

	return make_result(call->id, out, result);
}

static int
tool_file_write(const struct clm_tool_call *call, struct json_object *args, struct clm_tool_result **result)
{
	autofree char *path = arg_string(args, "path");
	autofree char *content = arg_string(args, "content");
	autoclosefile FILE *fp = NULL;

	if (path == NULL || content == NULL)
		return make_result(call->id, "Error: write_file requires 'path' and 'content' strings", result);

	fp = fopen(path, "w");
	if (fp == NULL) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "Error: cannot write '%s': %s", path, strerror(errno));
		return make_result(call->id, buf, result);
	}

	if (fputs(content, fp) == EOF)
		return make_result(call->id, "Error: write failed", result);

	return make_result(call->id, "ok: file written", result);
}

static int
tool_shell_exec(const struct clm_tool_call *call, struct json_object *args, struct clm_tool_result **result)
{
	autofree char *command = arg_string(args, "command");
	autofree char *out = NULL;
	FILE *fp;
	char buffer[1024];
	size_t out_len = 0;
	int truncated = 0;

	if (command == NULL)
		return make_result(call->id, "Error: missing required string argument 'command'", result);

	fp = popen(command, "r");
	if (fp == NULL)
		return make_result(call->id, "Error: failed to start command", result);

	out = malloc(CLM_TOOL_OUTPUT_CAP);
	if (out == NULL) {
		(void)pclose(fp);
		return -ENOMEM;
	}
	out[0] = '\0';

	for (;;) {
		size_t n = fread(buffer, 1, sizeof(buffer), fp);
		if (n == 0)
			break;
		if (out_len + n >= CLM_TOOL_OUTPUT_CAP) {
			size_t room = CLM_TOOL_OUTPUT_CAP - 1 - out_len;
			memcpy(out + out_len, buffer, room);
			out_len += room;
			truncated = 1;
			break;
		}
		memcpy(out + out_len, buffer, n);
		out_len += n;
	}
	out[out_len] = '\0';
	(void)pclose(fp);

	if (truncated) {
		static const char marker[] = "\n[output truncated]";
		size_t mlen = sizeof(marker) - 1;
		if (out_len + mlen < CLM_TOOL_OUTPUT_CAP)
			memcpy(out + out_len, marker, mlen + 1);
	}

	if (out_len == 0)
		return make_result(call->id, "(command produced no output)", result);

	return make_result(call->id, out, result);
}

int
clm_tool_execute(struct clm_agent *agent, const struct clm_tool_call *call, struct clm_tool_result **result)
{
	json_cleanup struct json_object *args = NULL;
	size_t i;

	ASSERT_RETURN(agent != NULL, -EINVAL);
	ASSERT_RETURN(call != NULL, -EINVAL);
	ASSERT_RETURN(result != NULL, -EINVAL);

	args = call->args ? json_tokener_parse(call->args) : NULL;
	if (args == NULL || json_object_get_type(args) != json_type_object)
		return make_result(call->id, "Error: tool arguments were not a valid JSON object", result);

	for (i = 0; i < agent->tool_count; i++) {
		if (strcmp(agent->tools[i].name, call->name) != 0)
			continue;
		switch (agent->tools[i].type) {
		case CLM_TOOL_FILE_READ:
			return tool_file_read(call, args, result);
		case CLM_TOOL_FILE_WRITE:
			return tool_file_write(call, args, result);
		case CLM_TOOL_SHELL_EXEC:
			return tool_shell_exec(call, args, result);
		default:
			break;
		}
	}

	return make_result(call->id, "Error: unknown tool", result);
}

static struct json_object *
str_prop(const char *desc)
{
	json_cleanup struct json_object *p = NULL;
	p = json_object_new_object();
	ASSERT_RETURN(p != NULL, NULL);
	struct json_object *jtype = json_object_new_string("string");
	ASSERT_RETURN(jtype != NULL, NULL);
	json_object_object_add(p, "type", jtype);
	struct json_object *jdesc = json_object_new_string(desc);
	ASSERT_RETURN(jdesc != NULL, NULL);
	json_object_object_add(p, "description", jdesc);
	struct json_object *ret = p;
	p = NULL;
	return ret;
}

static struct json_object *
int_prop(const char *desc)
{
	json_cleanup struct json_object *p = NULL;
	p = json_object_new_object();
	ASSERT_RETURN(p != NULL, NULL);
	struct json_object *jtype = json_object_new_string("integer");
	ASSERT_RETURN(jtype != NULL, NULL);
	json_object_object_add(p, "type", jtype);
	struct json_object *jdesc = json_object_new_string(desc);
	ASSERT_RETURN(jdesc != NULL, NULL);
	json_object_object_add(p, "description", jdesc);
	struct json_object *ret = p;
	p = NULL;
	return ret;
}

static struct json_object *
tool_schema(enum clm_tool_type type, const char *name)
{
	const char *desc = "";

	/* Build tool (root under json_cleanup) first */
	json_cleanup struct json_object *tool = NULL;
	tool = json_object_new_object();
	ASSERT_RETURN(tool != NULL, NULL);

	/* Build jfunc and add to tool */
	struct json_object *jfunc = json_object_new_string("function");
	ASSERT_RETURN(jfunc != NULL, NULL);
	json_object_object_add(tool, "type", jfunc);

	/* Build func and attach to tool immediately */
	struct json_object *func = json_object_new_object();
	ASSERT_RETURN(func != NULL, NULL);
	json_object_object_add(tool, "function", func);
	{
		struct json_object *jname = json_object_new_string(name);
		ASSERT_RETURN(jname != NULL, NULL);
		json_object_object_add(func, "name", jname);
	}
	{
		struct json_object *jdesc = json_object_new_string(desc);
		ASSERT_RETURN(jdesc != NULL, NULL);
		json_object_object_add(func, "description", jdesc);
	}

	/* Build params and attach to func immediately */
	struct json_object *params = json_object_new_object();
	ASSERT_RETURN(params != NULL, NULL);
	json_object_object_add(func, "parameters", params);
	{
		struct json_object *jtype = json_object_new_string("object");
		ASSERT_RETURN(jtype != NULL, NULL);
		json_object_object_add(params, "type", jtype);
	}

	/* Build props and add to params */
	struct json_object *props = json_object_new_object();
	ASSERT_RETURN(props != NULL, NULL);
	switch (type) {
	case CLM_TOOL_SHELL_EXEC:
		json_object_object_add(props, "command", str_prop("the shell command to execute"));
		desc = "execute a shell command and return its output";
		break;
	case CLM_TOOL_FILE_READ:
		json_object_object_add(props, "path", str_prop("path to the file"));
		json_object_object_add(props, "offset", int_prop("starting line, 1-indexed (default 1)"));
		json_object_object_add(props, "limit", int_prop("max lines to return (default 200)"));
		desc = "read a window of lines from a text file";
		break;
	case CLM_TOOL_FILE_WRITE:
		json_object_object_add(props, "path", str_prop("path to the file"));
		json_object_object_add(props, "content", str_prop("content to write"));
		desc = "write content to a file, overwriting it";
		break;
	}
	json_object_object_add(params, "properties", props);

	/* Build required and add to params */
	struct json_object *required = json_object_new_array();
	ASSERT_RETURN(required != NULL, NULL);
	json_object_object_add(params, "required", required);
	switch (type) {
	case CLM_TOOL_SHELL_EXEC:
		{
			struct json_object *jcmd = json_object_new_string("command");
			ASSERT_RETURN(jcmd != NULL, NULL);
			json_object_array_add(required, jcmd);
		}
		break;
	case CLM_TOOL_FILE_READ:
		{
			struct json_object *jpath = json_object_new_string("path");
			ASSERT_RETURN(jpath != NULL, NULL);
			json_object_array_add(required, jpath);
		}
		break;
	case CLM_TOOL_FILE_WRITE:
		{
			struct json_object *jpath2 = json_object_new_string("path");
			ASSERT_RETURN(jpath2 != NULL, NULL);
			json_object_array_add(required, jpath2);
			struct json_object *jcontent = json_object_new_string("content");
			ASSERT_RETURN(jcontent != NULL, NULL);
			json_object_array_add(required, jcontent);
		}
		break;
	}

	struct json_object *ret = tool;
	tool = NULL;
	return ret;
}

struct json_object *
clm_tools_build_schema(const struct clm_agent *agent)
{
	struct json_object *arr;
	size_t i;

	if (agent == NULL)
		return NULL;

	arr = json_object_new_array();
	if (arr == NULL)
		return NULL;

	/* json_object_array_add steals the reference; do not put afterward. */
	for (i = 0; i < agent->tool_count; i++)
		json_object_array_add(arr, tool_schema(agent->tools[i].type, agent->tools[i].name));

	return arr;
}
