// SPDX-License-Identifier: ISC
/* Frontend entry points shared within the clm binary (not installed). */
#ifndef CLM_FRONTEND_H
#define CLM_FRONTEND_H

#include "clm/clm.h"
#include "clm/session.h"

struct clm_lua_cfg; /* opaque; NULL if no config.lua was found */

/*
 * Run the interactive ncurses frontend on a fresh default loop. Blocks until
 * the user quits. Returns 0 on success, non-zero on setup failure.
 * plugin_dir may be NULL (uses XDG default).
 * lcfg may be NULL (no config file found).
 * forever_prompt may be NULL (normal one-turn-per-message behavior); when
 * set, this prompt is auto-resubmitted every time a turn completes with
 * nothing else queued, so the agent keeps going without a human re-prompting
 * it each turn.
 * session may be NULL (no session logging); when set, the TUI takes
 * ownership -- every history message is appended to it, /clear rotates to a
 * fresh session, and on exit the id is printed (or the file discarded if
 * nothing was said).
 * restore may be NULL (fresh session); when set, its messages are replayed
 * into the agent and rendered before the first prompt. The caller keeps
 * ownership and frees it after tui_run returns.
 */
int tui_run(const struct clm_cfg *cfg, const char *plugin_dir,
    struct clm_lua_cfg *lcfg, const char *forever_prompt,
    struct clm_session *session, const struct clm_history *restore);

#endif /* CLM_FRONTEND_H */
