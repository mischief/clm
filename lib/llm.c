// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "clm/llm.h"
#include "clm/http.h"
#include "clm/internal.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

int
clm_llm_new(struct clm_llm **ret, enum clm_provider provider, const char *api_key, const char *base_url, const char *model)
{
	struct clm_llm *llm;

	ASSERT_RETURN(ret != NULL, -EINVAL);
	ASSERT_RETURN(api_key != NULL, -EINVAL);
	ASSERT_RETURN(base_url != NULL, -EINVAL);
	ASSERT_RETURN(model != NULL, -EINVAL);

	llm = calloc(1, sizeof(*llm));
	if (llm == NULL)
		return -ENOMEM;

	llm->provider = provider;
	llm->api_key = strdup(api_key);
	llm->base_url = strdup(base_url);
	llm->model = strdup(model);

	if (llm->api_key == NULL || llm->base_url == NULL || llm->model == NULL) {
		clm_llm_free(llm);
		return -ENOMEM;
	}

	*ret = llm;
	return 0;
}

void
clm_llm_free(struct clm_llm *llm)
{
	if (llm == NULL)
		return;
	free(llm->api_key);
	free(llm->base_url);
	free(llm->model);
	free(llm);
}

int
clm_llm_chat(struct clm_llm *llm, struct json_object *messages,
    struct json_object *tools, struct clm_http_response *resp)
{
	json_cleanup struct json_object *req = NULL;
	struct json_object *jmodel = NULL;
	struct json_object *jstream = NULL;
	const char *body;
	int r;

	ASSERT_RETURN(llm != NULL, -EINVAL);
	ASSERT_RETURN(messages != NULL, -EINVAL);
	ASSERT_RETURN(resp != NULL, -EINVAL);

	req = json_object_new_object();
	ASSERT_RETURN(req != NULL, -ENOMEM);

	jmodel = json_object_new_string(llm->model);
	ASSERT_RETURN(jmodel != NULL, -ENOMEM);
	json_object_object_add(req, "model", jmodel);

	json_object_object_add(req, "messages", json_object_get(messages));

	jstream = json_object_new_boolean(0);
	ASSERT_RETURN(jstream != NULL, -ENOMEM);
	json_object_object_add(req, "stream", jstream);

	if (tools != NULL)
		json_object_object_add(req, "tools", json_object_get(tools));

	body = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
	if (body == NULL)
		return -ENOMEM;

	r = clm_http_post(llm->base_url, llm->api_key, body, resp);
	if (r < 0)
		return r;

	if (resp->status_code != 200) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "HTTP %d", resp->status_code);
		free(resp->error_msg);
		resp->error_msg = strdup(buf);
		return -EIO;
	}

	return 0;
}
