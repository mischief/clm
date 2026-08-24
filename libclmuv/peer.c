// SPDX-License-Identifier: ISC
/*
 * Agent-to-agent messaging over a unix socket, one per running clm.
 * Discovery is a readdir of <runtime>/clm: each instance binds
 * <session-id>.sock and writes a sibling .json describing itself. A message
 * is one JSON line, delivered through clm_agent_notify() so it lands between
 * turns. The two tool schemas never mention peers, so the cached prompt
 * prefix does not move as agents come and go.
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/peer.h"
#include "clm/tools.h"
#include "banned.h"

/* Longest message a peer may deliver, and the most it may deliver per
 * minute. Two agents that answer each other otherwise bill forever. */
#define PEER_MSG_MAX 8192
#define PEER_RATE_PER_MIN 6
#define PEER_HOPS_MAX 3
#define PEER_SEND_TIMEOUT_MS 250

struct peer_sender {
	char id[64];
	time_t window;
	int count;
};

struct clm_peer {
	struct clm_agent *agent;
	uv_loop_t *loop;
	uv_pipe_t server;
	bool listening;
	char dir[256];
	char sock_path[320];
	char meta_path[320];
	char id[64];
	char name[64];
	clm_peer_msg_cb cb;
	void *cb_user;
	struct peer_sender senders[16];
};

/* The one live instance, so the tool callbacks can reach it: tools carry a
 * user pointer, but the invocation hands it back per call, and both tools
 * need the same state. */
static struct clm_peer *the_peer;

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

/*
 * Where sockets live: $XDG_RUNTIME_DIR/clm, or /tmp/clm-<uid> where that is
 * unset (every BSD, and any Linux without a session manager). Created 0700
 * and refused if it already exists owned by anyone else -- a shared /tmp is
 * exactly where that matters.
 */
static int
peer_dir(char *buf, size_t len)
{
	const char *run = getenv("XDG_RUNTIME_DIR");
	struct stat st;

	if (run != NULL && run[0] != '\0')
		(void)snprintf(buf, len, "%s/clm", run);
	else
		(void)snprintf(buf, len, "/tmp/clm-%ld", (long)getuid());

	if (mkdir(buf, 0700) != 0 && errno != EEXIST)
		return -errno;
	if (lstat(buf, &st) != 0)
		return -errno;
	if (!S_ISDIR(st.st_mode) || st.st_uid != getuid())
		return -EACCES;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Receiving                                                           */
/* ------------------------------------------------------------------ */

/* True if `from` is within its message allowance; counts the message. */
static bool
rate_ok(struct clm_peer *p, const char *from)
{
	time_t now = time(NULL);
	size_t i, oldest = 0;

	for (i = 0; i < sizeof(p->senders) / sizeof(p->senders[0]); i++) {
		struct peer_sender *s = &p->senders[i];

		if (strcmp(s->id, from) != 0) {
			if (s->window < p->senders[oldest].window)
				oldest = i;
			continue;
		}
		if (now - s->window >= 60) {
			s->window = now;
			s->count = 0;
		}
		if (s->count >= PEER_RATE_PER_MIN)
			return false;
		s->count++;
		return true;
	}

	(void)snprintf(
	    p->senders[oldest].id, sizeof(p->senders[oldest].id), "%s", from);
	p->senders[oldest].window = now;
	p->senders[oldest].count = 1;
	return true;
}

/*
 * Hand a delivered message to the agent. The text is framed so the model can
 * see it came from another agent rather than from the user, and carries the
 * sender's id so a reply can be addressed.
 */
static void
deliver(struct clm_peer *p, const char *from, const char *name,
    const char *text, int hops)
{
	autofree char *framed = NULL;

	if (asprintf(&framed,
	        "[message from agent %s (%s), hop %d]\n%s\n"
	        "(Reply with agent_send to \"%s\" if a reply is warranted; "
	        "this is another agent, not the user.)",
	        from, name != NULL ? name : "?", hops, text, from) < 0)
		return;
	if (p->cb != NULL)
		p->cb(from, name, text, p->cb_user);
	(void)clm_agent_notify(p->agent, framed);
}

struct peer_conn {
	uv_pipe_t pipe;
	struct clm_peer *peer;
	char buf[PEER_MSG_MAX + 512];
	size_t len;
};

static void
conn_closed(uv_handle_t *h)
{
	free(h->data);
}

static void
conn_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
	struct peer_conn *c = h->data;
	size_t room = sizeof(c->buf) - c->len;

	(void)suggested;
	buf->base = c->buf + c->len;
	buf->len = room;
}

