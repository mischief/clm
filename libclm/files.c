// SPDX-License-Identifier: ISC
/* Image uploads to a provider's files endpoint. History keeps data URLs; a
 * request names each uploaded image by file id instead. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "clm/internal.h"
#include "banned.h"

static uint64_t file_hash(const char *s, size_t n);
static struct clm_file_ent *files_find(
    struct clm_agent *agent, const char *url, size_t len);
static int apply_part(
    struct clm_agent *agent, cJSON *part, const char **ret_missing);

int
clm_files_add(struct clm_agent *agent, const char *url, const char *file_id)
{
	const size_t len = strlen(url);
	struct clm_file_ent *e = files_find(agent, url, len);
	char *id = file_id != NULL ? strdup(file_id) : NULL;

	if (file_id != NULL && id == NULL)
		return -ENOMEM;
	if (e == NULL) {
		if (agent->nfiles >= CLM_FILES_MAX) {
			free(id);
			return -ENOSPC;
		}
		if (agent->nfiles == agent->cap_files) {
			size_t cap =
			    agent->cap_files ? agent->cap_files * 2 : 8;
			struct clm_file_ent *n =
			    reallocarray(agent->files, cap, sizeof(*n));

			if (n == NULL) {
				free(id);
				return -ENOMEM;
			}
			agent->files = n;
			agent->cap_files = cap;
		}
		e = &agent->files[agent->nfiles++];
		e->hash = file_hash(url, len);
		e->len = len;
		e->file_id = NULL;
	}
	free(e->file_id);
	e->file_id = id;
	e->when = time(NULL);
	return 0;
}

void
clm_files_clear(struct clm_agent *agent)
{
	for (size_t i = 0; i < agent->nfiles; i++)
		free(agent->files[i].file_id);
	free(agent->files);
	agent->files = NULL;
	agent->nfiles = 0;
	agent->cap_files = 0;
}

int
clm_files_apply(
    struct clm_agent *agent, cJSON *messages, const char **ret_missing)
{
	const cJSON *msg;

	if (ret_missing != NULL)
		*ret_missing = NULL;
	cJSON_ArrayForEach(msg, messages)
	{
		cJSON *content =
		    cJSON_GetObjectItemCaseSensitive(msg, "content");
		cJSON *part;

		if (!cJSON_IsArray(content))
			continue;
		cJSON_ArrayForEach(part, content)
		{
			if (apply_part(agent, part, ret_missing) < 0)
				return -ENOMEM;
		}
	}
	return 0;
}

char *
clm_files_multipart(const char *data_url,
    char ret_boundary[CLM_FILE_BOUNDARY_LEN], size_t *ret_len)
{
	const char *semi = strstr(data_url, ";base64,");
	const char *b64;
	char head[512];
	char tail[256];
	uint8_t *raw = NULL;
	size_t rawlen = 0, n;
	int hl, tl;
	char *body;

	if (strncmp(data_url, "data:", 5) != 0 || semi == NULL ||
	    semi - data_url - 5 > 64)
		return NULL;
	b64 = semi + 8;
	n = strlen(b64);
	raw = malloc(n / 4 * 3 + 1);
	if (raw == NULL || clm_base64_decode(b64, n, raw, &rawlen) < 0)
		goto fail;
	(void)snprintf(ret_boundary, CLM_FILE_BOUNDARY_LEN, "clm-%016llx",
	    (unsigned long long)file_hash(data_url, strlen(data_url)));
	if (memmem(raw, rawlen, ret_boundary, strlen(ret_boundary)) != NULL)
		goto fail;
	hl = snprintf(head, sizeof(head),
	    "--%s\r\nContent-Disposition: form-data; name=\"file\"; "
	    "filename=\"image\"\r\nContent-Type: %.*s\r\n\r\n",
	    ret_boundary, (int)(semi - data_url - 5), data_url + 5);
	tl = snprintf(tail, sizeof(tail),
	    "\r\n--%s\r\nContent-Disposition: form-data; "
	    "name=\"expires_in_seconds\"\r\n\r\n%d\r\n--%s--\r\n",
	    ret_boundary, CLM_FILE_EXPIRES_SECS, ret_boundary);
	if (hl < 0 || (size_t)hl >= sizeof(head) || tl < 0 ||
	    (size_t)tl >= sizeof(tail))
		goto fail;
	body = malloc((size_t)hl + rawlen + (size_t)tl + 1);
	if (body == NULL)
		goto fail;
	memcpy(body, head, (size_t)hl);
	memcpy(body + hl, raw, rawlen);
	memcpy(body + (size_t)hl + rawlen, tail, (size_t)tl + 1);
	*ret_len = (size_t)hl + rawlen + (size_t)tl;
	free(raw);
	return body;
fail:
	free(raw);
	return NULL;
}

char *
clm_files_parse_id(const char *body)
{
	cJSON *root;
	const char *id;
	char *out = NULL;

	if (body == NULL)
		return NULL;
	root = cJSON_Parse(body);
	id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id"));
	/* The id goes back into JSON and logs; keep it to plain characters. */
	if (id != NULL && id[0] != '\0' && strlen(id) < 256 &&
	    strspn(id,
	        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	        "0123456789_-") == strlen(id))
		out = strdup(id);
	cJSON_Delete(root);
	return out;
}

char *
clm_files_url(const char *base_url)
{
	static const char suffix[] = "/messages";
	const size_t n = strlen(base_url);
	const size_t sn = sizeof(suffix) - 1;
	char *out;

	if (n < sn || strcmp(base_url + n - sn, suffix) != 0)
		return NULL;
	out = malloc(n - sn + sizeof("/files"));
	if (out == NULL)
		return NULL;
	memcpy(out, base_url, n - sn);
	memcpy(out + n - sn, "/files", sizeof("/files"));
	return out;
}

/* FNV-1a, 64 bit. */
static uint64_t
file_hash(const char *s, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ULL;

	for (size_t i = 0; i < n; i++) {
		h ^= (uint8_t)s[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

static struct clm_file_ent *
files_find(struct clm_agent *agent, const char *url, size_t len)
{
	const uint64_t h = file_hash(url, len);

	for (size_t i = 0; i < agent->nfiles; i++)
		if (agent->files[i].hash == h && agent->files[i].len == len)
			return &agent->files[i];
	return NULL;
}

/* One content part: name a live upload, or report the image as missing. */
static int
apply_part(struct clm_agent *agent, cJSON *part, const char **ret_missing)
{
	cJSON *iu = cJSON_GetObjectItemCaseSensitive(part, "image_url");
	const char *url =
	    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(iu, "url"));
	const struct clm_file_ent *e;

	if (url == NULL || strncmp(url, "data:", 5) != 0)
		return 0;
	e = files_find(agent, url, strlen(url));
	if (e != NULL && e->file_id != NULL &&
	    difftime(time(NULL), e->when) < CLM_FILE_REUSE_SECS)
		return cJSON_AddStringToObject(iu, "file_id", e->file_id) ==
		        NULL
		    ? -ENOMEM
		    : 0;
	/* A failed upload stays inline. An expired one goes up again. */
	if (ret_missing != NULL && *ret_missing == NULL &&
	    (e == NULL || e->file_id != NULL))
		*ret_missing = url;
	return 0;
}
