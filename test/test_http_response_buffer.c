// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_response_buffer.h"
#include "tap.h"

static int
test_exact_limit(void)
{
	struct http_response_buffer buf = {.limit = 8};
	size_t written;

	TAP_CHECK(http_response_buffer_write(&buf, "abc", 1, 3, true, &written) ==
	        HTTP_RESPONSE_BUFFER_OK,
	    "first buffered write succeeds");
	TAP_CHECK(written == 3, "first buffered write reports its size");
	TAP_CHECK(http_response_buffer_write(&buf, "defgh", 5, 1, true, &written) ==
	        HTTP_RESPONSE_BUFFER_OK,
	    "exact-limit write succeeds");
	TAP_CHECK(written == 5, "exact-limit write reports its size");
	TAP_CHECK(buf.received == 8, "exact-limit write accounts all bytes");
	TAP_CHECK(buf.len == 8, "exact-limit write buffers all bytes");
	TAP_CHECK(strcmp(buf.data, "abcdefgh") == 0,
	    "exact-limit buffer is nul terminated");
	free(buf.data);
	return 0;
}

static int
test_over_limit(void)
{
	struct http_response_buffer buf = {.limit = 4};
	size_t written;

	TAP_CHECK(http_response_buffer_write(&buf, "abcd", 1, 4, true, &written) ==
	        HTTP_RESPONSE_BUFFER_OK,
	    "limit-sized response succeeds");
	TAP_CHECK(http_response_buffer_write(&buf, "e", 1, 1, true, &written) ==
	        HTTP_RESPONSE_BUFFER_TOO_LARGE,
	    "one byte over the limit is rejected");
	TAP_CHECK(written == 0, "over-limit write reports no consumed bytes");
	TAP_CHECK(buf.received == 4 && buf.len == 4,
	    "over-limit write leaves accounting unchanged");
	TAP_CHECK(strcmp(buf.data, "abcd") == 0,
	    "over-limit write leaves buffered data unchanged");
	free(buf.data);
	return 0;
}

static int
test_streaming_does_not_buffer(void)
{
	struct http_response_buffer buf = {.limit = 4};
	size_t written;

	TAP_CHECK(http_response_buffer_write(&buf, "abcd", 2, 2, false, &written) ==
	        HTTP_RESPONSE_BUFFER_OK,
	    "streamed write succeeds");
	TAP_CHECK(written == 4 && buf.received == 4,
	    "streamed write accounts bytes through the limit");
	TAP_CHECK(buf.data == NULL && buf.len == 0,
	    "streamed write does not retain response data");
	TAP_CHECK(http_response_buffer_write(&buf, "e", 1, 1, false, &written) ==
	        HTTP_RESPONSE_BUFFER_TOO_LARGE,
	    "streamed response still enforces the limit");
	return 0;
}

static int
test_multiplication_overflow(void)
{
	struct http_response_buffer buf = {.limit = SIZE_MAX};
	size_t written;

	TAP_CHECK(http_response_buffer_write(&buf, "x", SIZE_MAX, 2, false,
	          &written) == HTTP_RESPONSE_BUFFER_OVERFLOW,
	    "size times nmemb overflow is rejected");
	TAP_CHECK(written == 0 && buf.received == 0,
	    "multiplication overflow leaves accounting unchanged");
	return 0;
}

static int
test_received_overflow(void)
{
	struct http_response_buffer buf = {
	    .received = SIZE_MAX,
	    .limit = SIZE_MAX,
	};
	size_t written;

	TAP_CHECK(http_response_buffer_write(&buf, "x", 1, 1, false, &written) ==
	        HTTP_RESPONSE_BUFFER_OVERFLOW,
	    "received byte count overflow is rejected");
	TAP_CHECK(written == 0 && buf.received == SIZE_MAX,
	    "received overflow leaves accounting unchanged");
	return 0;
}

static int
test_allocation_size_overflow(void)
{
	struct http_response_buffer buf = {
	    .len = SIZE_MAX - 1,
	    .limit = SIZE_MAX,
	};
	size_t written;

	TAP_CHECK(http_response_buffer_write(&buf, "x", 1, 1, true, &written) ==
	        HTTP_RESPONSE_BUFFER_OVERFLOW,
	    "buffer length plus terminator overflow is rejected");
	TAP_CHECK(written == 0 && buf.len == SIZE_MAX - 1,
	    "allocation overflow leaves buffer length unchanged");
	return 0;
}

int
main(void)
{
	TAP_CHECK(tap_add("exact limit", test_exact_limit) == 0,
	    "register exact-limit test");
	TAP_CHECK(tap_add("over limit", test_over_limit) == 0,
	    "register over-limit test");
	TAP_CHECK(tap_add("streaming does not buffer", test_streaming_does_not_buffer) == 0,
	    "register streaming test");
	TAP_CHECK(tap_add("multiplication overflow", test_multiplication_overflow) == 0,
	    "register multiplication-overflow test");
	TAP_CHECK(tap_add("received overflow", test_received_overflow) == 0,
	    "register received-overflow test");
	TAP_CHECK(tap_add("allocation size overflow", test_allocation_size_overflow) == 0,
	    "register allocation-overflow test");
	return tap_run();
}
