// SPDX-License-Identifier: ISC
#ifndef CLM_HTTP_RESPONSE_BUFFER_H
#define CLM_HTTP_RESPONSE_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

enum http_response_buffer_result {
	HTTP_RESPONSE_BUFFER_OK,
	HTTP_RESPONSE_BUFFER_OVERFLOW,
	HTTP_RESPONSE_BUFFER_TOO_LARGE,
	HTTP_RESPONSE_BUFFER_NO_MEMORY,
};

struct http_response_buffer {
	char *data;
	size_t len;
	size_t received;
	size_t limit;
};

enum http_response_buffer_result http_response_buffer_write(
    struct http_response_buffer *buf, const void *contents, size_t size,
    size_t nmemb, bool retain, size_t *written);

#endif /* CLM_HTTP_RESPONSE_BUFFER_H */
