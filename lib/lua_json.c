// SPDX-License-Identifier: ISC
/*
 * Lua JSON bindings over cJSON. Exposes a global "json" table with:
 *   json.encode(value) -> string
 *   json.decode(string) -> value
 *   json.null          -> lightuserdata sentinel
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "banned.h"

/* Prototype — called from lua_plugin.c. */
int clm_lua_json_open(lua_State *L);

/* Push a cJSON object as a Lua value (table/string/number/etc).
 * Called from lua_plugin.c to decode tool args without a Lua hop. */
void clm_lua_push_json_value(lua_State *L, cJSON *obj);

/* Sentinel for JSON null. */
static char json_null_sentinel;

/* ------------------------------------------------------------------ */
/* decode: JSON string -> Lua value                                    */
/* ------------------------------------------------------------------ */

void
clm_lua_push_json_value(lua_State *L, cJSON *obj)
{
	if (obj == NULL) {
		lua_pushlightuserdata(L, &json_null_sentinel);
		return;
	}

	if (cJSON_IsNull(obj)) {
		lua_pushlightuserdata(L, &json_null_sentinel);
		return;
	}
	if (cJSON_IsBool(obj)) {
		lua_pushboolean(L, cJSON_IsTrue(obj));
		return;
	}
	if (cJSON_IsNumber(obj)) {
		double val = obj->valuedouble;
		lua_Integer ival = (lua_Integer)val;
		/* Exact round-trip check is intentional: only take the integer
		 * fast path when the double truly has no fractional part. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
		if ((double)ival == val) {
#pragma GCC diagnostic pop
			lua_pushinteger(L, ival);
		} else {
			lua_pushnumber(L, (lua_Number)val);
		}
		return;
	}
	if (cJSON_IsString(obj)) {
		lua_pushlstring(L, obj->valuestring, strlen(obj->valuestring));
		return;
	}
	if (cJSON_IsArray(obj)) {
		int size = cJSON_GetArraySize(obj);
		lua_createtable(L, size, 0);
		for (int i = 0; i < size; i++) {
			cJSON *item = cJSON_GetArrayItem(obj, i);
			clm_lua_push_json_value(L, item);
			lua_rawseti(L, -2, i + 1);
		}
		return;
	}
	if (cJSON_IsObject(obj)) {
		lua_newtable(L);
		for (cJSON *child = obj->child; child != NULL; child = child->next) {
			lua_pushstring(L, child->string);
			clm_lua_push_json_value(L, child);
			lua_rawset(L, -3);
		}
		return;
	}
	/* Should not reach here */
	lua_pushliteral(L, "[invalid JSON type]");
}

/* ------------------------------------------------------------------ */
/* decode: JSON string -> Lua value                                    */
/* ------------------------------------------------------------------ */

static int
lua_json_decode(lua_State *L)
{
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	cJSON *obj;

	/* Enforce per-plugin decode size limit. */
	lua_getfield(L, LUA_REGISTRYINDEX, "_clm_json_max");
	size_t max_len = (size_t)lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (max_len > 0 && len > max_len)
		return luaL_error(L, "json.decode: input too large (%zu > %zu)",
		    len, max_len);

	obj = cJSON_ParseWithLengthOpts(str, len, NULL, 0);
	if (obj == NULL) {
		const char *error_ptr = cJSON_GetErrorPtr();
		if (error_ptr != NULL) {
			return luaL_error(L, "json.decode: invalid JSON at offset %td",
			    error_ptr - str);
		} else {
			return luaL_error(L, "json.decode: out of memory");
		}
	}

	clm_lua_push_json_value(L, obj);
	cJSON_Delete(obj);
	return 1;
}

/* ------------------------------------------------------------------ */
/* encode: Lua value -> JSON string                                    */
/* ------------------------------------------------------------------ */

static cJSON *lua_to_json(lua_State *L, int idx, int depth);

/* Registry key for the metatable set by json.array() to force array encoding. */
#define CLM_JSON_ARRAY_MT "clm_json_array"

/* True if the table at idx carries the json.array() marker metatable. */
static bool
has_array_marker(lua_State *L, int idx)
{
	if (!lua_getmetatable(L, idx))
		return false;
	luaL_getmetatable(L, CLM_JSON_ARRAY_MT);
	bool marked = lua_rawequal(L, -1, -2);
	lua_pop(L, 2);
	return marked;
}

/*
 * Decide whether a Lua table encodes as a JSON array or object.
 *
 * Lua cannot distinguish an empty array from an empty object -- both are {}.
 * We default an empty (or otherwise ambiguous) table to a JSON OBJECT, because
 * empty objects dominate in practice (notably JSON Schema, e.g. an empty
 * "properties": {}). A strict server (Nova) rejects "properties": [], so the
 * object default is the safe one. To force an empty (or any) table to encode
 * as an array, wrap it with json.array().
 */
static bool
is_lua_array(lua_State *L, int idx)
{
	if (has_array_marker(L, idx))
		return true;

	size_t len = lua_rawlen(L, idx);
	if (len == 0)
		return false; /* ambiguous/empty: default to object */

	/* Verify no keys beyond 1..len. */
	lua_Integer count = 0;
	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		lua_pop(L, 1); /* pop value, keep key for iteration */
		count++;
		if (count > (lua_Integer)len) {
			lua_pop(L, 1); /* pop the key before returning */
			return false; /* extra keys beyond rawlen */
		}
	}
	return count == (lua_Integer)len;
}

