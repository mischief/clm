// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_response_buffer.h"

static int failures;

#define CHECK(cond, msg)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "fail: %s (%s:%d)\n", (msg), __FILE__, \
			    __LINE__);                                         \
			failures++;                                            \
		}                                                              \
	} while (0)

static void
test_exact_limit(void)
{
	struct http_response_buffer buf = {.limit = 8};
	size_t written;

	CHECK(http_response_buffer_write(&buf, "abc", 1, 3, true,
	    &written) == HTTP_RESPONSE_BUFFER_OK, "first buffered write succeeds");
	CHECK(written == 3, "first buffered write reports its size");
	CHECK(http_response_buffer_write(&buf, "defgh", 5, 1, true,
	    &written) == HTTP_RESPONSE_BUFFER_OK, "exact-limit write succeeds");
	CHECK(written == 5, "exact-limit write reports its size");
	CHECK(buf.received == 8, "exact-limit write accounts all bytes");
	CHECK(buf.len == 8, "exact-limit write buffers all bytes");
	CHECK(strcmp(buf.data, "abcdefgh") == 0,
	    "exact-limit buffer is nul terminated");
	free(buf.data);
}

static void
test_over_limit(void)
{
	struct http_response_buffer buf = {.limit = 4};
	size_t written;

	CHECK(http_response_buffer_write(&buf, "abcd", 1, 4, true,
	    &written) == HTTP_RESPONSE_BUFFER_OK, "limit-sized response succeeds");
	CHECK(http_response_buffer_write(&buf, "e", 1, 1, true,
	    &written) == HTTP_RESPONSE_BUFFER_TOO_LARGE,
	    "one byte over the limit is rejected");
	CHECK(written == 0, "over-limit write reports no consumed bytes");
	CHECK(buf.received == 4 && buf.len == 4,
	    "over-limit write leaves accounting unchanged");
	CHECK(strcmp(buf.data, "abcd") == 0,
	    "over-limit write leaves buffered data unchanged");
	free(buf.data);
}

static void
test_streaming_does_not_buffer(void)
{
	struct http_response_buffer buf = {.limit = 4};
	size_t written;

	CHECK(http_response_buffer_write(&buf, "abcd", 2, 2, false,
	    &written) == HTTP_RESPONSE_BUFFER_OK, "streamed write succeeds");
	CHECK(written == 4 && buf.received == 4,
	    "streamed write accounts bytes through the limit");
	CHECK(buf.data == NULL && buf.len == 0,
	    "streamed write does not retain response data");
	CHECK(http_response_buffer_write(&buf, "e", 1, 1, false,
	    &written) == HTTP_RESPONSE_BUFFER_TOO_LARGE,
	    "streamed response still enforces the limit");
}

static void
test_multiplication_overflow(void)
{
	struct http_response_buffer buf = {.limit = SIZE_MAX};
	size_t written;

	CHECK(http_response_buffer_write(&buf, "x", SIZE_MAX, 2, false,
	    &written) == HTTP_RESPONSE_BUFFER_OVERFLOW,
	    "size times nmemb overflow is rejected");
	CHECK(written == 0 && buf.received == 0,
	    "multiplication overflow leaves accounting unchanged");
}

static void
test_received_overflow(void)
{
	struct http_response_buffer buf = {
		.received = SIZE_MAX,
		.limit = SIZE_MAX,
	};
	size_t written;

	CHECK(http_response_buffer_write(&buf, "x", 1, 1, false,
	    &written) == HTTP_RESPONSE_BUFFER_OVERFLOW,
	    "received byte count overflow is rejected");
	CHECK(written == 0 && buf.received == SIZE_MAX,
	    "received overflow leaves accounting unchanged");
}

static void
test_allocation_size_overflow(void)
{
	struct http_response_buffer buf = {
		.len = SIZE_MAX - 1,
		.limit = SIZE_MAX,
	};
	size_t written;

	CHECK(http_response_buffer_write(&buf, "x", 1, 1, true,
	    &written) == HTTP_RESPONSE_BUFFER_OVERFLOW,
	    "buffer length plus terminator overflow is rejected");
	CHECK(written == 0 && buf.len == SIZE_MAX - 1,
	    "allocation overflow leaves buffer length unchanged");
}

int
main(void)
{
	test_exact_limit();
	test_over_limit();
	test_streaming_does_not_buffer();
	test_multiplication_overflow();
	test_received_overflow();
	test_allocation_size_overflow();

	if (failures != 0) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	printf("all http response buffer tests passed\n");
	return 0;
}
