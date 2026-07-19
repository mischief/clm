// SPDX-License-Identifier: ISC
#include "clm/session.h"
#include "clm/cleanup.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "useful.h"
#include "session_internal.h"
#include "banned.h"

#define SESSION_FMT_VERSION 1
#define SESSION_ID_MAX 64
#define SESSION_SNIPPET_MAX 80

struct clm_session {
	int fd;
	char *id;
	char *path;
	bool has_msgs; /* a user/assistant message exists, ever */
};

/*
 * Session ids embed straight into filenames, so reject anything outside
 * [0-9A-Za-z-] before any path is built: "../evil" must never reach the
 * filesystem.
 */
static bool
id_valid(const char *id)
{
	size_t i;

	if (id == NULL || id[0] == '\0')
		return false;
	for (i = 0; id[i] != '\0'; i++) {
		char c = id[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		      (c >= 'A' && c <= 'Z') || c == '-'))
			return false;
		if (i >= SESSION_ID_MAX)
			return false;
	}
	return true;
}

/* mkdir -p, final component and parents created 0700. */
static int
mkdir_p(const char *path)
{
	autofree char *buf = NULL;
	size_t i, len;

	len = strlen(path);
	if (len == 0)
		return -EINVAL;
	buf = strdup(path);
	if (buf == NULL)
		return -ENOMEM;

	for (i = 1; i <= len; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0700) < 0 && errno != EEXIST)
			return -errno;
		if (i < len)
			buf[i] = '/';
	}
	return 0;
}

int
clm_session_state_dir(char *buf, size_t bufsz)
{
	const char *base;
	int n;

	ASSERT_RETURN(buf != NULL && bufsz > 0, -EINVAL);

	base = getenv("XDG_STATE_HOME");
	if (base != NULL && base[0] != '\0') {
		n = snprintf(buf, bufsz, "%s/clm", base);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] == '\0')
			return -ENOENT;
		n = snprintf(buf, bufsz, "%s/.local/state/clm", home);
	}
	if (n < 0 || (size_t)n >= bufsz)
		return -ENAMETOOLONG;

	return mkdir_p(buf);
}

/* Resolve dir (NULL = default state dir) into a malloc'd string. */
static int
resolve_dir(const char *dir, char **out)
{
	if (dir == NULL) {
		char *buf = malloc(PATH_MAX);
		int r;

		if (buf == NULL)
			return -ENOMEM;
		r = clm_session_state_dir(buf, PATH_MAX);
		if (r < 0) {
			free(buf);
			return r;
		}
		*out = buf;
		return 0;
	}
	*out = strdup(dir);
	return *out != NULL ? 0 : -ENOMEM;
}

/* Build <dir>/<id>.jsonl into a malloc'd string. */
static int
session_path(const char *dir, const char *id, char **out)
{
	autofree char *d = NULL;
	int r;

	r = resolve_dir(dir, &d);
	if (r < 0)
		return r;
	if (asprintf(out, "%s/%s.jsonl", d, id) < 0) {
		*out = NULL;
		return -ENOMEM;
	}
	return 0;
}

/* YYYYMMDD-HHMMSS-xxxxxxxx: local time plus 8 hex chars of randomness. */
static int
generate_id(char *buf, size_t bufsz)
{
	struct tm tm;
	time_t now = time(NULL);
	unsigned char rnd[4];
	autoclose int fd = -1;

	if (localtime_r(&now, &tm) == NULL)
		return -EINVAL;

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
		/* No entropy source: degrade to pid/time salt -- ids only
		 * need uniqueness within one user's session dir. */
		uint32_t v = (uint32_t)now ^ ((uint32_t)getpid() << 16);
		memcpy(rnd, &v, sizeof(rnd));
	}

	int n =
	    snprintf(buf, bufsz, "%04d%02d%02d-%02d%02d%02d-%02x%02x%02x%02x",
	             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
	             tm.tm_min, tm.tm_sec, rnd[0], rnd[1], rnd[2], rnd[3]);
	if (n < 0 || (size_t)n >= bufsz)
		return -ENAMETOOLONG;
	return 0;
}

