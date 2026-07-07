// SPDX-License-Identifier: ISC
#ifndef CLM_COMPLETE_H
#define CLM_COMPLETE_H

#include <stdint.h>

struct ui;

/*
 * Handle a TAB keypress on the input line. Dispatches, in order:
 *   1. The first word, if it starts with '/': against a fixed list of
 *      slash-command names.
 *   2. A later word, if the first word names a command with a registered
 *      argument source (currently /model, /provider): config.lua names,
 *      and for /model, also a live GET /v1/models query against whatever
 *      connection is currently active.
 *   3. Anything else that looks like a path (contains '/' or starts with
 *      '~'): the filesystem.
 *
 * generation is u->complete_generation as of this keypress (bumped by
 * handle_keys in tui.c before dispatching here, on every key including
 * this TAB). The live /model query is async; its callback snapshots
 * generation at request time and compares against the live value when
 * the result arrives, dropping it silently if the user has since pressed
 * another key (typing on, another TAB, Enter, ...) rather than reaching
 * back into an input line that has moved on. Everything else here
 * resolves synchronously and never needs the check.
 */
void complete_input(struct ui *u, uint64_t generation);

#endif /* CLM_COMPLETE_H */
