// SPDX-License-Identifier: ISC
/* clm-gtk: a GTK4 chat window for a clm agent. */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/queue.h>

#include <adwaita.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/history.h"
#include "clm/host_uv.h"
#include "clm/lua_plugin.h"
#include "clm/session.h"
#include "cfg_tuning.h"
#include "loop_drain.h"
#include "model_spec.h"
#include "sysinfo.h"
#include "xdg.h"
#include "chat.h"
#include "uvsource.h"
#include "banned.h"

#define DEFAULT_URL "http://127.0.0.1:8081/v1"
#define SESSIONS_MAX 200
#define USER_NAME "Me"

struct app {
	GtkWindow *win;
	AdwWindowTitle *title;
	GtkListBox *sessions;
	GtkListBox *chat;
	GtkEntry *entry;
	GtkWidget *stop;
	GtkWidget *split;
	struct clm_lua_cfg *lcfg;
	struct clm_cfg cfg;
	char endpoint[256];
	char *sysinfo;
	char *spec_provider;
	char *model;
	GtkDropDown *models;
	guint model_idx;  /* the row showing the model in use */
	bool model_quiet; /* the selection is being set by code */
	uv_loop_t *loop;
	guint uv_src;
	struct clm_host *host;
	struct clm_agent *agent;
	struct clm_session *session;
	struct chat_row *cur;
	GPtrArray *tools; /* tool rows waiting for a result, oldest first */
	bool busy;
	bool stick;
	bool list_stale;
	bool sessions_quiet; /* the chat list selection is being set by code */
	const char *effort;  /* the header's choice; NULL takes config.lua's */
};

static void sessions_refresh(struct app *a);

static void
notice(struct app *a, const char *text)
{
	(void)chat_row_add(a->chat, CHAT_NOTICE, "clm", text, true);
	a->cur = NULL;
}

/* Config */

static const char *
model_or_provider_str(struct app *a, const char *wire, const char *key)
{
	const char *v = clm_lua_cfg_provider_model_str(
	    a->lcfg, a->spec_provider, wire, key);

	return v != NULL
	    ? v
	    : clm_lua_cfg_provider_str(a->lcfg, a->spec_provider, key);
}

/* Effort for the model in use: the header's choice, else config.lua's. */
static void
apply_effort(struct app *a)
{
	if (a->agent == NULL)
		return;
	(void)clm_agent_set_effort(a->agent,
	    a->effort != NULL ? a->effort
	                      : model_or_provider_str(a, a->model, "effort"));
}

static int
app_config(struct app *a, char **ret_err)
{
	char *cpath = xdg_config_path("clm/config.lua");
	char *adir = xdg_config_path("clm/agents");
	const char *wire = NULL;
	const char *url, *key;

	*ret_err = NULL;
	a->lcfg = cpath != NULL ? clm_lua_cfg_load(cpath, ret_err) : NULL;
	free(cpath);
	if (a->lcfg != NULL)
		(void)clm_lua_cfg_load_agent(a->lcfg, adir, NULL);
	free(adir);
	if (a->lcfg == NULL)
		return -ENOENT;
	split_provider_model(
	    clm_lua_cfg_get_str(a->lcfg, "model"), &a->spec_provider, &wire);
	if (a->spec_provider == NULL || wire == NULL)
		return -EINVAL;
	url = clm_lua_cfg_provider_str(a->lcfg, a->spec_provider, "url");
	key = clm_lua_cfg_provider_str(a->lcfg, a->spec_provider, "api_key");
	a->cfg.provider = clm_provider_from_str(
	    clm_lua_cfg_provider_str(a->lcfg, a->spec_provider, "kind"));
	a->cfg.provider_name = a->spec_provider;
	a->cfg.api_key = key != NULL ? key : "";
	a->cfg.disable_parallel_tool_calls = true;
	clm_cfg_apply_tuning(a->lcfg, a->spec_provider, wire, &a->cfg);
	clm_provider_build_url(a->endpoint, sizeof(a->endpoint),
	    url != NULL ? url : DEFAULT_URL, a->cfg.provider);
	a->cfg.base_url = a->endpoint;
	a->model = strdup(wire);
	if (a->model == NULL)
		return -ENOMEM;
	a->cfg.model = a->model;
	a->cfg.stream = 1;
	a->sysinfo = clm_cli_sysinfo();
	a->cfg.system_prompt = clm_lua_cfg_get_str(a->lcfg, "system_prompt");
	a->cfg.system_prompt_suffix = a->sysinfo;
	return 0;
}

