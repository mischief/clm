// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <string.h>

#include <gtk/gtk.h>
#include <md4c.h>

#include "mdview.h"
#include "banned.h"

#define BLOCK_GAP 10
#define ITEM_GAP 3
/* A table cell wraps past this many characters. */
#define CELL_MAX_CHARS 48

static const char md_css[] =
    ".md-code { background-color: alpha(currentColor, 0.08);"
    " border-radius: 6px; padding: 8px 10px; }"
    ".md-quote { border-left: 3px solid alpha(currentColor, 0.3);"
    " padding-left: 10px; }"
    ".md-table { border: 1px solid alpha(currentColor, 0.25);"
    " border-radius: 6px; }"
    ".md-cell { padding: 4px 10px; }"
    ".md-row { border-top: 1px solid alpha(currentColor, 0.15); }"
    ".md-col { border-left: 1px solid alpha(currentColor, 0.15); }"
    ".md-th { font-weight: bold;"
    " background-color: alpha(currentColor, 0.06); }";

struct md_list {
	bool ordered;
	unsigned next; /* number of the next item */
};

struct mdc {
	GPtrArray *boxes; /* open containers, innermost last */
	GArray *lists;    /* open lists, innermost last */
	GString *inl;     /* inline text of the open leaf block */
	GtkGrid *grid;    /* the open table */
	int row, col;
	bool head;
	bool code; /* inl is the raw text of a code block */
};

static void
css_once(void)
{
	static bool done;
	GtkCssProvider *p;
	GdkDisplay *d = gdk_display_get_default();

	if (done || d == NULL)
		return;
	done = true;
	p = gtk_css_provider_new();
	gtk_css_provider_load_from_string(p, md_css);
	gtk_style_context_add_provider_for_display(
	    d, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static GtkBox *
cur(struct mdc *c)
{
	return g_ptr_array_index(c->boxes, c->boxes->len - 1);
}

static GtkWidget *
push_box(struct mdc *c, int gap)
{
	GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, gap);

	g_ptr_array_add(c->boxes, b);
	return b;
}

static void
pop_box(struct mdc *c)
{
	if (c->boxes->len > 1)
		g_ptr_array_set_size(c->boxes, (gint)c->boxes->len - 1);
}

static GtkWidget *
text_label(const char *s, bool markup)
{
	GtkWidget *l = gtk_label_new(NULL);

	if (markup)
		gtk_label_set_markup(GTK_LABEL(l), s);
	else
		gtk_label_set_text(GTK_LABEL(l), s);
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_focusable(l, FALSE);
	return l;
}

/* Close a paragraph, or the bare text of a tight list item. */
static void
flush_para(struct mdc *c)
{
	if (c->inl->len > 0)
		gtk_box_append(cur(c), text_label(c->inl->str, true));
	g_string_truncate(c->inl, 0);
}

static void
add_heading(struct mdc *c, unsigned level)
{
	static const char *const cls[] = {"title-2", "title-3", "title-4"};
	GtkWidget *l = text_label(c->inl->str, true);

	gtk_widget_add_css_class(l, level <= 3 ? cls[level - 1] : "heading");
	/* A heading sits closer to its own text than to what precedes it. */
	if (gtk_widget_get_first_child(GTK_WIDGET(cur(c))) != NULL)
		gtk_widget_set_margin_top(l, 4);
	gtk_box_append(cur(c), l);
	g_string_truncate(c->inl, 0);
}

static void
add_code(struct mdc *c)
{
	GtkWidget *l;

	while (c->inl->len > 0 && c->inl->str[c->inl->len - 1] == '\n')
		g_string_truncate(c->inl, c->inl->len - 1);
	l = text_label(c->inl->str, false);
	gtk_widget_add_css_class(l, "monospace");
	gtk_widget_add_css_class(l, "md-code");
	gtk_box_append(cur(c), l);
	g_string_truncate(c->inl, 0);
	c->code = false;
}

static void
add_cell(struct mdc *c, const MD_BLOCK_TD_DETAIL *d)
{
	GtkWidget *l = text_label(c->inl->str, true);
	float x = d->align == MD_ALIGN_RIGHT ? 1
	    : d->align == MD_ALIGN_CENTER    ? 0.5f
	                                     : 0;

	g_string_truncate(c->inl, 0);
	if (c->grid == NULL) {
		g_object_ref_sink(l);
		g_object_unref(l);
		return;
	}
	gtk_label_set_xalign(GTK_LABEL(l), x);
	gtk_label_set_yalign(GTK_LABEL(l), 0);
	/*
	 * A cell has one width: its text's, or CELL_MAX_CHARS when longer.
	 * A cell free to shrink makes the table measure absurdly tall.
	 */
	if (g_utf8_strlen(gtk_label_get_text(GTK_LABEL(l)), -1) >
	    CELL_MAX_CHARS) {
		gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD);
		gtk_label_set_width_chars(GTK_LABEL(l), CELL_MAX_CHARS);
		gtk_label_set_max_width_chars(GTK_LABEL(l), CELL_MAX_CHARS);
	} else {
		gtk_label_set_wrap(GTK_LABEL(l), FALSE);
	}
	gtk_widget_add_css_class(l, "md-cell");
	if (c->head)
		gtk_widget_add_css_class(l, "md-th");
	if (c->row > 0)
		gtk_widget_add_css_class(l, "md-row");
	if (c->col > 0)
		gtk_widget_add_css_class(l, "md-col");
	gtk_grid_attach(c->grid, l, c->col, c->row, 1, 1);
	c->col++;
}

