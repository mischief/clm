// SPDX-License-Identifier: ISC
/* Unit test for the subscription-OAuth crypto helpers and token-store refresh
 * (libclmfront/oauth.c). We include the .c directly to reach its static
 * SHA-256 / base64url / store helpers. Network is never touched: the HTTP
 * transport is swapped for a fake via clm_oauth_test_set_transport (built
 * with -DCLM_OAUTH_TEST). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define CLM_OAUTH_TEST 1
void clm_oauth_test_set_transport(
    int (*fn)(const char *, const char *, int *, char **));
#include "../libclmfront/oauth.c"

static int fails;

/* ---- fake token endpoint (rotates refresh tokens like the real one) ---- */

static pthread_mutex_t fake_mtx = PTHREAD_MUTEX_INITIALIZER;
static char fake_srv_refresh[64] = "S0";
static int fake_refresh_count = 0; /* successful rotations */
static int fake_invalid_grant = 0; /* rejected refresh attempts */
static int fake_reject_all = 0;    /* if set, reject even a valid token */

static void
fake_reset(void)
{
	pthread_mutex_lock(&fake_mtx);
	strcpy(fake_srv_refresh, "S0");
	fake_refresh_count = 0;
	fake_invalid_grant = 0;
	fake_reject_all = 0;
	pthread_mutex_unlock(&fake_mtx);
}

/* The fake's "server-side" current refresh token. */
static const char *
fake_curr_refresh(void)
{
	return fake_srv_refresh;
}

static int
fake_http_post(const char *url, const char *body, int *code, char **resp)
{
	cJSON *obj = cJSON_Parse(body);
	cJSON *gt, *rtobj;
	const char *gt_s, *rt_s;
	char nbuf[16];
	(void)url;

	pthread_mutex_lock(&fake_mtx);
	gt = cJSON_GetObjectItem(obj, "grant_type");
	gt_s = cJSON_GetStringValue(gt);
	if (gt_s != NULL && strcmp(gt_s, "refresh_token") == 0) {
		rtobj = cJSON_GetObjectItem(obj, "refresh_token");
		rt_s = cJSON_GetStringValue(rtobj);
		if (!fake_reject_all && rt_s != NULL &&
		    strcmp(rt_s, fake_srv_refresh) == 0) {
			cJSON *resp_o = cJSON_CreateObject();
			cJSON *scope_arr;

			fake_refresh_count++;
			(void)snprintf(
			    nbuf, sizeof(nbuf), "%d", fake_refresh_count);
			strcpy(fake_srv_refresh, "S");
			strcat(fake_srv_refresh, nbuf);
			cJSON_AddStringToObject(resp_o, "access_token", "AT");
			cJSON_AddStringToObject(
			    resp_o, "refresh_token", fake_srv_refresh);
			cJSON_AddNumberToObject(resp_o, "expires_in", 3600);
			cJSON_AddNumberToObject(
			    resp_o, "refresh_token_expires_in", 86400 * 30);
			scope_arr = cJSON_CreateArray();
			cJSON_AddItemToArray(
			    scope_arr, cJSON_CreateString("user:profile"));
			cJSON_AddItemToObject(resp_o, "scope", scope_arr);
			*code = 200;
			*resp = cJSON_PrintUnformatted(resp_o);
			cJSON_Delete(resp_o);
		} else {
			fake_invalid_grant++;
			*code = 400;
			*resp =
			    strdup("{\"type\":\"error\",\"error\":{\"type\":"
			           "\"invalid_grant\",\"message\":\"token "
			           "rotated\"}}");
		}
	} else {
		*code = 400;
		*resp = strdup(
		    "{\"type\":\"error\",\"error\":{\"type\":"
		    "\"invalid_request_error\",\"message\":\"bad grant\"}}");
	}
	cJSON_Delete(obj);
	pthread_mutex_unlock(&fake_mtx);
	return 0;
}

/* ---- SHA-256 / b64url / PKCE ---- */

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

/* ---- token-store resolve / refresh (fake transport, no network) ---- */

static char *
temp_store_path(void)
{
	const char *scratch = getenv("CLM_SCRATCH");
	char tmpl[1024];
	int fd;

	if (scratch != NULL && scratch[0] != '\0')
		(void)snprintf(
		    tmpl, sizeof(tmpl), "%s/oauth-test-store-XXXXXX", scratch);
	else
		(void)snprintf(
		    tmpl, sizeof(tmpl), "/tmp/oauth-test-store-XXXXXX");
	fd = mkstemp(tmpl);
	if (fd < 0)
		return NULL;
	close(fd);
	(void)unlink(tmpl);
	return strdup(tmpl);
}

/* Write a store with the given access/refresh tokens and lifetime. */
static void
write_store(
    const char *path, const char *access, const char *refresh, long expires_in)
{
	(void)clm_oauth_store_save(path, CLM_ANTHROPIC_OAUTH_CLIENT_ID, access,
	    refresh, expires_in, expires_in, "user:profile");
}