/* Parse one delivered line and hand it to the agent. */
static void
conn_handle(struct peer_conn *c)
{
	json_cleanup cJSON *msg = NULL;
	struct clm_peer *p = c->peer;
	cJSON *from, *name, *text, *hops;
	int hop;

	c->buf[c->len] = '\0';
	msg = cJSON_Parse(c->buf);
	if (msg == NULL || !cJSON_IsObject(msg))
		return;
	from = cJSON_GetObjectItemCaseSensitive(msg, "from");
	name = cJSON_GetObjectItemCaseSensitive(msg, "from_name");
	text = cJSON_GetObjectItemCaseSensitive(msg, "text");
	hops = cJSON_GetObjectItemCaseSensitive(msg, "hops");
	if (!cJSON_IsString(from) || !cJSON_IsString(text))
		return;

	hop = cJSON_IsNumber(hops) ? (int)hops->valuedouble : 1;
	if (hop > PEER_HOPS_MAX)
		return;
	if (!rate_ok(p, from->valuestring))
		return;
	deliver(p, from->valuestring,
	    cJSON_IsString(name) ? name->valuestring : NULL, text->valuestring,
	    hop);
}

static void
conn_read(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf)
{
	struct peer_conn *c = s->data;

	(void)buf;
	if (nread > 0) {
		c->len += (size_t)nread;
		if (c->len < sizeof(c->buf) - 1)
			return; /* wait for EOF, the sender closes */
	}
	if (c->len > 0)
		conn_handle(c);
	if (!uv_is_closing((uv_handle_t *)s))
		uv_close((uv_handle_t *)s, conn_closed);
}

static void
on_connection(uv_stream_t *server, int status)
{
	struct clm_peer *p = server->data;
	struct peer_conn *c;

	if (status != 0)
		return;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return;
	c->peer = p;
	if (uv_pipe_init(p->loop, &c->pipe, 0) != 0) {
		free(c);
		return;
	}
	c->pipe.data = c;
	if (uv_accept(server, (uv_stream_t *)&c->pipe) != 0) {
		uv_close((uv_handle_t *)&c->pipe, conn_closed);
		return;
	}
	(void)uv_read_start((uv_stream_t *)&c->pipe, conn_alloc, conn_read);
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

/*
 * Write one line to a peer's socket. Plain sockets with a short timeout
 * rather than the loop: a send is a local write that either lands at once or
 * is answered by a peer that is wedged, and neither case should be allowed
 * to park the event loop.
 */
static int
send_line(const char *path, const char *line)
{
	struct sockaddr_un sa;
	struct pollfd pfd;
	size_t left = strlen(line);
	const char *p = line;
	int fd, r;

	if (strlen(path) >= sizeof(sa.sun_path))
		return -ENAMETOOLONG;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	(void)snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		if (errno != EINPROGRESS) {
			r = -errno;
			(void)close(fd);
			return r;
		}
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, PEER_SEND_TIMEOUT_MS) <= 0) {
			(void)close(fd);
			return -ETIMEDOUT;
		}
	}

	while (left > 0) {
		ssize_t w = write(fd, p, left);

		if (w > 0) {
			p += w;
			left -= (size_t)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			pfd.fd = fd;
			pfd.events = POLLOUT;
			if (poll(&pfd, 1, PEER_SEND_TIMEOUT_MS) <= 0)
				break;
			continue;
		}
		break;
	}
	(void)close(fd);
	return left == 0 ? 0 : -EIO;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

