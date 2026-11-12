// SPDX-License-Identifier: ISC
/*
 * SHA-256 (FIPS 180-4) shared by libclmsession and libclmfront. Kept as a
 * private, header-only inline implementation so it is never exposed to
 * callers of the libraries (nothing here is installed) and inject no link
 * symbols: each translation unit that needs it gets its own copy, so there
 * is nothing to collide or leak. The desktop build intentionally avoids a
 * crypto dependency (no OpenSSL/libmd); on esp32 ESP-IDF's mbedTLS covers
 * hashing instead.
 */
#ifndef CLM_SHA256_INTERNAL_H
#define CLM_SHA256_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLM_SHA256_DIGEST 32

struct clm_sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t buflen;
};

static const uint32_t clm_sha256_k[64] = {
    0x428a2f98,
    0x71374491,
    0xb5c0fbcf,
    0xe9b5dba5,
    0x3956c25b,
    0x59f111f1,
    0x923f82a4,
    0xab1c5ed5,
    0xd807aa98,
    0x12835b01,
    0x243185be,
    0x550c7dc3,
    0x72be5d74,
    0x80deb1fe,
    0x9bdc06a7,
    0xc19bf174,
    0xe49b69c1,
    0xefbe4786,
    0x0fc19dc6,
    0x240ca1cc,
    0x2de92c6f,
    0x4a7484aa,
    0x5cb0a9dc,
    0x76f988da,
    0x983e5152,
    0xa831c66d,
    0xb00327c8,
    0xbf597fc7,
    0xc6e00bf3,
    0xd5a79147,
    0x06ca6351,
    0x14292967,
    0x27b70a85,
    0x2e1b2138,
    0x4d2c6dfc,
    0x53380d13,
    0x650a7354,
    0x766a0abb,
    0x81c2c92e,
    0x92722c85,
    0xa2bfe8a1,
    0xa81a664b,
    0xc24b8b70,
    0xc76c51a3,
    0xd192e819,
    0xd6990624,
    0xf40e3585,
    0x106aa070,
    0x19a4c116,
    0x1e376c08,
    0x2748774c,
    0x34b0bcb5,
    0x391c0cb3,
    0x4ed8aa4a,
    0x5b9cca4f,
    0x682e6ff3,
    0x748f82ee,
    0x78a5636f,
    0x84c87814,
    0x8cc70208,
    0x90befffa,
    0xa4506ceb,
    0xbef9a3f7,
    0xc67178f2,
};

static inline uint32_t
clm_sha256_ror(uint32_t x, int n)
{
	return x >> n | x << (32 - n);
}

static inline void
clm_sha256_block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, hh;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		    (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = clm_sha256_ror(w[i - 15], 7) ^
		    clm_sha256_ror(w[i - 15], 18) ^ w[i - 15] >> 3;
		uint32_t s1 = clm_sha256_ror(w[i - 2], 17) ^
		    clm_sha256_ror(w[i - 2], 19) ^ w[i - 2] >> 10;

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = h[0];
	b = h[1];
	c = h[2];
	d = h[3];
	e = h[4];
	f = h[5];
	g = h[6];
	hh = h[7];
	for (i = 0; i < 64; i++) {
		uint32_t t1 = hh +
		    (clm_sha256_ror(e, 6) ^ clm_sha256_ror(e, 11) ^
		        clm_sha256_ror(e, 25)) +
		    ((e & f) ^ (~e & g)) + clm_sha256_k[i] + w[i];
		uint32_t t2 = (clm_sha256_ror(a, 2) ^ clm_sha256_ror(a, 13) ^
		                  clm_sha256_ror(a, 22)) +
		    ((a & b) ^ (a & c) ^ (b & c));

		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

/* Streaming interface (used by OAuth PKCE). */
static inline void
clm_sha256_init(struct clm_sha256 *s)
{
	static const uint32_t iv[8] = {
	    0x6a09e667,
	    0xbb67ae85,
	    0x3c6ef372,
	    0xa54ff53a,
	    0x510e527f,
	    0x9b05688c,
	    0x1f83d9ab,
	    0x5be0cd19,
	};
	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->buflen = 0;
}

static inline void
clm_sha256_update(struct clm_sha256 *s, const void *data, size_t n)
{
	const uint8_t *p = data;

	s->len += n;
	while (n > 0) {
		size_t take = 64 - s->buflen;

		if (take > n)
			take = n;
		memcpy(s->buf + s->buflen, p, take);
		s->buflen += take;
		p += take;
		n -= take;
		if (s->buflen == 64) {
			clm_sha256_block(s->h, s->buf);
			s->buflen = 0;
		}
	}
}

static inline void
clm_sha256_final(struct clm_sha256 *s, uint8_t out[CLM_SHA256_DIGEST])
{
	uint64_t bits = s->len * 8;
	uint8_t tail[128];
	size_t tlen;
	int i;

	memset(tail, 0, sizeof(tail));
	tail[0] = 0x80;
	tlen = s->buflen + 1 + 8 <= 64 ? 64 : 128;
	if (s->buflen > 0)
		memcpy(tail, s->buf, s->buflen);
	tail[s->buflen] = 0x80;
	for (i = 0; i < 8; i++)
		tail[tlen - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
	clm_sha256_block(s->h, tail);
	if (tlen == 128)
		clm_sha256_block(s->h, tail + 64);
	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(s->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)s->h[i];
	}
}

/* One-shot hex digest (used by session image naming). */
static inline void
clm_sha256_hex(const void *data, size_t len, char out[65])
{
	struct clm_sha256 s;
	uint8_t d[CLM_SHA256_DIGEST];
	int i;

	clm_sha256_init(&s);
	clm_sha256_update(&s, data, len);
	clm_sha256_final(&s, d);
	for (i = 0; i < CLM_SHA256_DIGEST; i++)
		(void)snprintf(out + 2 * i, 3, "%02x", d[i]);
}

#endif /* CLM_SHA256_INTERNAL_H */
