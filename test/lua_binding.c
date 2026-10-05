// SPDX-License-Identifier: ISC
/*
 * Runs a Lua test script against the Lua binding (lua/clm_lua.c), with the
 * mock chat server from the TUI tests on its own thread. The script gets
 * the server's base URL in CLM_TEST_URL. It runs in two threads at once,
 * each with its own Lua state, so state leaking between them shows.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "mock_server.h"

#define NRUNS 2

int luaopen_clm(lua_State *L);

struct run {
	const char *script;
	int rc;
};

static void *
run_script(void *arg)
{
	struct run *r = arg;
	lua_State *L = luaL_newstate();

	luaL_openlibs(L);
	luaL_requiref(L, "clm", luaopen_clm, 0);
	lua_pop(L, 1);
	r->rc = luaL_dofile(L, r->script);
	if (r->rc != LUA_OK)
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
	lua_close(L);
	return NULL;
}

int
main(int argc, char **argv)
{
	struct mock_server *srv;
	struct run runs[NRUNS];
	pthread_t th[NRUNS];
	char url[128], *p;
	int i, failed = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s script.lua\n", argv[0]);
		return 2;
	}
	srv = mock_start();
	if (srv == NULL) {
		fprintf(stderr, "mock server failed to start\n");
		return 1;
	}
	(void)snprintf(url, sizeof(url), "%s", mock_url(srv));
	if ((p = strstr(url, "/chat/completions")) != NULL)
		*p = '\0';
	setenv("CLM_TEST_URL", url, 1);

	for (i = 0; i < NRUNS; i++) {
		runs[i].script = argv[1];
		runs[i].rc = -1;
		if (pthread_create(&th[i], NULL, run_script, &runs[i]) != 0)
			runs[i].rc = -1;
	}
	for (i = 0; i < NRUNS; i++) {
		pthread_join(th[i], NULL);
		failed += runs[i].rc != LUA_OK;
	}
	mock_stop(srv);
	return failed == 0 ? 0 : 1;
}
