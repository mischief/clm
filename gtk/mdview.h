/* SPDX-License-Identifier: ISC */
/* Markdown as GTK widgets: one widget per block, stacked in a box. */
#ifndef CLM_GTK_MDVIEW_H
#define CLM_GTK_MDVIEW_H

#include <stddef.h>

#include <gtk/gtk.h>

/* An empty view. */
GtkWidget *md_view_new(void);

/* Replace the view's blocks with [md, md+len) rendered. md is UTF-8. */
void md_view_set(GtkWidget *view, const char *md, size_t len);

#endif /* CLM_GTK_MDVIEW_H */
