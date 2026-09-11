// SPDX-License-Identifier: ISC
#ifndef CLMLUA_POLICY_H
#define CLMLUA_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include <lua.h>

struct clm_lua_plugin;

/*
 * The capabilities a plugin can reach the outside world through. Every one
 * of them is a way out of the interpreter, so they all pass the same
 * checkpoint (clm_lua_policy_check_*) rather than each growing its own ad
 * hoc test. Adding a binding that touches the host means adding it here.
 */
enum clm_lua_cap {
	CLM_LUA_CAP_READ_FILE,
	CLM_LUA_CAP_WRITE_FILE,
	CLM_LUA_CAP_HTTP,
	CLM_LUA_CAP__COUNT,
};

/*
 * ALLOW and DENY are final. ASK means "outside the static policy, but a
 * frontend could still authorize it" -- until the permission request is
 * generalized to carry its own continuation (see clm_permission_req in
 * clm.h, which today can only resume a parked tool invocation), there is
 * nothing to prompt with mid-invocation, so callers treat ASK as DENY and
 * say so in the error. That is the one seam this prototype leaves open.
 */
enum clm_lua_verdict {
	CLM_LUA_ALLOW,
	CLM_LUA_ASK,
	CLM_LUA_DENY,
};

struct clm_lua_policy {
	char **roots[CLM_LUA_CAP__COUNT];
	size_t nroots[CLM_LUA_CAP__COUNT];

	/*
	 * Set while a plugin's top-level chunk runs. Load-time code has no
	 * invocation behind it -- nobody asked for it and there is no turn to
	 * attribute it to -- so every capability is refused outright there,
	 * whatever the roots say. Plugins register tools at load; they do not
	 * need the network or the filesystem to do it.
	 */
	bool loading;
};

void clm_lua_policy_init(struct clm_lua_policy *p);
void clm_lua_policy_free(struct clm_lua_policy *p);

/* Add an allowed root: a directory prefix for the file caps, or a host for
 * CLM_LUA_CAP_HTTP ("example.com", or ".example.com" to match subdomains).
 * Returns 0, or negative errno. */
int clm_lua_policy_add(struct clm_lua_policy *p, enum clm_lua_cap cap,
    const char *value);

/* Install the built-in default roots: the plugin's own directory (read) and
 * the agent scratch dir (read/write), with no HTTP host allowed. */
int clm_lua_policy_defaults(struct clm_lua_policy *p, const char *plugin_dir,
    const char *scratch_dir);

/*
 * Resolve path and decide whether cap may touch it. On ALLOW, *resolved is
 * set to a malloc'd absolute path the caller should open INSTEAD of the one
 * it was handed -- reopening the original string would re-walk the symlinks
 * the check just resolved. *why gets a static reason string otherwise.
 */
enum clm_lua_verdict clm_lua_policy_check_path(const struct clm_lua_policy *p,
    enum clm_lua_cap cap, const char *path, char **resolved, const char **why);

/* Decide whether the plugin may reach url. */
enum clm_lua_verdict clm_lua_policy_check_url(const struct clm_lua_policy *p,
    const char *url, const char **why);

/*
 * A capability call parked on a permission prompt. Opaque: the file
 * bindings and the http bindings park the same way but finish differently,
 * so the payload each one needs is its own business.
 */
struct clm_lua_cap_park;

/*
 * Ask the frontend to authorize something policy does not cover.
 *
 * Returns 1 if the answer was already known and the caller may proceed
 * now; 0 if the call is parked, in which case *park is set and the caller
 * must `return lua_yieldk(L, 0, (lua_KContext)*park, k)` so k runs when
 * the answer arrives; or negative errno with *why set (-EACCES when the
 * answer was a refusal). On 0 the park owns ud and frees it with ud_free;
 * on anything else ud stays the caller's to free.
 */
int clm_lua_cap_ask(lua_State *L, struct clm_lua_plugin *plugin,
    const char *label, const char *detail, void *ud, void (*ud_free)(void *),
    struct clm_lua_cap_park **park, const char **why);

/* In a continuation: what the answer was, the park's payload, and its
 * label. The park is freed with clm_lua_cap_park_free. */
bool clm_lua_cap_park_allowed(const struct clm_lua_cap_park *park);
void *clm_lua_cap_park_ud(const struct clm_lua_cap_park *park);
const char *clm_lua_cap_park_label(const struct clm_lua_cap_park *park);
void clm_lua_cap_park_free(struct clm_lua_cap_park *park);

/* The plugin whose coroutine is running, from the registry, or NULL. */
struct clm_lua_plugin *clm_lua_plugin_current(lua_State *L);
/* That plugin's policy. */
struct clm_lua_policy *clm_lua_plugin_policy(struct clm_lua_plugin *plugin);

#endif /* CLMLUA_POLICY_H */
