// SPDX-License-Identifier: ISC

/* Linux half of the system-prompt host block (see sysinfo.h). */
#include <sys/vfs.h>

#include <stddef.h>
#include <stdio.h>
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
void
clm_cli_sysinfo_fstype(const char *path, char *buf, size_t len)
{
	struct statfs sfs;
	const char *name = "";

	if (statfs(path, &sfs) == 0) {
		switch ((unsigned long)sfs.f_type) {
		case 0x01021994UL:
			name = "tmpfs";
			break;
		case 0x858458f6UL:
			name = "ramfs";
			break;
		case 0xef53UL:
			name = "ext";
			break;
		case 0x9123683eUL:
			name = "btrfs";
			break;
		case 0x58465342UL:
			name = "xfs";
			break;
		case 0x2fc12fc1UL:
			name = "zfs";
			break;
		case 0x6969UL:
			name = "nfs";
			break;
		case 0x794c7630UL:
			name = "overlayfs";
			break;
		default:
			break;
		}
	}
	(void)snprintf(buf, len, "%s", name);
}
