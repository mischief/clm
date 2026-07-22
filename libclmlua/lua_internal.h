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

/*
 * Per-coroutine tracking for the generalized multi-coroutine async model.
 * Every coroutine created during a tool invocation (main + any spawned via
 * coroutine.create or clm.spawn) is registered here.  This lets http.get/post
 * (and future async primitives) be called from any of them.
 */
struct clm_lua_coroutine {
	TAILQ_ENTRY(clm_lua_coroutine) entry;
	struct clm_lua_plugin *plugin;
	lua_State *co;
	int ref;                 /* registry ref on plugin->L */
	bool is_main;
	TAILQ_HEAD(, clm_lua_pending) pendings;
};

int clm_lua_pending_add(struct clm_lua_plugin *plugin,
    struct clm_lua_pending *pending, clm_lua_pending_teardown_fn teardown);
struct clm_lua_plugin *clm_lua_pending_remove(
    struct clm_lua_pending *pending);

/* Multi-coroutine management */
struct clm_lua_coroutine *clm_lua_coroutine_register(struct clm_lua_plugin *plugin,
    lua_State *co, bool is_main);
void clm_lua_coroutine_unregister(struct clm_lua_coroutine *lco);
struct clm_lua_coroutine *clm_lua_coroutine_find(lua_State *L, lua_State *co);
bool clm_lua_coroutine_is_valid(lua_State *L, lua_State *co);

#endif /* CLMLUA_INTERNAL_H */
