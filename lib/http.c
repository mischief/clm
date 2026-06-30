// SPDX-License-Identifier: ISC
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "clm/http.h"
#include "clm/cleanup.h"
#include "useful.h"
#include "banned.h"

struct http_buf {
	char *data;
	size_t len;
};

static size_t
http_write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct http_buf *buf = userp;
	char *ptr;

	ptr = realloc(buf->data, buf->len + realsize + 1);
	if (ptr == NULL)
		return 0; /* signal error to curl; caller frees buf->data */

	buf->data = ptr;
	memcpy(buf->data + buf->len, contents, realsize);
	buf->len += realsize;
	buf->data[buf->len] = '\0';

	return realsize;
}

int
clm_http_post(const char *url, const char *api_key, const char *json_body, struct clm_http_response *resp)
{
	CURL *curl;
	CURLcode res;
	struct curl_slist *headers = NULL;
	struct http_buf buf = {0};
	long code = 0;
	char auth_header[512];

	ASSERT_RETURN(url != NULL, -EINVAL);
	ASSERT_RETURN(api_key != NULL, -EINVAL);
	ASSERT_RETURN(json_body != NULL, -EINVAL);
	ASSERT_RETURN(resp != NULL, -EINVAL);

	resp->status_code = 0;
	resp->body = NULL;
	resp->error_msg = NULL;

	if (strlen(api_key) + sizeof("Authorization: Bearer ") >= sizeof(auth_header))
		return -EINVAL;
	(void)snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);

	curl = curl_easy_init();
	if (curl == NULL)
		return -ENOMEM;

	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth_header);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		free(buf.data);
		resp->error_msg = strdup(curl_easy_strerror(res));
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		return -EIO;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	resp->status_code = (int)code;
	resp->body = buf.data;

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return 0;
}

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