/* Serialize obj onto one line and append it with a single write. */
static int
write_line(int fd, const cJSON *obj)
{
	autofree char *line = cJSON_PrintUnformatted(obj);
	size_t len;
	char *nl;

	if (line == NULL)
		return -ENOMEM;

	len = strlen(line);
	nl = realloc(line, len + 2);
	if (nl == NULL)
		return -ENOMEM;
	line = nl;
	line[len] = '\n';
	line[len + 1] = '\0';

	ssize_t w = write(fd, line, len + 1);
	if (w < 0)
		return -errno;
	if ((size_t)w != len + 1)
		return -EIO;
	return 0;
}

static struct clm_session *
session_alloc(int fd, const char *id, const char *path)
{
	struct clm_session *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	s->fd = fd;
	s->id = strdup(id);
	s->path = strdup(path);
	if (s->id == NULL || s->path == NULL) {
		free(s->id);
		free(s->path);
		free(s);
		return NULL;
	}
	return s;
}

int
clm_session_create(const char *dir, const char *model,
                   const char *provider_name, const char *agent_name,
                   struct clm_session **out)
{
	char id[SESSION_ID_MAX];
	autofree char *path = NULL;
	json_cleanup cJSON *meta = NULL;
	autoclose int fd = -1;
	struct clm_session *s;
	int r;

	ASSERT_RETURN(out != NULL, -EINVAL);
	*out = NULL;

	r = generate_id(id, sizeof(id));
	if (r < 0)
		return r;
	r = session_path(dir, id, &path);
	if (r < 0)
		return r;

	fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC,
	          0600);
	if (fd < 0)
		return -errno;

	meta = cJSON_CreateObject();
	if (meta == NULL ||
	    cJSON_AddStringToObject(meta, "type", "meta") == NULL ||
	    cJSON_AddNumberToObject(meta, "v", SESSION_FMT_VERSION) == NULL ||
	    cJSON_AddStringToObject(meta, "id", id) == NULL ||
	    cJSON_AddNumberToObject(meta, "created", (double)time(NULL)) ==
	        NULL)
		goto fail_nomem;
	if (model != NULL &&
	    cJSON_AddStringToObject(meta, "model", model) == NULL)
		goto fail_nomem;
	if (provider_name != NULL &&
	    cJSON_AddStringToObject(meta, "provider", provider_name) == NULL)
		goto fail_nomem;
	if (agent_name != NULL &&
	    cJSON_AddStringToObject(meta, "agent", agent_name) == NULL)
		goto fail_nomem;

	r = write_line(fd, meta);
	if (r < 0)
		goto fail;

	s = session_alloc(fd, id, path);
	if (s == NULL)
		goto fail_nomem;
	fd = -1; /* owned by s now */
	*out = s;
	return 0;

fail_nomem:
	r = -ENOMEM;
fail:
	(void)unlink(path);
	return r;
}

int
clm_session_open(const char *dir, const char *id, struct clm_session **out)
{
	autofree char *path = NULL;
	autoclose int fd = -1;
	struct clm_session *s;
	int r;

	ASSERT_RETURN(out != NULL, -EINVAL);
	*out = NULL;

	if (!id_valid(id))
		return -EINVAL;
	r = session_path(dir, id, &path);
	if (r < 0)
		return r;

	fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	s = session_alloc(fd, id, path);
	if (s == NULL)
		return -ENOMEM;
	fd = -1;
	/* A session worth resuming has messages; treat it as non-empty so
	 * an exit right after resume never deletes the file. */
	s->has_msgs = true;
	*out = s;
	return 0;
}

