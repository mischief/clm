// SPDX-License-Identifier: ISC
/* Shared XDG path helper for the clm binary (not installed). */
#ifndef CLM_XDG_H
#define CLM_XDG_H

/*
 * Build a path under the XDG config dir: $XDG_CONFIG_HOME/<suffix> or
 * ~/.config/<suffix>. Returns a malloc'd string, or NULL.
 */
char *xdg_config_path(const char *suffix);

#endif /* CLM_XDG_H */