/* Sessions */

/* Write the agent's system prompt to the log; it skips a repeat. */
static void
session_log_prompt(struct app *a)
{
	const struct clm_message *sys =
	    TAILQ_FIRST(clm_agent_get_history(a->agent));

	if (a->session != NULL && sys != NULL && sys->role == CLM_ROLE_SYSTEM)
		(void)clm_session_append(a->session, sys, NULL);
}

static void
session_start(struct app *a)
{
	if (clm_session_create(NULL, a->model, a->cfg.provider_name,
	        clm_lua_cfg_get_agent_name(a->lcfg), &a->session) < 0) {
		notice(a, "session logging is off");
		return;
	}
	session_log_prompt(a);
}

/* Close the log. An empty one is deleted. */
static void
session_close(struct app *a)
{
	if (a->session == NULL)
		return;
	if (clm_session_is_empty(a->session))
		(void)clm_session_discard(a->session);
	else
		clm_session_free(a->session);
	a->session = NULL;
}

static void
render_message(struct app *a, const struct clm_message *m)
{
	const struct clm_tool_call *tc;
	struct chat_row *cr;

	switch (m->role) {
	case CLM_ROLE_USER:
		if (m->content != NULL &&
		    strncmp(m->content, "[context update]", 16) != 0)
			(void)chat_row_add(
			    a->chat, CHAT_USER, USER_NAME, m->content, false);
		break;
	case CLM_ROLE_ASSISTANT:
		if (m->content != NULL && m->content[0] != '\0')
			(void)chat_row_add(
			    a->chat, CHAT_MODEL, a->model, m->content, false);
		TAILQ_FOREACH(tc, &m->tool_calls, entries)
		{
			cr = chat_row_add(
			    a->chat, CHAT_TOOL, tc->name, tc->args, false);
			g_ptr_array_add(a->tools, cr);
		}
		break;
	case CLM_ROLE_TOOL:
		if (a->tools->len > 0) {
			chat_row_set_result(
			    g_ptr_array_index(a->tools, 0), m->content, false);
			g_ptr_array_remove_index(a->tools, 0);
		}
		break;
	default:
		break;
	}
}

static void
render_history(struct app *a, const struct clm_history *h)
{
	const struct clm_message *m;

	chat_clear(a->chat);
	g_ptr_array_set_size(a->tools, 0);
	a->cur = NULL;
	TAILQ_FOREACH(m, h, entries)
	render_message(a, m);
	g_ptr_array_set_size(a->tools, 0);
	a->stick = true;
}

static int
session_resume(struct app *a, const char *id)
{
	struct clm_history h;
	struct clm_session *s = NULL;
	int r;

	clm_history_init(&h);
	r = clm_session_load(NULL, id, &h, NULL);
	if (r == 0)
		r = clm_session_open(NULL, id, &s);
	if (r < 0) {
		clm_history_free(&h);
		return r;
	}
	(void)clm_history_repair_dangling_tool_calls(&h);
	session_close(a);
	(void)clm_agent_clear_history(a->agent);
	r = clm_agent_restore_history(a->agent, &h);
	a->session = s;
	session_log_prompt(a);
	render_history(a, &h);
	clm_history_free(&h);
	return r;
}

/* Put the chat list's selection back on the session in use. */
static void
sessions_select_current(struct app *a)
{
	const char *cur = a->session != NULL ? clm_session_id(a->session) : "";
	GtkWidget *row;

	a->sessions_quiet = true;
	gtk_list_box_unselect_all(a->sessions);
	for (row = gtk_widget_get_first_child(GTK_WIDGET(a->sessions));
	    row != NULL; row = gtk_widget_get_next_sibling(row)) {
		const char *id = g_object_get_data(G_OBJECT(row), "id");

		if (id != NULL && strcmp(id, cur) == 0) {
			gtk_list_box_select_row(
			    a->sessions, GTK_LIST_BOX_ROW(row));
			break;
		}
	}
	a->sessions_quiet = false;
}

/*
 * Open the chosen session. A selection, not an activation: a click selects
 * and activates, and a screen reader or test can only select.
 */
