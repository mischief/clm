// SPDX-License-Identifier: ISC

/* Fallback half of the system-prompt host block for hosts with no port of
 * their own (see sysinfo.h). uname and the core count still work; memory and
 * userland guidance are left out rather than guessed. */
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "sysinfo.h"
#include "banned.h"

uint64_t
clm_cli_sysinfo_physmem(void)
{
	return 0;
}

const char *
clm_cli_sysinfo_hints(void)
{
	return "";
}

void
clm_cli_sysinfo_fstype(const char *path, char *buf, size_t len)
{
	(void)path;
	(void)snprintf(buf, len, "%s", "");
}
