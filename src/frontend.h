// SPDX-License-Identifier: ISC
/* Frontend entry points shared within the clm binary (not installed). */
#ifndef CLM_FRONTEND_H
#define CLM_FRONTEND_H

#include "clm/clm.h"

/*
 * Run the interactive ncurses frontend on a fresh default loop. Blocks until
 * the user quits. Returns 0 on success, non-zero on setup failure.
 * plugin_dir may be NULL (uses XDG default).
 */
int tui_run(const struct clm_cfg *cfg, const char *plugin_dir);

#endif /* CLM_FRONTEND_H */