static void
on_session_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user)
{
	struct app *a = user;
	const char *id =
	    row != NULL ? g_object_get_data(G_OBJECT(row), "id") : NULL;
	bool was_empty;

	(void)box;
	if (a->sessions_quiet || id == NULL ||
	    (a->session != NULL && strcmp(clm_session_id(a->session), id) == 0))
		return;
	if (a->busy) {
		notice(a, "busy; stop the turn first");
		sessions_select_current(a);
		return;
	}
	was_empty = clm_session_is_empty(a->session);
	if (session_resume(a, id) < 0) {
		notice(a, "could not open that session");
		sessions_select_current(a);
	}
	if (was_empty)
		sessions_refresh(a);
}

static void
on_new_chat(GtkButton *b, gpointer user)
{
	struct app *a = user;

	(void)b;
	if (a->busy) {
		notice(a, "busy; stop the turn first");
		return;
	}
	session_close(a);
	(void)clm_agent_clear_history(a->agent);
	session_start(a);
	chat_clear(a->chat);
	a->cur = NULL;
	sessions_refresh(a);
}

static GtkWidget *
session_row(const struct clm_session_info *in)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *title =
	    gtk_label_new(in->first_user != NULL ? in->first_user : "(empty)");
	g_autoptr(GDateTime) t = g_date_time_new_from_unix_local(in->created);
	g_autofree char *when =
	    t != NULL ? g_date_time_format(t, "%b %d %H:%M") : NULL;
	g_autofree char *sub = g_strdup_printf("%s  %s",
	    in->model != NULL ? in->model : "", when != NULL ? when : "");
	GtkWidget *subl = gtk_label_new(sub);
	GtkWidget *row = gtk_list_box_row_new();

	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(subl), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(subl), 0);
	gtk_widget_add_css_class(subl, "dim-label");
	gtk_widget_add_css_class(subl, "caption");
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);
	gtk_box_append(GTK_BOX(box), title);
	gtk_box_append(GTK_BOX(box), subl);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_accessible_update_property(GTK_ACCESSIBLE(row),
	    GTK_ACCESSIBLE_PROPERTY_LABEL,
	    in->first_user != NULL ? in->first_user : "(empty)", -1);
	g_object_set_data_full(G_OBJECT(row), "id", g_strdup(in->id), g_free);
	return row;
}

static void
sessions_refresh(struct app *a)
{
	struct clm_session_info *infos = NULL;
	const char *cur = a->session != NULL ? clm_session_id(a->session) : "";
	size_t n = 0;

	a->list_stale = false;
	a->sessions_quiet = true;
	gtk_list_box_remove_all(a->sessions);
	if (clm_session_list(NULL, &infos, &n) != 0) {
		a->sessions_quiet = false;
		return;
	}
	for (size_t i = 0; i < n && i < SESSIONS_MAX; i++) {
		GtkWidget *row;

		/* A session with nothing said has nothing to show. */
		if (infos[i].first_user == NULL)
			continue;
		row = session_row(&infos[i]);
		gtk_list_box_append(a->sessions, row);
		if (strcmp(infos[i].id, cur) == 0)
			gtk_list_box_select_row(
			    a->sessions, GTK_LIST_BOX_ROW(row));
	}
	clm_session_list_free(infos, n);
	a->sessions_quiet = false;
}

/* Agent callbacks */

static void
cb_text(const char *text, void *user)
{
	struct app *a = user;

	if (a->cur == NULL)
		a->cur =
		    chat_row_add(a->chat, CHAT_MODEL, a->model, NULL, true);
	chat_row_append(a->cur, text);
}

static void
cb_reasoning(const char *text, void *user)
{
	struct app *a = user;

	if (a->cur == NULL)
		a->cur =
		    chat_row_add(a->chat, CHAT_MODEL, a->model, NULL, true);
	chat_row_append_reasoning(a->cur, text);
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	struct app *a = user;

	a->cur = NULL;
	g_ptr_array_add(
	    a->tools, chat_row_add(a->chat, CHAT_TOOL, name, args, true));
}

static void
cb_tool_result(const char *name, const char *content,
    enum clm_tool_outcome outcome, void *user)
{
	struct app *a = user;

	(void)name;
	if (a->tools->len == 0)
		return;
	chat_row_set_result(
	    g_ptr_array_index(a->tools, 0), content, outcome != CLM_TOOL_OK);
	g_ptr_array_remove_index(a->tools, 0);
}

struct perm {
	struct app *a;
	const struct clm_permission_req *req;
};

