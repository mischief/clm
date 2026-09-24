// SPDX-License-Identifier: ISC
#ifndef CLM_HOST_H
#define CLM_HOST_H

/*
 * clm_host: the platform services the agent core needs, behind a tiny vtable.
 *
 * The core is loop- and transport-agnostic. It never includes <uv.h> or
 * <curl/curl.h>; instead the embedder supplies a struct clm_host with an HTTP
 * transport and (optionally) a one-shot timer, then hands it to clm_agent_new.
 *
 *   - Desktop:  a curl+libuv adapter (see the libclm-uv adapter).
 *   - Embedded: a blocking esp_http_client adapter, timer_set left NULL.
 *
 * This is deliberately NOT a general event-loop abstraction: the agent makes
 * one request at a time and never multiplexes file descriptors, so the surface
 * is just "make a request", "run a timer" and, optionally, "run a process".
 */

#include <stddef.h>
#include <stdint.h>

#include "clm/http.h"

/* Opaque handles owned by the host implementation. */
struct clm_http_call; /* one in-flight HTTP request */
struct clm_timer;     /* one scheduled one-shot timer */
struct clm_proc;      /* one child process */

/*
 * A portable HTTP request description — no transport types leak in here.
 * headers, when non-NULL, is a NULL-terminated array of "Name: Value" strings
 * the request should send in addition to the defaults.
 */
struct clm_http_req {
	const char *url;
	const char *api_key;        /* Bearer auth; NULL or "" for none */
	const char *body;           /* request body; NULL => GET, else POST */
	const char *const *headers; /* NULL-terminated extra headers, or NULL */
	const char
	    *client_suffix; /* User-Agent "(tool: <suffix>)" tag, or NULL */
};

/* One-shot timer callback. */
typedef void (*clm_timer_cb)(void *arg);

/*
 * A child process to start. argv[0] is looked up in PATH. The child inherits
 * the environment and working directory, and runs in its own session and
 * process group. stdin_data, when non-NULL, is written to its standard input,
 * which is then closed; when NULL the child reads end of file.
 */
struct clm_proc_req {
	const char *const *argv; /* NULL-terminated */
	const char *stdin_data;
	size_t stdin_len;
};

/* Output from the child: fd is 1 (stdout) or 2 (stderr). */
typedef void (*clm_proc_data_cb)(
    int fd, const char *data, size_t len, void *user);

/*
 * Called once, after the child exits and both output pipes reach end of
 * file. status is the exit status, or -1 when signal (nonzero) killed it.
 * The handle is freed when this returns.
 */
typedef void (*clm_proc_exit_cb)(int64_t status, int signal, void *user);

struct clm_host {
	/*
	 * start an http request. a negative return means startup failed: no
	 * callback is invoked and *out remains null. zero means accepted and
	 * exactly one of success/error is invoked, either before returning or
	 * later. data may be null. on a 2xx streamed response, data receives
	 * body chunks and success gets a null body. *out is cleared before
	 * startup and receives a cancellable handle only when the request
	 * remains in flight after this call returns.
	 */
	int (*http_post)(void *ctx, const struct clm_http_req *req,
	    clm_http_success_cb success, clm_http_error_cb error,
	    clm_http_data_cb data, void *user, struct clm_http_call **out);

	/* abort an in-flight request; delivers the error callback with
	 * -ECANCELED. safe once on a non-null handle returned by http_post,
	 * before completion. */
	void (*http_cancel)(struct clm_http_call *call);

	/*
	 * Schedule a one-shot timer to fire cb(arg) after ms milliseconds.
	 * Optional: a host may leave timer_set NULL, in which case the core
	 * disables per-tool timeouts (a blocking transport enforces its own
	 * network timeout anyway). *out (if non-NULL) receives a handle usable
	 * with timer_cancel.
	 */
	int (*timer_set)(void *ctx, uint64_t ms, clm_timer_cb cb, void *arg,
	    struct clm_timer **out);

	/* Cancel and free a pending timer (the callback will not fire
	 * afterward). */
	void (*timer_cancel)(struct clm_timer *timer);

	void *ctx; /* opaque, passed to http_post/timer_set */

	/*
	 * The host's native event loop (e.g. the desktop adapter's
	 * uv_loop_t*), for tools that need to reach the underlying platform
	 * directly -- see clm_tool_invocation_loop(). NULL when the host has
	 * no such thing (e.g. a blocking embedded transport). Kept separate
	 * from ctx on purpose: ctx is whatever the adapter's own callbacks
	 * need (and may wrap more than the loop), while this is specifically
	 * the loop, for external consumers that must not depend on the
	 * adapter's private ctx layout.
	 */
	void *native_loop;

	/*
	 * Optional: start a child process. NULL when the host cannot. A
	 * negative return means it did not start and no callback runs.
	 * Otherwise exit runs exactly once, later, never before this
	 * returns. data may be NULL to discard output.
	 */
	int (*proc_spawn)(void *ctx, const struct clm_proc_req *req,
	    clm_proc_data_cb data, clm_proc_exit_cb exit, void *user,
	    struct clm_proc **out);

	/* Send signal to the child's process group. Valid until exit runs. */
	void (*proc_kill)(struct clm_proc *proc, int signal);

	/*
	 * Give the handle back before exit runs: no more callbacks. The host
	 * stops the child (SIGTERM, then SIGKILL after a grace period) and
	 * frees the handle itself.
	 */
	void (*proc_detach)(struct clm_proc *proc);
};

#endif /* CLM_HOST_H */