/* Read a peer's sibling metadata file, or NULL. */
static cJSON *
read_meta(const char *dir, const char *id)
{
	char path[400];
	char buf[1024];
	FILE *f;
	size_t n;

	(void)snprintf(path, sizeof(path), "%s/%s.json", dir, id);
	f = fopen(path, "r");
	if (f == NULL)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	(void)fclose(f);
	buf[n] = '\0';
	return cJSON_Parse(buf);
}

/* True if something is listening; unlinks the socket and metadata if not. */
static bool
peer_alive(const char *dir, const char *id)
{
	char sock[400], meta[400];
	struct sockaddr_un sa;
	int fd;
	bool ok;

	(void)snprintf(sock, sizeof(sock), "%s/%s.sock", dir, id);
	if (strlen(sock) >= sizeof(sa.sun_path))
		return false;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	(void)snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock);
	ok = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
	(void)close(fd);
	if (!ok) {
		(void)unlink(sock);
		(void)snprintf(meta, sizeof(meta), "%s/%s.json", dir, id);
		(void)unlink(meta);
	}
	return ok;
}

/*
 * Every live peer as a JSON array. Also reaps sockets left by an instance
 * that died, since a failed connect is the only way to tell.
 */
static cJSON *
list_peers(struct clm_peer *p, bool include_self)
{
	cJSON *arr = cJSON_CreateArray();
	DIR *d;
	struct dirent *de;

	if (arr == NULL)
		return NULL;
	d = opendir(p->dir);
	if (d == NULL)
		return arr;
	while ((de = readdir(d)) != NULL) {
		char id[64];
		size_t n = strlen(de->d_name);
		cJSON *meta, *entry;

		if (n <= 5 || strcmp(de->d_name + n - 5, ".sock") != 0)
			continue;
		if (n - 5 >= sizeof(id))
			continue;
		memcpy(id, de->d_name, n - 5);
		id[n - 5] = '\0';
		if (!include_self && strcmp(id, p->id) == 0)
			continue;
		if (!peer_alive(p->dir, id))
			continue;

		entry = cJSON_CreateObject();
		if (entry == NULL)
			break;
		cJSON_AddStringToObject(entry, "id", id);
		cJSON_AddStringToObject(
		    entry, "short", strlen(id) > 8 ? id + strlen(id) - 8 : id);
		meta = read_meta(p->dir, id);
		if (meta != NULL) {
			cJSON *k;
			static const char *keys[] = {
			    "name", "model", "cwd", "started"};
			size_t i;

			for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
				k = cJSON_GetObjectItemCaseSensitive(
				    meta, keys[i]);
				if (k != NULL)
					cJSON_AddItemToObject(entry, keys[i],
					    cJSON_Duplicate(k, 1));
			}
			cJSON_Delete(meta);
		}
		cJSON_AddItemToArray(arr, entry);
	}
	(void)closedir(d);
	return arr;
}

/* Resolve a caller-supplied target to a full session id. */
static char *
resolve_target(struct clm_peer *p, const char *want)
{
	json_cleanup cJSON *peers = list_peers(p, false);
	cJSON *e;
	char *hit = NULL;
	int hits = 0;

	if (peers == NULL || want == NULL)
		return NULL;
	cJSON_ArrayForEach(e, peers)
	{
		cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
		cJSON *name = cJSON_GetObjectItemCaseSensitive(e, "name");
		bool match =
		    cJSON_IsString(id) && strstr(id->valuestring, want) != NULL;

		if (!match && cJSON_IsString(name))
			match = strcmp(name->valuestring, want) == 0;
		if (!match)
			continue;
		hits++;
		if (hits == 1)
			hit = strdup(id->valuestring);
	}
	if (hits != 1) {
		free(hit);
		return NULL;
	}
	return hit;
}

