// SPDX-License-Identifier: ISC
#include "clm/session.h"
#include "clm/cleanup.h"
#include "clm/log.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
	bool has_msgs;        /* a user/assistant message exists, ever */
	uint64_t prompt_hash; /* of the last prompt record; 0 if none */
};

static uint64_t last_prompt_hash(const char *path);
static bool has_suffix(const char *name, const char *suffix);
static void blob_gc(const char *log_path);

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

/* Create <dir>/<id>.jsonl and write its meta line. id is already valid. */
static int
session_create_at(const char *dir, const char *id, const char *model,
    const char *provider_name, const char *agent_name, struct clm_session **out)
{
	autofree char *path = NULL;
	json_cleanup cJSON *meta = NULL;
	autoclose int fd = -1;
	struct clm_session *s;
	int r;

	r = session_path(dir, id, &path);
	if (r < 0)
		return r;

	fd = open(
	    path, O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
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
clm_session_create(const char *dir, const char *model,
    const char *provider_name, const char *agent_name, struct clm_session **out)
{
	char id[SESSION_ID_MAX];
	int r;

	ASSERT_RETURN(out != NULL, -EINVAL);
	*out = NULL;

	r = generate_id(id, sizeof(id));
	if (r < 0)
		return r;
	return session_create_at(
	    dir, id, model, provider_name, agent_name, out);
}

int
clm_session_create_id(const char *dir, const char *id, const char *model,
    const char *provider_name, const char *agent_name, struct clm_session **out)
{
	ASSERT_RETURN(out != NULL, -EINVAL);
	*out = NULL;

	if (!id_valid(id) || strlen(id) >= SESSION_ID_MAX)
		return -EINVAL;
	return session_create_at(
	    dir, id, model, provider_name, agent_name, out);
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
	s->prompt_hash = last_prompt_hash(path);
	/* Images left by a write that died before its log line. */
	blob_gc(path);
	*out = s;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Images: one file per image in <id>.blobs, named by its SHA-256.     */
/* ------------------------------------------------------------------ */

#define SESSION_BLOB_MAX (32 * 1024 * 1024) /* sanity cap on load */

/* "<dir>/<id>.jsonl" -> "<dir>/<id>.blobs" */
static char *
blob_dir_of(const char *log_path)
{
	size_t n = strlen(log_path);
	char *out;

	if (n > 6 && strcmp(log_path + n - 6, ".jsonl") == 0)
		n -= 6;
	if (asprintf(&out, "%.*s.blobs", (int)n, log_path) < 0)
		return NULL;
	return out;
}

static const char *
blob_ext(const char *media_type)
{
	if (strcmp(media_type, "image/png") == 0)
		return "png";
	if (strcmp(media_type, "image/jpeg") == 0)
		return "jpg";
	if (strcmp(media_type, "image/gif") == 0)
		return "gif";
	if (strcmp(media_type, "image/webp") == 0)
		return "webp";
	return "bin";
}

static bool
sha_valid(const char *sha)
{
	if (sha == NULL || strlen(sha) != 64)
		return false;
	for (size_t i = 0; i < 64; i++)
		if (!((sha[i] >= '0' && sha[i] <= '9') ||
		        (sha[i] >= 'a' && sha[i] <= 'f')))
			return false;
	return true;
}

/* Write data to path unless it is there: temp name, fsync, rename. */
static int
blob_write(const char *path, const uint8_t *data, size_t len)
{
	autofree char *tmp = NULL;
	int fd, r = 0;

	if (access(path, F_OK) == 0)
		return 0;
	if (asprintf(&tmp, "%s.tmp", path) < 0)
		return -ENOMEM;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;
	while (len > 0) {
		ssize_t w = write(fd, data, len);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			r = w < 0 ? -errno : -EIO;
			break;
		}
		data += w;
		len -= (size_t)w;
	}
	if (r == 0 && fsync(fd) != 0)
		r = -errno;
	(void)close(fd);
	if (r == 0 && rename(tmp, path) != 0)
		r = -errno;
	if (r < 0)
		(void)unlink(tmp);
	return r;
}

/*
 * Store each image of m in the blob directory and add the references to
 * obj as "attachments". The files are on disk before the caller writes
 * the line that names them.
 */
static int
store_attachments(const char *log_path, const struct clm_message *m, cJSON *obj)
{
	autofree char *bdir = NULL;
	cJSON *arr;

	if (m->n_attachments == 0)
		return 0;
	bdir = blob_dir_of(log_path);
	if (bdir == NULL)
		return -ENOMEM;
	if (mkdir(bdir, 0700) != 0 && errno != EEXIST)
		return -errno;
	arr = cJSON_AddArrayToObject(obj, "attachments");
	if (arr == NULL)
		return -ENOMEM;
	for (size_t i = 0; i < m->n_attachments; i++) {
		const struct clm_attachment *a = &m->attachments[i];
		autofree char *path = NULL;
		cJSON *e = cJSON_CreateObject();
		char sha[65];
		int r;

		if (e == NULL)
			return -ENOMEM;
		cJSON_AddItemToArray(arr, e);
		session_sha256_hex(a->data, a->len, sha);
		if (asprintf(&path, "%s/%s.%s", bdir, sha,
		        blob_ext(a->media_type)) < 0)
			return -ENOMEM;
		r = blob_write(path, a->data, a->len);
		if (r < 0)
			return r;
		if (cJSON_AddStringToObject(e, "type", "image") == NULL ||
		    cJSON_AddStringToObject(e, "media_type", a->media_type) ==
		        NULL ||
		    cJSON_AddStringToObject(e, "sha256", sha) == NULL ||
		    cJSON_AddNumberToObject(e, "bytes", (double)a->len) == NULL)
			return -ENOMEM;
	}
	return 0;
}

/* Read a blob file whole. NULL when it is missing or unreadable. */
static uint8_t *
blob_read(const char *path, size_t *len)
{
	autoclosefile FILE *f = fopen(path, "re");
	uint8_t *buf;
	struct stat st;

	if (f == NULL || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size <= 0 || st.st_size > SESSION_BLOB_MAX)
		return NULL;
	buf = malloc((size_t)st.st_size);
	if (buf == NULL)
		return NULL;
	if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
		free(buf);
		return NULL;
	}
	*len = (size_t)st.st_size;
	return buf;
}

/* Append every image hash a log file names to refs (a JSON array of
 * strings used as a set). A missing file names none. */
static void
blob_refs_from(const char *path, cJSON *refs)
{
	autoclosefile FILE *f = fopen(path, "re");
	autofree char *line = NULL;
	size_t cap = 0;
	static const char key[] = "\"sha256\":\"";

	if (f == NULL)
		return;
	while (getline(&line, &cap, f) >= 0) {
		const char *p = line;

		while ((p = strstr(p, key)) != NULL) {
			char sha[65];

			p += sizeof(key) - 1;
			if (strlen(p) < 64)
				break;
			memcpy(sha, p, 64);
			sha[64] = '\0';
			if (sha_valid(sha))
				cJSON_AddItemToArray(
				    refs, cJSON_CreateString(sha));
			p += 64;
		}
	}
}

static bool
refs_have(const cJSON *refs, const char *name)
{
	const cJSON *r;

	cJSON_ArrayForEach(r, refs)
	{
		if (strncmp(r->valuestring, name, 64) == 0)
			return true;
	}
	return false;
}

/*
 * Remove the images that neither the log nor its .bak names, and any
 * temp file a write left behind. Runs only after a log is safely in
 * place, so a failed compaction never costs an image.
 */
static void
blob_gc(const char *log_path)
{
	autofree char *bdir = blob_dir_of(log_path);
	autofree char *bak = NULL;
	json_cleanup cJSON *refs = cJSON_CreateArray();
	autoclosedir DIR *dp = NULL;
	struct dirent *de;

	if (bdir == NULL || refs == NULL ||
	    asprintf(&bak, "%s.bak", log_path) < 0)
		return;
	dp = opendir(bdir);
	if (dp == NULL)
		return;
	blob_refs_from(log_path, refs);
	blob_refs_from(bak, refs);
	while ((de = readdir(dp)) != NULL) {
		autofree char *path = NULL;

		if (de->d_name[0] == '.')
			continue;
		if (!has_suffix(de->d_name, ".tmp") &&
		    refs_have(refs, de->d_name))
			continue;
		if (asprintf(&path, "%s/%s", bdir, de->d_name) >= 0)
			(void)unlink(path);
	}
	(void)rmdir(bdir); /* only succeeds when empty */
}

/* Delete a blob directory and everything in it. */
static void
blob_dir_remove(const char *bdir)
{
	autoclosedir DIR *dp = opendir(bdir);
	struct dirent *de;

	if (dp == NULL)
		return;
	while ((de = readdir(dp)) != NULL) {
		autofree char *path = NULL;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (asprintf(&path, "%s/%s", bdir, de->d_name) >= 0)
			(void)unlink(path);
	}
	(void)rmdir(bdir);
}

/* FNV-1a: only tells one prompt from the next, not a security hash. */
static uint64_t
prompt_hash(const char *s)
{
	uint64_t h = 14695981039346656037ULL;

	for (; *s != '\0'; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h != 0 ? h : 1;
}

/* A prompt record: the system message as sent, kept for reference and never
 * replayed. Written only when it differs from the last one. */
static int
write_prompt(struct clm_session *s, int fd, const char *content)
{
	json_cleanup cJSON *obj = cJSON_CreateObject();
	uint64_t h = prompt_hash(content);
	char hex[17];
	int r;

	if (h == s->prompt_hash)
		return 0;
	if (s->prompt_hash != 0)
		clm_debug("session %s: system prompt changed since the last "
		          "record",
		    s->id);
	(void)snprintf(hex, sizeof(hex), "%016" PRIx64, h);
	if (obj == NULL ||
	    cJSON_AddStringToObject(obj, "type", "prompt") == NULL ||
	    cJSON_AddStringToObject(obj, "hash", hex) == NULL ||
	    cJSON_AddStringToObject(obj, "content", content) == NULL)
		return -ENOMEM;
	r = write_line(fd, obj);
	if (r == 0)
		s->prompt_hash = h;
	return r;
}

/* The plain text of a system message, malloc'd, or NULL. */
static char *
system_text(const struct clm_message *m, const struct clm_compressor *cz)
{
	json_cleanup cJSON *obj = clm_message_to_json_full(m, cz);
	const char *c = cJSON_GetStringValue(
	    cJSON_GetObjectItemCaseSensitive(obj, "content"));

	return strdup(c != NULL ? c : "");
}

/* The hash of the last prompt record in the file at path, or 0. */
static uint64_t
last_prompt_hash(const char *path)
{
	autoclosefile FILE *f = fopen(path, "re");
	autofree char *line = NULL;
	size_t cap = 0;
	uint64_t h = 0;

	if (f == NULL)
		return 0;
	while (getline(&line, &cap, f) >= 0) {
		json_cleanup cJSON *obj = NULL;
		const char *hex;

		static const char pre[] = "{\"type\":\"prompt\"";

		if (strncmp(line, pre, sizeof(pre) - 1) != 0)
			continue;
		obj = cJSON_Parse(line);
		hex = cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(obj, "hash"));
		if (hex != NULL)
			h = strtoull(hex, NULL, 16);
	}
	return h;
}

int
clm_session_append(struct clm_session *s, const struct clm_message *m,
    const struct clm_compressor *cz)
{
	json_cleanup cJSON *obj = NULL;
	int r;

	ASSERT_RETURN(s != NULL && m != NULL, -EINVAL);

	if (m->role == CLM_ROLE_SYSTEM) {
		autofree char *text = system_text(m, cz);

		if (text == NULL)
			return -ENOMEM;
		return write_prompt(s, s->fd, text);
	}

	obj = clm_message_to_json_full(m, cz);
	if (obj == NULL)
		return -ENOMEM;
	if (cJSON_AddStringToObject(obj, "type", "msg") == NULL)
		return -ENOMEM;
	r = store_attachments(s->path, m, obj);
	if (r < 0)
		return r;

	r = write_line(s->fd, obj);
	if (r < 0)
		return r;

	if (m->role == CLM_ROLE_USER || m->role == CLM_ROLE_ASSISTANT)
		s->has_msgs = true;
	return 0;
}

/* Read the log's first line (the meta record) so a rewrite can keep it. */
static char *
read_meta_line(const char *path)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;

	if (f == NULL)
		return NULL;
	n = getline(&line, &cap, f);
	(void)fclose(f);
	if (n <= 0) {
		free(line);
		return NULL;
	}
	if (line[n - 1] == '\n')
		line[n - 1] = '\0';
	return line;
}

