// SPDX-License-Identifier: ISC
/*
 * Peer socket delivery: a sender learns whether the recipient took the
 * message. A refusal must come back as an answer, not as silence -- a
 * dropped message the sender believes it delivered is the bug this guards.
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
#include "clm/peer.h"
#include "tap.h"

#define CHECK(cond, msg) TAP_CHECK(cond, msg)

#define BURST 25

static int delivered;
static char last_text[256];

static void
on_peer_msg(const char *from, const char *name, const char *text, void *user)
{
	(void)from;
	(void)name;
	(void)user;
	delivered++;
	(void)snprintf(last_text, sizeof(last_text), "%s", text ? text : "");
}

/*
 * Deliver one line and return the recipient's answer. The recipient is this
 * same process, so the loop is stepped while waiting for it.
 */
static int
send_and_ack(
    uv_loop_t *loop, const char *path, const char *line, char *ack, size_t len)
{
	struct sockaddr_un sa;
	struct pollfd pfd;
	ssize_t n;
	int fd, i;

	ack[0] = '\0';
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	(void)snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		(void)close(fd);
		return -errno;
	}
	if (write(fd, line, strlen(line)) < 0) {
		(void)close(fd);
		return -errno;
	}
	(void)shutdown(fd, SHUT_WR);

	for (i = 0; i < 500; i++) {
		uv_run(loop, UV_RUN_NOWAIT);
		pfd.fd = fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 5) > 0)
			break;
	}
	n = read(fd, ack, len - 1);
	(void)close(fd);
	if (n < 0)
		return -errno;
	ack[n] = '\0';
	return 0;
}

static char *
msg_line(const char *from, const char *text)
{
	char *line = NULL;

	if (asprintf(&line,
	        "{\"v\":1,\"from\":\"%s\",\"from_name\":\"t\",\"text\":\"%s\"}"
	        "\n",
	        from, text) < 0)
		return NULL;
	return line;
}

static int
test_peer(void *arg)
{
	uv_loop_t *loop = arg;
	char dir[] = "/tmp/clm-peer-test-XXXXXX";
	char sock[512], ack[256];
	struct clm_cfg cfg = {0};
	struct clm_host *host = NULL;
	struct clm_agent *agent = NULL;
	struct clm_peer *peer = NULL;
	char *line;
	int i, accepted = 0;

	CHECK(mkdtemp(dir) != NULL, "temp runtime dir");
	(void)setenv("XDG_RUNTIME_DIR", dir, 1);

	/* Nothing listens on port 1: a delivered message starts a turn that
	 * fails at once, which is all this test wants from the agent. */
	cfg.api_key = "test";
	cfg.base_url = "http://127.0.0.1:1/v1/chat/completions";
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = "test-model";
	CHECK(clm_host_uv_new(loop, &host) == 0, "clm_host_uv_new");
	CHECK(clm_agent_new(&cfg, host, NULL, NULL, &agent) == 0,
	    "clm_agent_new");
	CHECK(clm_peer_start(agent, loop, "testpeer", "t", "m", on_peer_msg,
	          NULL, &peer) == 0,
	    "clm_peer_start");
	(void)snprintf(sock, sizeof(sock), "%s/clm/testpeer.sock", dir);

	line = msg_line("sender", "hello");
	CHECK(line != NULL, "build message");
	CHECK(send_and_ack(loop, sock, line, ack, sizeof(ack)) == 0,
	    "send one message");
	CHECK(strstr(ack, "\"ok\":true") != NULL, "accepted message is acked");
	CHECK(delivered == 1, "message reached the agent");
	CHECK(strcmp(last_text, "hello") == 0, "text arrived intact");
	free(line);

	CHECK(send_and_ack(loop, sock, "{not json}\n", ack, sizeof(ack)) == 0,
	    "send garbage");
	CHECK(strstr(ack, "\"ok\":false") != NULL, "garbage is refused");
	CHECK(strstr(ack, "malformed") != NULL, "refusal says why");

	/* A burst is delivered in full: nothing throttles or drops it. */
	for (i = 0; i < BURST; i++) {
		line = msg_line("sender", "flood");
		if (line == NULL)
			break;
		if (send_and_ack(loop, sock, line, ack, sizeof(ack)) == 0 &&
		    strstr(ack, "\"ok\":true") != NULL)
			accepted++;
		free(line);
	}
	CHECK(accepted == BURST, "every message in a burst is accepted");
	CHECK(delivered == BURST + 1, "every message reached the agent");

	/* Let the turns the deliveries started fail and unwind: freeing the
	 * agent under an in-flight request leaks it. */
	for (i = 0; i < 2000; i++) {
		enum clm_agent_state st = clm_agent_get_state(agent);

		if (st != CLM_STATE_THINKING && st != CLM_STATE_CALLING_TOOL)
			break;
		uv_run(loop, UV_RUN_ONCE);
	}

	clm_peer_free(peer);
	clm_agent_free(agent);
	clm_host_uv_free(host);
	uv_run(loop, UV_RUN_DEFAULT);
	return 0;
}

int
main(void)
{
	uv_loop_t *loop = uv_default_loop();

	TAP_ADD("peer delivery is acknowledged", test_peer, loop);
	return tap_run();
}
