// SPDX-License-Identifier: ISC
/*
 * Peer, MCP and HTTP teardown all finish inside uv_close callbacks, so a
 * frontend that abandons the loop after the last *_free frees none of them.
 * Guard: a frontend's teardown leaves no handle on the loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
#include "clm/mcp.h"
#include "clm/peer.h"
#include "loop_drain.h"
#include "tap.h"

#define CHECK(cond, msg) TAP_CHECK(cond, msg)

static void
count_handle(uv_handle_t *h, void *arg)
{
	(void)h;
	(*(int *)arg)++;
}

static int
live_handles(uv_loop_t *loop)
{
	int n = 0;

	uv_walk(loop, count_handle, &n);
	return n;
}

static int
test_drain(void *arg)
{
	char dir[] = "/tmp/clm-drain-test-XXXXXX";
	struct clm_cfg cfg = {0};
	struct clm_host *host = NULL;
	struct clm_agent *agent = NULL;
	struct clm_peer *peer = NULL;
	struct clm_mcp_client *mcp = NULL;
	struct clm_mcp_server_cfg mcp_cfg = {0};
	char *const argv[] = {(char *)"/bin/cat", NULL};
	uv_loop_t loop;
	int i;

	(void)arg;
	CHECK(uv_loop_init(&loop) == 0, "loop init");
	CHECK(mkdtemp(dir) != NULL, "temp runtime dir");
	(void)setenv("XDG_RUNTIME_DIR", dir, 1);

	/* Nothing listens on port 1, so the health probe this starts fails
	 * fast; the point is the handles it leaves behind. */
	cfg.api_key = "test";
	cfg.base_url = "http://127.0.0.1:1/v1/chat/completions";
	cfg.provider = CLM_PROVIDER_OPENAI;
	cfg.model = "test-model";
	CHECK(clm_host_uv_new(&loop, &host) == 0, "clm_host_uv_new");
	CHECK(clm_agent_new(&cfg, host, NULL, NULL, &agent) == 0,
	    "clm_agent_new");
	CHECK(clm_peer_start(agent, &loop, "draintest", "t", "m", NULL, NULL,
	          &peer) == 0,
	    "clm_peer_start");

	/* cat speaks no MCP, so the handshake never completes -- what matters
	 * is the spawned child and its pipes. */
	mcp_cfg.name = "drain";
	mcp_cfg.transport = CLM_MCP_STDIO;
	mcp_cfg.argv = argv;
	CHECK(clm_mcp_connect(agent, &loop, &mcp_cfg, NULL, NULL, NULL,
	          &mcp) == 0,
	    "clm_mcp_connect");

	for (i = 0; i < 10; i++)
		uv_run(&loop, UV_RUN_NOWAIT);
	CHECK(live_handles(&loop) > 0, "the loop is carrying handles");

	clm_mcp_client_free(mcp);
	clm_peer_free(peer);
	clm_agent_free(agent);
	clm_host_uv_free(host);
	CHECK(clm_drain_loop(&loop) == 0, "the drain leaves no handle behind");
	(void)rmdir(dir);
	return 0;
}

int
main(void)
{
	TAP_ADD("teardown settles every uv handle", test_drain, NULL);
	return tap_run();
}
