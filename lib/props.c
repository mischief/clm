// SPDX-License-Identifier: ISC
/*
 * Parsing of llama.cpp's GET /props response. Kept in its own translation unit
 * (pure, json-c only, no libuv/curl/agent state) so it is unit-testable by
 * compiling this one file into the test, without widening the public ABI.
 *
 * This is backend-specific (llama.cpp); as more backend quirks appear this is
 * the kind of thing a per-backend ops module would own.
 */
#include <json-c/json.h>

#include "clm/internal.h"
#include "clm/cleanup.h"

int
clm_parse_props(const char *body, long *ctx_out)
{
	json_cleanup struct json_object *root = NULL;
	struct json_object *dgs = NULL, *nctx = NULL, *slots = NULL, *bi = NULL;
	long n_ctx, n_slots;

	if (body == NULL || ctx_out == NULL)
		return -1;
	root = json_tokener_parse(body);
	if (root == NULL || json_object_get_type(root) != json_type_object)
		return -1;

	/* build_info is llama.cpp-specific; its absence means "not llama.cpp". */
	if (!json_object_object_get_ex(root, "build_info", &bi))
		return -1;

	if (!json_object_object_get_ex(root, "default_generation_settings", &dgs) ||
	    !json_object_object_get_ex(dgs, "n_ctx", &nctx))
		return -1;
	n_ctx = (long)json_object_get_int64(nctx);
	if (n_ctx <= 0)
		return -1;

	/* Context is shared across parallel slots; a conversation gets a share. */
	n_slots = 1;
	if (json_object_object_get_ex(root, "total_slots", &slots)) {
		long s = (long)json_object_get_int64(slots);
		if (s > 1)
			n_slots = s;
	}

	*ctx_out = n_ctx / n_slots;
	return 0;
}
