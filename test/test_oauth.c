// SPDX-License-Identifier: ISC
/* Unit test for the subscription-OAuth crypto helpers (libclmfront/oauth.c).
 * We include the .c directly to reach its static SHA-256 / base64url
 * implementations; the network token exchange is not exercised here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libclmfront/oauth.c"

static int fails;

static void
check_sha256(void)
{
	struct clm_sha256 sh;
	unsigned char d[CLM_SHA256_DIGEST];
	const char *want = "ba7816bf8f01cfea414140de5dae2223"
	                   "b00361a396177a9cb410ff61f20015ad";
	char got[80];
	int i;
	size_t o = 0;

	clm_sha256_init(&sh);
	clm_sha256_update(&sh, "abc", 3);
	clm_sha256_final(&sh, d);
	for (i = 0; i < CLM_SHA256_DIGEST; i++)
		o += (size_t)snprintf(got + o, sizeof(got) - o, "%02x", d[i]);
	if (strcmp(got, want) != 0) {
		printf("  FAIL sha256(abc): %s\n", got);
		fails++;
	} else {
		printf("  ok sha256(abc)\n");
	}
}

static void
check_b64url(void)
{
	char b[128];

	b64url_encode((const unsigned char *)"any carnal pleasure.", 20, b);
	if (strcmp(b, "YW55IGNhcm5hbCBwbGVhc3VyZS4") != 0) {
		printf("  FAIL b64url: %s\n", b);
		fails++;
	} else {
		printf("  ok b64url\n");
	}
}

static void
check_pkce(void)
{
	char v[128], c[64], c2[64];
	struct clm_sha256 sh;
	unsigned char d[CLM_SHA256_DIGEST];
	size_t i;

	if (clm_oauth_pkce(v, sizeof(v), c, sizeof(c)) != 0) {
		printf("  FAIL pkce generation\n");
		fails++;
		return;
	}
	if (strlen(v) != 86) {
		printf("  FAIL verifier len %zu\n", strlen(v));
		fails++;
	}
	if (strlen(c) != 43) {
		printf("  FAIL challenge len %zu\n", strlen(c));
		fails++;
	}
	for (i = 0; i < strlen(v); i++) {
		char ch = v[i];
		if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) {
			printf("  FAIL verifier charset at %zu\n", i);
			fails++;
			break;
		}
	}
	clm_sha256_init(&sh);
	clm_sha256_update(&sh, v, strlen(v));
	clm_sha256_final(&sh, d);
	b64url_encode(d, CLM_SHA256_DIGEST, c2);
	if (strcmp(c, c2) != 0) {
		printf("  FAIL challenge != b64url(sha256(verifier))\n");
		fails++;
	} else {
		printf("  ok pkce\n");
	}
}

int
main(void)
{
	check_sha256();
	check_b64url();
	check_pkce();
	if (fails != 0) {
		printf("oauth: %d check(s) failed\n", fails);
		return 1;
	}
	printf("oauth: all checks passed\n");
	return 0;
}