static void
perm_response(AdwAlertDialog *d, const char *resp, gpointer user)
{
	struct perm *p = user;
	enum clm_permission_decision dec = CLM_PERM_DENY_ONCE;

	(void)d;
	if (strcmp(resp, "once") == 0)
		dec = CLM_PERM_ALLOW_ONCE;
	else if (strcmp(resp, "always") == 0)
		dec = CLM_PERM_ALLOW_ALWAYS;
	(void)clm_tool_permission_respond(p->a->agent, p->req, dec);
}

static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct app *a = user;
	struct perm *p = g_new0(struct perm, 1);
	g_autofree char *head =
	    g_strdup_printf("Run %s?", clm_permission_req_name(req));
	const char *reason = clm_permission_req_reason(req);
	g_autofree char *body =
	    g_strdup_printf("%s%s%s", clm_permission_req_args(req),
	        reason != NULL ? "\n\n" : "", reason != NULL ? reason : "");
	AdwDialog *d = adw_alert_dialog_new(head, body);

	p->a = a;
	p->req = req;
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "deny", "_Deny",
	    "always", "Allow _Always", "once", "_Allow", NULL);
	adw_alert_dialog_set_response_appearance(
	    ADW_ALERT_DIALOG(d), "once", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "once");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "deny");
	g_signal_connect_data(d, "response", G_CALLBACK(perm_response), p,
	    (GClosureNotify)(void (*)(void))g_free, 0);
	adw_dialog_present(d, GTK_WIDGET(a->win));
}

static const char *
state_name(enum clm_agent_state s)
{
	switch (s) {
	case CLM_STATE_THINKING:
		return "thinking";
	case CLM_STATE_CALLING_TOOL:
		return "running a tool";
	case CLM_STATE_RATE_LIMITED:
		return "rate limited";
	case CLM_STATE_ERROR:
		return "error";
	default:
		return "idle";
	}
}

static void
cb_state(enum clm_agent_state s, void *user)
{
	struct app *a = user;

	a->busy = s == CLM_STATE_THINKING || s == CLM_STATE_CALLING_TOOL ||
	    s == CLM_STATE_RATE_LIMITED;
	adw_window_title_set_subtitle(a->title, state_name(s));
	gtk_widget_set_visible(a->stop, a->busy);
}

static void
cb_turn_done(int status, void *user)
{
	struct app *a = user;

	a->cur = NULL;
	g_ptr_array_set_size(a->tools, 0);
	if (status < 0 && status != -ECANCELED) {
		g_autofree char *msg =
		    g_strdup_printf("turn failed: %s", g_strerror(-status));
		notice(a, msg);
	}
	if (a->list_stale)
		sessions_refresh(a);
}

static void
cb_notice(const char *text, void *user)
{
	notice(user, text);
}

static void
cb_connection(enum clm_conn_status s, const char *detail, void *user)
{
	struct app *a = user;

	if (s == CLM_CONN_OFFLINE)
		adw_window_title_set_subtitle(
		    a->title, detail != NULL ? detail : "offline");
	else if (s == CLM_CONN_ONLINE)
		adw_window_title_set_subtitle(a->title, "idle");
}

static void
cb_message(const struct clm_message *m, void *user)
{
	struct app *a = user;

	if (a->session != NULL && clm_session_append(a->session, m, NULL) < 0) {
		notice(a, "session logging failed; off");
		clm_session_free(a->session);
		a->session = NULL;
	}
}

static const struct clm_callbacks callbacks = {
    .on_assistant_text = cb_text,
    .on_reasoning = cb_reasoning,
    .on_tool_begin = cb_tool_begin,
    .on_permission = cb_permission,
    .on_tool_result = cb_tool_result,
    .on_connection = cb_connection,
    .on_state = cb_state,
    .on_turn_done = cb_turn_done,
    .on_notice = cb_notice,
    .on_message = cb_message,
};

/* Window */

static void
on_send(GtkWidget *w, gpointer user)
{
	struct app *a = user;
	const char *text = gtk_editable_get_text(GTK_EDITABLE(a->entry));
	int r;

	(void)w;
	if (a->agent == NULL || text[0] == '\0')
		return;
	if (clm_session_is_empty(a->session))
		a->list_stale = true;
	(void)chat_row_add(a->chat, CHAT_USER, USER_NAME, text, true);
	a->cur = NULL;
	a->stick = true;
	r = a->busy ? clm_agent_notify(a->agent, text)
	            : clm_agent_submit(a->agent, text);
	if (r < 0)
		notice(a, g_strerror(-r));
	gtk_editable_set_text(GTK_EDITABLE(a->entry), "");
}

