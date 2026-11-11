// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "clm/clm.h"
#include "clm/lua_plugin.h"
#include "cfg_tuning.h"
#include "conn.h"
#include "fileio.h"
#ifdef CLM_OAUTH
#include "oauth.h"
#endif
#include "banned.h"

/* A key of the model's entry under mprov, else of provider's, or NULL. */
static const char *
model_or_provider_str(struct clm_lua_cfg *lcfg, const char *provider,
    const char *mprov, const char *model, const char *key)
{
	const char *v = NULL;

	if (lcfg == NULL)
		return NULL;
	if (mprov != NULL && model != NULL)
		v = clm_lua_cfg_provider_model_str(lcfg, mprov, model, key);
	if (v == NULL && provider != NULL)
		v = clm_lua_cfg_provider_str(lcfg, provider, key);
	return v;
}

/* Copy s into buf; -ENAMETOOLONG rather than a cut name. */
static int
copy_name(char *buf, size_t size, const char *s)
{
	size_t n = strlen(s);

	if (n >= size)
		return -ENAMETOOLONG;
	memcpy(buf, s, n + 1);
	return 0;
}

#ifdef CLM_OAUTH
/*
 * Subscription OAuth: a provider that names an oauth_store gets a fresh
 * access token (refreshed as needed) as its key. The token outlives the
 * call in a slot of its own, since cfg.api_key points at it; the agent
 * copies it, and the next resolve replaces it.
 */
static const char *
oauth_key(struct clm_lua_cfg *lcfg, const char *provider, const char *key)
{
	static char *token;
	const char *store, *cid;
	char *path, *tok = NULL, *err = NULL;

	if (lcfg == NULL || provider == NULL)
		return key;
	store = clm_lua_cfg_provider_str(lcfg, provider, "oauth_store");
	if (store == NULL || store[0] == '\0')
		return key;
	cid = clm_lua_cfg_provider_str(lcfg, provider, "oauth_client_id");
	path = clm_oauth_expand(store);
	if (path != NULL && clm_oauth_resolve(path, cid, &tok, &err) == 0 &&
	    tok != NULL) {
		free(token);
		token = tok;
		key = token;
	} else {
		clm_dprintf(STDERR_FILENO, "oauth: %s\n",
		    err != NULL ? err : "resolve failed");
		free(tok);
	}
	free(err);
	free(path);
	return key;
}
#endif

int
clm_front_conn_resolve(struct clm_lua_cfg *lcfg, const char *provider,
    const char *model_provider, const char *model, struct clm_front_conn *c)
{
	const char *url = NULL, *key = NULL, *kind = NULL;
	const char *env = getenv("CLM_API_KEY");
	const char *mprov = model_provider != NULL ? model_provider : provider;
	int r;

	memset(c, 0, sizeof(*c));
	r = copy_name(
	    c->provider, sizeof(c->provider), provider != NULL ? provider : "");
	if (r == 0)
		r = copy_name(c->model, sizeof(c->model),
		    model != NULL ? model : CLM_FRONT_DEFAULT_MODEL);
	if (r < 0)
		return r;

	if (lcfg != NULL && provider != NULL) {
		url = clm_lua_cfg_provider_str(lcfg, provider, "url");
		key = clm_lua_cfg_provider_str(lcfg, provider, "api_key");
		kind = clm_lua_cfg_provider_str(lcfg, provider, "kind");
	}
	c->found = url != NULL;
	/* The environment overrides config when it is set at all; set empty,
	 * it says the connection needs no key. An empty key from config is the
	 * same answer, and so is none. */
	if (env != NULL)
		key = env;
#ifdef CLM_OAUTH
	else
		key = oauth_key(lcfg, provider, key);
#endif
	if (key == NULL)
		key = "";

	c->cfg.provider = clm_provider_from_str(kind);
	c->cfg.provider_name = c->provider[0] != '\0' ? c->provider : NULL;
	c->cfg.api_key = key;
	c->cfg.model = c->model;
	c->cfg.stream = 1;
	c->cfg.disable_parallel_tool_calls = true;
	clm_provider_build_url(c->url, sizeof(c->url),
	    url != NULL ? url : CLM_FRONT_DEFAULT_URL, c->cfg.provider);
	c->cfg.base_url = c->url;

	if (lcfg != NULL && provider != NULL) {
		clm_cfg_apply_tuning(lcfg, mprov, c->model, &c->cfg);
		c->cfg.rate_tokens_per_sec = clm_lua_cfg_provider_int(
		    lcfg, provider, "rate_tokens_per_sec", 0);
		c->cfg.rate_burst =
		    clm_lua_cfg_provider_int(lcfg, provider, "rate_burst", 0);
		c->cfg.disable_parallel_tool_calls =
		    clm_lua_cfg_provider_int(
		        lcfg, provider, "disable_parallel_tool_calls", 1) != 0;
	}
	c->effort =
	    model_or_provider_str(lcfg, provider, mprov, c->model, "effort");
	c->compact_effort = model_or_provider_str(
	    lcfg, provider, mprov, c->model, "compact_effort");
	return 0;
}
