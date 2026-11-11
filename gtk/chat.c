// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <string.h>

#include <adwaita.h>
#include <gtk/gtk.h>

#include "chat.h"
#include "mdview.h"
#include "banned.h"

/* A tool result shows this much; the full text stays in history. */
#define RESULT_MAX_LINES 6
#define RESULT_MAX_BYTES 600
/* Streamed markdown is laid out again at most this often. */
#define MD_REDRAW_MS 50
/* Set on a list whose rows show their reasoning. */
#define SHOW_THINK_KEY "show-thinking"

struct chat_row {
	GtkLabel *body;    /* plain rows */
	GtkWidget *mdbody; /* markdown rows */
	guint redraw;      /* pending markdown redraw, or 0 */
	GtkLabel *think;
	GtkLabel *result;
	GString *text;
	GString *think_text;
	GtkListBox *list;
};

static void
chat_row_free(gpointer p)
{
	struct chat_row *cr = p;

	if (cr->redraw != 0)
		g_source_remove(cr->redraw);
	g_string_free(cr->text, TRUE);
	g_string_free(cr->think_text, TRUE);
	g_free(cr);
}

static GtkLabel *
body_label(const char *css)
{
	GtkWidget *l = gtk_label_new(NULL);

	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_focusable(l, FALSE);
	if (css != NULL)
		gtk_widget_add_css_class(l, css);
	return GTK_LABEL(l);
}

/* Show cr->text in the body. Text that is not UTF-8 yet shows as is. */
static void
body_update(struct chat_row *cr)
{
	if (cr->mdbody == NULL) {
		gtk_label_set_text(cr->body, cr->text->str);
		return;
	}
	if (g_utf8_validate_len(cr->text->str, cr->text->len, NULL))
		md_view_set(cr->mdbody, cr->text->str, cr->text->len);
}

static gboolean
on_redraw(gpointer user)
{
	struct chat_row *cr = user;

	cr->redraw = 0;
	body_update(cr);
	return G_SOURCE_REMOVE;
}

/* The reasoning line shows when the list says so and there is any. */
static void
think_update(struct chat_row *cr)
{
	bool show =
	    g_object_get_data(G_OBJECT(cr->list), SHOW_THINK_KEY) != NULL;

	gtk_widget_set_visible(
	    GTK_WIDGET(cr->think), show && cr->think_text->len > 0);
}

static GtkWidget *
avatar_for(enum chat_kind kind, const char *who)
{
	GtkWidget *a = adw_avatar_new(36, who, TRUE);

	gtk_widget_set_valign(a, GTK_ALIGN_START);
	if (kind == CHAT_TOOL)
		adw_avatar_set_icon_name(
		    ADW_AVATAR(a), "utilities-terminal-symbolic");
	if (kind == CHAT_NOTICE)
		adw_avatar_set_icon_name(
		    ADW_AVATAR(a), "dialog-information-symbolic");
	if (kind == CHAT_TOOL || kind == CHAT_NOTICE)
		adw_avatar_set_show_initials(ADW_AVATAR(a), FALSE);
	return a;
}

/* The bold name and the dim time beside it. */
static GtkWidget *
header_line(const char *who, bool stamp)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *name = gtk_label_new(who);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autofree char *hm = stamp ? g_date_time_format(now, "%H:%M") : NULL;
	GtkWidget *when = gtk_label_new(hm);

	gtk_widget_add_css_class(name, "heading");
	gtk_widget_add_css_class(when, "dim-label");
	gtk_widget_add_css_class(when, "caption");
	gtk_widget_set_valign(when, GTK_ALIGN_BASELINE_CENTER);
	gtk_widget_set_valign(name, GTK_ALIGN_BASELINE_CENTER);
	gtk_box_append(GTK_BOX(box), name);
	gtk_box_append(GTK_BOX(box), when);
	return box;
}

static GtkWidget *
row_box(GtkWidget *avatar, GtkWidget *col)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);
	gtk_widget_set_hexpand(col, TRUE);
	gtk_box_append(GTK_BOX(box), avatar);
	gtk_box_append(GTK_BOX(box), col);
	return box;
}

