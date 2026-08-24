// SPDX-License-Identifier: ISC
/* Frontend entry points shared within the clm binary (not installed). */
#ifndef CLM_FRONTEND_H
#define CLM_FRONTEND_H

#include "clm/clm.h"
#include "clm/session.h"

struct clm_lua_cfg; /* opaque; NULL if no config.lua was found or loading it
                     * failed -- see config_load_err below for telling those
                     * two apart */

/*
 * Run the interactive ncurses frontend on a fresh default loop. Blocks until
 * the user quits. Returns 0 on success, non-zero on setup failure.
 * plugin_dir may be NULL (uses XDG default).
 * lcfg may be NULL (no config file found, or it failed to load).
 * config_load_err may be NULL (no config.lua, or the caller isn't
 * distinguishing that from a load failure); when non-NULL, lcfg is also
 * NULL and this holds clm_lua_cfg_load()'s human-readable error -- config.lua
 * exists but failed to parse/run/return a table (see clm_lua_cfg_load's doc
 * comment for examples). The TUI pushes this as a prominent, impossible-to-
 * miss error banner once curses is up (a plain clm_debug() line, gated
 * behind $CLM_DEBUG_LOG and off by default, is not enough for this -- a
 * silently-ignored config.lua otherwise looks identical to "nothing
 * configured yet", which cost real debugging time before this existed). The
 * caller retains ownership and frees it (it does not outlive this call).
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
 * repaired_tool_calls is the count returned by
 * clm_history_repair_dangling_tool_calls() when the caller ran it on
 * restore before this call (0 if restore is NULL or nothing needed
 * repair); the TUI mentions it in the "[resumed session ...]" banner so a
 * crash-recovered resume is visible, not silent.
 */
int tui_run(const struct clm_cfg *cfg, const char *plugin_dir,
    struct clm_lua_cfg *lcfg, const char *config_load_err,
    const char *forever_prompt, struct clm_session *session,
    const struct clm_history *restore, int repaired_tool_calls);

#endif /* CLM_FRONTEND_H */
