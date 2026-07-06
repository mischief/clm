// SPDX-License-Identifier: ISC
/*
 * Parsing of llama.cpp's GET /props response. Kept in its own translation unit
 * (pure, cJSON only, no libuv/curl/agent state) so it is unit-testable by
 * compiling this one file into the test, without widening the public ABI.
 *
 * This is backend-specific (llama.cpp); as more backend quirks appear this is
 * the kind of thing a per-backend ops module would own.
 */
#include <stdint.h>

#include <cJSON.h>

#include "clm/internal.h"
#include "clm/cleanup.h"

int
clm_parse_props(const char *body, int64_t *ctx_out)
{
	json_cleanup cJSON *root = NULL;
	cJSON *dgs = NULL, *nctx = NULL, *slots = NULL, *bi = NULL;
	int64_t n_ctx, n_slots;

	if (body == NULL || ctx_out == NULL)
		return -1;
	root = cJSON_Parse(body);
	if (root == NULL || !cJSON_IsObject(root))
		return -1;

	/* build_info is llama.cpp-specific; its absence means "not llama.cpp". */
	bi = cJSON_GetObjectItemCaseSensitive(root, "build_info");
	if (!bi)
		return -1;

	dgs = cJSON_GetObjectItemCaseSensitive(root, "default_generation_settings");
	if (!dgs || !(nctx = cJSON_GetObjectItemCaseSensitive(dgs, "n_ctx")))
		return -1;
	n_ctx = (int64_t)cJSON_GetNumberValue(nctx);
	if (n_ctx <= 0)
		return -1;

	/* Context is shared across parallel slots; a conversation gets a share. */
	n_slots = 1;
	slots = cJSON_GetObjectItemCaseSensitive(root, "total_slots");
	if (slots) {
		int64_t s = (int64_t)cJSON_GetNumberValue(slots);
		if (s > 1)
			n_slots = s;
	}

	/*
	 * Context sizes fit comfortably in a long on every target; assign
	 * without a cast so newer compilers don't flag a useless cast on LP64.
	 */
	*ctx_out = n_ctx / n_slots;
	return 0;
}