static void
enter_item(struct mdc *c, const MD_BLOCK_LI_DETAIL *d)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *mark, *body;
	struct md_list *ls = c->lists->len > 0
	    ? &g_array_index(c->lists, struct md_list, c->lists->len - 1)
	    : NULL;
	g_autofree char *num = NULL;
	const char *text = "\u2022";

	if (d->is_task) {
		text = d->task_mark == ' ' ? "\u2610" : "\u2611";
	} else if (ls != NULL && ls->ordered) {
		num = g_strdup_printf("%u.", ls->next++);
		text = num;
	}
	mark = gtk_label_new(text);
	gtk_widget_set_valign(mark, GTK_ALIGN_START);
	gtk_widget_add_css_class(mark, "dim-label");
	gtk_widget_set_margin_start(row, 6);
	gtk_box_append(GTK_BOX(row), mark);
	gtk_box_append(cur(c), row);
	body = push_box(c, ITEM_GAP);
	gtk_widget_set_hexpand(body, TRUE);
	gtk_box_append(GTK_BOX(row), body);
}

/* A table keeps its natural width and scrolls sideways when too wide. */
static GtkWidget *
table_scroller(GtkWidget *grid)
{
	GtkWidget *sw = gtk_scrolled_window_new();

	gtk_scrolled_window_set_policy(
	    GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_propagate_natural_height(
	    GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
	return sw;
}

static int
enter_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	struct mdc *c = user;
	struct md_list ls = {0};
	GtkBox *parent;
	GtkWidget *w;

	flush_para(c);
	parent = cur(c);
	switch (type) {
	case MD_BLOCK_QUOTE:
		w = push_box(c, BLOCK_GAP);
		gtk_widget_add_css_class(w, "md-quote");
		gtk_box_append(parent, w);
		break;
	case MD_BLOCK_OL:
		ls.ordered = true;
		ls.next = ((MD_BLOCK_OL_DETAIL *)detail)->start;
		G_GNUC_FALLTHROUGH;
	case MD_BLOCK_UL:
		g_array_append_val(c->lists, ls);
		gtk_box_append(parent, push_box(c, ITEM_GAP));
		break;
	case MD_BLOCK_LI:
		enter_item(c, detail);
		break;
	case MD_BLOCK_HR:
		gtk_box_append(
		    parent, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
		break;
	case MD_BLOCK_CODE:
		c->code = true;
		break;
	case MD_BLOCK_TABLE:
		w = gtk_grid_new();
		gtk_widget_add_css_class(w, "md-table");
		gtk_widget_set_halign(w, GTK_ALIGN_START);
		gtk_widget_set_overflow(w, GTK_OVERFLOW_HIDDEN);
		gtk_box_append(parent, table_scroller(w));
		c->grid = GTK_GRID(w);
		c->row = 0;
		break;
	case MD_BLOCK_THEAD:
		c->head = true;
		break;
	case MD_BLOCK_TBODY:
		c->head = false;
		break;
	case MD_BLOCK_TR:
		c->col = 0;
		break;
	default:
		break;
	}
	return 0;
}

static int
leave_block(MD_BLOCKTYPE type, void *detail, void *user)
{
	struct mdc *c = user;

	switch (type) {
	case MD_BLOCK_H:
		add_heading(c, ((MD_BLOCK_H_DETAIL *)detail)->level);
		break;
	case MD_BLOCK_CODE:
		add_code(c);
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD:
		add_cell(c, detail);
		break;
	case MD_BLOCK_TR:
		c->row++;
		break;
	case MD_BLOCK_TABLE:
		c->grid = NULL;
		c->head = false;
		break;
	case MD_BLOCK_OL:
	case MD_BLOCK_UL:
		if (c->lists->len > 0)
			g_array_set_size(c->lists, c->lists->len - 1);
		G_GNUC_FALLTHROUGH;
	case MD_BLOCK_QUOTE:
	case MD_BLOCK_LI:
		flush_para(c);
		pop_box(c);
		break;
	default:
		flush_para(c);
		break;
	}
	return 0;
}

static void
append_escaped(GString *out, const char *s, size_t n)
{
	g_autofree char *esc = g_markup_escape_text(s, (gssize)n);

	g_string_append(out, esc);
}

static void
append_attr(GString *out, const MD_ATTRIBUTE *a)
{
	append_escaped(out, a->text, a->size);
}

static int
span(struct mdc *c, MD_SPANTYPE type, void *detail, bool open)
{
	const char *tag = NULL;

	if (c->code)
		return 0;
	switch (type) {
	case MD_SPAN_EM:
		tag = "i";
		break;
	case MD_SPAN_STRONG:
		tag = "b";
		break;
	case MD_SPAN_CODE:
		tag = "tt";
		break;
	case MD_SPAN_DEL:
		tag = "s";
		break;
	case MD_SPAN_U:
		tag = "u";
		break;
	case MD_SPAN_A:
		if (!open) {
			tag = "a";
			break;
		}
		g_string_append(c->inl, "<a href=\"");
		append_attr(c->inl, &((MD_SPAN_A_DETAIL *)detail)->href);
		g_string_append(c->inl, "\">");
		return 0;
	default:
		return 0;
	}
	g_string_append_printf(c->inl, open ? "<%s>" : "</%s>", tag);
	return 0;
}

static int
enter_span(MD_SPANTYPE type, void *detail, void *user)
{
	return span(user, type, detail, true);
}

static int
leave_span(MD_SPANTYPE type, void *detail, void *user)
{
	return span(user, type, detail, false);
}

/* Pango markup knows the XML entities and numeric references only. */
static bool
entity_is_markup(const char *s, size_t n)
{
	static const char *const known[] = {
	    "&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};

	if (n > 2 && s[1] == '#')
		return true;
	for (size_t i = 0; i < G_N_ELEMENTS(known); i++) {
		if (n == strlen(known[i]) && memcmp(s, known[i], n) == 0)
			return true;
	}
	return false;
}

static int
text(MD_TEXTTYPE type, const MD_CHAR *s, MD_SIZE n, void *user)
{
	struct mdc *c = user;

	if (c->code) {
		g_string_append_len(c->inl, s, (gssize)n);
		return 0;
	}
	switch (type) {
	case MD_TEXT_NULLCHAR:
		g_string_append(c->inl, "\ufffd");
		break;
	case MD_TEXT_BR:
		g_string_append_c(c->inl, '\n');
		break;
	case MD_TEXT_SOFTBR:
		g_string_append_c(c->inl, ' ');
		break;
	case MD_TEXT_ENTITY:
		if (entity_is_markup(s, n))
			g_string_append_len(c->inl, s, (gssize)n);
		else
			append_escaped(c->inl, s, n);
		break;
	default:
		append_escaped(c->inl, s, n);
		break;
	}
	return 0;
}

GtkWidget *
md_view_new(void)
{
	css_once();
	return gtk_box_new(GTK_ORIENTATION_VERTICAL, BLOCK_GAP);
}

void
md_view_set(GtkWidget *view, const char *md, size_t len)
{
	struct mdc c = {0};
	MD_PARSER parser = {
	    .flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
	        MD_FLAG_TASKLISTS | MD_FLAG_NOHTML |
	        MD_FLAG_PERMISSIVEAUTOLINKS,
	    .enter_block = enter_block,
	    .leave_block = leave_block,
	    .enter_span = enter_span,
	    .leave_span = leave_span,
	    .text = text,
	};
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(view)) != NULL)
		gtk_box_remove(GTK_BOX(view), child);
	c.boxes = g_ptr_array_new();
	c.lists = g_array_new(FALSE, TRUE, sizeof(struct md_list));
	c.inl = g_string_new(NULL);
	g_ptr_array_add(c.boxes, view);
	if (md_parse(md, (MD_SIZE)len, &parser, &c) != 0) {
		g_string_truncate(c.inl, 0);
		g_string_append_len(c.inl, md, (gssize)len);
		gtk_box_append(GTK_BOX(view), text_label(c.inl->str, false));
	}
	g_ptr_array_free(c.boxes, TRUE);
	g_array_free(c.lists, TRUE);
	g_string_free(c.inl, TRUE);
}
