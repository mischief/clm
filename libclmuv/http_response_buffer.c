// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "http_response_buffer.h"
#include "banned.h"

enum http_response_buffer_result
http_response_buffer_write(struct http_response_buffer *buf,
    const void *contents, size_t size, size_t nmemb, bool retain,
    size_t *written)
{
	char *data;
	size_t new_len;
	size_t new_received;
	size_t realsize;

	*written = 0;
	if (size != 0 && nmemb > SIZE_MAX / size)
		return HTTP_RESPONSE_BUFFER_OVERFLOW;
	realsize = size * nmemb;

	if (realsize > SIZE_MAX - buf->received)
		return HTTP_RESPONSE_BUFFER_OVERFLOW;
	new_received = buf->received + realsize;
	if (new_received > buf->limit)
		return HTTP_RESPONSE_BUFFER_TOO_LARGE;

	if (!retain) {
		buf->received = new_received;
		*written = realsize;
		return HTTP_RESPONSE_BUFFER_OK;
	}

	if (realsize > SIZE_MAX - buf->len)
		return HTTP_RESPONSE_BUFFER_OVERFLOW;
	new_len = buf->len + realsize;
	if (new_len == SIZE_MAX)
		return HTTP_RESPONSE_BUFFER_OVERFLOW;

	data = realloc(buf->data, new_len + 1);
	if (data == NULL)
		return HTTP_RESPONSE_BUFFER_NO_MEMORY;
	memcpy(data + buf->len, contents, realsize);
	data[new_len] = '\0';
	buf->data = data;
	buf->len = new_len;
	buf->received = new_received;
	*written = realsize;
	return HTTP_RESPONSE_BUFFER_OK;
}
