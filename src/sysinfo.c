// SPDX-License-Identifier: ISC

/*
 * Portable half of the system-prompt host block: uname, core count, and the
 * free space of the filesystems the agent is most likely to write to.
 * Memory and userland hints come from the per-OS file meson linked in.
 */
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#include <stdbool.h>
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

/* Free space in bytes on the filesystem holding `path`, 0 if unknown. */
static uint64_t
fs_free(const char *path)
{
	struct statvfs vfs;

	if (statvfs(path, &vfs) != 0)
		return 0;
	return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
}

/* True if the two paths sit on the same filesystem. */
static bool
same_fs(const char *a, const char *b)
{
	struct stat sa, sb;

	if (stat(a, &sa) != 0 || stat(b, &sb) != 0)
		return false;
	return sa.st_dev == sb.st_dev;
}

const char *
clm_cli_sysinfo(void)
{
	static char block[2048];
	static int built;
	struct utsname u;
	char cwd[1024];
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
	disk = fs_free(".");
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		(void)snprintf(cwd, sizeof(cwd), ".");

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
	/* Name the directory the number belongs to: a split layout (the
	 * OpenBSD default) gives /, /usr, /var, /tmp, and /home each their
	 * own free space, and the one under the working directory says
	 * nothing about where the next write lands. */
	if (disk > 0 && (size_t)off < sizeof(block))
		off += snprintf(block + off, sizeof(block) - (size_t)off,
		    "\ndisk free on the filesystem holding %s: %llu GiB", cwd,
		    gib(disk));
	if (!same_fs(".", "/tmp") && fs_free("/tmp") > 0 &&
	    (size_t)off < sizeof(block))
		off += snprintf(block + off, sizeof(block) - (size_t)off,
		    "\ndisk free on /tmp: %llu GiB", gib(fs_free("/tmp")));
	if ((size_t)off < sizeof(block))
		off += snprintf(block + off, sizeof(block) - (size_t)off,
		    "\nother mount points may have their own free space; "
		    "check df(1) before a large write");
	if ((size_t)off < sizeof(block))
		(void)snprintf(block + off, sizeof(block) - (size_t)off, "%s",
		    clm_cli_sysinfo_hints());

	return block;
}
