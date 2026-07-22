// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <arpa/inet.h>

#include <uv.h>

#include "canned.h"

struct conn;

struct canned_server {
	uv_loop_t *loop;
	uv_tcp_t listener;
	int port;

	char **replies;
	int *statuses;
	size_t reply_head;
	size_t reply_count;
	size_t reply_cap;

	char *last_request;
	size_t request_count;
	bool pause_next;
	struct conn *paused;
};

struct conn {
	struct canned_server *srv;
	uv_tcp_t handle;
	uv_write_t wreq;
	char *buf;
	size_t len;
	size_t cap;
	char *resp;
	bool request_seen;
	bool paused;
	bool responded;
};

static void
alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf)
{
	(void)handle;
	buf->base = malloc(suggested);
	buf->len = buf->base ? suggested : 0;
}

static void
conn_append(struct conn *c, const char *data, size_t n)
{
	if (c->len + n + 1 > c->cap) {
		size_t nc = c->cap ? c->cap * 2 : 4096;
		char *p;
		while (nc < c->len + n + 1)
			nc *= 2;
		p = realloc(c->buf, nc);
		if (p == NULL)
			return;
		c->buf = p;
		c->cap = nc;
	}
	memcpy(c->buf + c->len, data, n);
	c->len += n;
	c->buf[c->len] = '\0';
}

static long
parse_content_length(const char *buf)
{
	const char *p = buf;
	while (*p != '\0') {
		const char *nl;
		if (strncasecmp(p, "content-length:", 15) == 0) {
			p += 15;
			while (*p == ' ' || *p == '\t')
				p++;
			return strtol(p, NULL, 10);
		}
		nl = strchr(p, '\n');
		if (nl == NULL)
			break;
		p = nl + 1;
	}
	return -1;
}

static bool
request_complete(struct conn *c)
{
	const char *hdr_end;
	size_t header_len;
	long clen;

	if (c->buf == NULL)
		return false;
	hdr_end = strstr(c->buf, "\r\n\r\n");
	if (hdr_end == NULL)
		return false;
	header_len = (size_t)(hdr_end - c->buf) + 4;
	clen = parse_content_length(c->buf);
	if (clen < 0)
		return true;
	return c->len >= header_len + (size_t)clen;
}

static const char *
canned_pop(struct canned_server *srv, int *status_out)
{
	if (srv->reply_head < srv->reply_count) {
		size_t i = srv->reply_head++;
		*status_out = srv->statuses[i];
		return srv->replies[i];
	}
	*status_out = 200;
	return "{}";
}

static const char *
status_line_for(int status)
{
	switch (status) {
	case 200: return "200 OK";
	case 400: return "400 Bad Request";
	case 401: return "401 Unauthorized";
	case 403: return "403 Forbidden";
	case 404: return "404 Not Found";
	case 429: return "429 Too Many Requests";
	case 500: return "500 Internal Server Error";
	default: return "500 Internal Server Error";
	}
}

static void
on_conn_close(uv_handle_t *handle)
{
	struct conn *c = handle->data;
	free(c->buf);
	free(c->resp);
	free(c);
}

static void
on_write(uv_write_t *req, int status)
{
	struct conn *c = req->data;
	(void)status;
	uv_close((uv_handle_t *)&c->handle, on_conn_close);
}

static void
send_response(struct conn *c)
{
	struct canned_server *srv = c->srv;
	int status;
	const char *body = canned_pop(srv, &status);
	size_t blen = strlen(body);
	const char *fmt =
	    "HTTP/1.1 %s\r\n"
	    "Content-Type: application/json\r\n"
	    "Content-Length: %zu\r\n"
	    "Connection: close\r\n\r\n";
	const char *status_line = status_line_for(status);
	int hlen;
	uv_buf_t b;

	uv_read_stop((uv_stream_t *)&c->handle);
	hlen = snprintf(NULL, 0, fmt, status_line, blen);
	c->resp = malloc((size_t)hlen + blen + 1);
	if (c->resp == NULL) {
		uv_close((uv_handle_t *)&c->handle, on_conn_close);
		return;
	}
	(void)snprintf(c->resp, (size_t)hlen + 1, fmt, status_line, blen);
	memcpy(c->resp + hlen, body, blen + 1);

	c->responded = true;
	c->wreq.data = c;
	b = uv_buf_init(c->resp, (unsigned)((size_t)hlen + blen));
	uv_write(&c->wreq, (uv_stream_t *)&c->handle, &b, 1, on_write);
}

