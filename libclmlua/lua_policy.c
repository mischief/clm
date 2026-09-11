// SPDX-License-Identifier: ISC
/*
 * The capability checkpoint for Lua plugins.
 *
 * The interpreter itself is sandboxed by omission (see sandbox_state in
 * lua_plugin.c: no os, no io, no require). That only matters if the
 * bindings clm installs in their place are not themselves a way out, which
 * is what this file enforces: a path or a URL is compared against an
 * explicit allowlist, default-deny, before the binding acts on it.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lauxlib.h>

#include "lua_policy.h"

void
clm_lua_policy_init(struct clm_lua_policy *p)
{
	memset(p, 0, sizeof(*p));
}

void
clm_lua_policy_free(struct clm_lua_policy *p)
{
	for (int c = 0; c < CLM_LUA_CAP__COUNT; c++) {
		for (size_t i = 0; i < p->nroots[c]; i++)
			free(p->roots[c][i]);
		free(p->roots[c]);
		p->roots[c] = NULL;
		p->nroots[c] = 0;
	}
}

int
clm_lua_policy_add(struct clm_lua_policy *p, enum clm_lua_cap cap,
    const char *value)
{
	char **grown;
	char *dup;

	if (value == NULL || *value == '\0')
		return -EINVAL;

	/*
	 * Roots are compared as resolved absolute paths, so resolve them once
	 * here rather than on every check. A root that does not exist yet is
	 * not an error -- it just never matches until it does.
	 */
	if (cap == CLM_LUA_CAP_READ_FILE || cap == CLM_LUA_CAP_WRITE_FILE) {
		/* Heap, not a stack char[PATH_MAX]: the frame-size limit is
		 * there because PATH_MAX is 4096 here. */
		char *buf = malloc(PATH_MAX);
		if (buf == NULL)
			return -ENOMEM;
		if (realpath(value, buf) == NULL) {
			int saved = errno;
			free(buf);
			return -saved;
		}
		dup = strdup(buf);
		free(buf);
	} else {
		dup = strdup(value);
	}
	if (dup == NULL)
		return -ENOMEM;

	grown = realloc(p->roots[cap], (p->nroots[cap] + 1) * sizeof(*grown));
	if (grown == NULL) {
		free(dup);
		return -ENOMEM;
	}
	p->roots[cap] = grown;
	p->roots[cap][p->nroots[cap]++] = dup;
	return 0;
}

int
clm_lua_policy_defaults(struct clm_lua_policy *p, const char *plugin_dir,
    const char *scratch_dir)
{
	int r;

	/* A plugin may read the directory it was loaded from: its own data
	 * files are the one thing it can be sure belongs to it. */
	if (plugin_dir != NULL) {
		r = clm_lua_policy_add(p, CLM_LUA_CAP_READ_FILE, plugin_dir);
		if (r < 0 && r != -ENOENT)
			return r;
	}
	/* Scratch is the only place anything may be written. */
	if (scratch_dir != NULL) {
		r = clm_lua_policy_add(p, CLM_LUA_CAP_READ_FILE, scratch_dir);
		if (r < 0 && r != -ENOENT)
			return r;
		r = clm_lua_policy_add(p, CLM_LUA_CAP_WRITE_FILE, scratch_dir);
		if (r < 0 && r != -ENOENT)
			return r;
	}
	/* No HTTP host by default: egress is opt-in per config. */
	return 0;
}

/*
 * Resolve path to an absolute, symlink-free form.
 *
 * realpath(3) fails on a path that does not exist yet, which is the normal
 * case for a write. Fall back to resolving the parent directory (which must
 * exist) and appending the final component, so a create still gets checked
 * against a real, resolved prefix. Anything deeper than one missing
 * component is refused rather than guessed at.
 */
static char *
resolve(const char *path)
{
	char *buf, *copy = NULL, *slash, *out = NULL;
	const char *base;
	int n;

	buf = malloc(PATH_MAX);
	if (buf == NULL)
		return NULL;

	if (realpath(path, buf) != NULL)
		return buf;
	if (errno != ENOENT)
		goto fail;

	copy = strdup(path);
	if (copy == NULL)
		goto fail;
	slash = strrchr(copy, '/');
	if (slash == copy) {
		/* "/name": the parent is the root directory. */
		base = path + 1;
		buf[0] = '/';
		buf[1] = '\0';
	} else if (slash != NULL) {
		*slash = '\0';
		base = path + (slash - copy) + 1;
		if (realpath(copy, buf) == NULL)
			goto fail;
	} else {
		/* A bare relative name: resolve against the cwd. */
		base = path;
		if (realpath(".", buf) == NULL)
			goto fail;
	}

	/*
	 * The tail must be a plain name. A ".." here would climb back out of
	 * the prefix that was just resolved, which is the whole trick this
	 * check exists to stop.
	 */
	if (strcmp(base, "..") == 0 || strcmp(base, ".") == 0 ||
	    strchr(base, '/') != NULL) {
		errno = EINVAL;
		goto fail;
	}

	out = malloc(PATH_MAX);
	if (out == NULL)
		goto fail;
	n = snprintf(out, PATH_MAX, "%s%s%s", buf,
	    strcmp(buf, "/") == 0 ? "" : "/", base);
	if (n < 0 || (size_t)n >= PATH_MAX) {
		free(out);
		out = NULL;
		errno = ENAMETOOLONG;
		goto fail;
	}
	free(copy);
	free(buf);
	return out;

fail:
	free(copy);
	free(buf);
	return NULL;
}

