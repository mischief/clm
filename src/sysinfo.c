// SPDX-License-Identifier: ISC

/*
 * Portable half of the system-prompt host block: uname, core count, and free
 * space on the working directory. Memory and userland hints come from the
 * per-OS file meson linked in (see sysinfo.h).
 */
#include <sys/statvfs.h>
#include <sys/utsname.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysinfo.h"
#include "banned.h"

/* Bytes rendered as a whole number of GiB, or 0 if the value is unknown. */
static unsigned long long
gib(uint64_t bytes)
{
	return (unsigned long long)(bytes / (1024ULL * 1024 * 1024));
}

/* Free space in bytes on the filesystem holding the working directory. */
static uint64_t
cwd_free(void)
{
	struct statvfs vfs;

	if (statvfs(".", &vfs) != 0)
		return 0;
	return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
}

const char *
clm_cli_sysinfo(void)
{
	static char block[2048];
	static int built;
	struct utsname u;
	long cores;
	uint64_t mem, disk;
	int off;

	if (built)
		return block;
	built = 1;

	if (uname(&u) != 0)
		return block;

	cores = sysconf(_SC_NPROCESSORS_ONLN);
	mem = clm_cli_sysinfo_physmem();
	disk = cwd_free();

	off = snprintf(block, sizeof(block), "host: %s %s %s (%s)", u.sysname,
	    u.release, u.machine, u.nodename);
	if (off < 0 || (size_t)off >= sizeof(block)) {
		block[0] = '\0';
		return block;
	}

	if (cores > 0)
		off += snprintf(block + off, sizeof(block) - (size_t)off,
		    "\ncpu cores: %ld", cores);
	if (mem > 0 && (size_t)off < sizeof(block))
		off += snprintf(block + off, sizeof(block) - (size_t)off,
		    "\nmemory: %llu GiB", gib(mem));
	if (disk > 0 && (size_t)off < sizeof(block))
		off += snprintf(block + off, sizeof(block) - (size_t)off,
		    "\ndisk free here: %llu GiB", gib(disk));
	if ((size_t)off < sizeof(block))
		(void)snprintf(block + off, sizeof(block) - (size_t)off, "%s",
		    clm_cli_sysinfo_hints());

	return block;
}
