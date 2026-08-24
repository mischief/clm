// SPDX-License-Identifier: ISC
#include <stdio.h>

#include <uv.h>

#include "clm/http_async.h"
#include "tap.h"

static int callbacks;

#define CHECK(cond, msg) TAP_CHECK(cond, msg)

static void
on_success(struct clm_http_response *resp, void *user)
{
	(void)resp;
	(void)user;
	callbacks++;
}

static void
on_error(int error_code, const char *error_msg, void *user)
{
	(void)error_code;
	(void)error_msg;
	(void)user;
	callbacks++;
}

static int
test_inline_completion(void)
{
	struct clm_http_request *req = (struct clm_http_request *)1;
	struct clm_http_mux *mux;
	uv_loop_t loop;
	int r;

	CHECK(uv_loop_init(&loop) == 0, "loop init");
	mux = clm_http_mux_new(&loop);
	CHECK(mux != NULL, "mux init");
	if (mux == NULL)
		return 1;
	r = clm_http_async_post(mux, "://invalid", NULL, NULL, NULL, on_success,
	    on_error, NULL, NULL, NULL, &req);
	CHECK(r == 0, "inline completion accepted");
	CHECK(req == NULL, "inline completion returned no handle");
	uv_run(&loop, UV_RUN_DEFAULT);
	CHECK(callbacks == 1, "inline completion delivered once");
	clm_http_mux_free(mux);
	uv_run(&loop, UV_RUN_DEFAULT);
	CHECK(uv_loop_close(&loop) == 0, "loop close");
	return 0;
}

int
main(void)
{
	TAP_CHECK(tap_add("inline completion", test_inline_completion) == 0,
	    "register inline completion test");
	return tap_run();
}
