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
test_inline_completion(void *arg)
{
	(void)arg;
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

/*
 * libcurl's global state is refcounted across muxes. Freeing one mux while
 * another lives must not tear it down, and a mux created after the last one
 * went away must find it initialized again.
 */
static int
test_overlapping_muxes(void *arg)
{
	struct clm_http_mux *a, *b, *c;
	uv_loop_t loop;

	(void)arg;
	callbacks = 0;
	CHECK(uv_loop_init(&loop) == 0, "loop init");
	a = clm_http_mux_new(&loop);
	b = clm_http_mux_new(&loop);
	CHECK(a != NULL && b != NULL, "two muxes");
	if (a == NULL || b == NULL)
		return 1;

	clm_http_mux_free(a);
	uv_run(&loop, UV_RUN_DEFAULT);
	CHECK(clm_http_async_post(b, "://invalid", NULL, NULL, NULL, on_success,
	          on_error, NULL, NULL, NULL, NULL) == 0,
	    "the surviving mux still works");
	uv_run(&loop, UV_RUN_DEFAULT);
	clm_http_mux_free(b);
	uv_run(&loop, UV_RUN_DEFAULT);

	c = clm_http_mux_new(&loop);
	CHECK(c != NULL, "a mux after the last one is freed");
	if (c != NULL) {
		CHECK(clm_http_async_post(c, "://invalid", NULL, NULL, NULL,
		          on_success, on_error, NULL, NULL, NULL, NULL) == 0,
		    "and it still works");
		uv_run(&loop, UV_RUN_DEFAULT);
		clm_http_mux_free(c);
		uv_run(&loop, UV_RUN_DEFAULT);
	}
	CHECK(callbacks == 2, "both requests completed");
	CHECK(uv_loop_close(&loop) == 0, "loop close");
	return 0;
}

int
main(void)
{
	TAP_ADD("inline completion", test_inline_completion, NULL);
	TAP_ADD("overlapping muxes", test_overlapping_muxes, NULL);
	return tap_run();
}
