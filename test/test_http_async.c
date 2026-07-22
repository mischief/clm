// SPDX-License-Identifier: ISC
#include <stdio.h>

#include <uv.h>

#include "clm/http_async.h"

static int failures;
static int callbacks;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "fail: %s (%s:%d)\n", (msg), __FILE__,  \
			    __LINE__);                                         \
			failures++;                                            \
		}                                                              \
	} while (0)

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

int
main(void)
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
	r = clm_http_async_post(mux, "://invalid", NULL, NULL, NULL,
	    on_success, on_error, NULL, NULL, NULL, &req);
	CHECK(r == 0, "inline completion accepted");
	CHECK(req == NULL, "inline completion returned no handle");
	uv_run(&loop, UV_RUN_DEFAULT);
	CHECK(callbacks == 1, "inline completion delivered once");
	clm_http_mux_free(mux);
	uv_run(&loop, UV_RUN_DEFAULT);
	CHECK(uv_loop_close(&loop) == 0, "loop close");

	if (failures != 0) {
		printf("test_http_async: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_http_async: pass\n");
	return 0;
}
