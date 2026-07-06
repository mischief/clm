// SPDX-License-Identifier: ISC
#ifndef CLM_COMPRESS_H
#define CLM_COMPRESS_H

#include <stddef.h>

/*
 * Optional history-content compression, installed by the embedder via
 * clm_agent_set_compressor(). Left NULL (the default), history content is
 * stored and serialized exactly as given -- this is the only supported mode
 * for embedders that do not call the setter. Intended for RAM-constrained
 * hosts (e.g. ESP32); desktop embedders have no reason to set this.
 *
 * Compressed content is opaque binary, not a C string: callers must not
 * assume it is NUL-terminated, and must track its length via
 * clm_message.content_len rather than strlen().
 */
struct clm_compressor {
	/*
	 * Compress src (src_len bytes) into a newly malloc'd buffer, returned
	 * via *out with length *out_len. Returns 0 on success, negative errno
	 * on failure -- the caller falls back to storing src uncompressed, so
	 * this is not fatal to the calling operation.
	 */
	int (*write)(void *ctx, const char *src, size_t src_len, char **out,
	    size_t *out_len);

	/*
	 * Inverse of write: decompress src (src_len bytes of opaque compressed
	 * data) into a newly malloc'd, NUL-terminated buffer returned via *out.
	 * Returns 0 on success, negative errno on failure.
	 */
	int (*read)(void *ctx, const char *src, size_t src_len, char **out);

	/*
	 * Minimum content length worth attempting to compress. Shorter content
	 * is always stored plain, skipping the CPU cost for messages too small
	 * to benefit.
	 */
	size_t min_len;

	void *ctx; /* opaque, passed to write/read */
};

#endif /* CLM_COMPRESS_H */
