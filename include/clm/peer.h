// SPDX-License-Identifier: ISC

/*
 * Agent-to-agent messaging over a unix socket, one per running clm. Needs
 * libuv, so it lives in the desktop layer beside shell_exec, not in the
 * portable core. See libclmuv/peer.c.
 */
#ifndef CLM_PEER_H
#define CLM_PEER_H

#include "clm/clm_export.h"

struct clm_agent;
struct clm_peer;
struct uv_loop_s;

/*
 * Bind this agent's socket so other clm instances of the same user can find
 * and message it. `id` names the socket (the session id); `name` and `model`
 * are recorded for listings. A NULL id is client only: this agent can find
 * and message others without announcing itself, which is what a run too
 * short to be worth answering wants. Returns 0 or a negative errno.
 */
typedef void (*clm_peer_msg_cb)(
    const char *from, const char *name, const char *text, void *user);

CLM_API int clm_peer_start(struct clm_agent *agent, struct uv_loop_s *loop,
    const char *id, const char *name, const char *model, clm_peer_msg_cb cb,
    void *user, struct clm_peer **out);

/*
 * Register agents_list and agent_send. Their schemas do not vary with the
 * peers present, so the prompt prefix a provider caches stays put.
 */
CLM_API int clm_peer_register_tools(struct clm_agent *agent);

/* Stop listening and remove the socket and its metadata. */
CLM_API void clm_peer_free(struct clm_peer *p);

#endif /* CLM_PEER_H */
