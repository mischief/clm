// SPDX-License-Identifier: ISC
/*
 * Captured terminal output reduced to plain text: overstrike collapsed the
 * way col(1) does, escape sequences and other control bytes dropped. Pure
 * translation unit, so it is unit-testable on its own.
 */
#include <stddef.h>
#include <stdint.h>

#include "clm/internal.h"

/* Drop the last code point already written to out[0..n). */
static size_t
drop_last(const uint8_t *out, size_t n)
{
	if (n == 0)
		return 0;
	n--;
	/* Back up over the continuation bytes of a multi-byte character. */
	while (n > 0 && (out[n] & 0xC0) == 0x80)
		n--;
	return n;
}

/*
 * Skip the escape sequence starting at data[i] (which is ESC). Returns the
 * offset just past it. CSI and OSC run until their own terminator; anything
 * else is a two-byte sequence.
 */
static size_t
skip_escape(const uint8_t *data, size_t len, size_t i)
{
	i++; /* past ESC */
	if (i >= len)
		return i;

	if (data[i] == '[') { /* CSI: parameters, then a final byte */
		for (i++; i < len; i++)
			if (data[i] >= 0x40 && data[i] <= 0x7E)
				return i + 1;
		return i;
	}
	if (data[i] == ']') { /* OSC: runs to BEL or ESC \ */
		for (i++; i < len; i++) {
			if (data[i] == 0x07)
				return i + 1;
			if (data[i] == 0x1B && i + 1 < len &&
			    data[i + 1] == '\\')
				return i + 2;
		}
		return i;
	}
	return i + 1;
}

size_t
clm_clean_text(uint8_t *data, size_t len)
{
	size_t i = 0, n = 0;

	if (data == NULL)
		return 0;

	for (i = 0; i < len; i++) {
		uint8_t b = data[i];

		if (b == 0x1B) {
			i = skip_escape(data, len, i) - 1;
			continue;
		}
		if (b == '\b') {
			/* Overstrike: the character before it loses. */
			n = drop_last(data, n);
			continue;
		}
		if (b == '\n' || b == '\t') {
			data[n++] = b;
			continue;
		}
		if (b < 0x20 || b == 0x7F)
			continue; /* other control bytes carry no text */
		data[n++] = b;
	}
	return n;
}