static void
on_stop(GtkButton *b, gpointer user)
{
	struct app *a = user;

	(void)b;
	if (a->agent != NULL)
		(void)clm_agent_cancel(a->agent);
}

/* Model selector */

/*
 * Point the live agent at a "provider/model-id" from config.lua. History,
 * tools and the session stay as they are.
 */
static int
model_switch(struct app *a, const char *spec)
{
	struct clm_cfg nc = {0};
	char url_buf[sizeof(a->endpoint)];
	char *prov = NULL, *model;
	const char *wire = NULL, *url;
	int r;

	split_provider_model(spec, &prov, &wire);
	url = prov != NULL ? clm_lua_cfg_provider_str(a->lcfg, prov, "url")
	                   : NULL;
	model = wire != NULL ? strdup(wire) : NULL;
	if (url == NULL || model == NULL) {
		free(prov);
		free(model);
		return url == NULL ? -ENOENT : -ENOMEM;
	}
	nc.provider = clm_provider_from_str(
	    clm_lua_cfg_provider_str(a->lcfg, prov, "kind"));
	clm_provider_build_url(url_buf, sizeof(url_buf), url, nc.provider);
	nc.base_url = url_buf;
	nc.api_key = clm_lua_cfg_provider_str(a->lcfg, prov, "api_key");
	nc.model = model;
	clm_cfg_apply_tuning(a->lcfg, prov, model, &nc);
	nc.rate_tokens_per_sec =
	    clm_lua_cfg_provider_int(a->lcfg, prov, "rate_tokens_per_sec", 0);
	nc.rate_burst =
	    clm_lua_cfg_provider_int(a->lcfg, prov, "rate_burst", 0);
	r = clm_agent_set_provider(a->agent, &nc);
	if (r < 0) {
		free(prov);
		free(model);
		return r;
	}
	free(a->spec_provider);
	free(a->model);
	a->spec_provider = prov;
	a->model = model;
	a->cfg.provider_name = prov;
	a->cfg.model = model;
	apply_effort(a);
	(void)clm_agent_set_compact_effort(
	    a->agent, model_or_provider_str(a, model, "compact_effort"));
	if (a->session != NULL)
		(void)clm_session_log_model(a->session, model, prov);
	adw_window_title_set_title(a->title, model);
	(void)clm_agent_check_connection(a->agent);
	return 0;
}

static void
on_model_selected(GObject *obj, GParamSpec *ps, gpointer user)
{
	struct app *a = user;
	GtkDropDown *dd = GTK_DROP_DOWN(obj);
	GtkStringObject *so = gtk_drop_down_get_selected_item(dd);
	guint idx = gtk_drop_down_get_selected(dd);
	const char *why = NULL;
	int r;

	(void)ps;
	if (a->model_quiet || so == NULL || idx == a->model_idx)
		return;
	if (a->agent == NULL)
		why = "no agent is running";
	else if (a->busy)
		why = "busy; stop the turn first";
	else if ((r = model_switch(a, gtk_string_object_get_string(so))) < 0)
		why = r == -EBUSY ? "busy; stop the turn first"
		                  : "could not switch model";
	if (why == NULL) {
		g_autofree char *msg = g_strdup_printf(
		    "switched to %s", gtk_string_object_get_string(so));

		a->model_idx = idx;
		notice(a, msg);
		return;
	}
	notice(a, why);
	a->model_quiet = true;
	gtk_drop_down_set_selected(dd, a->model_idx);
	a->model_quiet = false;
}

static int
strp_cmp(const void *x, const void *y)
{
	return strcmp(*(char *const *)x, *(char *const *)y);
}

/*
 * A dropdown names itself after the choice it shows, so a screen reader or a
 * test would see "default", not "Effort". Point it at a label that says what
 * the control is. With no caption on screen, the label stays unparented.
 */
static void
name_dropdown(GtkWidget *dd, GtkWidget *label)
{
	gtk_accessible_update_relation(GTK_ACCESSIBLE(dd),
	    GTK_ACCESSIBLE_RELATION_LABELLED_BY, label, NULL, -1);
	g_object_set_data_full(G_OBJECT(dd), "name-label",
	    g_object_ref_sink(label), g_object_unref);
}

