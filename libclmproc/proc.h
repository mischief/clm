// SPDX-License-Identifier: ISC
/*
 * clm_proc: run a shell command on a libuv loop, in a process group of its
 * own, and stop it reliably. Used by shell_exec, bg_exec, the monitor tools
 * and clm.exec. Internal: not installed.
 */
#ifndef CLM_PROC_H
#define CLM_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uv.h>

struct clm_proc;

/* What a collected output buffer keeps once it reaches max bytes. */
enum clm_proc_keep {
	CLM_PROC_KEEP_HEAD, /* the first max bytes; the rest is counted */
	CLM_PROC_KEEP_ENDS, /* the first head bytes and the newest rest */
};

struct clm_proc_result {
	int64_t exit_status; /* 0 when a signal ended it */
	int term_signal;     /* the signal that ended it, or 0 */
	bool timed_out;      /* opts.timeout_ms ran out */
	bool cancelled;      /* clm_proc_cancel, _stop_output or _kill ran */
	const char *output;  /* collected stdout and stderr, NUL-terminated */
	size_t len;
	size_t dropped; /* bytes of output that were not kept */
};

/* Output as it arrives, on stdout (fd 1) or stderr (fd 2); data is NULL and
 * n is 0 at the end of that stream. */
typedef void (*clm_proc_output_fn)(
    struct clm_proc *p, int fd, const char *data, size_t n, void *user);

/* Every handle is closed. p is freed when this returns. */
typedef void (*clm_proc_done_fn)(
    struct clm_proc *p, const struct clm_proc_result *r, void *user);

struct clm_proc_opts {
	const char *command;    /* run as SHELL -c command */
	const char *shell;      /* NULL: $SHELL, else /bin/sh */
	const char *cwd;        /* NULL: this process's */
	const char *stdin_data; /* NULL: /dev/null */
	enum clm_proc_keep keep;
	size_t max;          /* bytes kept; 0 means 1 MiB */
	size_t head;         /* CLM_PROC_KEEP_ENDS: bytes of the start kept */
	uint64_t timeout_ms; /* 0: none; at the end, as clm_proc_cancel */
	/* After the shell exits, wait this long for its pipes to close, then
	 * kill what still holds them; 0 waits for them however long. */
	uint64_t exit_grace_ms;
	clm_proc_output_fn output; /* set: output goes here, not to a buffer */
	clm_proc_done_fn done;
	void *user;
};

/* Start a command. Returns 0 and sets *out, or a negative libuv error;
 * then nothing was started and done is never called. */
int clm_proc_spawn(
    uv_loop_t *loop, const struct clm_proc_opts *o, struct clm_proc **out);

/* SIGTERM to the group, then after the grace period SIGKILL, and the pipes
 * are closed, so done always runs. */
void clm_proc_cancel(struct clm_proc *p);

/* Stop reading output now, then cancel. */
void clm_proc_stop_output(struct clm_proc *p);

/* SIGKILL to the group and close the pipes now, for a loop going away. */
void clm_proc_kill(struct clm_proc *p);

/* The owner has gone: done and output are not called again. */
void clm_proc_detach(struct clm_proc *p);

/* The grace between SIGTERM and SIGKILL: CLM_SHELL_KILL_GRACE_MS if set,
 * else 5000. */
uint64_t clm_proc_grace_ms(void);

#endif
