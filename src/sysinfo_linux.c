// SPDX-License-Identifier: ISC

/* Linux half of the system-prompt host block (see sysinfo.h). */
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "sysinfo.h"
#include "banned.h"

uint64_t
clm_cli_sysinfo_physmem(void)
{
	long pages = sysconf(_SC_PHYS_PAGES);
	long pagesz = sysconf(_SC_PAGESIZE);

	if (pages <= 0 || pagesz <= 0)
		return 0;
	return (uint64_t)pages * (uint64_t)pagesz;
}

const char *
clm_cli_sysinfo_hints(void)
{
	return "\n\nuserland notes for this host:\n"
	       "- GNU userland: grep -E, grep -r --include, sed -i, and "
	       "ps --forest all work.\n"
	       "- Privileges come from sudo(1) or doas(1), whichever is "
	       "installed.";
}
