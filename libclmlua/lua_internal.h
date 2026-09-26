// SPDX-License-Identifier: ISC
#ifndef CLMLUA_INTERNAL_H
#define CLMLUA_INTERNAL_H

#include <stdint.h>
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

/* Hooks a plugin added with clm.on (lua_hook.c). */
struct lua_hook;
TAILQ_HEAD(clm_lua_hook_list, lua_hook);
struct clm_lua_hook_list *clm_lua_plugin_hooks(struct clm_lua_plugin *plugin);

/* Add clm.on to the clm table at the top of the stack. */
void clm_lua_hook_open(struct lua_State *L, struct clm_lua_plugin *plugin);

/* Take the plugin's hooks off its agent. Call before the state closes. */
void clm_lua_hook_drop_all(struct clm_lua_plugin *plugin);

int clm_lua_resume_with_deadline(struct clm_lua_plugin *plugin,
    struct lua_State *co, struct lua_State *from, int nargs, int *nresults,
    uint64_t timeout_ms);
void clm_lua_mark_invocation_thread(
    struct lua_State *L, struct lua_State *co, int on);
void clm_lua_clear_invocation_registry(struct lua_State *L);

#endif /* CLMLUA_INTERNAL_H */