int
clm_session_rewrite(struct clm_session *s, const struct clm_history *h,
    const struct clm_compressor *cz)
{
	const struct clm_message *m;
	autofree char *meta = NULL;
	autofree char *tmp = NULL;
	int fd = -1, r = 0;
	bool msgs = false;

	ASSERT_RETURN(s != NULL && h != NULL, -EINVAL);

	meta = read_meta_line(s->path);
	if (meta == NULL)
		return -EIO;
	if (asprintf(&tmp, "%s.tmp", s->path) < 0)
		return -ENOMEM;

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;

	if (dprintf(fd, "%s\n", meta) < 0) {
		r = -EIO;
		goto fail;
	}
	TAILQ_FOREACH(m, h, entries)
	{
		json_cleanup cJSON *obj = NULL;

		/* The system prologue is rebuilt from config on resume; it
		 * is kept only as a prompt record. */
		if (m->role == CLM_ROLE_SYSTEM) {
			autofree char *text = system_text(m, cz);

			s->prompt_hash = 0;
			r = text != NULL ? write_prompt(s, fd, text) : -ENOMEM;
			if (r < 0)
				goto fail;
			continue;
		}
		obj = clm_message_to_json_full(m, cz);

		if (obj == NULL ||
		    cJSON_AddStringToObject(obj, "type", "msg") == NULL) {
			r = -ENOMEM;
			goto fail;
		}
		r = store_attachments(s->path, m, obj);
		if (r < 0)
			goto fail;
		r = write_line(fd, obj);
		if (r < 0)
			goto fail;
		if (m->role == CLM_ROLE_USER || m->role == CLM_ROLE_ASSISTANT)
			msgs = true;
	}

	/* The rename is only atomic with respect to a crash if the contents
	 * are on disk first. */
	if (fsync(fd) != 0) {
		r = -errno;
		goto fail;
	}

	/* One cycle of backup, by hard link rather than rename: the log keeps
	 * its name throughout, so a crash here can never leave the session
	 * without its file. Best effort -- a filesystem that refuses the link
	 * should not cost the compaction. */
	{
		autofree char *bak = NULL;

		if (asprintf(&bak, "%s.bak", s->path) >= 0) {
			(void)unlink(bak);
			if (link(s->path, bak) != 0) {
				/* No backup this cycle. Not worth failing
				 * the compaction over. */
			}
		}
	}

	if (rename(tmp, s->path) != 0) {
		r = -errno;
		goto fail;
	}

	(void)close(s->fd);
	s->fd = fd;
	s->has_msgs = msgs;
	blob_gc(s->path);
	if (lseek(s->fd, 0, SEEK_END) < 0)
		return -errno;
	return 0;

fail:
	(void)close(fd);
	(void)unlink(tmp);
	return r;
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
	{
		autofree char *bak = NULL;
		autofree char *bdir = blob_dir_of(s->path);

		if (asprintf(&bak, "%s.bak", s->path) >= 0)
			(void)unlink(bak);
		if (bdir != NULL)
			blob_dir_remove(bdir);
	}
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
/*
 * Read the images a message record names from bdir. A missing one adds a
 * note to the record's content instead, so the model is told. Returns
 * the number read into imgs (at most max).
 */
static size_t
load_attachments(
    cJSON *obj, const char *bdir, struct clm_attachment *imgs, size_t max)
{
	const cJSON *e;
	size_t n = 0;

	cJSON_ArrayForEach(
	    e, cJSON_GetObjectItemCaseSensitive(obj, "attachments"))
	{
		const char *mt = cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(e, "media_type"));
		const char *sha = cJSON_GetStringValue(
		    cJSON_GetObjectItemCaseSensitive(e, "sha256"));
		autofree char *path = NULL;
		uint8_t *data = NULL;
		size_t len = 0;

		if (mt == NULL || !sha_valid(sha) || n >= max)
			continue;
		if (bdir != NULL &&
		    asprintf(&path, "%s/%s.%s", bdir, sha, blob_ext(mt)) >= 0)
			data = blob_read(path, &len);
		if (data == NULL) {
			const char *c = cJSON_GetStringValue(
			    cJSON_GetObjectItemCaseSensitive(obj, "content"));
			autofree char *note = NULL;

			if (asprintf(&note,
			        "%s\n[image missing from the session "
			        "log: %s]",
			        c != NULL ? c : "", mt) >= 0)
				cJSON_ReplaceItemInObjectCaseSensitive(
				    obj, "content", cJSON_CreateString(note));
			continue;
		}
		imgs[n].media_type = (char *)mt;
		imgs[n].data = data;
		imgs[n].len = len;
		n++;
	}
	return n;
}

