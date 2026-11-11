#ifndef CLM_SEED_PLUGINS_H
#define CLM_SEED_PLUGINS_H

#include <stddef.h>

struct clm_seed_plugin {
	const char *name;
	const unsigned char *data;
	size_t len;
};

/* NULL-terminated (by name) array of builtin plugin sources, generated
 * at build time from the plugins directory. */
extern const struct clm_seed_plugin clm_seed_plugins[];

/*
 * Write any builtin plugins into dir that aren't already present, so a
 * fresh install has a usable starter set. Per-file idempotent (like
 * config.lua/secrets.lua in `clm setup`): a file that's already there,
 * whether a prior seed or a user edit, is left untouched, but a builtin
 * added to clm_seed_plugins after the user's last `clm setup` still gets
 * written on the next run. Safe to call on every run of `clm setup`.
 */
int clm_seed_default_plugins(const char *dir);

#endif
