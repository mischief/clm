/* SPDX-License-Identifier: ISC */
/* Chat transcript rows: an avatar, a name and time line, and a body. */
#ifndef CLM_GTK_CHAT_H
#define CLM_GTK_CHAT_H

#include <gtk/gtk.h>

enum chat_kind {
	CHAT_USER,
	CHAT_MODEL,
	CHAT_TOOL,
	CHAT_NOTICE,
};

struct chat_row;

/* Append a row to list. who names the sender; text may be NULL. stamp
 * shows the current time beside the name. The list owns the row. */
struct chat_row *chat_row_add(GtkListBox *list, enum chat_kind kind,
    const char *who, const char *text, bool stamp);

/* Append streamed text to the body, or to the dim reasoning line. */
void chat_row_append(struct chat_row *cr, const char *text);
void chat_row_append_reasoning(struct chat_row *cr, const char *text);

/* Set the dim line under a tool row's arguments to its result. */
void chat_row_set_result(struct chat_row *cr, const char *text, bool failed);

/* Remove every row. */
void chat_clear(GtkListBox *list);

#endif /* CLM_GTK_CHAT_H */
