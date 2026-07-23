// SPDX-License-Identifier: ISC
#ifndef CLMLUA_INTERNAL_H
#define CLMLUA_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>

#include <lua.h>

struct clm_lua_pending;
struct clm_lua_plugin;
struct clm_lua_call;
struct clm_lua_coroutine;
struct clm_lua_task;
struct clm_agent;
struct clm_tool_invocation;

typedef void (*clm_lua_pending_teardown_fn)(struct clm_lua_pending *pending);

TAILQ_HEAD(clm_lua_pending_list, clm_lua_pending);
TAILQ_HEAD(clm_lua_coroutine_list, clm_lua_coroutine);

struct clm_lua_pending {
	TAILQ_ENTRY(clm_lua_pending) plugin_entry;
	TAILQ_ENTRY(clm_lua_pending) coroutine_entry;
	struct clm_lua_plugin *plugin;
	struct clm_lua_coroutine *coroutine;
	clm_lua_pending_teardown_fn teardown;
};

/* one family is shared by every coroutine in a tool invocation. */
struct clm_lua_call {
	struct clm_lua_plugin *plugin;
	struct clm_tool_invocation *inv;
	char *tool_name;
	char *child_error;
	uint64_t serial;
	size_t active_coroutines;
	size_t unobserved_errors;
	bool completed;
	bool reported;
};

/* each registered coroutine owns its pending async operations. */
struct clm_lua_coroutine {
	TAILQ_ENTRY(clm_lua_coroutine) entry;
	struct clm_lua_plugin *plugin;
	struct clm_lua_call *call;
	struct clm_lua_task *task;
	lua_State *co;
	lua_State *main_L;
	struct clm_agent *agent;
	int ref;
	bool is_main;
	bool running;
	struct clm_lua_pending_list pendings;
};

int clm_lua_pending_add(struct clm_lua_plugin *plugin, lua_State *co,
    struct clm_lua_pending *pending, clm_lua_pending_teardown_fn teardown);
struct clm_lua_plugin *clm_lua_pending_remove(
    struct clm_lua_pending *pending);

struct clm_lua_coroutine *clm_lua_coroutine_register(
    struct clm_lua_plugin *plugin, struct clm_lua_call *call,
    lua_State *owner, lua_State *co, bool is_main);
void clm_lua_coroutine_unregister(struct clm_lua_coroutine *lco);
struct clm_lua_coroutine *clm_lua_coroutine_find(lua_State *co);
bool clm_lua_coroutine_is_valid(lua_State *co);
void clm_lua_coroutine_finish(struct clm_lua_coroutine *lco, int status,
    int nresults, const char *error);

#endif /* CLMLUA_INTERNAL_H */
