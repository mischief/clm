// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>

#include <time.h>

#include "oauth.h"

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4)                                              */
/* ------------------------------------------------------------------ */

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32

struct sha256 {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[SHA256_BLOCK];
	size_t buflen;
};

static const uint32_t sha256k[64] = {
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

static uint32_t
ror(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void
sha256_block(struct sha256 *s, const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) |
		    ((uint32_t)p[i * 4 + 1] << 16) |
		    ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 =
		    ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 =
		    ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	f = s->h[5];
	g = s->h[6];
	h = s->h[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		t1 = h + S1 + ch + sha256k[i] + w[i];
		t2 = S0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
	s->h[5] += f;
	s->h[6] += g;
	s->h[7] += h;
}

static void
sha256_init(struct sha256 *s)
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

static void
sha256_update(struct sha256 *s, const void *data, size_t n)
{
	const uint8_t *p = data;
	s->len += n;
	while (n > 0) {
		size_t take = SHA256_BLOCK - s->buflen;
		if (take > n)
			take = n;
		memcpy(s->buf + s->buflen, p, take);
		s->buflen += take;
		p += take;
		n -= take;
		if (s->buflen == SHA256_BLOCK) {
			sha256_block(s, s->buf);
			s->buflen = 0;
		}
	}
}

static void
sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST])
{
	uint64_t bits = s->len * 8;
	uint8_t pad[SHA256_BLOCK + 8];
	size_t padlen;
	int i;

	pad[0] = 0x80;
	memset(pad + 1, 0, sizeof(pad) - 1);
	padlen = (s->buflen < 56) ? 56 - s->buflen : 120 - s->buflen;
	sha256_update(s, pad, padlen);

	for (i = 0; i < 8; i++)
		pad[i] = (uint8_t)(bits >> ((7 - i) * 8));
	sha256_update(s, pad, 8);

	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(s->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)s->h[i];
	}
}

/* ------------------------------------------------------------------ */
/* base64url (RFC 4648 §5, unpadded)                                  */
/* ------------------------------------------------------------------ */

