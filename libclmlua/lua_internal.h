// SPDX-License-Identifier: ISC
#ifndef CLMLUA_INTERNAL_H
#define CLMLUA_INTERNAL_H

#include <sys/queue.h>

struct clm_lua_pending;
struct clm_lua_plugin;

typedef void (*clm_lua_pending_teardown_fn)(struct clm_lua_pending *pending);

struct clm_lua_pending {
	TAILQ_ENTRY(clm_lua_pending) entry;
	struct clm_lua_plugin *plugin;
	clm_lua_pending_teardown_fn teardown;
};

int clm_lua_pending_add(struct clm_lua_plugin *plugin,
    struct clm_lua_pending *pending, clm_lua_pending_teardown_fn teardown);
struct clm_lua_plugin *clm_lua_pending_remove(struct clm_lua_pending *pending);

struct clm_agent;
struct lua_State;

struct clm_agent *clm_lua_plugin_agent(const struct clm_lua_plugin *plugin);
struct lua_State *clm_lua_plugin_state(const struct clm_lua_plugin *plugin);
/* False after an unrecoverable error: its state must not be touched. */
int clm_lua_plugin_alive(const struct clm_lua_plugin *plugin);

/*
 * Run an event callback on the plugin's main state, under a deadline. push
 * runs inside the protected call: it pushes a function and its arguments
 * and returns the argument count. Errors are logged. Returns 0, or a
 * negative errno when the plugin is gone or the callback failed.
 */
int clm_lua_plugin_callback(struct clm_lua_plugin *plugin,
    int (*push)(struct lua_State *L, void *arg), void *arg);

/* Add clm.spawn, clm.exec, clm.after, clm.notify and clm.getenv to the
 * clm table at the top of the stack. */
void clm_lua_proc_open(struct lua_State *L, struct clm_lua_plugin *plugin);

#endif /* CLMLUA_INTERNAL_H */