/* ------------------------------------------------------------------ */
/* Tools                                                               */
/* ------------------------------------------------------------------ */

static void
tool_agents_list(struct clm_tool_invocation *inv, void *user)
{
	json_cleanup cJSON *peers = NULL;
	autofree char *out = NULL;

	(void)user;
	if (the_peer == NULL) {
		clm_tool_fail(inv, "peer messaging is not running");
		return;
	}
	peers = list_peers(the_peer, false);
	if (peers == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	if (cJSON_GetArraySize(peers) == 0) {
		clm_tool_complete(inv, "no other agents are running");
		return;
	}
	out = cJSON_PrintUnformatted(peers);
	clm_tool_complete(inv, out != NULL ? out : "[]");
}

static void
tool_agent_send(struct clm_tool_invocation *inv, void *user)
{
	const char *args = clm_tool_invocation_args(inv);
	json_cleanup cJSON *in = NULL;
	json_cleanup cJSON *msg = NULL;
	autofree char *target = NULL;
	autofree char *line = NULL;
	autofree char *body = NULL;
	cJSON *to, *text;
	char path[400];
	char note[128];
	int r;

	(void)user;
	if (the_peer == NULL) {
		clm_tool_fail(inv, "peer messaging is not running");
		return;
	}
	in = args != NULL ? cJSON_Parse(args) : NULL;
	to = in ? cJSON_GetObjectItemCaseSensitive(in, "to") : NULL;
	text = in ? cJSON_GetObjectItemCaseSensitive(in, "text") : NULL;
	if (!cJSON_IsString(to) || !cJSON_IsString(text)) {
		clm_tool_fail(inv, "need 'to' and 'text'");
		return;
	}
	if (strlen(text->valuestring) > PEER_MSG_MAX) {
		clm_tool_fail(inv, "message too long");
		return;
	}

	target = resolve_target(the_peer, to->valuestring);
	if (target == NULL) {
		clm_tool_fail(
		    inv, "no single agent matches that id; call agents_list");
		return;
	}

	msg = cJSON_CreateObject();
	if (msg == NULL) {
		clm_tool_fail(inv, "out of memory");
		return;
	}
	cJSON_AddNumberToObject(msg, "v", 1);
	cJSON_AddStringToObject(msg, "from", the_peer->id);
	cJSON_AddStringToObject(msg, "from_name", the_peer->name);
	cJSON_AddStringToObject(msg, "text", text->valuestring);
	cJSON_AddNumberToObject(msg, "hops", 1);
	body = cJSON_PrintUnformatted(msg);
	if (body == NULL || asprintf(&line, "%s\n", body) < 0) {
		clm_tool_fail(inv, "out of memory");
		return;
	}

	(void)snprintf(path, sizeof(path), "%s/%s.sock", the_peer->dir, target);
	r = send_line(path, line);
	if (r < 0) {
		(void)snprintf(
		    note, sizeof(note), "delivery failed: %s", strerror(-r));
		clm_tool_fail(inv, note);
		return;
	}
	(void)snprintf(note, sizeof(note), "delivered to %s", target);
	clm_tool_complete(inv, note);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void
write_meta(struct clm_peer *p, const char *model)
{
	json_cleanup cJSON *meta = cJSON_CreateObject();
	autofree char *text = NULL;
	char cwd[256];
	FILE *f;

	if (meta == NULL)
		return;
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		(void)snprintf(cwd, sizeof(cwd), "?");
	cJSON_AddStringToObject(meta, "id", p->id);
	cJSON_AddStringToObject(meta, "name", p->name);
	cJSON_AddStringToObject(meta, "model", model != NULL ? model : "?");
	cJSON_AddStringToObject(meta, "cwd", cwd);
	cJSON_AddNumberToObject(meta, "pid", (double)getpid());
	cJSON_AddNumberToObject(meta, "started", (double)time(NULL));
	text = cJSON_PrintUnformatted(meta);
	if (text == NULL)
		return;
	f = fopen(p->meta_path, "w");
	if (f == NULL)
		return;
	(void)fprintf(f, "%s\n", text);
	(void)fclose(f);
	(void)chmod(p->meta_path, 0600);
}

int
clm_peer_start(struct clm_agent *agent, uv_loop_t *loop, const char *id,
    const char *name, const char *model, clm_peer_msg_cb cb, void *user,
    struct clm_peer **out)
{
	struct clm_peer *p;
	int r;

	if (agent == NULL || loop == NULL || id == NULL || out == NULL)
		return -EINVAL;
	*out = NULL;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return -ENOMEM;
	p->agent = agent;
	p->loop = loop;
	p->cb = cb;
	p->cb_user = user;
	(void)snprintf(p->id, sizeof(p->id), "%s", id);
	(void)snprintf(
	    p->name, sizeof(p->name), "%s", name != NULL ? name : "clm");

	r = peer_dir(p->dir, sizeof(p->dir));
	if (r < 0) {
		free(p);
		return r;
	}
	(void)snprintf(
	    p->sock_path, sizeof(p->sock_path), "%s/%s.sock", p->dir, id);
	(void)snprintf(
	    p->meta_path, sizeof(p->meta_path), "%s/%s.json", p->dir, id);

	/* A socket left by a previous run of this same session id is dead by
	 * definition -- this process is the session now. */
	(void)unlink(p->sock_path);

	if (uv_pipe_init(loop, &p->server, 0) != 0) {
		free(p);
		return -EIO;
	}
	p->server.data = p;
	r = uv_pipe_bind(&p->server, p->sock_path);
	if (r != 0) {
		uv_close((uv_handle_t *)&p->server, NULL);
		free(p);
		return -EADDRINUSE;
	}
	r = uv_listen((uv_stream_t *)&p->server, 8, on_connection);
	if (r != 0) {
		uv_close((uv_handle_t *)&p->server, NULL);
		(void)unlink(p->sock_path);
		free(p);
		return -EIO;
	}
	p->listening = true;
	(void)chmod(p->sock_path, 0600);
	write_meta(p, model);

	the_peer = p;
	*out = p;
	return 0;
}

int
clm_peer_register_tools(struct clm_agent *agent)
{
	const struct clm_tool_def list_def = {
	    .name = "agents_list",
	    .description = "list the other clm agents running for this user, "
	                   "with their session id, name, model, and directory",
	    .params_schema = "{\"type\":\"object\",\"properties\":{}}",
	    .invoke = tool_agents_list,
	    .flags = CLM_TOOL_NO_PROMPT, /* read-only */
	};
	const struct clm_tool_def send_def = {
	    .name = "agent_send",
	    .description = "send a text message to another clm agent; it "
	                   "arrives as a message between that agent's turns, "
	                   "and cannot make it run anything",
	    .params_schema = "{\"type\":\"object\","
	                     "\"properties\":{"
	                     "\"to\":{\"type\":\"string\",\"description\":"
	                     "\"session id (or any unambiguous part of one) "
	                     "from agents_list\"},"
	                     "\"text\":{\"type\":\"string\",\"description\":"
	                     "\"what to say\"}},"
	                     "\"required\":[\"to\",\"text\"]}",
	    .invoke = tool_agent_send,
	};
	int r = clm_tool_add(agent, &list_def);

	if (r < 0)
		return r;
	return clm_tool_add(agent, &send_def);
}

void
clm_peer_free(struct clm_peer *p)
{
	if (p == NULL)
		return;
	if (p->listening && !uv_is_closing((uv_handle_t *)&p->server))
		uv_close((uv_handle_t *)&p->server, NULL);
	(void)unlink(p->sock_path);
	(void)unlink(p->meta_path);
	if (the_peer == p)
		the_peer = NULL;
	free(p);
}