/* A dropdown of config.lua's models, the one in use selected. */
static GtkWidget *
model_selector(struct app *a)
{
	GtkStringList *sl = gtk_string_list_new(NULL);
	GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
	char **names =
	    a->lcfg != NULL ? clm_lua_cfg_list_names(a->lcfg, "models") : NULL;
	g_autofree char *cur = a->spec_provider != NULL && a->model != NULL
	    ? g_strdup_printf("%s/%s", a->spec_provider, a->model)
	    : NULL;
	size_t n = 0;
	bool found = false;

	while (names != NULL && names[n] != NULL)
		n++;
	if (n > 0)
		qsort(names, n, sizeof(names[0]), strp_cmp);
	for (size_t i = 0; i < n; i++) {
		gtk_string_list_append(sl, names[i]);
		if (cur != NULL && strcmp(names[i], cur) == 0) {
			a->model_idx = (guint)i;
			found = true;
		}
	}
	clm_lua_cfg_free_str_list(names);
	/* The startup model need not have a models{} entry. */
	if (!found && cur != NULL) {
		gtk_string_list_append(sl, cur);
		a->model_idx = (guint)n;
	}
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(dd), TRUE);
	gtk_drop_down_set_expression(GTK_DROP_DOWN(dd),
	    gtk_property_expression_new(
	        GTK_TYPE_STRING_OBJECT, NULL, "string"));
	gtk_widget_set_tooltip_text(dd, "Model");
	name_dropdown(dd, gtk_label_new("Model"));
	gtk_widget_set_sensitive(dd, cur != NULL);
	if (cur != NULL)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), a->model_idx);
	g_signal_connect(
	    dd, "notify::selected", G_CALLBACK(on_model_selected), a);
	a->models = GTK_DROP_DOWN(dd);
	return dd;
}

/* Thinking: how much the model does, and whether it is shown. */

/* The first entry takes config.lua's effort; the rest are sent as named. */
static const char *const efforts[] = {
    "default", "none", "low", "medium", "high", "xhigh", "max", NULL};

static void
on_effort_selected(GObject *obj, GParamSpec *ps, gpointer user)
{
	struct app *a = user;
	guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));

	(void)ps;
	a->effort =
	    idx > 0 && idx < G_N_ELEMENTS(efforts) - 1 ? efforts[idx] : NULL;
	apply_effort(a);
}

/* A dim "Effort" caption and the dropdown beside it. */
static GtkWidget *
effort_selector(struct app *a)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *cap = gtk_label_new("Effort");
	GtkWidget *dd = gtk_drop_down_new_from_strings(efforts);

	gtk_widget_add_css_class(cap, "dim-label");
	name_dropdown(dd, cap);
	gtk_widget_set_tooltip_text(dd,
	    "Reasoning effort; default takes config.lua's. "
	    "A server rejects a level its model does not take.");
	g_signal_connect(
	    dd, "notify::selected", G_CALLBACK(on_effort_selected), a);
	gtk_box_append(GTK_BOX(box), cap);
	gtk_box_append(GTK_BOX(box), dd);
	return box;
}

static void
on_reveal_toggled(GtkToggleButton *b, gpointer user)
{
	struct app *a = user;

	chat_show_thinking(a->chat, gtk_toggle_button_get_active(b));
}

/* Follow new rows while the view sits at the bottom. */
static void
on_adj_changed(GtkAdjustment *adj, gpointer user)
{
	struct app *a = user;

	if (a->stick)
		gtk_adjustment_set_value(adj,
		    gtk_adjustment_get_upper(adj) -
		        gtk_adjustment_get_page_size(adj));
}

static void
on_adj_value(GtkAdjustment *adj, gpointer user)
{
	struct app *a = user;

	a->stick =
	    gtk_adjustment_get_value(adj) >= gtk_adjustment_get_upper(adj) -
	        gtk_adjustment_get_page_size(adj) - 40;
}