static cJSON *
lua_to_json(lua_State *L, int idx, int depth)
{
	if (depth > 64)
		return NULL; /* prevent stack overflow on deeply nested tables */

	idx = lua_absindex(L, idx);

	switch (lua_type(L, idx)) {
	case LUA_TNIL:
		return NULL; /* cJSON treats NULL as null in arrays/objects */
	case LUA_TBOOLEAN:
		return cJSON_CreateBool(lua_toboolean(L, idx));
	case LUA_TNUMBER:
		if (lua_isinteger(L, idx))
			return cJSON_CreateNumber(lua_tointeger(L, idx));
		return cJSON_CreateNumber(lua_tonumber(L, idx));
	case LUA_TSTRING: {
		size_t slen;
		const char *s = lua_tolstring(L, idx, &slen);
		return cJSON_CreateString(s);
	}
	case LUA_TLIGHTUSERDATA:
		/* json.null sentinel */
		if (lua_touserdata(L, idx) == &json_null_sentinel)
			return NULL;
		return cJSON_CreateString("(userdata)");
	case LUA_TTABLE: {
		if (is_lua_array(L, idx)) {
			size_t len = lua_rawlen(L, idx);
			cJSON *arr = cJSON_CreateArray();
			for (size_t i = 1; i <= len; i++) {
				lua_rawgeti(L, idx, (lua_Integer)i);
				cJSON_AddItemToArray(arr, lua_to_json(L, -1, depth + 1));
				lua_pop(L, 1);
			}
			return arr;
		}
		cJSON *obj = cJSON_CreateObject();
		lua_pushnil(L);
		while (lua_next(L, idx) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING) {
				const char *key = lua_tostring(L, -2);
				cJSON_AddItemToObject(obj, key, lua_to_json(L, -1, depth + 1));
			}
			lua_pop(L, 1);
		}
		return obj;
	}
	default:
		return cJSON_CreateString("(unsupported type)");
	}
}

static int
lua_json_encode(lua_State *L)
{
	cJSON *obj;
	char *s;

	luaL_checkany(L, 1);
	obj = lua_to_json(L, 1, 0);

	if (obj == NULL) {
		lua_pushstring(L, "null");
		return 1;
	}
	s = cJSON_PrintUnformatted(obj);
	if (s == NULL) {
		cJSON_Delete(obj);
		return luaL_error(L, "json.encode: out of memory");
	}
	lua_pushstring(L, s);
	free(s);
	cJSON_Delete(obj);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Module registration                                                 */
/* ------------------------------------------------------------------ */

/*
 * json.array(t) -> t : mark a table so json.encode always encodes it as a JSON
 * array, even when empty. Needed only for the (rare) empty-array case, since a
 * non-empty sequence is detected automatically and empty tables default to an
 * object. Returns the same table for convenient inline use.
 */
static int
lua_json_array(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_settop(L, 1);
	luaL_getmetatable(L, CLM_JSON_ARRAY_MT);
	lua_setmetatable(L, 1);
	return 1;
}

static const luaL_Reg json_funcs[] = {
	{"decode", lua_json_decode},
	{"encode", lua_json_encode},
	{"array", lua_json_array},
	{NULL, NULL},
};

int
clm_lua_json_open(lua_State *L)
{
	lua_newtable(L);
	luaL_setfuncs(L, json_funcs, 0);

	/* Marker metatable for json.array(); created once per state. */
	luaL_newmetatable(L, CLM_JSON_ARRAY_MT);
	lua_pop(L, 1);

	/* json.null sentinel */
	lua_pushlightuserdata(L, &json_null_sentinel);
	lua_setfield(L, -2, "null");

	lua_setglobal(L, "json");
	return 0;
}
