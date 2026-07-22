// SPDX-License-Identifier: ISC
#ifndef CLM_HTTP_H
#define CLM_HTTP_H

#include <stddef.h>

struct clm_http_response {
	int status_code;
	char *body;
	char *error_msg;
};

void clm_http_response_free(struct clm_http_response *resp);

/* HTTP completion callbacks. Exactly one of success/error fires per request. */
typedef void (*clm_http_success_cb)(struct clm_http_response *resp, void *user);
typedef void (*clm_http_error_cb)(int error_code, const char *error_msg, void *user);

/*
 * optional callback delivering response body bytes as they arrive. only invoked
 * for a 2xx response, so a streaming consumer never sees error bodies. on a
 * successful streamed request, the success callback receives a null body.
 */
typedef void (*clm_http_data_cb)(const char *data, size_t len, void *user);

#endif
