// SPDX-License-Identifier: ISC
#ifndef CLM_SYNTH_VFS_H
#define CLM_SYNTH_VFS_H

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

/*
 * A minimal, generic, read-only synthetic ESP-IDF VFS: register a static
 * table of directory entries at a mount prefix and get a real, walkable
 * directory (opendir/readdir/closedir/stat), plus optional static byte-
 * buffer file content (open/read/close), without hand-rolling ESP-IDF's
 * DIR-layout/dd_vfs_idx dance yourself.
 *
 * Why this exists: ESP-IDF's VFS has no equivalent of Plan 9's shared
 * devgen/devwalk/devdir/devdirread helpers (see 9front's
 * sys/src/9/port/devroot.c for the pattern this mirrors) -- every
 * synthetic filesystem driver reinvents opendir/readdir/stat from
 * scratch. This factors that out once so a caller just supplies a table.
 *
 * Concretely useful for a VFS prefix ESP-IDF itself can't route anywhere
 * a real filesystem would: the empty base_path "" is ESP-IDF's catch-all
 * "default VFS" (used only for paths no more specific registration
 * claims), and "/" itself can never be a real mount point at all
 * (esp_vfs_register() rejects any base_path shorter than 2 chars) -- so
 * synth_vfs_register("", ...) is how list_dir("/") gets something to
 * show instead of ENOENT.
 *
 * Multiple independent mounts (different base_path, different tables)
 * are fine -- entries/n are captured per-registration via ESP-IDF's
 * context-pointer mechanism, not global state.
 */
struct synth_dirent {
	const char *name;
	bool is_dir;      /* true: a placeholder subdirectory (e.g. a real
	                    * mount elsewhere, like "sd"). false: a file,
	                    * optionally backed by data/len. */
	const void *data; /* file content; NULL for an empty/zero-length
	                    * file, or for a directory entry (unused there). */
	size_t len;        /* content length; ignored for a directory entry. */
};

/*
 * Register a synthetic filesystem containing `entries`[0..n) at
 * `base_path` ("" for ESP-IDF's default/catch-all VFS, or a real
 * "/prefix" -- see esp_vfs_register()'s own base_path rules). `entries`
 * must outlive the registration: it is captured by pointer, not copied.
 */
esp_err_t synth_vfs_register(const char *base_path,
    const struct synth_dirent *entries, size_t n);

#endif
