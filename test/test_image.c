// SPDX-License-Identifier: ISC
/* test_image -- base64 and image header recognition (libclm/image.c). */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

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

static void
test_base64_decode(void)
{
	static const char *const in[] = {
	    "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
	static const char *const want[] = {
	    "", "f", "fo", "foo", "foob", "fooba", "foobar"};
	static const char *const bad[] = {"Zg=", "Zg",
	    "Z===", "Zg==Zg==", "Zm=v", "Zm9v\n", "Zm 9", "Zm9-", "=Zm9",
	    "Zm\0v", "Zh==", "Zm9="};
	static const uint8_t bin[] = {0x00, 0xff, 0x80, 0x7f, 0xfe};
	uint8_t out[16];
	char enc[16];
	size_t len;

	for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
		len = 99;
		CHECK(clm_base64_decode(in[i], strlen(in[i]), out, &len) == 0 &&
		        len == strlen(want[i]) &&
		        memcmp(out, want[i], len) == 0,
		    "base64 decode RFC 4648 vector");
	}
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(clm_base64_decode(bad[i], i == 9 ? 4 : strlen(bad[i]),
		          out, &len) == -EINVAL,
		    "base64 decode rejects bad input");
	clm_base64_encode(bin, sizeof(bin), enc);
	CHECK(clm_base64_decode(enc, strlen(enc), out, &len) == 0 &&
	        len == sizeof(bin) && memcmp(out, bin, len) == 0,
	    "base64 round trip of high bytes");
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

static void
test_files_url(void)
{
	char *u = clm_files_url("https://api.anthropic.com/v1/messages");

	CHECK(u != NULL && strcmp(u, "https://api.anthropic.com/v1/files") == 0,
	    "files url from messages url");
	free(u);
	CHECK(clm_files_url("http://h/v1/chat/completions") == NULL,
	    "no files url for another endpoint");
	CHECK(
	    clm_files_url("messages") == NULL, "no files url for a bare word");
}

static void
test_files_parse_id(void)
{
	char *id =
	    clm_files_parse_id("{\"id\":\"file_01Ab-9\",\"type\":\"file\"}");

	CHECK(id != NULL && strcmp(id, "file_01Ab-9") == 0, "upload id parsed");
	free(id);
	CHECK(clm_files_parse_id("{\"id\":\"file\\\"x\"}") == NULL,
	    "id with a quote is refused");
	CHECK(clm_files_parse_id("{\"id\":\"\"}") == NULL, "empty id refused");
	CHECK(clm_files_parse_id("{\"type\":\"file\"}") == NULL, "no id");
	CHECK(clm_files_parse_id("not json") == NULL, "bad json");
	CHECK(clm_files_parse_id(NULL) == NULL, "NULL body");
}

static void
test_files_multipart(void)
{
	static const char head_end[] = "Content-Type: image/png\r\n\r\nhi!";
	char boundary[CLM_FILE_BOUNDARY_LEN];
	char tail[128];
	size_t len = 0;
	char *b =
	    clm_files_multipart("data:image/png;base64,aGkh", boundary, &len);

	CHECK(b != NULL, "multipart built");
	if (b == NULL)
		return;
	CHECK(strncmp(b, "--clm-", 6) == 0 &&
	        strncmp(b + 2, boundary, strlen(boundary)) == 0,
	    "multipart starts with the boundary");
	CHECK(strstr(b, "name=\"file\"") != NULL && strstr(b, head_end) != NULL,
	    "file part holds the decoded bytes");
	(void)snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
	CHECK(len == strlen(b) && len > strlen(tail) &&
	        strcmp(b + len - strlen(tail), tail) == 0,
	    "multipart ends with the closing boundary");
	CHECK(
	    strstr(b, "name=\"expires_in_seconds\"\r\n\r\n2592000\r\n") != NULL,
	    "expiry field");
	free(b);
	CHECK(clm_files_multipart(
	          "data:image/png;base64,a=b!", boundary, &len) == NULL,
	    "bad base64 refused");
	CHECK(clm_files_multipart("https://x/y.png", boundary, &len) == NULL,
	    "not a data URL");
}

static void
test_files_apply(void)
{
	static const char json[] =
	    "[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\","
	    "\"image_url\":{\"url\":\"data:image/png;base64,QUFB\"}},"
	    "{\"type\":\"image_url\",\"image_url\":{\"url\":"
	    "\"data:image/png;base64,QkJC\"}}]}]";
	struct clm_agent *a = calloc(1, sizeof(*a));
	cJSON *m = cJSON_Parse(json);
	const char *missing = NULL;
	char url[64];
	int r = 0;

	CHECK(a != NULL && m != NULL, "apply: setup");
	if (a == NULL || m == NULL)
		goto done;
	CHECK(clm_files_add(a, "data:image/png;base64,QUFB", "file_a") == 0,
	    "add first");
	CHECK(clm_files_apply(a, m, &missing) == 0 && missing != NULL &&
	        strcmp(missing, "data:image/png;base64,QkJC") == 0,
	    "second image needs an upload");
	{
		char *s = cJSON_PrintUnformatted(m);

		CHECK(s != NULL && strstr(s, "\"file_id\":\"file_a\"") != NULL,
		    "first image named by id");
		free(s);
	}
	CHECK(clm_files_add(a, "data:image/png;base64,QkJC", NULL) == 0,
	    "record a failed upload");
	cJSON_Delete(m);
	m = cJSON_Parse(json);
	CHECK(clm_files_apply(a, m, &missing) == 0 && missing == NULL,
	    "a failed upload is not tried again");
	a->files[0].when -= CLM_FILE_REUSE_SECS + 1;
	cJSON_Delete(m);
	m = cJSON_Parse(json);
	CHECK(clm_files_apply(a, m, &missing) == 0 && missing != NULL &&
	        strcmp(missing, "data:image/png;base64,QUFB") == 0,
	    "an old upload goes up again");
	clm_files_clear(a);
	for (int i = 0; i < CLM_FILES_MAX && r == 0; i++) {
		(void)snprintf(url, sizeof(url), "data:image/png;base64,%d", i);
		r = clm_files_add(a, url, "file_x");
	}
	CHECK(r == 0 && clm_files_add(a, "data:x;base64,", NULL) == -ENOSPC,
	    "the cache is capped");
done:
	if (a != NULL)
		clm_files_clear(a);
	free(a);
	cJSON_Delete(m);
}

static int
test_image_suite(void *arg)
{
	(void)arg;
	test_files_url();
	test_files_parse_id();
	test_files_multipart();
	test_files_apply();
	test_base64();
	test_base64_decode();
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
