// SPDX-License-Identifier: ISC
/* test_image -- base64 and image header recognition (libclm/image.c). */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clm/internal.h"
#include "tap.h"

#define CHECK(cond, msg) TAP_CHECK(cond, msg)

static void
test_base64(void)
{
	static const char *const in[] = {
	    "", "f", "fo", "foo", "foob", "fooba", "foobar"};
	static const char *const want[] = {
	    "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
	char out[16];

	for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
		size_t n = strlen(in[i]);

		CHECK(clm_base64_len(n) == strlen(want[i]), "base64 length");
		clm_base64_encode((const uint8_t *)in[i], n, out);
		CHECK(strcmp(out, want[i]) == 0, "base64 RFC 4648 vector");
	}
}

static int
sniff(const uint8_t *d, size_t n, const char *mt, uint32_t w, uint32_t h)
{
	const char *got = NULL;
	uint32_t gw = 0, gh = 0;

	if (clm_image_sniff(d, n, &got, &gw, &gh) != 0)
		return 0;
	return strcmp(got, mt) == 0 && gw == w && gh == h;
}

static void
test_png(void)
{
	uint8_t d[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13,
	    'I', 'H', 'D', 'R', 0, 0, 2, 0x80, 0, 0, 1, 0xe0};

	CHECK(sniff(d, sizeof(d), "image/png", 640, 480), "png size");
	CHECK(clm_image_sniff(d, sizeof(d) - 1, NULL, NULL, NULL) == -EINVAL,
	    "png cut short");
}

static void
test_jpeg(void)
{
	/* SOI, APP0 (16 bytes), DHT (4 bytes), SOF2 with 480x640. */
	uint8_t d[] = {0xff, 0xd8, 0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', 0, 1,
	    1, 0, 0, 1, 0, 1, 0, 0, 0xff, 0xc4, 0, 4, 0, 0, 0xff, 0xc2, 0, 17,
	    8, 1, 0xe0, 2, 0x80, 3};
	uint8_t nosof[] = {0xff, 0xd8, 0xff, 0xe0, 0, 4, 0, 0, 0xff, 0xd9};

	CHECK(sniff(d, sizeof(d), "image/jpeg", 640, 480),
	    "jpeg size from SOF2 after APP0 and DHT");
	CHECK(
	    clm_image_sniff(nosof, sizeof(nosof), NULL, NULL, NULL) == -EINVAL,
	    "jpeg with no frame header");
}

static void
test_gif(void)
{
	uint8_t d[] = {'G', 'I', 'F', '8', '9', 'a', 10, 0, 20, 0};

	CHECK(sniff(d, sizeof(d), "image/gif", 10, 20), "gif size");
}

static void
test_webp(void)
{
	uint8_t x[30] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
	    'V', 'P', '8', 'X', 10, 0, 0, 0, 0, 0, 0, 0, 0x7f, 0x02, 0, 0xdf,
	    0x01, 0};
	uint8_t l[30] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
	    'V', 'P', '8', 'L', 5, 0, 0, 0, 0x2f};
	uint8_t v[30] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P',
	    'V', 'P', '8', ' ', 10, 0, 0, 0, 0, 0, 0, 0x9d, 0x01, 0x2a, 0x80,
	    0x02, 0xe0, 0x01};
	uint32_t bits = (640 - 1) | (uint32_t)(480 - 1) << 14;

	l[21] = bits & 0xff;
	l[22] = bits >> 8 & 0xff;
	l[23] = bits >> 16 & 0xff;
	l[24] = bits >> 24 & 0xff;
	CHECK(sniff(x, sizeof(x), "image/webp", 640, 480), "webp VP8X size");
	CHECK(sniff(l, sizeof(l), "image/webp", 640, 480), "webp VP8L size");
	CHECK(sniff(v, sizeof(v), "image/webp", 640, 480), "webp VP8 size");
}

static void
test_rejects(void)
{
	uint8_t zero[] = {'G', 'I', 'F', '8', '7', 'a', 0, 0, 5, 0};

	CHECK(clm_image_sniff((const uint8_t *)"hello world", 11, NULL, NULL,
	          NULL) == -EINVAL,
	    "text is not an image");
	CHECK(clm_image_sniff(zero, sizeof(zero), NULL, NULL, NULL) == -EINVAL,
	    "zero width is rejected");
	CHECK(clm_image_sniff(NULL, 0, NULL, NULL, NULL) == -EINVAL, "NULL");
}

static int
test_image_suite(void *arg)
{
	(void)arg;
	test_base64();
	test_png();
	test_jpeg();
	test_gif();
	test_webp();
	test_rejects();
	return 0;
}

int
main(void)
{
	TAP_ADD("image", test_image_suite, NULL);
	return tap_run();
}
