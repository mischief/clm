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
#include "libclm/sha256_internal.h"
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
	uint8_t digest[CLM_SHA256_DIGEST];
	struct clm_sha256 sh;
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

	clm_sha256_init(&sh);
	clm_sha256_update(&sh, verifier, strlen(verifier));
	clm_sha256_final(&sh, digest);
	if (challenge != NULL) {
		if (clen < 44)
			return -1;
		b64url_encode(digest, CLM_SHA256_DIGEST, challenge);
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
		if (err_body != NULL) {
			/* Prefer a parsed, readable error over the raw JSON. */
			cJSON *eo = cJSON_Parse(resp);
			const char *et = NULL, *ed = NULL;

			if (eo != NULL) {
				et = cJSON_GetStringValue(
				    cJSON_GetObjectItem(eo, "error"));
				ed = cJSON_GetStringValue(cJSON_GetObjectItem(
				    eo, "error_description"));
				if (et == NULL)
					et = cJSON_GetStringValue(
					    cJSON_GetObjectItem(
					        eo, "error_type"));
			}
			if (et != NULL && et[0] != '\0') {
				size_t n =
				    strlen(et) + (ed ? strlen(ed) + 3 : 0) + 16;
				char *msg = malloc(n);

				if (msg != NULL) {
					if (ed != NULL && ed[0] != '\0')
						(void)snprintf(
						    msg, n, "%s (%s)", et, ed);
					else
						(void)snprintf(
						    msg, n, "%s", et);
					cJSON_Delete(eo);
					free(resp);
					*err_body = msg;
					return -2;
				}
			}
			cJSON_Delete(eo);
			*err_body = resp;
		} else {
			free(resp);
		}
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
    const char *redirect_uri, const char *verifier, const char *state,
    char **access_out, char **refresh_out, long *expires_in,
    long *refresh_expires_in, char **scope_out, char **err_body)
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
	/* The exchange echoes the OAuth state (CSRF); the CLI's own request
	 * omits scope here. */
	if (state != NULL && state[0] != '\0')
		cJSON_AddStringToObject(obj, "state", state);
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
		/* A rejected refresh token means the pair was rotated or
		 * revoked out from under us (another tool using the same
		 * subscription). Re-authorizing with `clm oauth login` is the
		 * only way to get a fresh, independent pair. */
		if (err != NULL) {
			size_t n;
			char *msg;

			if (errbody == NULL)
				errbody = strdup("token refresh failed "
				                 "(network/HTTP)");
			n = strlen(errbody) + 80;
			msg = malloc(n);
			if (msg != NULL) {
				(void)snprintf(msg, n,
				    "%s -- run `clm oauth "
				    "login` to re-authorize",
				    errbody);
				*err = msg;
			} else {
				*err = errbody;
				errbody = NULL;
			}
			free(errbody);
		} else {
			free(errbody);
		}
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

int
clm_oauth_exchange_and_save(const char *path, const char *code,
    const char *verifier, const char *state, char **err)
{
	char *access = NULL, *refresh = NULL, *scope = NULL, *eb = NULL;
	const char *cid = CLM_ANTHROPIC_OAUTH_CLIENT_ID;
	long ei = 0, rtei = 0;
	int rc;

	if (err != NULL)
		*err = NULL;

	rc = clm_oauth_exchange_code(cid, code, NULL, verifier, state, &access,
	    &refresh, &ei, &rtei, &scope, &eb);
	if (rc != 0) {
		if (err != NULL)
			*err = eb ? eb
			          : strdup("token exchange failed "
			                   "(network/HTTP)");
		else
			free(eb);
		free(access);
		free(refresh);
		free(scope);
		return -1;
	}

	if (clm_oauth_store_save(path, cid, access, refresh, ei, rtei, scope) !=
	    0) {
		if (err != NULL)
			*err = strdup("could not write token store");
		free(access);
		free(refresh);
		free(scope);
		return -1;
	}
	free(access);
	free(refresh);
	free(scope);
	return 0;
}
