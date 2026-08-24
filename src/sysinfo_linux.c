// SPDX-License-Identifier: ISC

/* Linux half of the system-prompt host block (see sysinfo.h). */
#include <sys/vfs.h>

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

/* f_type magics from statfs(2); only the ones worth telling an agent
 * about, since a RAM-backed /tmp spends memory rather than disk. */
const char *
clm_cli_sysinfo_fstype(const char *path)
{
	struct statfs sfs;

	if (statfs(path, &sfs) != 0)
		return "";
	switch ((unsigned long)sfs.f_type) {
	case 0x01021994UL:
		return "tmpfs";
	case 0x858458f6UL:
		return "ramfs";
	case 0xef53UL:
		return "ext";
	case 0x9123683eUL:
		return "btrfs";
	case 0x58465342UL:
		return "xfs";
	case 0x2fc12fc1UL:
		return "zfs";
	case 0x6969UL:
		return "nfs";
	case 0x794c7630UL:
		return "overlayfs";
	default:
		return "";
	}
}
