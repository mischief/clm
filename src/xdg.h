// SPDX-License-Identifier: ISC
/* Shared XDG path helper for the clm binary (not installed). */
#ifndef CLM_XDG_H
#define CLM_XDG_H

/*
 * Build a path under the XDG config dir: $XDG_CONFIG_HOME/<suffix> or
 * ~/.config/<suffix>. Returns a malloc'd string, or NULL.
 */
char *xdg_config_path(const char *suffix);

/*
 * Build a path under the XDG cache dir: $XDG_CACHE_HOME/<suffix> or
 * ~/.cache/<suffix>. Returns a malloc'd string, or NULL.
 */
char *xdg_cache_path(const char *suffix);

/*
 * A private directory for this session's throwaway files, created 0700 at
 * $XDG_CACHE_HOME/clm/scratch/<key> with its parents. Cache, not /tmp: on
 * a machine where /tmp is tmpfs a build artifact left here would sit in
 * RAM. Returns a malloc'd path, or NULL if it could not be created.
 */
char *clm_cli_scratch_dir(const char *key);

/*
 * Delete scratch directories whose key is not in `live` (n entries) or that
 * have gone untouched for max_age_days. Returns the number removed.
 */
size_t clm_cli_scratch_gc(
    const char *const *live, size_t n, unsigned max_age_days);

#endif /* CLM_XDG_H */
