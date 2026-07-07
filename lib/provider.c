// SPDX-License-Identifier: ISC
#include <stddef.h>

#include "clm/provider.h"
#include "banned.h"

extern const struct clm_provider_ops clm_provider_ops_openai;
extern const struct clm_provider_ops clm_provider_ops_anthropic;

const struct clm_provider_ops *
clm_provider_ops_get(enum clm_provider provider)
{
	switch (provider) {
	case CLM_PROVIDER_ANTHROPIC:
		return &clm_provider_ops_anthropic;
	case CLM_PROVIDER_OPENAI:
	case CLM_PROVIDER_OLLAMA:
	default:
		/* Every OpenAI-compatible server (which is what OLLAMA means
		 * here -- see clm_provider_from_str()) speaks the canonical
		 * shape directly, so it gets the identity ops too. */
		return &clm_provider_ops_openai;
	}
}
