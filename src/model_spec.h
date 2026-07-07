// SPDX-License-Identifier: ISC
/*
 * Shared by main.c and tui.c: both resolve a "provider/model-id" spec (the
 * one format `model` takes everywhere -- config.lua's top-level `model`
 * key, agent profiles' own `model`, -m/--model, and the TUI's /model) down
 * to a provider connection + wire model id. Header-only static inline
 * rather than a new libclm.so export: this is pure string splitting with
 * no dependency on clm's Lua/agent state, and duplicating the two dozen
 * call sites across main.c/tui.c that would otherwise diverge is worse
 * than one shared header both already include indirectly via frontend.h's
 * neighborhood.
 */
#ifndef CLM_MODEL_SPEC_H
#define CLM_MODEL_SPEC_H

#include <string.h>

/*
 * Split spec at its first '/' into a provider name and a wire model id.
 *
 * On a match: *provider_out is a fresh malloc'd copy of the text before
 * the '/' (caller must free); *model_out borrows the remainder of spec
 * (everything after that first '/', valid exactly as long as spec is --
 * including any further '/' characters in it unsplit, since a wire model
 * id can itself contain one, e.g. huggingface's "Qwen/Qwen3-32B").
 *
 * If spec has no '/' at all (a bare wire id with no addressed provider,
 * e.g. typed straight from a live /v1/models listing): *provider_out is
 * NULL and *model_out borrows the whole of spec unchanged. Callers can
 * uniformly treat *model_out as "the wire id to request" either way, only
 * branching on *provider_out being NULL to fall back to whatever
 * provider is otherwise already active.
 *
 * spec may be NULL, in which case both outputs come back NULL.
 */
static inline void
split_provider_model(const char *spec, char **provider_out,
    const char **model_out)
{
	const char *slash = spec != NULL ? strchr(spec, '/') : NULL;

	if (spec == NULL) {
		*provider_out = NULL;
		*model_out = NULL;
	} else if (slash == NULL) {
		*provider_out = NULL;
		*model_out = spec;
	} else {
		*provider_out = strndup(spec, (size_t)(slash - spec));
		*model_out = slash + 1;
	}
}

#endif /* CLM_MODEL_SPEC_H */
