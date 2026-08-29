// SPDX-License-Identifier: ISC
#ifndef CLM_TEST_MOCK_SERVER_H
#define CLM_TEST_MOCK_SERVER_H

/*
 * A deterministic OpenAI-compatible chat-completions server for the TUI
 * tests. It ignores the prompt and answers with canned markdown, streamed as
 * SSE, unless the prompt names one of the scripted tool scenarios. It runs
 * its own libuv loop on its own thread, so a test driving a pty never has to
 * pump it.
 */
struct mock_server;

/* Start the server on 127.0.0.1:<ephemeral>. NULL on failure. */
struct mock_server *mock_start(void);

/* The chat-completions endpoint to hand the client. */
const char *mock_url(const struct mock_server *s);

/* Directory the "manytest" tool calls touch, one file per call. */
const char *mock_scratch(const struct mock_server *s);

/* Append each request body, as one JSON line, to path. NULL turns it off. */
void mock_request_log(struct mock_server *s, const char *path);

/* How many tool calls "manytest" asks for in one batch. */
int mock_many_calls(void);

/* Stop serving and free the server. */
void mock_stop(struct mock_server *s);

#endif /* CLM_TEST_MOCK_SERVER_H */
