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

#endif