struct chat_row *
chat_row_add(GtkListBox *list, enum chat_kind kind, const char *who,
    const char *text, bool stamp)
{
	struct chat_row *cr = g_new0(struct chat_row, 1);
	GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *row;
	PangoAttrList *italic;

	cr->text = g_string_new(text != NULL ? text : "");
	cr->think_text = g_string_new(NULL);
	cr->think = body_label("dim-label");
	italic = pango_attr_list_new();
	pango_attr_list_insert(
	    italic, pango_attr_style_new(PANGO_STYLE_ITALIC));
	gtk_label_set_attributes(cr->think, italic);
	pango_attr_list_unref(italic);
	gtk_widget_set_visible(GTK_WIDGET(cr->think), FALSE);
	gtk_widget_set_margin_bottom(GTK_WIDGET(cr->think), 6);
	cr->list = list;
	gtk_box_append(GTK_BOX(col), header_line(who, stamp));
	gtk_box_append(GTK_BOX(col), GTK_WIDGET(cr->think));
	if (kind == CHAT_MODEL) {
		cr->mdbody = md_view_new();
		gtk_box_append(GTK_BOX(col), cr->mdbody);
	} else {
		cr->body = body_label(kind == CHAT_TOOL ? "monospace" : NULL);
		gtk_box_append(GTK_BOX(col), GTK_WIDGET(cr->body));
	}
	body_update(cr);
	if (kind == CHAT_TOOL) {
		cr->result = body_label("monospace");
		gtk_widget_add_css_class(GTK_WIDGET(cr->result), "dim-label");
		gtk_widget_set_visible(GTK_WIDGET(cr->result), FALSE);
		gtk_box_append(GTK_BOX(col), GTK_WIDGET(cr->result));
	}

	row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_child(
	    GTK_LIST_BOX_ROW(row), row_box(avatar_for(kind, who), col));
	g_object_set_data_full(G_OBJECT(row), "chat-row", cr, chat_row_free);
	gtk_list_box_append(list, row);
	return cr;
}

void
chat_row_append(struct chat_row *cr, const char *text)
{
	g_string_append(cr->text, text);
	if (cr->mdbody == NULL)
		body_update(cr);
	else if (cr->redraw == 0)
		cr->redraw = g_timeout_add(MD_REDRAW_MS, on_redraw, cr);
}

void
chat_row_append_reasoning(struct chat_row *cr, const char *text)
{
	g_string_append(cr->think_text, text);
	gtk_label_set_text(cr->think, cr->think_text->str);
	think_update(cr);
}

/* The first few lines of s, at most RESULT_MAX_BYTES of them. */
static char *
result_head(const char *s)
{
	size_t n = 0;
	unsigned lines = 0;
	bool cut;

	while (s[n] != '\0' && n < RESULT_MAX_BYTES) {
		if (s[n] == '\n')
			lines++;
		if (lines == RESULT_MAX_LINES)
			break;
		n++;
	}
	cut = s[n] != '\0';
	/* Do not split a UTF-8 sequence. */
	while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	return g_strdup_printf("%.*s%s", (int)n, s, cut ? "\n..." : "");
}

void
chat_row_set_result(struct chat_row *cr, const char *text, bool failed)
{
	g_autofree char *head = NULL;

	if (cr->result == NULL || text == NULL)
		return;
	head = result_head(text);
	gtk_label_set_text(cr->result, head);
	if (failed)
		gtk_widget_add_css_class(GTK_WIDGET(cr->result), "error");
	gtk_widget_set_visible(GTK_WIDGET(cr->result), TRUE);
}

void
chat_show_thinking(GtkListBox *list, bool show)
{
	GtkWidget *row;

	g_object_set_data(G_OBJECT(list), SHOW_THINK_KEY, show ? list : NULL);
	for (row = gtk_widget_get_first_child(GTK_WIDGET(list)); row != NULL;
	    row = gtk_widget_get_next_sibling(row)) {
		struct chat_row *cr =
		    g_object_get_data(G_OBJECT(row), "chat-row");

		if (cr != NULL)
			think_update(cr);
	}
}

void
chat_clear(GtkListBox *list)
{
	gtk_list_box_remove_all(list);
}
