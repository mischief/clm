// SPDX-License-Identifier: ISC
/*
 * Parsing of an OpenAI-compatible GET /v1/models response body -- {"data":
 * [{"id": "..."}, ...]} -- into a flat list of model ids. Kept in its own
 * pure translation unit (cJSON only, no libuv/curl/agent state) so it is
 * unit-testable by compiling this one file into the test, same rationale
 * as props.c for GET /props.
 */
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

	cJSON_ArrayForEach(item, data) {
		cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
		char *dup;

		if (!cJSON_IsString(id) || id->valuestring == NULL)
			continue;

		if (n + 1 >= cap) {
			size_t newcap = cap ? cap * 2 : 16;
			char **grown = realloc(list,
			    (newcap + 1) * sizeof(*grown));
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
