// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "clm/llm.h"
#include "clm/internal.h"
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
