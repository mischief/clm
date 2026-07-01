// SPDX-License-Identifier: ISC
/* Frontend entry points shared within the clm binary (not installed). */
#ifndef CLM_FRONTEND_H
#define CLM_FRONTEND_H

#include "clm/clm.h"

/*
 * Run the interactive ncurses frontend on a fresh default loop. Blocks until
 * the user quits. Returns 0 on success, non-zero on setup failure.
 */
int tui_run(const struct clm_cfg *cfg);

#endif /* CLM_FRONTEND_H */
