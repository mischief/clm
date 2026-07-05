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
 * fresh install has a usable starter set. No-ops (returns 0) once a
 * ".seeded" marker exists in dir, so it only runs once and never
 * clobbers files the user has since edited or deleted.
 */
int clm_seed_default_plugins(const char *dir);

#endif