int
session_parse_line(struct clm_history *hist, const char *line, size_t len,
    cJSON **out_meta, const char *blob_dir)
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
		struct clm_attachment imgs[16];
		struct clm_message *before = TAILQ_LAST(hist, clm_history);
		size_t n = load_attachments(obj, blob_dir, imgs, 16);
		int r = clm_message_from_json(hist, obj, NULL);
		struct clm_message *m = TAILQ_LAST(hist, clm_history);

		for (size_t i = 0; i < n; i++) {
			if (r == 0 && m != NULL && m != before &&
			    clm_message_add_attachment(m, imgs[i].media_type,
			        imgs[i].data, imgs[i].len) < 0)
				r = -ENOMEM;
			free(imgs[i].data);
		}
		/* A malformed message line is skipped like any other bad
		 * line; only allocation failure is fatal. */
		return r == -ENOMEM ? -ENOMEM : 0;
	}

	return 0; /* unknown type: forward compatibility */
}

int
clm_session_load(
    const char *dir, const char *id, struct clm_history *hist, cJSON **out_meta)
{
	autofree char *path = NULL;
	autofree char *bdir = NULL;
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

	bdir = blob_dir_of(path);
	while ((n = getline(&line, &cap, f)) >= 0) {
		r = session_parse_line(hist, line, (size_t)n, out_meta, bdir);
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

/* True if `name` ends with `suffix`. */
static bool
has_suffix(const char *name, const char *suffix)
{
	size_t n = strlen(name), sn = strlen(suffix);

	return n > sn && strcmp(name + n - sn, suffix) == 0;
}

int
clm_session_gc(const char *dir, unsigned max_age_days, size_t *removed)
{
	autofree char *d = NULL;
	autoclosedir DIR *dp = NULL;
	struct dirent *de;
	time_t now = time(NULL);
	size_t n = 0;
	int r;

	if (removed != NULL)
		*removed = 0;
	if (max_age_days == 0)
		return 0;

	r = resolve_dir(dir, &d);
	if (r < 0)
		return r;
	dp = opendir(d);
	if (dp == NULL)
		return errno == ENOENT ? 0 : -errno;

	while ((de = readdir(dp)) != NULL) {
		autofree char *path = NULL;
		struct stat st;
		double age_days;
		bool tmp = has_suffix(de->d_name, ".tmp");

		if (!tmp && !has_suffix(de->d_name, ".jsonl") &&
		    !has_suffix(de->d_name, ".bak"))
			continue;
		if (asprintf(&path, "%s/%s", d, de->d_name) < 0)
			return -ENOMEM;
		if (stat(path, &st) != 0)
			continue;
		age_days = difftime(now, st.st_mtime) / (60 * 60 * 24);
		/* A .tmp belongs to a rewrite that died: it is garbage as
		 * soon as it is a day old, whatever the retention is. */
		if (age_days < (tmp ? 1.0 : (double)max_age_days))
			continue;
		if (unlink(path) == 0)
			n++;
	}

	/* A blob directory goes with its log. */
	rewinddir(dp);
	while ((de = readdir(dp)) != NULL) {
		autofree char *bdir = NULL;
		autofree char *log = NULL;
		size_t nl = strlen(de->d_name);

		if (!has_suffix(de->d_name, ".blobs"))
			continue;
		if (asprintf(&bdir, "%s/%s", d, de->d_name) < 0 ||
		    asprintf(&log, "%s/%.*s.jsonl", d, (int)(nl - 6),
		        de->d_name) < 0)
			return -ENOMEM;
		if (access(log, F_OK) != 0)
			blob_dir_remove(bdir);
	}

	if (removed != NULL)
		*removed = n;
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
