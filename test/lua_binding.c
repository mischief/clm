// SPDX-License-Identifier: ISC
/*
 * Runs a Lua test script against the Lua binding (lua/clm_lua.c), with the
 * mock chat server from the TUI tests on its own thread. The script gets
 * the server's base URL in CLM_TEST_URL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "mock_server.h"

int luaopen_clm(lua_State *L);

int
main(int argc, char **argv)
{
	struct mock_server *srv;
	lua_State *L;
	char url[128], *p;
	int rc;

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

	L = luaL_newstate();
	luaL_openlibs(L);
	luaL_requiref(L, "clm", luaopen_clm, 0);
	lua_pop(L, 1);
	rc = luaL_dofile(L, argv[1]);
	if (rc != LUA_OK)
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
	lua_close(L);
	mock_stop(srv);
	return rc == LUA_OK ? 0 : 1;
}
