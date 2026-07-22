// SPDX-License-Identifier: ISC
#ifndef CLM_TEST_CANNED_H
#define CLM_TEST_CANNED_H

#include <stddef.h>

#include <uv.h>

/*
 * A minimal canned HTTP server for tests. It runs on the caller's uv_loop
 * (the same loop the clm agent uses), so the whole exchange is single-threaded
 * and deterministic. Each incoming request is answered with the next queued
 * reply body, FIFO. The server reads the full request (honouring
 * Content-Length) and responds with "Connection: close", so the client opens a
 * fresh connection per turn -- matching how the agent issues one request per
 * turn.
 */
struct canned_server;

/* Start listening on 127.0.0.1:<ephemeral> using loop. NULL on failure. */
struct canned_server *canned_start(uv_loop_t *loop);

/* The port the server bound to. */
int canned_port(const struct canned_server *s);

/* Queue a JSON response body (copied); served FIFO, one per request. */
void canned_reply(struct canned_server *s, const char *json_body);

/* Same, but with a caller-chosen HTTP status line instead of the default
 * "200 OK" -- for exercising error-response handling (e.g. a plain-text 400
 * body, as some OpenAI-compatible backends send). */
void canned_reply_status(struct canned_server *s, int status, const char *body);

/* hold the next complete request open without responding. canned_resume()
 * sends its queued reply later. this is useful for deterministic timeout
 * tests where the response must become available after cancellation. */
void canned_pause_next(struct canned_server *s);
void canned_resume(struct canned_server *s);

/* Number of requests received so far. */
size_t canned_request_count(const struct canned_server *s);

/* The body of the most recent request (NUL-terminated), or NULL. */
const char *canned_last_request(const struct canned_server *s);

/* Close the server. Run the loop afterwards so the close completes. */
void canned_stop(struct canned_server *s);

#endif /* CLM_TEST_CANNED_H */
