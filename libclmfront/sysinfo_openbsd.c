// SPDX-License-Identifier: ISC

/* OpenBSD half of the system-prompt host block (see sysinfo.h). */
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sysinfo.h"
#include "banned.h"

void
clm_cli_sysinfo_fstype(const char *path, char *buf, size_t len)
{
	struct statfs sfs;

	if (statfs(path, &sfs) != 0)
		(void)snprintf(buf, len, "%s", "");
	else
		(void)snprintf(buf, len, "%s", sfs.f_fstypename);
}

uint64_t
clm_cli_sysinfo_physmem(void)
{
	int mib[2] = {CTL_HW, HW_PHYSMEM64};
	int64_t mem = 0;
	size_t len = sizeof(mem);

	if (sysctl(mib, 2, &mem, &len, NULL, 0) != 0 || mem < 0)
		return 0;
	return (uint64_t)mem;
}

const char *
clm_cli_sysinfo_hints(void)
{
	/* Every line here is a command spelling the model gets wrong by
	 * default, because its habits come from GNU userland. */
	return "\n\nuserland notes for this host:\n"
	       "- grep(1) is POSIX: \"a\\|b\" does not mean alternation. "
	       "Use grep -E \"a|b\".\n"
	       "- grep(1) has no --include; use find . -name '*.c' -exec grep "
	       "-n pattern {} +\n"
	       "- Privileges come from doas(1), not sudo(1).\n"
	       "- Packages: pkg_add, pkg_info, pkg_delete. Ports live in "
	       "/usr/ports.\n"
	       "- sed(1), awk(1), and ps(1) are the BSD versions; GNU-only "
	       "flags (sed -i without an argument, ps --forest) fail.\n"
	       "- The man pages here are authoritative: read man(1) before "
	       "guessing at a flag.";
}