/* True if path is root itself or lies underneath it. */
static bool
under(const char *path, const char *root)
{
	size_t len = strlen(root);

	if (strncmp(path, root, len) != 0)
		return false;
	if (path[len] == '\0')
		return true;
	/* Guard against /srv/data matching /srv/database. */
	if (strcmp(root, "/") == 0)
		return true;
	return path[len] == '/';
}

enum clm_lua_verdict
clm_lua_policy_check_path(const struct clm_lua_policy *p, enum clm_lua_cap cap,
    const char *path, char **resolved, const char **why)
{
	char *abs;

	*resolved = NULL;
	*why = NULL;

	if (p->loading) {
		*why = "not available while a plugin is loading";
		return CLM_LUA_DENY;
	}

	abs = resolve(path);
	if (abs == NULL) {
		*why = "path could not be resolved";
		return CLM_LUA_DENY;
	}

	for (size_t i = 0; i < p->nroots[cap]; i++) {
		if (under(abs, p->roots[cap][i])) {
			*resolved = abs;
			return CLM_LUA_ALLOW;
		}
	}

	free(abs);
	*why = "outside the paths this plugin may touch";
	return CLM_LUA_ASK;
}

/*
 * Pull the host out of a URL, lowercased, without a port. Deliberately
 * minimal: anything that is not a plain http/https URL with a host is
 * refused rather than parsed generously, because a parser that disagrees
 * with libcurl's about where the host ends is a way past this check.
 */
static bool
url_host(const char *url, char *out, size_t outlen)
{
	const char *h, *e;
	size_t n;

	if (strncasecmp(url, "http://", 7) == 0)
		h = url + 7;
	else if (strncasecmp(url, "https://", 8) == 0)
		h = url + 8;
	else
		return false;

	/* No userinfo: "https://evil.com@good.com/" reads as good.com to
	 * curl but as evil.com to a careless eye, so refuse the shape. */
	for (e = h; *e != '\0' && *e != '/'; e++) {
		if (*e == '@')
			return false;
	}

	for (e = h; *e != '\0' && *e != '/' && *e != ':'; e++)
		;
	n = (size_t)(e - h);
	if (n == 0 || n >= outlen)
		return false;
	for (size_t i = 0; i < n; i++)
		out[i] = (char)tolower((unsigned char)h[i]);
	out[n] = '\0';
	return true;
}

enum clm_lua_verdict
clm_lua_policy_check_url(const struct clm_lua_policy *p, const char *url,
    const char **why)
{
	char host[256];

	*why = NULL;

	if (p->loading) {
		*why = "not available while a plugin is loading";
		return CLM_LUA_DENY;
	}
	/*
	 * A grant carrying a scheme is matched as a literal URL prefix. That
	 * is the escape hatch for transports this parser deliberately does
	 * not understand (the test host's "test://", say): the user has to
	 * write out the exact prefix, so it grants no more than it says.
	 */
	for (size_t i = 0; i < p->nroots[CLM_LUA_CAP_HTTP]; i++) {
		const char *allow = p->roots[CLM_LUA_CAP_HTTP][i];
		size_t al;

		if (strstr(allow, "://") == NULL)
			continue;
		al = strlen(allow);
		if (strncmp(url, allow, al) == 0)
			return CLM_LUA_ALLOW;
	}

	if (!url_host(url, host, sizeof(host))) {
		*why = "not a plain http/https URL with a host";
		return CLM_LUA_DENY;
	}

	for (size_t i = 0; i < p->nroots[CLM_LUA_CAP_HTTP]; i++) {
		const char *allow = p->roots[CLM_LUA_CAP_HTTP][i];

		if (strstr(allow, "://") != NULL)
			continue;
		if (allow[0] == '.') {
			/* ".example.com" matches example.com and any
			 * subdomain of it, but not "notexample.com". */
			size_t hl = strlen(host), al = strlen(allow);
			if (hl == al - 1 &&
			    strcmp(host, allow + 1) == 0)
				return CLM_LUA_ALLOW;
			if (hl > al &&
			    strcmp(host + (hl - al), allow) == 0)
				return CLM_LUA_ALLOW;
			continue;
		}
		if (strcmp(host, allow) == 0)
			return CLM_LUA_ALLOW;
	}

	*why = "host is not on the plugin's allowlist";
	return CLM_LUA_ASK;
}

