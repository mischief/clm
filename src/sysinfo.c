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

#define SYSINFO_MAX 2048
#define SYSINFO_PATH_MAX 1024

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

/*
 * Write ", <type>" for a filesystem worth naming, "" otherwise. A RAM-backed
 * filesystem is the case that matters: writing there spends memory, and
 * whatever lands in it is gone after a reboot.
 */
static void
fstype_note(const char *path, char *buf, size_t len)
{
	char type[64];
	bool ramdisk;

	clm_cli_sysinfo_fstype(path, type, sizeof(type));
	if (type[0] == '\0') {
		if (len > 0)
			buf[0] = '\0';
		return;
	}
	ramdisk = strcmp(type, "tmpfs") == 0 || strcmp(type, "ramfs") == 0 ||
	    strcmp(type, "mfs") == 0;
	(void)snprintf(buf, len, ", %s%s", type, ramdisk ? ", in RAM" : "");
}

char *
clm_cli_sysinfo(void)
{
	struct utsname u;
	char *block, *cwd;
	char cwd_fs[80], tmp_fs[80];
	long cores;
	uint64_t mem, disk, tmp_free;
	int off;

	block = malloc(SYSINFO_MAX);
	cwd = malloc(SYSINFO_PATH_MAX);
	if (block == NULL || cwd == NULL || uname(&u) != 0) {
		free(block);
		free(cwd);
		return NULL;
	}

	cores = sysconf(_SC_NPROCESSORS_ONLN);
	mem = clm_cli_sysinfo_physmem();
	disk = fs_free(".");
	if (getcwd(cwd, SYSINFO_PATH_MAX) == NULL)
		(void)snprintf(cwd, SYSINFO_PATH_MAX, ".");
	fstype_note(".", cwd_fs, sizeof(cwd_fs));

	off = snprintf(block, SYSINFO_MAX, "host: %s %s %s (%s)", u.sysname,
	    u.release, u.machine, u.nodename);
	if (off < 0 || off >= SYSINFO_MAX) {
		free(block);
		free(cwd);
		return NULL;
	}

	if (cores > 0)
		off += snprintf(block + off, (size_t)(SYSINFO_MAX - off),
		    "\ncpu cores: %ld", cores);
	if (mem > 0 && off < SYSINFO_MAX)
		off += snprintf(block + off, (size_t)(SYSINFO_MAX - off),
		    "\nmemory: %llu GiB", gib(mem));
	/* Name the directory the number belongs to: a split layout (the
	 * OpenBSD default) gives /, /usr, /var, /tmp, and /home each their
	 * own free space, and the one under the working directory says
	 * nothing about where the next write lands. */
	if (disk > 0 && off < SYSINFO_MAX)
		off += snprintf(block + off, (size_t)(SYSINFO_MAX - off),
		    "\nfree space where the working directory lives (%s%s): "
		    "%llu GiB",
		    cwd, cwd_fs, gib(disk));
	tmp_free = same_fs(".", "/tmp") ? 0 : fs_free("/tmp");
	if (tmp_free > 0 && off < SYSINFO_MAX) {
		fstype_note("/tmp", tmp_fs, sizeof(tmp_fs));
		off += snprintf(block + off, (size_t)(SYSINFO_MAX - off),
		    "\nfree space on /tmp (%s): %llu GiB",
		    tmp_fs[0] != '\0' ? tmp_fs + 2 : "unknown type",
		    gib(tmp_free));
	}
	if (off < SYSINFO_MAX)
		off += snprintf(block + off, (size_t)(SYSINFO_MAX - off),
		    "\nother mount points may have their own free space; "
		    "check df(1) before a large write");
	if (off < SYSINFO_MAX)
		(void)snprintf(block + off, (size_t)(SYSINFO_MAX - off), "%s",
		    clm_cli_sysinfo_hints());

	free(cwd);
	return block;
}