static void
respond(struct conn *c)
{
	struct canned_server *srv = c->srv;

	free(srv->last_request);
	srv->last_request = strdup(c->buf ? c->buf : "");
	srv->request_count++;
	c->request_seen = true;

	if (srv->pause_next) {
		srv->pause_next = false;
		srv->paused = c;
		c->paused = true;
		return;
	}
	send_response(c);
}

static void
on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
	struct conn *c = stream->data;

	if (nread > 0) {
		conn_append(c, buf->base, (size_t)nread);
		if (!c->request_seen && request_complete(c))
			respond(c);
	} else if (nread < 0) {
		if (!c->responded) {
			if (c->paused && c->srv->paused == c)
				c->srv->paused = NULL;
			uv_close((uv_handle_t *)stream, on_conn_close);
		}
	}
	free(buf->base);
}

static void
on_connection(uv_stream_t *server, int status)
{
	struct canned_server *srv = server->data;
	struct conn *c;

	if (status < 0)
		return;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return;
	c->srv = srv;
	uv_tcp_init(srv->loop, &c->handle);
	c->handle.data = c;
	if (uv_accept(server, (uv_stream_t *)&c->handle) == 0)
		uv_read_start((uv_stream_t *)&c->handle, alloc_cb, on_read);
	else
		uv_close((uv_handle_t *)&c->handle, on_conn_close);
}

struct canned_server *
canned_start(uv_loop_t *loop)
{
	struct canned_server *srv;
	struct sockaddr_in addr;
	struct sockaddr_storage ss;
	int namelen = sizeof(ss);

	srv = calloc(1, sizeof(*srv));
	if (srv == NULL)
		return NULL;
	srv->loop = loop;

	if (uv_ip4_addr("127.0.0.1", 0, &addr) != 0 ||
	    uv_tcp_init(loop, &srv->listener) != 0) {
		free(srv);
		return NULL;
	}
	srv->listener.data = srv;
	if (uv_tcp_bind(&srv->listener, (const struct sockaddr *)&addr, 0) != 0 ||
	    uv_listen((uv_stream_t *)&srv->listener, 16, on_connection) != 0) {
		uv_close((uv_handle_t *)&srv->listener, NULL);
		free(srv);
		return NULL;
	}
	if (uv_tcp_getsockname(&srv->listener, (struct sockaddr *)&ss, &namelen) != 0) {
		uv_close((uv_handle_t *)&srv->listener, NULL);
		free(srv);
		return NULL;
	}
	srv->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
	return srv;
}

int
canned_port(const struct canned_server *s)
{
	return s->port;
}

void
canned_reply_status(struct canned_server *s, int status, const char *body)
{
	if (s->reply_count == s->reply_cap) {
		size_t nc = s->reply_cap ? s->reply_cap * 2 : 8;
		char **p = realloc(s->replies, nc * sizeof(*p));
		int *sp;
		if (p == NULL)
			return;
		s->replies = p;
		sp = realloc(s->statuses, nc * sizeof(*sp));
		if (sp == NULL)
			return;
		s->statuses = sp;
		s->reply_cap = nc;
	}
	s->statuses[s->reply_count] = status;
	s->replies[s->reply_count++] = strdup(body);
}

void
canned_reply(struct canned_server *s, const char *json_body)
{
	canned_reply_status(s, 200, json_body);
}

void
canned_pause_next(struct canned_server *s)
{
	if (s != NULL)
		s->pause_next = true;
}

void
canned_resume(struct canned_server *s)
{
	struct conn *c;

	if (s == NULL || s->paused == NULL)
		return;
	c = s->paused;
	s->paused = NULL;
	c->paused = false;
	send_response(c);
}

size_t
canned_request_count(const struct canned_server *s)
{
	return s->request_count;
}

const char *
canned_last_request(const struct canned_server *s)
{
	return s->last_request;
}

static void
on_listener_close(uv_handle_t *handle)
{
	struct canned_server *srv = handle->data;
	size_t i;
	for (i = 0; i < srv->reply_count; i++)
		free(srv->replies[i]);
	free(srv->replies);
	free(srv->statuses);
	free(srv->last_request);
	free(srv);
}

void
canned_stop(struct canned_server *s)
{
	if (s->paused != NULL) {
		struct conn *c = s->paused;
		s->paused = NULL;
		c->paused = false;
		uv_read_stop((uv_stream_t *)&c->handle);
		uv_close((uv_handle_t *)&c->handle, on_conn_close);
	}
	uv_close((uv_handle_t *)&s->listener, on_listener_close);
}