static const char b64url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void
b64url_encode(const uint8_t *in, size_t n, char *out)
{
	size_t i, o = 0;

	for (i = 0; i + 2 < n; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) |
		    ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = b64url[(v >> 18) & 63];
		out[o++] = b64url[(v >> 12) & 63];
		out[o++] = b64url[(v >> 6) & 63];
		out[o++] = b64url[v & 63];
	}
	if (n - i == 1) {
		uint32_t v = (uint32_t)in[i] << 16;
		out[o++] = b64url[(v >> 18) & 63];
		out[o++] = b64url[(v >> 12) & 63];
	} else if (n - i == 2) {
		uint32_t v =
		    ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
		out[o++] = b64url[(v >> 18) & 63];
		out[o++] = b64url[(v >> 12) & 63];
		out[o++] = b64url[(v >> 6) & 63];
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* PKCE                                                               */
/* ------------------------------------------------------------------ */

int
clm_oauth_pkce(char *verifier, size_t vlen, char *challenge, size_t clen)
{
	uint8_t raw[CLM_OAUTH_VERIFIER_LEN];
	uint8_t digest[SHA256_DIGEST];
	struct sha256 sh;
	int fd;
	ssize_t got = 0;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;
	while (got < (ssize_t)sizeof(raw)) {
		ssize_t r = read(fd, raw + got, sizeof(raw) - (size_t)got);
		if (r <= 0) {
			close(fd);
			return -1;
		}
		got += r;
	}
	close(fd);

	if (vlen < (CLM_OAUTH_VERIFIER_LEN + 2) / 3 * 4 + 1)
		return -1;
	b64url_encode(raw, sizeof(raw), verifier);

	sha256_init(&sh);
	sha256_update(&sh, verifier, strlen(verifier));
	sha256_final(&sh, digest);
	if (challenge != NULL) {
		if (clen < 44)
			return -1;
		b64url_encode(digest, SHA256_DIGEST, challenge);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Synchronous HTTPS via libcurl                                       */
/* ------------------------------------------------------------------ */

struct post_ctx {
	char *body;
	size_t len;
	char *err;
};

static size_t
post_write_cb(char *ptr, size_t sz, size_t nmemb, void *ud)
{
	struct post_ctx *ctx = ud;
	size_t n = sz * nmemb;
	char *np = realloc(ctx->body, ctx->len + n + 1);

	if (np == NULL)
		return 0;
	ctx->body = np;
	memcpy(ctx->body + ctx->len, ptr, n);
	ctx->len += n;
	ctx->body[ctx->len] = '\0';
	return n;
}

/*
 * POST `json` to url. On success returns 0 with *resp (malloc'd) set and
 * *code = HTTP status. On transport failure returns -1.
 */
static int
http_post_json(const char *url, const char *json, int *code, char **resp)
{
	CURL *curl;
	struct curl_slist *hdrs = NULL;
	struct post_ctx ctx = {0};
	char errbuf[CURL_ERROR_SIZE] = {0};
	CURLcode rc;

	curl = curl_easy_init();
	if (curl == NULL)
		return -1;

	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	hdrs = curl_slist_append(hdrs, "anthropic-beta: oauth-2025-04-20");
	hdrs = curl_slist_append(hdrs, "Accept: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, post_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "clm/oauth");

	rc = curl_easy_perform(curl);
	if (rc != CURLE_OK) {
		curl_slist_free_all(hdrs);
		curl_easy_cleanup(curl);
		free(ctx.body);
		return -1;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, code);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	*resp = ctx.body ? ctx.body : strdup("");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Token endpoint call                                                 */
/* ------------------------------------------------------------------ */

static int
token_post(const char *url, const char *body, char **access_out,
    char **refresh_out, long *expires_in, long *refresh_expires_in,
    char **scope_out, char **err_body)
{
	cJSON *obj;
	int code = 0;
	char *resp = NULL;
	const char *at, *rt, *scope;
	int rc = 0;

	if (http_post_json(url, body, &code, &resp) != 0)
		return -1;

	if (code != 200) {
		if (err_body != NULL)
			*err_body = resp;
		else
			free(resp);
		return -2;
	}

	obj = cJSON_Parse(resp);
	free(resp);
	if (obj == NULL)
		return -1;

	at = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "access_token"));
	rt = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "refresh_token"));
	scope = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "scope"));
	if (at == NULL || at[0] == '\0') {
		cJSON_Delete(obj);
		return -1;
	}

	*access_out = strdup(at);
	if (rt != NULL)
		*refresh_out = strdup(rt);
	else
		*refresh_out = NULL;
	if (scope_out != NULL)
		*scope_out = scope ? strdup(scope) : NULL;

	*expires_in = 0;
	if (cJSON_IsNumber(cJSON_GetObjectItem(obj, "expires_in")))
		*expires_in =
		    (long)cJSON_GetObjectItem(obj, "expires_in")->valuedouble;
	*refresh_expires_in = 0;
	if (cJSON_IsNumber(
	        cJSON_GetObjectItem(obj, "refresh_token_expires_in")))
		*refresh_expires_in =
		    (long)cJSON_GetObjectItem(obj, "refresh_token_expires_in")
		        ->valuedouble;

	cJSON_Delete(obj);
	return rc;
}

int
clm_oauth_exchange_code(const char *client_id, const char *code,
    const char *redirect_uri, const char *verifier, char **access_out,
    char **refresh_out, long *expires_in, long *refresh_expires_in,
    char **scope_out, char **err_body)
{
	cJSON *obj = cJSON_CreateObject();
	char *body;
	const char *cid = client_id ? client_id : CLM_ANTHROPIC_OAUTH_CLIENT_ID;
	int rc;

	if (obj == NULL)
		return -1;
	cJSON_AddStringToObject(obj, "grant_type", "authorization_code");
	cJSON_AddStringToObject(obj, "code", code);
	cJSON_AddStringToObject(obj, "redirect_uri",
	    redirect_uri ? redirect_uri : CLM_ANTHROPIC_OAUTH_REDIRECT);
	cJSON_AddStringToObject(obj, "client_id", cid);
	cJSON_AddStringToObject(obj, "code_verifier", verifier);
	body = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (body == NULL)
		return -1;

	rc = token_post(CLM_ANTHROPIC_OAUTH_TOKEN_URL, body, access_out,
	    refresh_out, expires_in, refresh_expires_in, scope_out, err_body);
	free(body);
	return rc;
}

