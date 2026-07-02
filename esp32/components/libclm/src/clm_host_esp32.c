// SPDX-License-Identifier: ISC
/*
 * ESP-IDF clm_host implementation — a synchronous esp_http_client transport for
 * the libclm core, replacing the desktop libcurl+libuv host (lib/host_uv.c,
 * excluded on this port).
 *
 * The embedded agent runs on one task with no event loop: http_post() performs
 * a blocking request and invokes the completion callbacks inline. It uses the
 * streaming open/read API so that, for a streamed (SSE) turn, body bytes are
 * fed to data_cb as they arrive off the socket — the core reassembles the
 * "data: {...}" lines itself. For a non-streamed request (health, /props) there
 * is no data_cb, so the whole body is accumulated and handed to success_cb.
 *
 * Timers are not implemented (timer_set/timer_cancel are NULL): the core then
 * disables per-tool timeouts, and the 60s socket timeout bounds a stuck request.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#include "clm/host.h"
#include "clm/http.h"

static const char *TAG = "clm_host";

/* Growable response-body accumulator (non-streaming path only). */
struct body_accum {
	char *data;
	size_t len;
	size_t cap;
	int oom;
};

static int
accum_append(struct body_accum *b, const char *data, size_t n)
{
	if (b->oom)
		return -1;
	if (b->len + n + 1 > b->cap) {
		size_t ncap = b->cap ? b->cap * 2 : 2048;
		while (ncap < b->len + n + 1)
			ncap *= 2;
		char *np = realloc(b->data, ncap);
		if (np == NULL) {
			b->oom = 1;
			return -1;
		}
		b->data = np;
		b->cap = ncap;
	}
	memcpy(b->data + b->len, data, n);
	b->len += n;
	b->data[b->len] = '\0';
	return 0;
}

/* Apply a portable "Name: Value" header to the client (splits on the colon). */
static void
apply_header(esp_http_client_handle_t c, const char *hdr)
{
	const char *colon = strchr(hdr, ':');
	if (colon == NULL)
		return;
	size_t nlen = (size_t)(colon - hdr);
	char name[128];
	if (nlen == 0 || nlen >= sizeof(name))
		return;
	memcpy(name, hdr, nlen);
	name[nlen] = '\0';
	const char *val = colon + 1;
	while (*val == ' ')
		val++;
	esp_http_client_set_header(c, name, val);
}

static int
esp32_http_post(void *ctx, const struct clm_http_req *req,
    clm_http_success_cb success, clm_http_error_cb error,
    clm_http_data_cb data, void *user, struct clm_http_call **out)
{
	(void)ctx;
	char ua[128];

	if (out != NULL)
		*out = NULL; /* synchronous: nothing is ever in flight to cancel */
	if (req == NULL || req->url == NULL || success == NULL || error == NULL)
		return -EINVAL;

	esp_http_client_config_t cfg = {
		.url = req->url,
		.method = req->body != NULL ? HTTP_METHOD_POST : HTTP_METHOD_GET,
		.timeout_ms = 60000,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.buffer_size = 1024,
		.buffer_size_tx = 2048,
	};

	esp_http_client_handle_t c = esp_http_client_init(&cfg);
	if (c == NULL) {
		error(-EIO, "http init failed", user);
		return 0;
	}

	if (req->body != NULL)
		esp_http_client_set_header(c, "Content-Type", "application/json");
	if (req->api_key != NULL && req->api_key[0] != '\0') {
		char auth[512];
		(void)snprintf(auth, sizeof(auth), "Bearer %s", req->api_key);
		esp_http_client_set_header(c, "Authorization", auth);
	}
	if (req->headers != NULL) {
		for (const char *const *h = req->headers; *h != NULL; h++)
			apply_header(c, *h);
	}
	if (req->client_suffix != NULL && req->client_suffix[0] != '\0')
		(void)snprintf(ua, sizeof(ua), "clm-esp32 (tool: %s)",
		    req->client_suffix);
	else
		(void)snprintf(ua, sizeof(ua), "clm-esp32");
	esp_http_client_set_header(c, "User-Agent", ua);
	if (data != NULL)
		esp_http_client_set_header(c, "Accept", "text/event-stream");

	/* Open the connection and stream the request/response by hand so body
	 * chunks can be delivered incrementally. */
	int blen = req->body != NULL ? (int)strlen(req->body) : 0;
	esp_err_t err = esp_http_client_open(c, blen);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
		esp_http_client_cleanup(c);
		error(-EIO, esp_err_to_name(err), user);
		return 0;
	}
	if (blen > 0 && esp_http_client_write(c, req->body, blen) < 0) {
		esp_http_client_close(c);
		esp_http_client_cleanup(c);
		error(-EIO, "request write failed", user);
		return 0;
	}
	if (esp_http_client_fetch_headers(c) < 0) {
		esp_http_client_close(c);
		esp_http_client_cleanup(c);
		error(-EIO, "fetch headers failed", user);
		return 0;
	}

	int status = esp_http_client_get_status_code(c);

	/* Read the body. Streamed turns feed data_cb; everything else accumulates
	 * a whole body for success_cb. */
	struct body_accum body = {0};
	char buf[1024];
	int n;
	while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
		if (data != NULL)
			data(buf, (size_t)n, user);
		else
			(void)accum_append(&body, buf, (size_t)n);
	}

	esp_http_client_close(c);
	esp_http_client_cleanup(c);

	if (n < 0) {
		free(body.data);
		error(-EIO, "response read failed", user);
		return 0;
	}
	if (body.oom) {
		free(body.data);
		error(-ENOMEM, "response too large", user);
		return 0;
	}

	struct clm_http_response resp = {0};
	resp.status_code = status;
	/* On a streamed turn the body was already delivered chunk-by-chunk; the
	 * core reads its accumulated stream state, so pass no body here. */
	resp.body = data != NULL ? NULL : body.data; /* ownership -> callback */
	success(&resp, user);
	return 0;
}

static void
esp32_http_cancel(struct clm_http_call *call)
{
	/* Requests complete synchronously; nothing is ever in flight. */
	(void)call;
}

int
clm_host_esp32_new(struct clm_host **out)
{
	struct clm_host *h;

	if (out == NULL)
		return -EINVAL;

	h = calloc(1, sizeof(*h));
	if (h == NULL)
		return -ENOMEM;

	h->http_post = esp32_http_post;
	h->http_cancel = esp32_http_cancel;
	h->timer_set = NULL; /* no timers: core disables per-tool timeouts */
	h->timer_cancel = NULL;
	h->ctx = NULL;

	*out = h;
	return 0;
}

void
clm_host_esp32_free(struct clm_host *host)
{
	free(host);
}
