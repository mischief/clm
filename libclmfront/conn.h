// SPDX-License-Identifier: ISC
/*
 * One place that turns a provider and a model of config.lua into a
 * connection: the url for the provider's wire dialect, the key, the tuning
 * and rate limits, and the reasoning effort. The headless cli, the tui and
 * clm-gtk all start and switch connections, and each wrote this out by hand;
 * two of the copies built the endpoint as ".../chat/completions" whatever
 * the provider's dialect was.
 */
#ifndef CLM_FRONT_CONN_H
#define CLM_FRONT_CONN_H

#include <stdbool.h>

#include "clm/clm.h"

struct clm_lua_cfg;

#define CLM_FRONT_DEFAULT_URL "http://127.0.0.1:8081/v1"
#define CLM_FRONT_DEFAULT_MODEL "local-model"

struct clm_front_conn {
	/*
	 * The connection fields of cfg are filled: base_url, api_key,
	 * provider, provider_name, model, stream, the context tuning, the
	 * rate limits and disable_parallel_tool_calls. The system prompt and
	 * the rest are the caller's. Its strings point into this struct, so
	 * the struct must stay put and outlive any use of cfg; copying it
	 * leaves the copy pointing at the original.
	 */
	struct clm_cfg cfg;
	char url[512];
	char provider[128]; /* empty when none was named */
	char model[256];
	/* From config.lua, which owns them: the model's entry, else the
	 * provider's. NULL takes the backend's default. */
	const char *effort;
	const char *compact_effort;
	/* The provider has a url in config.lua. When it does not, url is the
	 * local default and the caller may want to say so. */
	bool found;
};

/*
 * Resolve provider and model, the two halves of a "provider/model-id" spec
 * (see model_spec.h). Either may be NULL: no provider falls to the local
 * default url, no model to "local-model". lcfg may be NULL. Returns 0 or
 * -ENAMETOOLONG; a provider missing from config.lua is not an error, see
 * found.
 *
 * model_provider names the provider whose models{} entry describes the model.
 * It is the same as provider except when --provider moves the connection to
 * a mirror: the mirror then serves the same model under the original
 * provider's tuning. NULL means provider.
 *
 * The key comes from CLM_API_KEY when that is set, even set empty, as
 * clm-config(5) says; else from the provider's api_key; else none.
 */
int clm_front_conn_resolve(struct clm_lua_cfg *lcfg, const char *provider,
    const char *model_provider, const char *model, struct clm_front_conn *c);

#endif /* CLM_FRONT_CONN_H */