static void
check_resolve_fresh(void)
{
	char *path = temp_store_path();
	char *tok = NULL, *err = NULL;
	int rc;

	if (path == NULL) {
		fails++;
		return;
	}
	fake_reset();
	write_store(path, "AT0", "RT0", 86400 * 30); /* valid for a month */
	fake_reject_all = 1; /* any refresh attempt must fail */
	rc = clm_oauth_resolve(path, NULL, &tok, &err);
	if (!(rc == 0 && tok != NULL && strcmp(tok, "AT0") == 0)) {
		printf("  FAIL resolve uses fresh access without network "
		       "(rc=%d err=%s)\n",
		    rc, err ? err : "?");
		fails++;
	} else {
		printf("  ok resolve uses fresh access without network\n");
	}
	if (fake_refresh_count != 0) {
		printf("  FAIL resolve refreshed a fresh token\n");
		fails++;
	}
	free(tok);
	free(err);
	unlink(path);
	free(path);
}

static void
check_resolve_expired_refreshes(void)
{
	char *path = temp_store_path();
	char *tok = NULL, *err = NULL;
	int rc;

	if (path == NULL) {
		fails++;
		return;
	}
	fake_reset();
	write_store(path, "AT0", fake_curr_refresh(), -3600); /* expired */
	rc = clm_oauth_resolve(path, NULL, &tok, &err);
	if (!(rc == 0 && tok != NULL && strcmp(tok, "AT0") != 0)) {
		printf(
		    "  FAIL expired access triggers refresh (rc=%d err=%s)\n",
		    rc, err ? err : "?");
		fails++;
	} else {
		printf("  ok expired access triggers refresh\n");
	}
	if (fake_refresh_count != 1) {
		printf(
		    "  FAIL expected 1 refresh, saw %d\n", fake_refresh_count);
		fails++;
	}
	if (strcmp(fake_curr_refresh(), "S1") != 0) {
		printf("  FAIL server refresh token advanced to S1\n");
		fails++;
	}
	/* Store must now hold the refreshed access token and rotated refresh.
	 */
	{
		struct clm_oauth_store st;

		if (clm_oauth_store_load(path, &st) != 0 ||
		    st.access_token == NULL ||
		    strcmp(st.access_token, "AT") != 0 ||
		    st.refresh_token == NULL ||
		    strcmp(st.refresh_token, "S1") != 0) {
			printf(
			    "  FAIL store holds rotated pair after refresh\n");
			fails++;
		} else {
			printf("  ok store holds rotated pair after refresh\n");
		}
		clm_oauth_store_free(&st);
	}
	free(tok);
	free(err);
	unlink(path);
	free(path);
}

static void
check_resolve_revoked_errors(void)
{
	char *path = temp_store_path();
	char *tok = NULL, *err = NULL;
	int rc;

	if (path == NULL) {
		fails++;
		return;
	}
	fake_reset();
	write_store(path, "AT0", "RT0", -3600);
	fake_reject_all = 1;
	rc = clm_oauth_resolve(path, NULL, &tok, &err);
	if (rc == 0) {
		printf("  FAIL revoked refresh should fail\n");
		fails++;
	} else if (err == NULL || strstr(err, "re-authorize") == NULL) {
		printf(
		    "  FAIL revoked refresh error mentions re-authorize: %s\n",
		    err ? err : "(null)");
		fails++;
	} else {
		printf("  ok revoked refresh reports re-authorize\n");
	}
	free(tok);
	free(err);
	unlink(path);
	free(path);
}

/* ---- concurrency: two sessions sharing one store ---- */

struct resolve_arg {
	const char *path;
	int rc;
	char *tok;
};

static void *
resolver_thread(void *arg)
{
	struct resolve_arg *a = arg;
	char *err = NULL;

	a->rc = clm_oauth_resolve(a->path, NULL, &a->tok, &err);
	free(err);
	return NULL;
}

static void
check_concurrent_resolve(void)
{
	char *path = temp_store_path();
	pthread_t t1, t2;
	struct resolve_arg a = {0}, b = {0};
	int ok;

	if (path == NULL) {
		fails++;
		return;
	}
	fake_reset();
	write_store(path, "AT0", fake_curr_refresh(), -3600); /* both expired */

	a.path = path;
	b.path = path;
	pthread_create(&t1, NULL, resolver_thread, &a);
	pthread_create(&t2, NULL, resolver_thread, &b);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);

	ok = a.rc == 0 && b.rc == 0 && a.tok != NULL && b.tok != NULL;
	if (!ok) {
		printf("  FAIL concurrent resolves both succeed (a=%d b=%d)\n",
		    a.rc, b.rc);
		fails++;
	}
	if (fake_refresh_count != 1) {
		printf("  FAIL concurrent resolves did exactly 1 refresh, saw "
		       "%d\n",
		    fake_refresh_count);
		fails++;
	} else {
		printf(
		    "  ok concurrent resolves: 1 refresh, both got tokens\n");
	}
	free(a.tok);
	free(b.tok);
	unlink(path);
	free(path);
}

int
main(void)
{
	clm_oauth_test_set_transport(fake_http_post);
	check_sha256();
	check_b64url();
	check_pkce();
	check_resolve_fresh();
	check_resolve_expired_refreshes();
	check_resolve_revoked_errors();
	check_concurrent_resolve();
	if (fails != 0) {
		printf("oauth: %d check(s) failed\n", fails);
		return 1;
	}
	printf("oauth: all checks passed\n");
	return 0;
}
