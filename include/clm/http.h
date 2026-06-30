// SPDX-License-Identifier: ISC
#ifndef CLM_HTTP_H
#define CLM_HTTP_H

#include <stddef.h>

struct clm_http_response {
	int status_code;
	char *body;
	char *error_msg;
};

int clm_http_post(const char *url, const char *api_key, const char *json_body, struct clm_http_response *resp);
void clm_http_response_free(struct clm_http_response *resp);

#endif
