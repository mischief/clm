// SPDX-License-Identifier: ISC
/*
 * Shared by main.c and tui.c. Each key is read from the provider first and
 * the model entry second, the shape `effort` already has.
 */
#ifndef CLM_CFG_TUNING_H
#define CLM_CFG_TUNING_H

#include "clm/clm.h"
#include "clm/lua_plugin.h"

/* Context tuning for one connection: provider values, model overriding. */
static inline void
clm_cfg_apply_tuning(struct clm_lua_cfg *lcfg, const char *provider,
    const char *model, struct clm_cfg *cfg)
{
	int64_t size, tokens, pct, vision, files;

	if (lcfg == NULL || cfg == NULL || provider == NULL)
		return;

	size = clm_lua_cfg_provider_int(lcfg, provider, "context_size", 0);
	pct = clm_lua_cfg_provider_int(lcfg, provider, "autocompact_pct", 0);
	tokens =
	    clm_lua_cfg_provider_int(lcfg, provider, "autocompact_tokens", 0);
	vision = clm_lua_cfg_provider_int(lcfg, provider, "vision", 0);
	files = clm_lua_cfg_provider_int(lcfg, provider, "file_uploads", 0);

	if (model != NULL) {
		size = clm_lua_cfg_provider_model_int(
		    lcfg, provider, model, "context_size", size);
		pct = clm_lua_cfg_provider_model_int(
		    lcfg, provider, model, "autocompact_pct", pct);
		tokens = clm_lua_cfg_provider_model_int(
		    lcfg, provider, model, "autocompact_tokens", tokens);
		vision = clm_lua_cfg_provider_model_int(
		    lcfg, provider, model, "vision", vision);
		files = clm_lua_cfg_provider_model_int(
		    lcfg, provider, model, "file_uploads", files);
	}

	cfg->context_size = size;
	cfg->autocompact_pct = (int)pct;
	cfg->autocompact_tokens = tokens;
	cfg->vision = vision > 0 ? 1 : vision < 0 ? -1 : 0;
	cfg->file_uploads = files > 0 ? 1 : files < 0 ? -1 : 0;
}

#endif /* CLM_CFG_TUNING_H */