int
clm_oauth_refresh(const char *client_id, const char *refresh_token,
    const char *scope, char **access_out, char **refresh_out, long *expires_in,
    long *refresh_expires_in, char **scope_out, char **err_body)
{
	cJSON *obj = cJSON_CreateObject();
	char *body;
	const char *cid = client_id ? client_id : CLM_ANTHROPIC_OAUTH_CLIENT_ID;
	int rc;

	if (obj == NULL)
		return -1;
	cJSON_AddStringToObject(obj, "grant_type", "refresh_token");
	cJSON_AddStringToObject(obj, "refresh_token", refresh_token);
	cJSON_AddStringToObject(obj, "client_id", cid);
	if (scope != NULL)
		cJSON_AddStringToObject(obj, "scope", scope);
	body = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (body == NULL)
		return -1;

	rc = token_post(CLM_ANTHROPIC_OAUTH_TOKEN_URL, body, access_out,
	    refresh_out, expires_in, refresh_expires_in, scope_out, err_body);
	free(body);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Token store                                                         */
/* ------------------------------------------------------------------ */

static char *
read_all(const char *path)
{
	int fd = open(path, O_RDONLY);
	char buf[512];
	char *all = NULL;
	size_t len = 0, cap = 0;
	ssize_t r;

	if (fd < 0)
		return NULL;
	for (;;) {
		r = read(fd, buf, sizeof(buf));
		if (r <= 0)
			break;
		if (len + (size_t)r + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 8192;
			char *np = realloc(all, ncap);
			if (np == NULL) {
				free(all);
				close(fd);
				return NULL;
			}
			all = np;
			cap = ncap;
		}
		memcpy(all + len, buf, (size_t)r);
		len += (size_t)r;
		all[len] = '\0';
	}
	close(fd);
	return all;
}

static char *
scopes_join(const cJSON *arr)
{
	const cJSON *it;
	size_t total = 0, n = 0;
	char *out;

	if (arr == NULL || !cJSON_IsArray(arr))
		return NULL;
	for (it = arr->child; it != NULL; it = it->next) {
		if (cJSON_IsString(it)) {
			total += strlen(it->valuestring) + 1;
			n++;
		}
	}
	if (n == 0)
		return NULL;
	out = malloc(total + 1);
	if (out == NULL)
		return NULL;
	out[0] = '\0';
	n = 0;
	for (it = arr->child; it != NULL; it = it->next) {
		if (cJSON_IsString(it)) {
			if (n++ > 0)
				strcat(out, " ");
			strcat(out, it->valuestring);
		}
	}
	return out;
}

void
clm_oauth_store_free(struct clm_oauth_store *s)
{
	free(s->client_id);
	free(s->access_token);
	free(s->refresh_token);
	free(s->scope);
	memset(s, 0, sizeof(*s));
}

int
clm_oauth_store_load(const char *path, struct clm_oauth_store *out)
{
	cJSON *obj, *v;
	char *raw;
	const char *s;
	long n;

	memset(out, 0, sizeof(*out));
	raw = read_all(path);
	if (raw == NULL)
		return -1;
	obj = cJSON_Parse(raw);
	free(raw);
	if (obj == NULL)
		return -1;

	s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "client_id"));
	if (s != NULL)
		out->client_id = strdup(s);
	s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "access_token"));
	if (s != NULL)
		out->access_token = strdup(s);
	s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "refresh_token"));
	if (s != NULL)
		out->refresh_token = strdup(s);

	v = cJSON_GetObjectItem(obj, "expires_at");
	if (cJSON_IsNumber(v))
		out->expires_at = (long)v->valuedouble;
	v = cJSON_GetObjectItem(obj, "refresh_token_expires_at");
	if (cJSON_IsNumber(v))
		out->refresh_expires_at = (long)v->valuedouble;

	s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "scope"));
	if (s != NULL) {
		out->scope = strdup(s);
	} else {
		out->scope = scopes_join(cJSON_GetObjectItem(obj, "scopes"));
	}

	cJSON_Delete(obj);
	/* require at least an access token */
	if (out->access_token == NULL || out->access_token[0] == '\0') {
		clm_oauth_store_free(out);
		return -1;
	}
	(void)n;
	return 0;
}