int
clm_session_append(struct clm_session *s, const struct clm_message *m,
                   const struct clm_compressor *cz)
{
	json_cleanup cJSON *obj = NULL;
	int r;

	ASSERT_RETURN(s != NULL && m != NULL, -EINVAL);

	obj = clm_message_to_json_full(m, cz);
	if (obj == NULL)
		return -ENOMEM;
	if (cJSON_AddStringToObject(obj, "type", "msg") == NULL)
		return -ENOMEM;

	r = write_line(s->fd, obj);
	if (r < 0)
		return r;

	if (m->role == CLM_ROLE_USER || m->role == CLM_ROLE_ASSISTANT)
		s->has_msgs = true;
	return 0;
}

const char *
clm_session_id(const struct clm_session *s)
{
	return s != NULL ? s->id : NULL;
}

bool
clm_session_is_empty(const struct clm_session *s)
{
	return s == NULL || !s->has_msgs;
}

int
clm_session_discard(struct clm_session *s)
{
	int r = 0;

	ASSERT_RETURN(s != NULL, -EINVAL);
	if (unlink(s->path) < 0)
		r = -errno;
	clm_session_free(s);
	return r;
}

void
clm_session_free(struct clm_session *s)
{
	if (s == NULL)
		return;
	if (s->fd >= 0)
		close(s->fd);
	free(s->id);
	free(s->path);
	free(s);
}

/*
 * Parse one JSONL line into hist (also the fuzz entry point, see
 * session_internal.h). Unknown types and unparsable lines are skipped
 * (returns 0): the loader must survive a truncated final line and lines
 * written by a newer clm. A meta line is validated for version and, when
 * out_meta is non-NULL and still empty, handed to the caller.
 */
int
session_parse_line(struct clm_history *hist, const char *line, size_t len,
                   cJSON **out_meta)
{
	json_cleanup cJSON *obj = NULL;
	const char *type;

	obj = cJSON_ParseWithLength(line, len);
	if (obj == NULL || !cJSON_IsObject(obj))
		return 0; /* tolerated: truncated/garbage line */

	type =
	    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "type"));
	if (type == NULL)
		return 0;

	if (strcmp(type, "meta") == 0) {
		cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, "v");
		if (cJSON_IsNumber(v) && v->valuedouble > SESSION_FMT_VERSION)
			return -EPROTONOSUPPORT;
		if (out_meta != NULL && *out_meta == NULL) {
			*out_meta = obj;
			obj = NULL; /* stolen */
		}
		return 0;
	}

	if (strcmp(type, "msg") == 0) {
		int r = clm_message_from_json(hist, obj, NULL);
		/* A malformed message line is skipped like any other bad
		 * line; only allocation failure is fatal. */
		return r == -ENOMEM ? -ENOMEM : 0;
	}

	return 0; /* unknown type: forward compatibility */
}

int
clm_session_load(const char *dir, const char *id, struct clm_history *hist,
                 cJSON **out_meta)
{
	autofree char *path = NULL;
	autoclosefile FILE *f = NULL;
	autofree char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int r;

	ASSERT_RETURN(hist != NULL, -EINVAL);
	if (out_meta != NULL)
		*out_meta = NULL;

	if (!id_valid(id))
		return -EINVAL;
	r = session_path(dir, id, &path);
	if (r < 0)
		return r;

	f = fopen(path, "re");
	if (f == NULL)
		return -errno;

	while ((n = getline(&line, &cap, f)) >= 0) {
		r = session_parse_line(hist, line, (size_t)n, out_meta);
		if (r < 0)
			return r;
	}
	if (ferror(f))
		return -EIO;

	return 0;
}

/* Newest first: descending by created, then by id for a stable order. */
static int
info_cmp(const void *a, const void *b)
{
	const struct clm_session_info *ia = a, *ib = b;

	if (ia->created != ib->created)
		return ia->created < ib->created ? 1 : -1;
	return strcmp(ib->id, ia->id);
}

static void
info_clear(struct clm_session_info *info)
{
	free(info->id);
	free(info->model);
	free(info->agent);
	free(info->first_user);
}

static char *
strdup_or_null(const char *s)
{
	return s != NULL ? strdup(s) : NULL;
}

