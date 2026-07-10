// SPDX-License-Identifier: ISC
/*
 * Core HTTP helpers that are transport-independent. The response struct
 * (clm/http.h) is filled by whatever clm_host transport is in use and consumed
 * by the core, so its destructor belongs in the core rather than in any one
 * transport (desktop curl or the embedded esp_http_client host).
 */
#include <stdlib.h>

#include "clm/http.h"
#include "banned.h"

void
clm_http_response_free(struct clm_http_response *resp)
{
	if (resp == NULL)
		return;
	free(resp->body);
	free(resp->error_msg);
	resp->body = NULL;
	resp->error_msg = NULL;
	resp->status_code = 0;
}
