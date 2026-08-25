// SPDX-License-Identifier: ISC
/*
 * Parsing of an OpenAI-compatible GET /v1/models response body -- {"data":
 * [{"id": "..."}, ...]} -- into a flat list of model ids, and of the context
 * window a catalogue entry carries. Kept in its own pure translation unit
 * (cJSON only, no libuv/curl/agent state) so it is unit-testable by compiling
 * this one file into the test, same rationale as props.c for GET /props.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "clm/internal.h"
#include "clm/cleanup.h"

char **
clm_parse_models_list(const char *body)
{
	json_cleanup cJSON *root = NULL;
	cJSON *data, *item;
	autofreev char **list = NULL;
	size_t cap = 0, n = 0;

	if (body == NULL)
		return NULL;
	root = cJSON_Parse(body);
	if (root == NULL || !cJSON_IsObject(root))
		return NULL;

	data = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (!cJSON_IsArray(data))
		return NULL;

	cJSON_ArrayForEach(item, data)
	{
		cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
		char *dup;

		if (!cJSON_IsString(id) || id->valuestring == NULL)
			continue;

		if (n + 1 >= cap) {
			size_t newcap = cap ? cap * 2 : 16;
			char **grown =
			    realloc(list, (newcap + 1) * sizeof(*grown));
			if (grown == NULL)
				return NULL;
			list = grown;
			cap = newcap;
		}
		dup = strdup(id->valuestring);
		if (dup == NULL)
			return NULL;
		list[n++] = dup;
		list[n] = NULL;
	}

	if (n == 0)
		return NULL;

	char **ret = list;
	list = NULL;
	return ret;
}

void
clm_free_models_list(char **ids)
{
	char **p;

	if (ids == NULL)
		return;
	for (p = ids; *p != NULL; p++)
		free(*p);
	free(ids);
}

/*
 * Context window of one catalogue entry. A listing can report both a nominal
 * window and, under top_provider, the window of the endpoint requests land
 * on; the second one rejects the request, so it wins. Returns 0 on success,
 * -1 when the entry says nothing usable.
 */
static int
entry_ctx(const cJSON *item, int64_t *ctx_out)
{
	const cJSON *top, *v;
	int64_t ctx = 0;

	v = cJSON_GetObjectItemCaseSensitive(item, "context_length");
	if (cJSON_IsNumber(v) && v->valuedouble > 0)
		ctx = (int64_t)v->valuedouble;

	top = cJSON_GetObjectItemCaseSensitive(item, "top_provider");
	if (cJSON_IsObject(top)) {
		v = cJSON_GetObjectItemCaseSensitive(top, "context_length");
		if (cJSON_IsNumber(v) && v->valuedouble > 0)
			ctx = (int64_t)v->valuedouble;
	}

	if (ctx <= 0)
		return -1;
	*ctx_out = ctx;
	return 0;
}

int
clm_parse_models_ctx_for(const char *body, const char *model, int64_t *ctx_out)
{
	json_cleanup cJSON *root = NULL;
	cJSON *data, *item;

	if (body == NULL || model == NULL || ctx_out == NULL)
		return -1;
	root = cJSON_Parse(body);
	if (root == NULL || !cJSON_IsObject(root))
		return -1;

	data = cJSON_GetObjectItemCaseSensitive(root, "data");
	if (!cJSON_IsArray(data))
		return -1;

	cJSON_ArrayForEach(item, data)
	{
		cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");

		if (!cJSON_IsString(id) || id->valuestring == NULL)
			continue;
		if (strcmp(id->valuestring, model) != 0)
			continue;
		return entry_ctx(item, ctx_out);
	}

	return -1;
}
