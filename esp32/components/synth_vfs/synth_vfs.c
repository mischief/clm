// SPDX-License-Identifier: ISC
#include "synth_vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs.h"

static const char *TAG = "synth-vfs";

/* Per-registration state, reached via ESP-IDF's context-pointer mechanism
 * (ESP_VFS_FLAG_CONTEXT_PTR + the ctx esp_vfs_register() is given) -- NOT
 * global statics, unlike a single hardcoded root filesystem: this lets
 * more than one synth_vfs_register() call coexist (e.g. "" for the VFS
 * default plus a real "/whatever" prefix later), each with its own table. */
struct synth_mount {
	const struct synth_dirent *entries;
	size_t n;
};

static bool
is_root_path(const char *path)
{
	return path[0] == '\0' || strcmp(path, "/") == 0;
}

static const struct synth_dirent *
find_entry(const struct synth_mount *m, const char *path)
{
	const char *name = path[0] == '/' ? path + 1 : path;
	for (size_t i = 0; i < m->n; i++) {
		if (strcmp(name, m->entries[i].name) == 0)
			return &m->entries[i];
	}
	return NULL;
}

/*
 * NOT an opaque handle of our own design: esp_vfs_opendir() (vfs_calls.c)
 * writes a dd_vfs_idx field directly into whatever DIR* our opendir_p
 * returns ("ret->dd_vfs_idx = vfs->offset"), on the assumption it's a
 * real newlib DIR. `dir` MUST be the first member so the pointer we
 * return (and get back in readdir_p/closedir_p) is address-identical to
 * &synth_dir.dir -- standard C "first member aliasing" (a pointer to a
 * struct and to its first member are interconvertible by the standard).
 * Get this wrong and it doesn't crash cleanly: it's a stray write past a
 * too-small allocation, which manifests as opendir() "succeeding" while
 * every readdir() afterward silently returns nothing.
 */
struct synth_dir {
	DIR dir;
	const struct synth_mount *mount;
	size_t idx;
};

static DIR *
synth_opendir_p(void *ctx, const char *name)
{
	const struct synth_mount *m = ctx;
	struct synth_dir *d;

	if (!is_root_path(name)) {
		errno = ENOENT;
		return NULL;
	}
	d = calloc(1, sizeof(*d));
	if (d == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	d->mount = m;
	return &d->dir;
}

/* readdir(3)'s classic (non-reentrant) contract: one scratch dirent, valid
 * until the next readdir() call on ANY DIR* -- matches every other in-tree
 * VFS driver (FATFS included), so callers (list_dir's opendir/readdir
 * loop) already only assume that. Not thread-safe; fine for this project
 * (single-threaded agent loop touching the filesystem). */
static struct dirent s_dirent;

static struct dirent *
synth_readdir_p(void *ctx, DIR *pdir)
{
	struct synth_dir *d = (struct synth_dir *)pdir;
	const struct synth_dirent *e;

	(void)ctx;
	if (d->idx >= d->mount->n)
		return NULL;

	e = &d->mount->entries[d->idx++];
	memset(&s_dirent, 0, sizeof(s_dirent));
#ifdef DT_DIR
	s_dirent.d_type = e->is_dir ? DT_DIR : DT_REG;
#endif
	(void)strncpy(s_dirent.d_name, e->name, sizeof(s_dirent.d_name) - 1);
	return &s_dirent;
}

static int
synth_closedir_p(void *ctx, DIR *pdir)
{
	(void)ctx;
	free(pdir);
	return 0;
}

static int
synth_stat_p(void *ctx, const char *path, struct stat *st)
{
	const struct synth_mount *m = ctx;
	const struct synth_dirent *e;

	memset(st, 0, sizeof(*st));
	if (is_root_path(path)) {
		st->st_mode = S_IFDIR | 0555;
		return 0;
	}
	e = find_entry(m, path);
	if (e == NULL) {
		errno = ENOENT;
		return -1;
	}
	if (e->is_dir) {
		st->st_mode = S_IFDIR | 0555;
	} else {
		st->st_mode = S_IFREG | 0444;
		st->st_size = (off_t)e->len;
	}
	return 0;
}

/* ---- static byte-buffer file content (Plan 9's addbootfile, roughly) ---- */

#define SYNTH_MAX_OPEN 8

static struct {
	bool used;
	const struct synth_dirent *entry;
	size_t pos;
} s_open[SYNTH_MAX_OPEN];

static int
synth_open_p(void *ctx, const char *path, int oflags, int mode)
{
	const struct synth_mount *m = ctx;
	const struct synth_dirent *e;

	(void)mode;
	if ((oflags & O_ACCMODE) != O_RDONLY || (oflags & O_CREAT)) {
		errno = EROFS; /* read-only filesystem */
		return -1;
	}
	if (is_root_path(path)) {
		errno = EISDIR;
		return -1;
	}
	e = find_entry(m, path);
	if (e == NULL) {
		errno = ENOENT;
		return -1;
	}
	if (e->is_dir) {
		errno = EISDIR;
		return -1;
	}
	for (int fd = 0; fd < SYNTH_MAX_OPEN; fd++) {
		if (!s_open[fd].used) {
			s_open[fd].used = true;
			s_open[fd].entry = e;
			s_open[fd].pos = 0;
			return fd;
		}
	}
	errno = ENFILE; /* out of synthetic file slots */
	return -1;
}

static ssize_t
synth_read_p(void *ctx, int fd, void *dst, size_t size)
{
	(void)ctx;
	if (fd < 0 || fd >= SYNTH_MAX_OPEN || !s_open[fd].used) {
		errno = EBADF;
		return -1;
	}
	const struct synth_dirent *e = s_open[fd].entry;
	size_t remaining = e->len - s_open[fd].pos;
	size_t n = size < remaining ? size : remaining;
	if (n > 0 && e->data != NULL) {
		memcpy(dst, (const char *)e->data + s_open[fd].pos, n);
		s_open[fd].pos += n;
	}
	return (ssize_t)n;
}

static int
synth_close_p(void *ctx, int fd)
{
	(void)ctx;
	if (fd < 0 || fd >= SYNTH_MAX_OPEN || !s_open[fd].used) {
		errno = EBADF;
		return -1;
	}
	s_open[fd].used = false;
	s_open[fd].entry = NULL;
	return 0;
}

static const esp_vfs_t synth_vfs_ops = {
	.flags = ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_READONLY_FS,
	.open_p = synth_open_p,
	.read_p = synth_read_p,
	.close_p = synth_close_p,
	.stat_p = synth_stat_p,
	.opendir_p = synth_opendir_p,
	.readdir_p = synth_readdir_p,
	.closedir_p = synth_closedir_p,
};

esp_err_t
synth_vfs_register(const char *base_path, const struct synth_dirent *entries,
    size_t n)
{
	/* Leaked deliberately: this is meant for a handful of board-lifetime
	 * registrations at init, never unregistered, so there's no
	 * unregister-time free() to pair it with. */
	struct synth_mount *m = calloc(1, sizeof(*m));
	esp_err_t err;

	if (m == NULL)
		return ESP_ERR_NO_MEM;
	m->entries = entries;
	m->n = n;

	err = esp_vfs_register(base_path, &synth_vfs_ops, m);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "register(\"%s\") failed: %s", base_path,
		    esp_err_to_name(err));
		free(m);
	}
	return err;
}