int
clm_oauth_store_save(const char *path, const char *client_id,
    const char *access, const char *refresh, long expires_in,
    long refresh_expires_in, const char *scope)
{
	cJSON *obj = cJSON_CreateObject();
	char *json;
	char tmp[1024];
	int fd;
	ssize_t w;
	long now = (long)time(NULL);

	if (obj == NULL)
		return -1;
	cJSON_AddNumberToObject(obj, "version", 1);
	cJSON_AddStringToObject(obj, "client_id", client_id);
	cJSON_AddStringToObject(obj, "access_token", access);
	if (refresh != NULL)
		cJSON_AddStringToObject(obj, "refresh_token", refresh);
	cJSON_AddNumberToObject(
	    obj, "expires_at", (double)(now + expires_in) * 1000.0);
	cJSON_AddNumberToObject(obj, "refresh_token_expires_at",
	    (double)(now + refresh_expires_in) * 1000.0);
	if (scope != NULL && scope[0] != '\0') {
		cJSON *arr = cJSON_CreateArray();
		const char *sp = scope;
		if (arr == NULL) {
			cJSON_Delete(obj);
			return -1;
		}
		while (*sp != '\0') {
			const char *p = strchr(sp, ' ');
			size_t n = p ? (size_t)(p - sp) : strlen(sp);
			char *tok = strndup(sp, n);
			if (tok != NULL) {
				cJSON_AddItemToArray(
				    arr, cJSON_CreateString(tok));
				free(tok);
			}
			sp = p ? p + 1 : sp + n;
		}
		cJSON_AddItemToObject(obj, "scopes", arr);
	}
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (json == NULL)
		return -1;

	(void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		free(json);
		return -1;
	}
	w = write(fd, json, strlen(json));
	close(fd);
	free(json);
	if (w < 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int
clm_oauth_resolve(
    const char *path, const char *client_id, char **access_out, char **err)
{
	struct clm_oauth_store st;
	const char *cid = client_id ? client_id : CLM_ANTHROPIC_OAUTH_CLIENT_ID;
	char *na = NULL, *nr = NULL, *nscope = NULL, *errbody = NULL;
	long ei = 0, rtei = 0;
	const char *scope;
	int rc;

	*access_out = NULL;
	if (err != NULL)
		*err = NULL;

	if (clm_oauth_store_load(path, &st) != 0) {
		if (err != NULL)
			*err = strdup("cannot load token store (run `clm "
			              "oauth login` first)");
		return -1;
	}

	if (st.access_token != NULL && st.refresh_token != NULL &&
	    st.access_token[0] != '\0' &&
	    st.expires_at > (long)time(NULL) * 1000 + 60000) {
		*access_out = strdup(st.access_token);
		clm_oauth_store_free(&st);
		return 0;
	}

	if (st.refresh_token == NULL || st.refresh_token[0] == '\0') {
		if (err != NULL)
			*err = strdup("token store has no refresh token; "
			              "run `clm oauth login` again");
		clm_oauth_store_free(&st);
		return -1;
	}

	scope = st.scope;
	rc = clm_oauth_refresh(cid, st.refresh_token, scope, &na, &nr, &ei,
	    &rtei, &nscope, &errbody);
	if (rc != 0) {
		if (err != NULL)
			*err = errbody
			    ? errbody
			    : strdup("token refresh failed (network/HTTP)");
		else
			free(errbody);
		clm_oauth_store_free(&st);
		return -1;
	}

	if (clm_oauth_store_save(
	        path, cid, na, nr, ei, rtei, nscope ? nscope : scope) != 0) {
		free(na);
		free(nr);
		free(nscope);
		if (err != NULL)
			*err = strdup("could not write refreshed token store");
		clm_oauth_store_free(&st);
		return -1;
	}
	free(nr);
	free(nscope);
	clm_oauth_store_free(&st);
	*access_out = na;
	return 0;
}

char *
clm_oauth_expand(const char *p)
{
	if (p != NULL && p[0] == '~' && p[1] == '/') {
		const char *home = getenv("HOME");
		if (home != NULL) {
			size_t n = strlen(home) + strlen(p + 1) + 1;
			char *out = malloc(n);
			if (out == NULL)
				return NULL;
			(void)snprintf(out, n, "%s%s", home, p + 1);
			return out;
		}
	}
	return p != NULL ? strdup(p) : NULL;
}
