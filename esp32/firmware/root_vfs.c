// SPDX-License-Identifier: ISC
#include "root_vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs.h"

static const char *TAG = "root-vfs";

static const char *const *s_entries;
static size_t s_nentries;

/*
 * NOT an opaque handle of our own design: esp_vfs_opendir() (vfs_calls.c)
 * writes a dd_vfs_idx field directly into whatever DIR* our opendir
 * returns ("ret->dd_vfs_idx = vfs->offset"), on the assumption it's a
 * real newlib DIR. `dir` MUST be the first member so the pointer we
 * return (and get back in readdir/closedir) is address-identical to
 * &root_dir.dir -- i.e. this is the standard C "first member aliasing"
 * trick, not a violation of strict aliasing (a pointer to a struct and a
 * pointer to its first member are interconvertible by the standard).
 * Getting this wrong doesn't crash cleanly -- it's a stray write into
 * whatever comes after a too-small allocation, which manifested as
 * opendir() "succeeding" but every readdir() silently returning nothing
 * (dd_vfs_idx landed on garbage, get_vfs_for_index() resolved it to the
 * wrong/no vfs).
 */
struct root_dir {
	DIR dir;
	size_t idx;
};

static bool
is_root_path(const char *path)
{
	return path[0] == '\0' || strcmp(path, "/") == 0;
}

static DIR *
root_opendir(const char *name)
{
	struct root_dir *d;

	if (!is_root_path(name)) {
		errno = ENOENT;
		return NULL;
	}
	d = calloc(1, sizeof(*d));
	if (d == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	return &d->dir;
}

/* readdir(3)'s classic (non-reentrant) contract: static storage, valid
 * until the next call on any DIR* -- matches every other in-tree VFS
 * (FATFS included), so callers (list_dir's opendir/readdir loop) already
 * only assume that. */
static struct dirent s_dirent;

static struct dirent *
root_readdir(DIR *pdir)
{
	struct root_dir *d = (struct root_dir *)pdir;

	if (d->idx >= s_nentries)
		return NULL;

	memset(&s_dirent, 0, sizeof(s_dirent));
#ifdef DT_DIR
	s_dirent.d_type = DT_DIR;
#endif
	(void)strncpy(s_dirent.d_name, s_entries[d->idx],
	    sizeof(s_dirent.d_name) - 1);
	d->idx++;
	return &s_dirent;
}

static int
root_closedir(DIR *pdir)
{
	free(pdir);
	return 0;
}

static int
root_stat(const char *path, struct stat *st)
{
	const char *name;

	memset(st, 0, sizeof(*st));
	if (is_root_path(path)) {
		st->st_mode = S_IFDIR | 0555;
		return 0;
	}
	name = path[0] == '/' ? path + 1 : path;
	for (size_t i = 0; i < s_nentries; i++) {
		if (strcmp(name, s_entries[i]) == 0) {
			st->st_mode = S_IFDIR | 0555;
			return 0;
		}
	}
	errno = ENOENT;
	return -1;
}

/* This is a catch-all (empty base_path -- see root_vfs_register below), so
 * it sees every open() anywhere in the firmware for a path nothing else
 * claimed, not just ours. Fail closed rather than leaving this unset: an
 * always-ENOENT open is exactly what "no vfs matched this path" already
 * meant before this file existed, so nothing else's behavior changes. */
static int
root_open(const char *path, int flags, int mode)
{
	(void)path;
	(void)flags;
	(void)mode;
	errno = ENOENT;
	return -1;
}

static const esp_vfs_t root_vfs_ops = {
	.flags = ESP_VFS_FLAG_DEFAULT,
	.open = root_open,
	.opendir = root_opendir,
	.readdir = root_readdir,
	.closedir = root_closedir,
	.stat = root_stat,
};

void
root_vfs_register(const char *const *entries, size_t n)
{
	esp_err_t err;

	s_entries = entries;
	s_nentries = n;

	/* Empty base_path = ESP-IDF's "default VFS": lowest-priority match,
	 * used only for paths no other (more specific) registered prefix
	 * claims. Real mounts like "/sd" keep routing to their own driver
	 * as always; this only ever sees what's left over. */
	err = esp_vfs_register("", &root_vfs_ops, NULL);
	if (err != ESP_OK)
		ESP_LOGW(TAG, "root_vfs_register failed: %s",
		    esp_err_to_name(err));
}
