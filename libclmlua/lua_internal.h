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
struct clm_lua_plugin *clm_lua_pending_remove(
    struct clm_lua_pending *pending);

#endif /* CLMLUA_INTERNAL_H */