static GtkWidget *
scrolled(GtkWidget *child)
{
	GtkWidget *sw = gtk_scrolled_window_new();

	gtk_scrolled_window_set_policy(
	    GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
	gtk_widget_set_vexpand(sw, TRUE);
	return sw;
}

static GtkWidget *
sidebar_page(struct app *a)
{
	GtkWidget *tv = adw_toolbar_view_new();
	GtkWidget *hb = adw_header_bar_new();
	GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
	GtkWidget *list = gtk_list_box_new();

	adw_header_bar_set_title_widget(
	    ADW_HEADER_BAR(hb), adw_window_title_new("Chats", ""));
	gtk_widget_set_tooltip_text(add, "New chat");
	gtk_accessible_update_property(
	    GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL, "New chat", -1);
	g_signal_connect(add, "clicked", G_CALLBACK(on_new_chat), a);
	adw_header_bar_pack_start(ADW_HEADER_BAR(hb), add);
	gtk_widget_add_css_class(list, "navigation-sidebar");
	gtk_accessible_update_property(GTK_ACCESSIBLE(list),
	    GTK_ACCESSIBLE_PROPERTY_LABEL, "Saved chats", -1);
	g_signal_connect(
	    list, "row-selected", G_CALLBACK(on_session_selected), a);
	a->sessions = GTK_LIST_BOX(list);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), scrolled(list));
	gtk_widget_set_size_request(tv, 280, -1);
	return tv;
}

static GtkWidget *
composer(struct app *a)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *entry = gtk_entry_new();
	GtkWidget *send = gtk_button_new_from_icon_name("go-up-symbolic");

	a->stop = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
	gtk_widget_set_tooltip_text(a->stop, "Stop");
	gtk_accessible_update_property(
	    GTK_ACCESSIBLE(a->stop), GTK_ACCESSIBLE_PROPERTY_LABEL, "Stop", -1);
	gtk_widget_set_tooltip_text(send, "Send");
	gtk_accessible_update_property(
	    GTK_ACCESSIBLE(send), GTK_ACCESSIBLE_PROPERTY_LABEL, "Send", -1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(entry),
	    GTK_ACCESSIBLE_PROPERTY_LABEL, "Message", -1);
	gtk_widget_set_visible(a->stop, FALSE);
	gtk_widget_add_css_class(a->stop, "circular");
	gtk_widget_add_css_class(a->stop, "destructive-action");
	gtk_widget_add_css_class(send, "circular");
	gtk_widget_add_css_class(send, "suggested-action");
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Message");
	gtk_widget_set_hexpand(entry, TRUE);
	g_signal_connect(entry, "activate", G_CALLBACK(on_send), a);
	g_signal_connect(send, "clicked", G_CALLBACK(on_send), a);
	g_signal_connect(a->stop, "clicked", G_CALLBACK(on_stop), a);
	a->entry = GTK_ENTRY(entry);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 12);
	gtk_box_append(GTK_BOX(box), entry);
	gtk_box_append(GTK_BOX(box), a->stop);
	gtk_box_append(GTK_BOX(box), send);
	return box;
}

static GtkWidget *
chat_page(struct app *a)
{
	GtkWidget *tv = adw_toolbar_view_new();
	GtkWidget *hb = adw_header_bar_new();
	GtkWidget *list = gtk_list_box_new();
	GtkWidget *sw = scrolled(list);
	GtkWidget *side = gtk_toggle_button_new();
	GtkWidget *reveal = gtk_toggle_button_new();
	GtkAdjustment *adj =
	    gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));

	a->title = ADW_WINDOW_TITLE(
	    adw_window_title_new(a->model != NULL ? a->model : "clm", ""));
	adw_header_bar_set_title_widget(
	    ADW_HEADER_BAR(hb), GTK_WIDGET(a->title));
	adw_header_bar_pack_end(ADW_HEADER_BAR(hb), model_selector(a));
	adw_header_bar_pack_end(ADW_HEADER_BAR(hb), effort_selector(a));
	gtk_button_set_icon_name(GTK_BUTTON(reveal), "view-reveal-symbolic");
	gtk_widget_set_tooltip_text(reveal, "Show thinking");
	gtk_accessible_update_property(GTK_ACCESSIBLE(reveal),
	    GTK_ACCESSIBLE_PROPERTY_LABEL, "Show thinking", -1);
	g_signal_connect(reveal, "toggled", G_CALLBACK(on_reveal_toggled), a);
	adw_header_bar_pack_end(ADW_HEADER_BAR(hb), reveal);
	gtk_button_set_icon_name(GTK_BUTTON(side), "sidebar-show-symbolic");
	gtk_widget_set_tooltip_text(side, "Chats");
	gtk_accessible_update_property(
	    GTK_ACCESSIBLE(side), GTK_ACCESSIBLE_PROPERTY_LABEL, "Chats", -1);
	g_object_bind_property(side, "active", a->split, "show-sidebar",
	    G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	adw_header_bar_pack_start(ADW_HEADER_BAR(hb), side);
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_set_valign(list, GTK_ALIGN_END);
	a->chat = GTK_LIST_BOX(list);
	gtk_accessible_update_property(GTK_ACCESSIBLE(list),
	    GTK_ACCESSIBLE_PROPERTY_LABEL, "Conversation", -1);
	/* Hidden unless config.lua says otherwise, as in the TUI. */
	if (a->lcfg != NULL &&
	    g_strcmp0(clm_lua_cfg_get_str(a->lcfg, "show_thinking"), "true") ==
	        0)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(reveal), TRUE);
	g_signal_connect(adj, "changed", G_CALLBACK(on_adj_changed), a);
	g_signal_connect(adj, "value-changed", G_CALLBACK(on_adj_value), a);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), sw);
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), composer(a));
	return tv;
}

