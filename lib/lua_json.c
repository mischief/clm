// SPDX-License-Identifier: ISC
/*
 * Lua JSON bindings over json-c. Exposes a global "json" table with:
 *   json.encode(value) -> string
 *   json.decode(string) -> value
 *   json.null          -> lightuserdata sentinel
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <json-c/json.h>

#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
#include <lua5.4/lualib.h>

#include "banned.h"

/* Prototype — called from lua_plugin.c. */
int clm_lua_json_open(lua_State *L);

/* Push a json-c object as a Lua value (table/string/number/etc).
 * Called from lua_plugin.c to decode tool args without a Lua hop. */
void clm_lua_push_json_value(lua_State *L, struct json_object *obj);

/* Sentinel for JSON null. */
static char json_null_sentinel;

/* ------------------------------------------------------------------ */
/* decode: JSON string -> Lua value                                    */
/* ------------------------------------------------------------------ */

void
clm_lua_push_json_value(lua_State *L, struct json_object *obj)
{
	if (obj == NULL) {
		lua_pushlightuserdata(L, &json_null_sentinel);
		return;
	}

	switch (json_object_get_type(obj)) {
	case json_type_null:
		lua_pushlightuserdata(L, &json_null_sentinel);
		break;
	case json_type_boolean:
		lua_pushboolean(L, json_object_get_boolean(obj));
		break;
	case json_type_int:
		lua_pushinteger(L, (lua_Integer)json_object_get_int64(obj));
		break;
	case json_type_double:
		lua_pushnumber(L, json_object_get_double(obj));
		break;
	case json_type_string:
		lua_pushlstring(L, json_object_get_string(obj),
		    (size_t)json_object_get_string_len(obj));
		break;
	case json_type_array: {
		size_t len = json_object_array_length(obj);
		lua_createtable(L, (int)len, 0);
		for (size_t i = 0; i < len; i++) {
			clm_lua_push_json_value(L, json_object_array_get_idx(obj, i));
			lua_rawseti(L, -2, (lua_Integer)(i + 1));
		}
		break;
	}
	case json_type_object: {
		lua_newtable(L);
		struct json_object_iterator it = json_object_iter_begin(obj);
		struct json_object_iterator end = json_object_iter_end(obj);
		while (!json_object_iter_equal(&it, &end)) {
			const char *key = json_object_iter_peek_name(&it);
			struct json_object *val = json_object_iter_peek_value(&it);
			lua_pushstring(L, key);
			clm_lua_push_json_value(L, val);
			lua_rawset(L, -3);
			json_object_iter_next(&it);
		}
		break;
	}
	}
}

static int
lua_json_decode(lua_State *L)
{
	size_t len;
	const char *str = luaL_checklstring(L, 1, &len);
	struct json_tokener *tok;
	struct json_object *obj;

	tok = json_tokener_new();
	if (tok == NULL)
		return luaL_error(L, "json.decode: out of memory");

	obj = json_tokener_parse_ex(tok, str, (int)len);
	if (obj == NULL) {
		enum json_tokener_error err = json_tokener_get_error(tok);
		const char *msg = json_tokener_error_desc(err);
		json_tokener_free(tok);
		return luaL_error(L, "json.decode: %s", msg);
	}
	json_tokener_free(tok);

	clm_lua_push_json_value(L, obj);
	json_object_put(obj);
	return 1;
}

/* ------------------------------------------------------------------ */
/* encode: Lua value -> JSON string                                    */
/* ------------------------------------------------------------------ */

static struct json_object *lua_to_json(lua_State *L, int idx, int depth);

/*
 * Determine if a Lua table at idx is an array (sequential integer keys
 * starting from 1 with no gaps).
 */
static bool
is_lua_array(lua_State *L, int idx)
{
	size_t len = lua_rawlen(L, idx);
	if (len == 0) {
		/* Could be an empty object or empty array; check for any keys. */
		lua_pushnil(L);
		if (lua_next(L, idx) != 0) {
			lua_pop(L, 2);
			return false; /* has string keys */
		}
		return true; /* truly empty: treat as array */
	}
	/* Verify no keys beyond 1..len. */
	lua_Integer count = 0;
	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		lua_pop(L, 1); /* pop value, keep key for iteration */
		count++;
		if (count > (lua_Integer)len)
			return false; /* extra keys beyond rawlen */
	}
	return count == (lua_Integer)len;
}

static struct json_object *
lua_to_json(lua_State *L, int idx, int depth)
{
	if (depth > 64)
		return NULL; /* prevent stack overflow on deeply nested tables */

	idx = lua_absindex(L, idx);

	switch (lua_type(L, idx)) {
	case LUA_TNIL:
		return NULL; /* json-c treats NULL as null in arrays/objects */
	case LUA_TBOOLEAN:
		return json_object_new_boolean(lua_toboolean(L, idx));
	case LUA_TNUMBER:
		if (lua_isinteger(L, idx))
			return json_object_new_int64(lua_tointeger(L, idx));
		return json_object_new_double(lua_tonumber(L, idx));
	case LUA_TSTRING: {
		size_t slen;
		const char *s = lua_tolstring(L, idx, &slen);
		return json_object_new_string_len(s, (int)slen);
	}
	case LUA_TLIGHTUSERDATA:
		/* json.null sentinel */
		if (lua_touserdata(L, idx) == &json_null_sentinel)
			return NULL;
		return json_object_new_string("(userdata)");
	case LUA_TTABLE: {
		if (is_lua_array(L, idx)) {
			size_t len = lua_rawlen(L, idx);
			struct json_object *arr = json_object_new_array_ext((int)len);
			for (size_t i = 1; i <= len; i++) {
				lua_rawgeti(L, idx, (lua_Integer)i);
				json_object_array_add(arr, lua_to_json(L, -1, depth + 1));
				lua_pop(L, 1);
			}
			return arr;
		}
		struct json_object *obj = json_object_new_object();
		lua_pushnil(L);
		while (lua_next(L, idx) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING) {
				const char *key = lua_tostring(L, -2);
				json_object_object_add(obj, key,
				    lua_to_json(L, -1, depth + 1));
			}
			lua_pop(L, 1);
		}
		return obj;
	}
	default:
		return json_object_new_string("(unsupported type)");
	}
}

static int
lua_json_encode(lua_State *L)
{
	struct json_object *obj;
	const char *s;

	luaL_checkany(L, 1);
	obj = lua_to_json(L, 1, 0);

	if (obj == NULL) {
		lua_pushstring(L, "null");
		return 1;
	}
	s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
	lua_pushstring(L, s);
	json_object_put(obj);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Module registration                                                 */
/* ------------------------------------------------------------------ */

static const luaL_Reg json_funcs[] = {
	{"decode", lua_json_decode},
	{"encode", lua_json_encode},
	{NULL, NULL},
};

int
clm_lua_json_open(lua_State *L)
{
	lua_newtable(L);
	luaL_setfuncs(L, json_funcs, 0);

	/* json.null sentinel */
	lua_pushlightuserdata(L, &json_null_sentinel);
	lua_setfield(L, -2, "null");

	lua_setglobal(L, "json");
	return 0;
}
