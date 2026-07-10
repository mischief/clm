// SPDX-License-Identifier: ISC
#ifndef CLM_ROOT_VFS_H
#define CLM_ROOT_VFS_H

#include <stddef.h>

/*
 * A synthetic, read-only "/" filesystem: ESP-IDF's VFS is a set of
 * registered mount-point prefixes (like "/sd"), not a real filesystem
 * tree -- nothing is registered at "/" itself (esp_vfs_register()
 * actively rejects base_path lengths < 2, so "/" can never be a real
 * mount point), so opendir("/") normally just ENOENTs. This registers an
 * empty-prefix ("") "default" VFS -- ESP-IDF's catch-all for any path that
 * doesn't match a more specific registered prefix -- whose opendir/readdir
 * fabricate a listing of `entries` for the path "/" specifically, and
 * ENOENT everything else. So list_dir("/") now shows e.g. "sd/", giving
 * the model somewhere to start instead of a dead end.
 *
 * entries/n must outlive the registration (this doesn't copy them --
 * callers pass a static array). Call once per board, after board-specific
 * mounting (so real mount points like "/sd" are registered at their own
 * higher-priority prefix first and continue to route there normally --
 * this only ever sees paths nothing else claimed).
 */
void root_vfs_register(const char *const *entries, size_t n);

#endif
