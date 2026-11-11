// SPDX-License-Identifier: ISC
/*
 * GLib polls the libuv backend fd (epoll or kqueue) and uses the libuv
 * timer deadline as its poll timeout. A wakeup with work runs the libuv
 * loop once without blocking.
 */
#include <stdbool.h>

#include <glib.h>
#include <uv.h>

#include "uvsource.h"
#include "banned.h"

struct uv_gsource {
	GSource base;
	uv_loop_t *loop;
	gpointer tag;
	bool watching;
};

/*
 * uv_run() does not poll an empty loop, so the backend fd can stay readable
 * and GLib would spin. Watch the fd only while the loop has work. A change
 * to the poll set wakes the context, so make it only when the state flips.
 */
static void
uvs_watch(struct uv_gsource *s, bool alive)
{
	if (alive == s->watching)
		return;
	g_source_modify_unix_fd(&s->base, s->tag, alive ? G_IO_IN : 0);
	s->watching = alive;
}

static gboolean
uvs_prepare(GSource *src, gint *ret_timeout)
{
	struct uv_gsource *s = (struct uv_gsource *)src;
	bool alive;

	uv_update_time(s->loop);
	alive = uv_loop_alive(s->loop) != 0;
	uvs_watch(s, alive);
	/* An empty loop reports a timeout of 0. */
	*ret_timeout = alive ? uv_backend_timeout(s->loop) : -1;
	return *ret_timeout == 0;
}

/*
 * Dispatch only when the fd is readable or a timer is due. GLib changes the
 * poll set around each dispatch, which wakes the context again, so a source
 * that always dispatches never lets the loop sleep.
 */
static gboolean
uvs_check(GSource *src)
{
	struct uv_gsource *s = (struct uv_gsource *)src;

	if (!s->watching)
		return FALSE;
	if (g_source_query_unix_fd(src, s->tag) != 0)
		return TRUE;
	uv_update_time(s->loop);
	return uv_backend_timeout(s->loop) == 0;
}

static gboolean
uvs_dispatch(GSource *src, GSourceFunc cb, gpointer user)
{
	struct uv_gsource *s = (struct uv_gsource *)src;

	(void)cb;
	(void)user;
	(void)uv_run(s->loop, UV_RUN_NOWAIT);
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs uvs_funcs = {
    .prepare = uvs_prepare,
    .check = uvs_check,
    .dispatch = uvs_dispatch,
};

guint
clm_uv_source_attach(uv_loop_t *loop)
{
	GSource *src;
	struct uv_gsource *s;
	guint id;

	g_return_val_if_fail(loop != NULL, 0);
	src = g_source_new(&uvs_funcs, sizeof(struct uv_gsource));
	s = (struct uv_gsource *)src;
	s->loop = loop;
	s->tag = g_source_add_unix_fd(src, uv_backend_fd(loop), G_IO_IN);
	s->watching = true;
	id = g_source_attach(src, NULL);
	g_source_unref(src);
	return id;
}
