// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clm/cleanup.h"
#include "seed_plugins.h"
#include "useful.h"
#include "banned.h"

/* mkdir -p: create dir and any missing parents. path is modified in place
 * (slashes swapped for NUL and restored) but left unchanged on return. */
static int
mkdir_p(char *path)
{
	for (char *p = path + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) != 0 && errno != EEXIST) {
			*p = '/';
			return -errno;
		}
		*p = '/';
	}
	if (mkdir(path, 0755) != 0 && errno != EEXIST)
		return -errno;
	return 0;
}

static int
write_file(const char *path, const unsigned char *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
		return errno == EEXIST ? 0 : -errno;

	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);
		if (n < 0) {
			int err = -errno;
			close(fd);
			(void)unlink(path);
			return err;
		}
		off += (size_t)n;
	}
	close(fd);
	return 0;
}

int
clm_seed_default_plugins(const char *dir)
{
	ASSERT_RETURN(dir != NULL, -EINVAL);

	autofree char *dir_copy = strdup(dir);
	if (dir_copy == NULL)
		return -ENOMEM;
	int r = mkdir_p(dir_copy);
	if (r < 0)
		return r;

	size_t dirlen = strlen(dir);

	/* write_file() is O_EXCL, so this is naturally idempotent per file:
	 * a plugin that's already there (whether it's the same seed from an
	 * earlier run, or a user edit) is left untouched, and any builtin
	 * added to clm_seed_plugins since the user's last `clm setup` still
	 * gets written on the next run instead of being locked out forever
	 * by a one-shot directory marker. Same contract as config.lua /
	 * secrets.lua above. If you deliberately delete a builtin plugin
	 * file, re-running `clm setup` will bring it back -- same tradeoff
	 * as those two. */
	for (const struct clm_seed_plugin *p = clm_seed_plugins;
	    p->name != NULL; p++) {
		autofree char *path = malloc(dirlen + 1 + strlen(p->name) + 1);
		if (path == NULL)
			continue;
		(void)snprintf(path, dirlen + 1 + strlen(p->name) + 1,
		    "%s/%s", dir, p->name);
		(void)write_file(path, p->data, p->len);
	}

	return 0;
}
