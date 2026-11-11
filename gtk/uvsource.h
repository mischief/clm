/* SPDX-License-Identifier: ISC */
/* A GSource that runs a libuv loop inside the GLib main loop. */
#ifndef CLM_GTK_UVSOURCE_H
#define CLM_GTK_UVSOURCE_H

#include <glib.h>
#include <uv.h>

/* Attach loop to the default GLib context. Returns the source id, or 0. */
guint clm_uv_source_attach(uv_loop_t *loop);

#endif /* CLM_GTK_UVSOURCE_H */