/* First SESSION_SNIPPET_MAX bytes of s, newlines flattened to spaces. */
static char *
snippet(const char *s)
{
	char *out;
	size_t i, len = strlen(s);

	if (len > SESSION_SNIPPET_MAX)
		len = SESSION_SNIPPET_MAX;
	out = malloc(len + 1);
	if (out == NULL)
		return NULL;
	for (i = 0; i < len; i++)
		out[i] = (s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
	out[len] = '\0';
	return out;
}

/* Fill one listing row by reading a session file's meta and history. */
static int
info_fill(const char *dir, const char *id, struct clm_session_info *info)
{
	struct clm_history hist;
	json_cleanup cJSON *meta = NULL;
	struct clm_message *m;
	int r;

	memset(info, 0, sizeof(*info));
	clm_history_init(&hist);

	r = clm_session_load(dir, id, &hist, &meta);
	if (r < 0) {
		clm_history_free(&hist);
		return r;
	}

	info->id = strdup(id);
	if (info->id == NULL)
		goto fail;

	if (meta != NULL) {
		cJSON *created =
		    cJSON_GetObjectItemCaseSensitive(meta, "created");
		if (cJSON_IsNumber(created))
			info->created = (int64_t)created->valuedouble;
		info->model = strdup_or_null(cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(meta, "model")));
		info->agent = strdup_or_null(cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(meta, "agent")));
	}

	TAILQ_FOREACH(m, &hist, entries)
	{
		if (m->role != CLM_ROLE_USER || m->content == NULL)
			continue;
		if (strncmp(m->content, "[context update]",
		            strlen("[context update]")) == 0)
			continue;
		info->first_user = snippet(m->content);
		break;
	}

	clm_history_free(&hist);
	return 0;

fail:
	info_clear(info);
	clm_history_free(&hist);
	return -ENOMEM;
}

int
clm_session_list(const char *dir, struct clm_session_info **out, size_t *out_n)
{
	autofree char *d = NULL;
	autoclosedir DIR *dp = NULL;
	struct clm_session_info *infos = NULL;
	size_t n = 0, cap = 0;
	struct dirent *de;
	int r;

	ASSERT_RETURN(out != NULL && out_n != NULL, -EINVAL);
	*out = NULL;
	*out_n = 0;

	r = resolve_dir(dir, &d);
	if (r < 0)
		return r;

	dp = opendir(d);
	if (dp == NULL)
		return errno == ENOENT ? 0 : -errno;

	while ((de = readdir(dp)) != NULL) {
		char id[SESSION_ID_MAX + 8];
		size_t namelen = strlen(de->d_name);
		const char *suffix = ".jsonl";
		size_t sfxlen = strlen(suffix);

		if (namelen <= sfxlen || namelen - sfxlen >= sizeof(id))
			continue;
		if (strcmp(de->d_name + namelen - sfxlen, suffix) != 0)
			continue;
		memcpy(id, de->d_name, namelen - sfxlen);
		id[namelen - sfxlen] = '\0';
		if (!id_valid(id))
			continue;

		if (n == cap) {
			size_t ncap = cap == 0 ? 8 : cap * 2;
			struct clm_session_info *tmp =
			    realloc(infos, ncap * sizeof(*infos));
			if (tmp == NULL) {
				clm_session_list_free(infos, n);
				return -ENOMEM;
			}
			infos = tmp;
			cap = ncap;
		}

		/* An unreadable/newer-format file is skipped, not fatal:
		 * one bad file must not hide every other session. */
		if (info_fill(dir, id, &infos[n]) == 0)
			n++;
	}

	if (n == 0) {
		free(infos);
		return 0;
	}

	qsort(infos, n, sizeof(*infos), info_cmp);
	*out = infos;
	*out_n = n;
	return 0;
}

void
clm_session_list_free(struct clm_session_info *infos, size_t n)
{
	size_t i;

	if (infos == NULL)
		return;
	for (i = 0; i < n; i++)
		info_clear(&infos[i]);
	free(infos);
}
