// SPDX-License-Identifier: ISC

/*
 * Host facts appended to the agent's system prompt by the clm binary's
 * frontends. Not installed. The portable half lives in sysinfo.c; the hooks
 * below come from whichever sysinfo_<os>.c meson selected, so per-OS
 * knowledge is a link-time choice, not a #ifdef.
 */
#ifndef CLM_CLI_SYSINFO_H
#define CLM_CLI_SYSINFO_H

#include <stddef.h>
#include <stdint.h>

/*
 * OS, hardware, and the userland quirks that decide which command spellings
 * work here. Returns a malloc'd string the caller frees, or NULL on
 * allocation failure. Holds no state of its own.
 */
char *clm_cli_sysinfo(void);

/* Installed physical memory in bytes, or 0 when the host cannot report it.
 * Implemented per OS. */
uint64_t clm_cli_sysinfo_physmem(void);

/* Free-text guidance about this host's userland: command spellings that work
 * here and ones that only exist elsewhere. "" when there is nothing worth
 * saying. Implemented per OS. */
const char *clm_cli_sysinfo_hints(void);

/* Write the filesystem type of `path` ("ext", "tmpfs", "ffs", ...) into buf,
 * or "" when the host cannot report it. Implemented per OS. */
void clm_cli_sysinfo_fstype(const char *path, char *buf, size_t len);

#endif /* CLM_CLI_SYSINFO_H */