/* Lifecycle */

static int
agent_start(struct app *a)
{
	int r;

	a->loop = uv_default_loop();
	r = clm_host_uv_new(a->loop, &a->host);
	if (r < 0)
		return r;
	r = clm_agent_new(&a->cfg, a->host, &callbacks, a, &a->agent);
	if (r < 0)
		return r;
	(void)clm_tools_register_shell(a->agent);
	(void)clm_tools_register_bg(a->agent);
	(void)clm_tools_register_monitor(a->agent);
	apply_effort(a);
	a->uv_src = clm_uv_source_attach(a->loop);
	session_start(a);
	(void)clm_agent_check_connection(a->agent);
	return 0;
}

static void
on_activate(GApplication *gapp, gpointer user)
{
	struct app *a = user;
	char *err = NULL;
	int r;

	if (a->win != NULL) {
		gtk_window_present(a->win);
		return;
	}
	r = app_config(a, &err);
	a->win = GTK_WINDOW(adw_application_window_new(GTK_APPLICATION(gapp)));
	gtk_window_set_title(a->win, "clm");
	gtk_window_set_default_size(a->win, 1100, 760);
	/* The chat list starts hidden; the header's toggle shows it. */
	a->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_show_sidebar(
	    ADW_OVERLAY_SPLIT_VIEW(a->split), FALSE);
	adw_overlay_split_view_set_sidebar(
	    ADW_OVERLAY_SPLIT_VIEW(a->split), sidebar_page(a));
	adw_overlay_split_view_set_content(
	    ADW_OVERLAY_SPLIT_VIEW(a->split), chat_page(a));
	adw_application_window_set_content(
	    ADW_APPLICATION_WINDOW(a->win), a->split);
	if (r < 0)
		notice(
		    a, err != NULL ? err : "config.lua has no provider/model");
	else if (agent_start(a) < 0)
		notice(a, "could not start the agent");
	free(err);
	sessions_refresh(a);
	gtk_window_present(a->win);
}

static void
on_shutdown(GApplication *gapp, gpointer user)
{
	struct app *a = user;

	(void)gapp;
	session_close(a);
	clm_agent_free(a->agent);
	clm_host_uv_free(a->host);
	if (a->uv_src != 0)
		g_source_remove(a->uv_src);
	(void)clm_drain_loop(a->loop);
	clm_lua_cfg_free(a->lcfg);
	free(a->sysinfo);
	free(a->spec_provider);
	free(a->model);
}

/* SIGINT and SIGTERM quit cleanly, so an empty session log is removed. */
static gboolean
on_signal(gpointer user)
{
	g_application_quit(G_APPLICATION(user));
	return G_SOURCE_CONTINUE;
}

int
main(int argc, char *argv[])
{
	struct app a;
	AdwApplication *gapp;
	int status;

	memset(&a, 0, sizeof(a));
	a.tools = g_ptr_array_new();
	a.stick = true;
	gapp =
	    adw_application_new("org.offblast.clm", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
	g_signal_connect(gapp, "shutdown", G_CALLBACK(on_shutdown), &a);
	(void)g_unix_signal_add(SIGINT, on_signal, gapp);
	(void)g_unix_signal_add(SIGTERM, on_signal, gapp);
	status = g_application_run(G_APPLICATION(gapp), argc, argv);
	g_object_unref(gapp);
	g_ptr_array_free(a.tools, TRUE);
	return status;
}
