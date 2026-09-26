// SPDX-License-Identifier: ISC
/*
 * Image helpers for image input: base64 for data URLs, and recognition of
 * an image by its first bytes, with its size read from the header. Nothing
 * here decodes pixels.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "clm/internal.h"
#include "banned.h"

size_t
clm_base64_len(size_t n)
{
	return (n + 2) / 3 * 4;
}

void
clm_base64_encode(const uint8_t *in, size_t n, char *out)
{
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	                          "abcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;

	for (i = 0; i + 2 < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 |
		    in[i + 2];

		out[o++] = tab[v >> 18 & 63];
		out[o++] = tab[v >> 12 & 63];
		out[o++] = tab[v >> 6 & 63];
		out[o++] = tab[v & 63];
	}
	if (i < n) {
		uint32_t v = (uint32_t)in[i] << 16;

		if (i + 1 < n)
			v |= (uint32_t)in[i + 1] << 8;
		out[o++] = tab[v >> 18 & 63];
		out[o++] = tab[v >> 12 & 63];
		out[o++] = i + 1 < n ? tab[v >> 6 & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
}

static uint32_t
be16(const uint8_t *p)
{
	return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}

static uint32_t
le16(const uint8_t *p)
{
	return (uint32_t)p[1] << 8 | p[0];
}

static uint32_t
le24(const uint8_t *p)
{
	return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static int
sniff_png(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
	/* The IHDR chunk comes first: length, "IHDR", width, height. */
	if (n < 24 || memcmp(d + 12, "IHDR", 4) != 0)
		return -EINVAL;
	*w = be32(d + 16);
	*h = be32(d + 20);
	return 0;
}

/* Walk the markers to the first start-of-frame, which holds the size. */
static int
sniff_jpeg(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
	size_t i = 2;

	while (i + 4 <= n) {
		uint8_t m;
		size_t seg;

		if (d[i] != 0xff)
			return -EINVAL;
		m = d[i + 1];
		if (m == 0xff) { /* fill byte */
			i++;
			continue;
		}
		if (m == 0x01 || (m >= 0xd0 && m <= 0xd8)) { /* no length */
			i += 2;
			continue;
		}
		seg = be16(d + i + 2);
		if (seg < 2)
			return -EINVAL;
		/* SOF0..SOF15, except DHT (c4), JPG (c8) and DAC (cc). */
		if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 &&
		    m != 0xcc) {
			if (i + 9 > n || seg < 7)
				return -EINVAL;
			*h = be16(d + i + 5);
			*w = be16(d + i + 7);
			return 0;
		}
		i += 2 + seg;
	}
	return -EINVAL;
}

static int
sniff_gif(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
	if (n < 10)
		return -EINVAL;
	*w = le16(d + 6);
	*h = le16(d + 8);
	return 0;
}

static int
sniff_webp(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
	if (n < 30)
		return -EINVAL;
	if (memcmp(d + 12, "VP8X", 4) == 0) {
		*w = le24(d + 24) + 1;
		*h = le24(d + 27) + 1;
		return 0;
	}
	if (memcmp(d + 12, "VP8L", 4) == 0) {
		uint32_t b;

		if (d[20] != 0x2f)
			return -EINVAL;
		b = (uint32_t)d[21] | (uint32_t)d[22] << 8 |
		    (uint32_t)d[23] << 16 | (uint32_t)d[24] << 24;
		*w = (b & 0x3fff) + 1;
		*h = (b >> 14 & 0x3fff) + 1;
		return 0;
	}
	if (memcmp(d + 12, "VP8 ", 4) == 0) {
		/* Frame tag, then the 9d 01 2a start code, then the size. */
		if (d[23] != 0x9d || d[24] != 0x01 || d[25] != 0x2a)
			return -EINVAL;
		*w = le16(d + 26) & 0x3fff;
		*h = le16(d + 28) & 0x3fff;
		return 0;
	}
	return -EINVAL;
}

int
clm_image_sniff(const uint8_t *data, size_t len, const char **media_type,
    uint32_t *width, uint32_t *height)
{
	uint32_t w = 0, h = 0;
	const char *mt;
	int r;

	if (data == NULL)
		return -EINVAL;
	if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
		mt = "image/png";
		r = sniff_png(data, len, &w, &h);
	} else if (len >= 3 && memcmp(data, "\xff\xd8\xff", 3) == 0) {
		mt = "image/jpeg";
		r = sniff_jpeg(data, len, &w, &h);
	} else if (len >= 6 &&
	    (memcmp(data, "GIF87a", 6) == 0 ||
	        memcmp(data, "GIF89a", 6) == 0)) {
		mt = "image/gif";
		r = sniff_gif(data, len, &w, &h);
	} else if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
	    memcmp(data + 8, "WEBP", 4) == 0) {
		mt = "image/webp";
		r = sniff_webp(data, len, &w, &h);
	} else {
		return -EINVAL;
	}
	if (r < 0 || w == 0 || h == 0)
		return -EINVAL;
	if (media_type != NULL)
		*media_type = mt;
	if (width != NULL)
		*width = w;
	if (height != NULL)
		*height = h;
	return 0;
}
