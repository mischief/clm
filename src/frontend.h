// SPDX-License-Identifier: ISC
/* Frontend entry points shared within the clm binary (not installed). */
#ifndef CLM_FRONTEND_H
#define CLM_FRONTEND_H

#include "clm/clm.h"

struct clm_lua_cfg; /* opaque; may be NULL when CLM_LUA is disabled */

/*
 * Run the interactive ncurses frontend on a fresh default loop. Blocks until
 * the user quits. Returns 0 on success, non-zero on setup failure.
 * plugin_dir may be NULL (uses XDG default).
 * lcfg may be NULL (no config file found).
 * forever_prompt may be NULL (normal one-turn-per-message behavior); when
 * set, this prompt is auto-resubmitted every time a turn completes with
 * nothing else queued, so the agent keeps going without a human re-prompting
 * it each turn.
 */
int tui_run(const struct clm_cfg *cfg, const char *plugin_dir,
    struct clm_lua_cfg *lcfg, const char *forever_prompt);

#endif /* CLM_FRONTEND_H */
