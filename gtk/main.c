// SPDX-License-Identifier: ISC
/* clm-gtk: a GTK4 chat window for a clm agent. */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/queue.h>

#include <adwaita.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "fileio.h"
#include "clm/history.h"
#include "clm/host_uv.h"
#include "clm/lua_plugin.h"
#include "clm/peer.h"
#include "clm/session.h"
#include "bringup.h"
#include "conn.h"
#include "loop_drain.h"
#include "model_spec.h"
#include "sysinfo.h"
#include "xdg.h"
#include "proc.h"
#include "sysinfo.h"
#include "version.h"
#include "chat.h"
#include "uvsource.h"
#include "banned.h"

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
	/* The system prompt suffix: host facts and the session's scratch
	 * directory. It follows the session. */
	char *host_block;
	char **volatile_tools; /* agent policy; the agent borrows it */
	char *spec_provider;   /* owns the model's entry in config.lua */
	char *provider; /* the connection in use; differs under --provider */
	char *model;
	/* Command line. */
	char *opt_agent, *opt_model, *opt_provider, *opt_resume,
	    *opt_plugins_dir;
	GPtrArray *opt_plugins; /* -P names; NULL-terminated once started */
	bool allow_all;         /* run every tool call without asking */
	bool auto_mode;         /* likewise, unless a pre_tool hook objects */
	GtkDropDown *models;
	guint model_idx;  /* the row showing the model in use */
	bool model_quiet; /* the selection is being set by code */
	uv_loop_t *loop;
	guint uv_src;
	struct clm_host *host;
	/* The agent and what hangs off it: plugins, MCP clients, the peer
	 * socket. Started and stopped together, see bringup.h. */
	struct clm_front_agent fa;
	struct clm_session *session;
	struct chat_row *cur;
	GPtrArray *tools; /* tool rows waiting for a result, oldest first */
	bool busy;
	bool stick;
	bool list_stale;
	bool compacting;         /* a /compact or autocompact is running */
	bool autocompacting;     /* ...and the agent started it */
	int64_t ctx_used;        /* tokens the next prompt carries */
	char usage[64];          /* the last turn's rate and cache line */
	GtkLabel *ctx;           /* context gauge and rate, in the header */
	GtkLabel *mode;          /* says when tool calls run without asking */
	GtkToggleButton *reveal; /* show thinking */
	GtkStringList *model_list;
	GtkDropDown *effort_dd;
	bool effort_quiet;
	char *agent_name;
	/* A "! command" the user ran: one at a time. */
	struct clm_proc *shell_proc;
	char *shell_cmd;
	struct chat_row *shell_row;
	bool sessions_quiet; /* the chat list selection is being set by code */
	const char *effort;  /* the header's choice; NULL takes config.lua's */
};

static void sessions_refresh(struct app *a);
static void ctx_update(struct app *a);

static void
notice(struct app *a, const char *text)
{
	(void)chat_row_add(a->chat, CHAT_NOTICE, "clm", text, true);
	a->cur = NULL;
}

/* What another agent said, shown as a row of its own. */
static void
peer_row(struct app *a, const char *from, const char *name, const char *text,
    bool stamp)
{
	size_t n = strlen(from);
	g_autofree char *who = g_strdup_printf("peer %s (%s)",
	    n > 8 ? from + n - 8 : from, name != NULL ? name : "?");

	(void)chat_row_add(a->chat, CHAT_USER, who, text, stamp);
	a->cur = NULL;
}

static void
cb_peer_message(
    const char *from, const char *name, const char *text, void *user)
{
	struct app *a = user;

	peer_row(a, from, name, text, true);
	a->stick = true;
}

/* The header's context gauge: how full the window is, and the last rate. */
static void
ctx_update(struct app *a)
{
	g_autofree char *t = NULL;
	int64_t max =
	    a->fa.agent != NULL ? clm_agent_get_ctx_max(a->fa.agent) : 0;

	if (a->ctx == NULL)
		return;
	if (max > 0 && a->ctx_used > 0) {
		int pct = (int)((a->ctx_used * 100) / max);

		t = g_strdup_printf("ctx %d%%%s%s", pct > 100 ? 100 : pct,
		    a->usage[0] != '\0' ? "  " : "", a->usage);
	} else if (a->usage[0] != '\0') {
		t = g_strdup(a->usage);
	}
	gtk_label_set_text(a->ctx, t != NULL ? t : "");
	gtk_widget_set_visible(GTK_WIDGET(a->ctx), t != NULL);
}

/* Config */

/* A key of the model's entry, else of the provider's. */
static const char *
model_cfg_str(struct app *a, const char *key)
{
	const char *mp =
	    a->spec_provider != NULL ? a->spec_provider : a->provider;
	const char *v = NULL;

	if (a->lcfg == NULL)
		return NULL;
	if (mp != NULL)
		v = clm_lua_cfg_provider_model_str(a->lcfg, mp, a->model, key);
	if (v == NULL && a->provider != NULL)
		v = clm_lua_cfg_provider_str(a->lcfg, a->provider, key);
	return v;
}

/* Effort for the model in use: the header's choice, else config.lua's. */
static void
apply_effort(struct app *a)
{
	if (a->fa.agent == NULL)
		return;
	(void)clm_agent_set_effort(a->fa.agent,
	    a->effort != NULL ? a->effort : model_cfg_str(a, "effort"));
	(void)clm_agent_set_compact_effort(
	    a->fa.agent, model_cfg_str(a, "compact_effort"));
}

/*
 * Load config.lua and the agent profile, then settle which model the window
 * starts on. Nothing here needs the window, and the model dropdown needs the
 * answer, so it runs first. A config that is absent is fine: the agent then
 * starts on the local default. A broken one is reported in *ret_err.
 */
static void
app_setup(struct app *a, char **ret_err)
{
	g_autofree char *cpath = xdg_config_path("clm/config.lua");
	g_autofree char *adir = xdg_config_path("clm/agents");
	const char *spec = a->opt_model;
	const char *wire = NULL;

	*ret_err = NULL;
	a->lcfg = cpath != NULL ? clm_lua_cfg_load(cpath, ret_err) : NULL;
	if (a->lcfg != NULL) {
		const char *perm;

		if (clm_lua_cfg_load_agent(a->lcfg, adir, a->opt_agent) < 0 &&
		    a->opt_agent != NULL) {
			g_free(*ret_err);
			*ret_err = g_strdup_printf(
			    "agent '%s' not found", a->opt_agent);
		}
		perm = clm_lua_cfg_get_str(a->lcfg, "permissions");
		if (perm != NULL && strcmp(perm, "auto") == 0)
			a->auto_mode = true;
		if (spec == NULL)
			spec = clm_lua_cfg_get_str(a->lcfg, "model");
	}
	split_provider_model(spec, &a->spec_provider, &wire);
	a->model = g_strdup(wire != NULL ? wire : CLM_FRONT_DEFAULT_MODEL);
	a->provider = g_strdup(
	    a->opt_provider != NULL ? a->opt_provider : a->spec_provider);
}

/* Sessions */

/* Write the agent's system prompt to the log; it skips a repeat. */
static void
session_log_prompt(struct app *a)
{
	const struct clm_message *sys =
	    TAILQ_FIRST(clm_agent_get_history(a->fa.agent));

	if (a->session != NULL && sys != NULL && sys->role == CLM_ROLE_SYSTEM)
		(void)clm_session_append(a->session, sys, NULL);
}

static void
session_attached(struct app *a)
{
	char *block;

	if (a->session == NULL || a->fa.agent == NULL)
		return;
	/* Other agents address this one by the session id, and the scratch
	 * directory is named after it, so a new session moves both. */
	(void)clm_front_agent_rebind_peer(&a->fa, a->loop,
	    clm_session_id(a->session), a->agent_name, a->model,
	    cb_peer_message, a);
	block =
	    clm_front_scratch_rebind(a->fa.agent, clm_session_id(a->session));
	if (block != NULL) {
		free(a->host_block);
		a->host_block = block;
	}
}

/* Open a log for a new conversation. */
static void
session_start(struct app *a)
{
	if (clm_session_create(
	        NULL, a->model, a->provider, a->agent_name, &a->session) < 0) {
		notice(a, "session logging is off");
		return;
	}
	if (a->fa.agent != NULL) {
		session_log_prompt(a);
		session_attached(a);
	}
}

/* Close the log. An empty one is deleted, and so is its scratch directory
 * when nothing was left in it. */
static void
session_close(struct app *a)
{
	g_autofree char *scratch = NULL;

	if (a->session == NULL)
		return;
	scratch = clm_cli_scratch_dir(clm_session_id(a->session));
	if (clm_session_is_empty(a->session))
		(void)clm_session_discard(a->session);
	else
		clm_session_free(a->session);
	a->session = NULL;
	if (scratch != NULL)
		(void)rmdir(scratch);
}

/* The context gauge and the rate line belong to the conversation. */
static void
gauge_reset(struct app *a)
{
	a->ctx_used = 0;
	a->usage[0] = '\0';
	ctx_update(a);
}

static void
render_message(struct app *a, const struct clm_message *m)
{
	const struct clm_tool_call *tc;
	struct chat_row *cr;

	switch (m->role) {
	case CLM_ROLE_USER: {
		const char *from, *name, *text;
		g_autofree char *buf = NULL;

		if (m->content == NULL)
			break;
		buf = clm_front_peer_unframe(m->content, &from, &name, &text);
		if (buf != NULL)
			peer_row(a, from, name, text, false);
		else if (strncmp(m->content, "[context update]", 16) != 0)
			(void)chat_row_add(
			    a->chat, CHAT_USER, USER_NAME, m->content, false);
		break;
	}
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
	(void)clm_agent_clear_history(a->fa.agent);
	r = clm_agent_restore_history(a->fa.agent, &h);
	a->session = s;
	session_log_prompt(a);
	session_attached(a);
	gauge_reset(a);
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
	if (a->busy || a->shell_proc != NULL) {
		notice(a,
		    a->busy ? "busy; stop the turn first"
		            : "a ! command is still running");
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
new_chat(struct app *a)
{
	if (a->fa.agent == NULL)
		return;
	if (a->busy || a->shell_proc != NULL) {
		notice(a,
		    a->busy ? "busy; stop the turn first"
		            : "a ! command is still running");
		return;
	}
	session_close(a);
	(void)clm_agent_clear_history(a->fa.agent);
	session_start(a);
	chat_clear(a->chat);
	a->cur = NULL;
	gauge_reset(a);
	sessions_refresh(a);
}

static void
on_new_chat(GtkButton *b, gpointer user)
{
	(void)b;
	new_chat(user);
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
	(void)clm_tool_permission_respond(p->a->fa.agent, p->req, dec);
}

static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct app *a = user;
	struct perm *p;

	/* Nobody is watching this one: answer for them. The header says the
	 * window is running this way, so it cannot pass for a normal one. A
	 * hook that asked for a person still gets one. */
	if ((a->allow_all || a->auto_mode) &&
	    clm_permission_req_reason(req) == NULL) {
		(void)clm_tool_permission_respond(
		    a->fa.agent, req, CLM_PERM_ALLOW_ONCE);
		return;
	}
	p = g_new0(struct perm, 1);
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

/* A history rewrite is not reported by on_message, so a compaction has to be
 * written to the log by hand, or a resume would replay what it folded. */
static void
session_after_compact(struct app *a)
{
	const struct clm_history *h;

	if (a->session == NULL)
		return;
	h = clm_agent_get_history(a->fa.agent);
	if (h == NULL)
		return;
	if (clm_session_rewrite(a->session, h, NULL) < 0) {
		notice(a, "session rewrite failed; logging is off");
		clm_session_free(a->session);
		a->session = NULL;
	}
}

static void
cb_state(enum clm_agent_state s, void *user)
{
	struct app *a = user;

	/* A compaction between tool batches has no turn end to report on, so
	 * look for one at every state change. */
	if (clm_agent_take_mid_chain_compact_started(a->fa.agent)) {
		notice(a, "context over threshold, auto-compacting...");
		a->usage[0] = '\0';
		ctx_update(a);
	}
	if (clm_agent_take_mid_chain_compact_succeeded(a->fa.agent)) {
		notice(a, "autocompact complete, resuming");
		session_after_compact(a);
	}
	if (clm_agent_take_mid_chain_compact_error(a->fa.agent)) {
		g_autofree char *msg =
		    g_strdup_printf("autocompact failed, continuing anyway: %s",
		        clm_agent_get_last_error(a->fa.agent));

		notice(a, msg);
	}
	a->busy = s == CLM_STATE_THINKING || s == CLM_STATE_CALLING_TOOL ||
	    s == CLM_STATE_RATE_LIMITED || a->compacting;
	adw_window_title_set_subtitle(a->title, state_name(s));
	gtk_widget_set_visible(a->stop, a->busy);
}

static void
cb_usage(const struct clm_usage *u, void *user)
{
	struct app *a = user;

	a->ctx_used = (int64_t)u->prompt_tokens + u->completion_tokens;
	if (u->tokens_per_sec > 0 && u->cache_read_tokens > 0)
		(void)g_snprintf(a->usage, sizeof(a->usage),
		    "%.0f tok/s, %d cached", u->tokens_per_sec,
		    u->cache_read_tokens);
	else if (u->tokens_per_sec > 0)
		(void)g_snprintf(a->usage, sizeof(a->usage), "%.0f tok/s",
		    u->tokens_per_sec);
	else if (u->cache_read_tokens > 0)
		(void)g_snprintf(a->usage, sizeof(a->usage), "%d cached",
		    u->cache_read_tokens);
	else
		a->usage[0] = '\0';
	ctx_update(a);
}

static void
cb_finish_reason(enum clm_finish_reason r, void *user)
{
	struct app *a = user;

	if (r == CLM_FINISH_LENGTH)
		notice(a, "truncated: hit token limit");
	else if (r == CLM_FINISH_CONTENT_FILTER)
		notice(a, "stopped: content filter");
	else if (r == CLM_FINISH_EMPTY)
		notice(a, "empty response");
}

static void
cb_turn_done(int status, void *user)
{
	struct app *a = user;
	bool was_compacting = a->autocompacting;

	a->autocompacting = false;
	a->cur = NULL;
	g_ptr_array_set_size(a->tools, 0);
	if (status < 0 && status != -ECANCELED && !was_compacting) {
		g_autofree char *msg = g_strdup_printf(
		    "turn failed: %s", clm_agent_get_last_error(a->fa.agent));

		notice(a, msg);
		/* A failed turn may mean the server went away; look again. */
		(void)clm_agent_check_connection(a->fa.agent);
	} else if (was_compacting && status < 0 && status != -ECANCELED) {
		g_autofree char *msg =
		    g_strdup_printf("autocompact failed, continuing anyway: %s",
		        clm_agent_get_last_error(a->fa.agent));

		notice(a, msg);
	}
	if (a->compacting) {
		a->compacting = false;
		if (status == 0)
			session_after_compact(a);
	}
	/* Compact here once a finished turn left the context over the
	 * threshold, unless this call is itself a compaction ending. */
	if (!was_compacting && (status == 0 || status == -ECANCELED) &&
	    clm_agent_over_autocompact_threshold(a->fa.agent) &&
	    clm_agent_compact(a->fa.agent) == 0) {
		a->compacting = true;
		a->autocompacting = true;
		a->busy = true;
		gtk_widget_set_visible(a->stop, TRUE);
		a->ctx_used = 0;
		a->usage[0] = '\0';
		ctx_update(a);
		notice(a, "context over threshold, auto-compacting...");
		return;
	}
	if (a->list_stale)
		sessions_refresh(a);
}

static void
cb_notice(const char *text, void *user)
{
	notice(user, text);
}

/* Plugin and MCP progress and failures, as chat notices. */
static void
cb_status(const char *msg, bool error, void *user)
{
	(void)error;
	if (msg != NULL)
		notice(user, msg);
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

/* Where a cut tool result goes whole, for the model's file tools. */
static int
cb_spool(const char *call_id, const char *tool, const void *data, size_t len,
    char *path, size_t pathsz, void *user)
{
	struct app *a = user;

	(void)tool;
	if (a->session == NULL)
		return -ENOENT;
	return clm_session_spool(a->session, call_id, data, len, path, pathsz);
}

static const struct clm_callbacks callbacks = {
    .on_assistant_text = cb_text,
    .on_reasoning = cb_reasoning,
    .on_tool_begin = cb_tool_begin,
    .on_permission = cb_permission,
    .on_tool_result = cb_tool_result,
    .on_finish_reason = cb_finish_reason,
    .on_usage = cb_usage,
    .on_connection = cb_connection,
    .on_state = cb_state,
    .on_turn_done = cb_turn_done,
    .on_notice = cb_notice,
    .on_message = cb_message,
};

/* Window */

/* "! <cmd>" */

#define SHELL_ESCAPE_MAX 65536 /* output bytes kept */
#define SHELL_ESCAPE_TIMEOUT_MS 120000

static void
shell_status_line(const struct clm_proc_result *r, char *buf, size_t n)
{
	if (r->timed_out)
		(void)g_snprintf(buf, n, "[! timed out]\n");
	else if (r->term_signal != 0)
		(void)g_snprintf(
		    buf, n, "[! killed by signal %d]\n", r->term_signal);
	else
		(void)g_snprintf(
		    buf, n, "[! exit %lld]\n", (long long)r->exit_status);
}

static void
shell_escape_done(
    struct clm_proc *p, const struct clm_proc_result *r, void *user)
{
	struct app *a = user;
	GString *shown = g_string_new(NULL);
	GString *block = g_string_new("[shell] $ ");
	char tag[64];

	(void)p;
	a->shell_proc = NULL;
	shell_status_line(r, tag, sizeof(tag));
	if (r->output != NULL && r->len > 0) {
		g_string_append(shown, r->output);
		g_string_append(
		    block, a->shell_cmd != NULL ? a->shell_cmd : "");
		g_string_append(block, "\n");
		g_string_append(block, r->output);
		if (r->output[r->len - 1] != '\n') {
			g_string_append_c(shown, '\n');
			g_string_append_c(block, '\n');
		}
	} else {
		g_string_append(
		    block, a->shell_cmd != NULL ? a->shell_cmd : "");
		g_string_append(block, "\n");
	}
	if (r->dropped > 0)
		g_string_append(shown, "[! output truncated]\n");
	g_string_append(shown, tag);
	g_string_append(block, tag);
	if (a->shell_row != NULL)
		chat_row_set_result(
		    a->shell_row, shown->str, r->exit_status != 0);
	a->shell_row = NULL;
	/* The model reads it in the form the shell tool's results have. Sent
	 * mid-turn it lands at the next round trip, like any queued message. */
	if (a->fa.agent != NULL) {
		if (a->busy)
			(void)clm_agent_notify(a->fa.agent, block->str);
		else
			(void)clm_agent_submit(a->fa.agent, block->str);
	}
	g_string_free(shown, TRUE);
	g_string_free(block, TRUE);
}

static void
run_shell_escape(struct app *a, const char *cmd)
{
	struct clm_proc_opts o = {0};

	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	if (*cmd == '\0')
		return;
	if (a->shell_proc != NULL) {
		notice(a, "a ! command is still running");
		return;
	}
	g_free(a->shell_cmd);
	a->shell_cmd = g_strdup(cmd);
	a->shell_row = chat_row_add(a->chat, CHAT_TOOL, "!", cmd, true);
	o.command = a->shell_cmd;
	o.timeout_ms = SHELL_ESCAPE_TIMEOUT_MS;
	o.exit_grace_ms = clm_proc_grace_ms();
	o.max = SHELL_ESCAPE_MAX;
	o.done = shell_escape_done;
	o.user = a;
	if (clm_proc_spawn(a->loop, &o, &a->shell_proc) < 0) {
		a->shell_proc = NULL;
		a->shell_row = NULL;
		notice(a, "failed to run ! command");
	}
}

/* Slash commands */

struct cmd_wait {
	struct app *a;
	char name[40];
};

static void
cmd_done(int status, const char *text, void *user)
{
	struct cmd_wait *w = user;

	if (status == 0 && text != NULL && text[0] != '\0') {
		notice(w->a, text);
	} else if (status < 0) {
		g_autofree char *msg = g_strdup_printf(
		    "/%s: %s", w->name, text != NULL ? text : "failed");

		notice(w->a, msg);
	}
	g_free(w);
}

static void
cmd_compact(struct app *a)
{
	int rc = clm_agent_compact(a->fa.agent);

	if (rc == 0) {
		a->busy = true;
		a->compacting = true;
		a->usage[0] = '\0';
		gtk_widget_set_visible(a->stop, TRUE);
		ctx_update(a);
		notice(a, "compacting the conversation...");
	} else {
		notice(a,
		    rc == -EBUSY ? "busy; try again when idle"
		                 : "compaction failed to start");
	}
}

static void
mode_update(struct app *a)
{
	const char *t = a->allow_all ? "allow-all-tools"
	    : a->auto_mode           ? "auto"
	                             : NULL;

	if (a->mode == NULL)
		return;
	gtk_label_set_text(a->mode, t != NULL ? t : "");
	gtk_widget_set_visible(GTK_WIDGET(a->mode), t != NULL);
}

/* Auto mode without a pre_tool hook would be plain allow-all. */
static void
auto_check_hooks(struct app *a)
{
	if (!a->auto_mode || clm_agent_pre_tool_hook_count(a->fa.agent) > 0)
		return;
	a->auto_mode = false;
	notice(a,
	    "auto mode off: no plugin with a pre_tool hook, such as "
	    "jev_guard");
	mode_update(a);
}

static void
cmd_auto(struct app *a, const char *arg)
{
	if (strcmp(arg, "off") == 0 || (arg[0] == '\0' && a->auto_mode)) {
		a->auto_mode = false;
		notice(a, "auto mode off: tool calls ask");
	} else {
		a->auto_mode = true;
		auto_check_hooks(a);
		if (a->auto_mode)
			notice(a,
			    "auto mode on: tool calls run unless a hook "
			    "blocks or asks");
	}
	mode_update(a);
}

static void
cmd_cd(struct app *a, const char *arg)
{
	g_autofree char *target = NULL;
	g_autofree char *cwd = NULL;
	g_autofree char *msg = NULL;

	if (arg[0] == '\0' ||
	    (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')))
		target = g_strconcat(
		    g_get_home_dir(), arg[0] == '~' ? arg + 1 : "", NULL);
	else
		target = g_strdup(arg);
	if (chdir(target) != 0) {
		msg = g_strdup_printf("cd: %s: %s", target, g_strerror(errno));
		notice(a, msg);
		return;
	}
	cwd = g_get_current_dir();
	msg = g_strdup_printf("cwd: %s", cwd);
	notice(a, msg);
}

static void
cmd_session(struct app *a)
{
	g_autofree char *msg = NULL;
	const char *id;

	if (a->session == NULL) {
		notice(a, "session: not being logged");
		return;
	}
	id = clm_session_id(a->session);
	msg =
	    g_strdup_printf("session: %s\nresume: clm-gtk --resume %s", id, id);
	notice(a, msg);
}

static void cmd_model(struct app *a, const char *arg);
static void cmd_effort(struct app *a, const char *arg);
static void cmd_agent(struct app *a, const char *arg);

static void
run_command(struct app *a, const char *line)
{
	const char *arg = line + 1;
	char name[40];
	size_t wlen;
	struct cmd_wait *w;
	int r;

	while (*arg != '\0' && *arg != ' ')
		arg++;
	wlen = (size_t)(arg - (line + 1));
	while (*arg == ' ')
		arg++;
	if (wlen == 0 || wlen >= sizeof(name)) {
		notice(a, "unknown command");
		return;
	}
	memcpy(name, line + 1, wlen);
	name[wlen] = '\0';
	if (strcmp(name, "clear") == 0 || strcmp(name, "new") == 0) {
		new_chat(a);
		return;
	}
	if (strcmp(name, "compact") == 0) {
		cmd_compact(a);
		return;
	}
	if (strcmp(name, "auto") == 0) {
		cmd_auto(a, arg);
		return;
	}
	if (strcmp(name, "model") == 0) {
		cmd_model(a, arg);
		return;
	}
	if (strcmp(name, "effort") == 0) {
		cmd_effort(a, arg);
		return;
	}
	if (strcmp(name, "agent") == 0 || strcmp(name, "a") == 0) {
		cmd_agent(a, arg);
		return;
	}
	if (strcmp(name, "cd") == 0) {
		cmd_cd(a, arg);
		return;
	}
	if (strcmp(name, "session") == 0) {
		cmd_session(a);
		return;
	}
	if (strcmp(name, "quit") == 0 || strcmp(name, "exit") == 0) {
		gtk_window_close(a->win);
		return;
	}
	/* Then the commands plugins registered. */
	w = g_new0(struct cmd_wait, 1);
	w->a = a;
	(void)g_snprintf(w->name, sizeof(w->name), "%s", name);
	r = clm_lua_command_run(a->fa.lua_env, name, arg, cmd_done, w);
	if (r == 0)
		return;
	g_free(w);
	notice(a, r == -ENOENT ? "unknown command" : "could not start it");
}

static void
on_send(GtkWidget *w, gpointer user)
{
	struct app *a = user;
	g_autofree char *text =
	    g_strdup(gtk_editable_get_text(GTK_EDITABLE(a->entry)));
	int r;

	(void)w;
	if (a->fa.agent == NULL || text[0] == '\0')
		return;
	gtk_editable_set_text(GTK_EDITABLE(a->entry), "");
	if (text[0] == '!') {
		run_shell_escape(a, text + 1);
		return;
	}
	if (text[0] == '/') {
		run_command(a, text);
		return;
	}
	if (clm_session_is_empty(a->session))
		a->list_stale = true;
	(void)chat_row_add(a->chat, CHAT_USER, USER_NAME, text, true);
	a->cur = NULL;
	a->stick = true;
	r = a->busy ? clm_agent_notify(a->fa.agent, text)
	            : clm_agent_submit(a->fa.agent, text);
	if (r < 0)
		notice(a, g_strerror(-r));
}

static void
on_stop(GtkButton *b, gpointer user)
{
	struct app *a = user;

	(void)b;
	if (a->fa.agent != NULL)
		(void)clm_agent_cancel(a->fa.agent);
}

/* Model selector */

/*
 * Point the live agent at a "provider/model-id" from config.lua. History,
 * tools and the session stay as they are.
 */
static int
model_switch(struct app *a, const char *spec)
{
	g_autofree struct clm_front_conn *c = g_new0(struct clm_front_conn, 1);
	char *prov = NULL;
	const char *wire = NULL;
	int r;

	split_provider_model(spec, &prov, &wire);
	if (prov == NULL || wire == NULL ||
	    clm_lua_cfg_provider_str(a->lcfg, prov, "url") == NULL) {
		free(prov);
		return -ENOENT;
	}
	r = clm_front_conn_resolve(a->lcfg, prov, NULL, wire, c);
	if (r == 0)
		r = clm_agent_set_provider(a->fa.agent, &c->cfg);
	if (r < 0) {
		free(prov);
		return r;
	}
	free(a->spec_provider);
	free(a->provider);
	g_free(a->model);
	a->spec_provider = prov;
	a->provider = g_strdup(prov);
	a->model = g_strdup(wire);
	apply_effort(a);
	clm_peer_set_model(a->fa.peer, a->model);
	if (a->session != NULL)
		(void)clm_session_log_model(a->session, a->model, a->provider);
	adw_window_title_set_title(a->title, a->model);
	ctx_update(a);
	(void)clm_agent_check_connection(a->fa.agent);
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
	if (a->fa.agent == NULL)
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

	a->model_list = sl;
	char **names =
	    a->lcfg != NULL ? clm_lua_cfg_list_names(a->lcfg, "models") : NULL;
	g_autofree char *cur = a->provider != NULL && a->model != NULL
	    ? g_strdup_printf("%s/%s", a->provider, a->model)
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
	if (a->effort_quiet)
		return;
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
	a->effort_dd = GTK_DROP_DOWN(dd);
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

	a->title = ADW_WINDOW_TITLE(adw_window_title_new(a->model, ""));
	adw_header_bar_set_title_widget(
	    ADW_HEADER_BAR(hb), GTK_WIDGET(a->title));
	adw_header_bar_pack_end(ADW_HEADER_BAR(hb), model_selector(a));
	adw_header_bar_pack_end(ADW_HEADER_BAR(hb), effort_selector(a));
	a->ctx = GTK_LABEL(gtk_label_new(""));
	gtk_widget_add_css_class(GTK_WIDGET(a->ctx), "dim-label");
	gtk_widget_add_css_class(GTK_WIDGET(a->ctx), "caption");
	gtk_widget_set_tooltip_text(
	    GTK_WIDGET(a->ctx), "Context in use, and the last reply's rate");
	gtk_accessible_update_property(GTK_ACCESSIBLE(a->ctx),
	    GTK_ACCESSIBLE_PROPERTY_LABEL, "Context", -1);
	adw_header_bar_pack_end(ADW_HEADER_BAR(hb), GTK_WIDGET(a->ctx));
	a->mode = GTK_LABEL(gtk_label_new(""));
	gtk_widget_add_css_class(GTK_WIDGET(a->mode), "error");
	gtk_widget_add_css_class(GTK_WIDGET(a->mode), "caption-heading");
	gtk_widget_set_tooltip_text(
	    GTK_WIDGET(a->mode), "Tool calls run without asking");
	gtk_accessible_update_property(GTK_ACCESSIBLE(a->mode),
	    GTK_ACCESSIBLE_PROPERTY_LABEL, "Permission mode", -1);
	adw_header_bar_pack_start(ADW_HEADER_BAR(hb), GTK_WIDGET(a->mode));
	mode_update(a);
	a->reveal = GTK_TOGGLE_BUTTON(reveal);
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

/* Show the model in use in the dropdown, without switching to it. */
static void
select_model_row(struct app *a)
{
	g_autofree char *cur = g_strdup_printf(
	    "%s/%s", a->provider != NULL ? a->provider : "", a->model);
	guint n;

	if (a->models == NULL || a->provider == NULL)
		return;
	n = g_list_model_get_n_items(G_LIST_MODEL(a->model_list));
	for (guint i = 0; i < n; i++) {
		if (g_strcmp0(gtk_string_list_get_string(a->model_list, i),
		        cur) == 0) {
			a->model_quiet = true;
			a->model_idx = i;
			gtk_drop_down_set_selected(a->models, i);
			a->model_quiet = false;
			return;
		}
	}
	gtk_string_list_append(a->model_list, cur);
	a->model_quiet = true;
	a->model_idx = n;
	gtk_drop_down_set_selected(a->models, n);
	a->model_quiet = false;
	gtk_widget_set_sensitive(GTK_WIDGET(a->models), TRUE);
}

static void
select_effort_row(struct app *a)
{
	guint idx = 0;

	for (guint i = 1; efforts[i] != NULL; i++)
		if (a->effort != NULL && strcmp(a->effort, efforts[i]) == 0)
			idx = i;
	if (a->effort_dd == NULL)
		return;
	a->effort_quiet = true;
	gtk_drop_down_set_selected(a->effort_dd, idx);
	a->effort_quiet = false;
}

/* What starting an agent needs from this window. */
static void
front_opts(struct app *a, const struct clm_cfg *cfg, struct clm_front_opts *fo)
{
	memset(fo, 0, sizeof(*fo));
	fo->cfg = cfg;
	fo->host = a->host;
	fo->loop = a->loop;
	fo->callbacks = &callbacks;
	fo->user = a;
	fo->lcfg = a->lcfg;
	fo->spool = cb_spool;
	fo->spool_user = a;
	fo->peer_id = a->session != NULL ? clm_session_id(a->session) : NULL;
	fo->peer_name = a->agent_name;
	fo->peer_cb = cb_peer_message;
	fo->peer_user = a;
	fo->plugin_dir = a->opt_plugins_dir;
	fo->seed_plugins = true;
	fo->opt_plugins = a->opt_plugins != NULL
	    ? (const char *const *)a->opt_plugins->pdata
	    : NULL;
	fo->status = cb_status;
	fo->status_user = a;
}

/*
 * The model of a conversation can say how to run a "! <command>": tell it
 * the user can.
 */
static void
tell_shell_escape(struct app *a)
{
	(void)clm_agent_set_prompt_part(a->fa.agent, "shell_escape",
	    "The user can run a shell command by typing \"! <command>\"; its "
	    "output then comes to you as a message that starts with \"[shell] "
	    "$ \". When you want a command run that is destructive, needs the "
	    "user's judgement, or that you should not run yourself, ask the "
	    "user to run it with \"! <command>\" and read the result from that "
	    "message.");
}

/*
 * Resolve the connection, make or reopen the session, and start the agent
 * with everything the other frontends give theirs.
 */
static int
agent_start(struct app *a)
{
	g_autofree struct clm_front_conn *c = g_new0(struct clm_front_conn, 1);
	g_autofree char *sysinfo = clm_cli_sysinfo();
	g_autofree char *scratch = NULL;
	struct clm_history restore;
	struct clm_front_opts fo;
	struct clm_cfg cfg;
	g_autofree char *resume = NULL;
	bool resuming = false;
	int r;

	r = clm_front_conn_resolve(
	    a->lcfg, a->provider, a->spec_provider, a->model, c);
	if (r < 0)
		return r;
	if (!c->found && a->provider != NULL)
		notice(a,
		    "that provider has no url in config.lua; using the "
		    "local default");
	cfg = c->cfg;
	cfg.system_prompt = clm_lua_cfg_get_str(a->lcfg, "system_prompt");
	a->volatile_tools = clm_lua_cfg_get_str_list(a->lcfg, "volatile_tools");
	cfg.volatile_tools = (const char *const *)a->volatile_tools;

	/* The session first: the scratch directory and the peer socket are
	 * both named after it. */
	clm_history_init(&restore);
	if (a->opt_resume != NULL) {
		size_t hits = 0;

		r = clm_front_session_find(a->opt_resume, &resume, &hits);
		if (r == 0)
			r = clm_session_load(NULL, resume, &restore, NULL);
		if (r == 0)
			r = clm_session_open(NULL, resume, &a->session);
		if (r < 0) {
			g_autofree char *msg = g_strdup_printf(hits > 1
			        ? "%s matches %zu sessions; starting fresh"
			        : "no session matching %s; starting fresh",
			    a->opt_resume, hits);

			notice(a, msg);
			clm_history_free(&restore);
			clm_history_init(&restore);
			a->session = NULL;
		} else {
			(void)clm_history_repair_dangling_tool_calls(&restore);
			resuming = true;
		}
	}
	if (!resuming) {
		clm_front_sweep_old(a->lcfg);
		if (clm_session_create(NULL, a->model, a->provider,
		        a->agent_name, &a->session) < 0) {
			a->session = NULL;
			notice(a, "session logging is off");
		}
	}
	if (a->session != NULL) {
		a->host_block = clm_cli_scratch_attach(
		    clm_session_id(a->session), sysinfo, true, &scratch);
		cfg.system_prompt_suffix =
		    a->host_block != NULL ? a->host_block : sysinfo;
	} else {
		cfg.system_prompt_suffix = sysinfo;
	}

	a->loop = uv_default_loop();
	r = clm_host_uv_new(a->loop, &a->host);
	if (r < 0)
		goto out;
	front_opts(a, &cfg, &fo);
	fo.effort = a->effort != NULL ? a->effort : c->effort;
	fo.compact_effort = c->compact_effort;
	r = clm_front_agent_start(&fo, &a->fa);
	if (r < 0)
		goto out;
	tell_shell_escape(a);
	a->uv_src = clm_uv_source_attach(a->loop);
	if (resuming) {
		if (clm_agent_restore_history(a->fa.agent, &restore) < 0)
			notice(a,
			    "could not restore that session; starting "
			    "fresh");
		else
			render_history(a, &restore);
	}
	session_log_prompt(a);
	auto_check_hooks(a);
	clm_front_agent_connect_mcp(&a->fa, &fo);
	(void)clm_agent_check_connection(a->fa.agent);
out:
	clm_history_free(&restore);
	return r;
}

/* /agent, /model, /provider and /effort */

static void
cmd_model(struct app *a, const char *arg)
{
	g_autofree char *spec = NULL;

	if (arg[0] == '\0') {
		notice(a, "usage: /model provider/model-id");
		return;
	}
	spec = strchr(arg, '/') != NULL
	    ? g_strdup(arg)
	    : g_strdup_printf(
	          "%s/%s", a->provider != NULL ? a->provider : "", arg);
	if (a->busy) {
		notice(a, "busy; stop the turn first");
	} else if (model_switch(a, spec) < 0) {
		notice(a, "could not switch model");
	} else {
		g_autofree char *msg = g_strdup_printf("switched to %s", spec);

		select_model_row(a);
		notice(a, msg);
	}
}

static void
cmd_effort(struct app *a, const char *arg)
{
	g_autofree char *msg = NULL;

	if (arg[0] != '\0') {
		a->effort = NULL;
		for (guint i = 1; efforts[i] != NULL; i++)
			if (strcmp(arg, efforts[i]) == 0)
				a->effort = efforts[i];
		if (a->effort == NULL && strcmp(arg, "default") != 0) {
			notice(a,
			    "effort: none, low, medium, high, xhigh, max, "
			    "or default");
			return;
		}
		apply_effort(a);
		select_effort_row(a);
	}
	msg = g_strdup_printf(
	    "effort: %s", a->effort != NULL ? a->effort : "default");
	notice(a, msg);
}

/*
 * Switch the agent profile: a whole new agent, with the profile's prompt,
 * model, plugins and tools, on a new session.
 */
static void
cmd_agent(struct app *a, const char *arg)
{
	g_autofree char *adir = xdg_config_path("clm/agents");
	g_autofree struct clm_front_conn *c = g_new0(struct clm_front_conn, 1);
	g_autofree char *spec_provider = NULL;
	g_autofree char *sysinfo = clm_cli_sysinfo();
	const char *spec, *wire = NULL;
	struct clm_front_opts fo;
	struct clm_cfg cfg;
	g_autofree char *msg = NULL;

	if (arg[0] == '\0') {
		notice(a, "usage: /agent <name>");
		return;
	}
	if (a->busy || a->shell_proc != NULL) {
		notice(a, "busy; try again when idle");
		return;
	}
	if (clm_lua_cfg_load_agent(a->lcfg, adir, arg) < 0) {
		msg = g_strdup_printf("agent '%s' not found", arg);
		notice(a, msg);
		return;
	}
	spec = clm_lua_cfg_get_str(a->lcfg, "model");
	split_provider_model(spec, &spec_provider, &wire);
	if (clm_front_conn_resolve(a->lcfg, spec_provider, NULL, wire, c) < 0) {
		notice(a, "provider or model name too long");
		return;
	}
	cfg = c->cfg;
	cfg.system_prompt = clm_lua_cfg_get_str(a->lcfg, "system_prompt");
	cfg.system_prompt_suffix =
	    a->host_block != NULL ? a->host_block : sysinfo;
	cfg.volatile_tools = (const char *const *)clm_lua_cfg_get_str_list(
	    a->lcfg, "volatile_tools");

	/* The plugins and MCP clients unregister their tools from the old
	 * agent, so they go while it is alive. */
	clm_front_agent_stop(&a->fa);
	front_opts(a, &cfg, &fo);
	g_free(a->agent_name);
	a->agent_name = g_strdup(arg);
	fo.peer_name = a->agent_name;
	fo.peer_id = NULL; /* session_attached binds it to the new session */
	fo.effort = a->effort != NULL ? a->effort : c->effort;
	fo.compact_effort = c->compact_effort;
	fo.agent_plugins = arg;
	if (clm_front_agent_start(&fo, &a->fa) < 0) {
		notice(a, "failed to create agent");
		return;
	}
	free(a->spec_provider);
	a->spec_provider = g_steal_pointer(&spec_provider);
	g_free(a->provider);
	a->provider = g_strdup(a->spec_provider);
	g_free(a->model);
	a->model = g_strdup(c->model);
	tell_shell_escape(a);
	auto_check_hooks(a);
	clm_front_agent_connect_mcp(&a->fa, &fo);
	/* The old log belongs to the old agent. */
	session_close(a);
	session_start(a);
	chat_clear(a->chat);
	a->cur = NULL;
	gauge_reset(a);
	adw_window_title_set_title(a->title, a->model);
	select_model_row(a);
	sessions_refresh(a);
	msg = g_strdup_printf("switched to agent: %s", arg);
	notice(a, msg);
	(void)clm_agent_check_connection(a->fa.agent);
}

static void
on_activate(GApplication *gapp, gpointer user)
{
	struct app *a = user;
	g_autofree char *err = NULL;

	if (a->win != NULL) {
		gtk_window_present(a->win);
		return;
	}
	app_setup(a, &err);
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
	if (a->lcfg == NULL && err != NULL) {
		g_autofree char *msg =
		    g_strdup_printf("config.lua failed to load: %s", err);

		notice(a, msg);
	} else if (err != NULL) {
		notice(a, err);
	}
	select_effort_row(a);
	if (agent_start(a) < 0)
		notice(a, "could not start the agent");
	mode_update(a);
	sessions_refresh(a);
	sessions_select_current(a);
	gtk_window_present(a->win);
}

static void
on_shutdown(GApplication *gapp, gpointer user)
{
	struct app *a = user;

	(void)gapp;
	/* A "! <cmd>" still running must not call back into a freed window. */
	if (a->shell_proc != NULL) {
		clm_proc_detach(a->shell_proc);
		clm_proc_kill(a->shell_proc);
	}
	session_close(a);
	clm_front_agent_stop(&a->fa);
	clm_host_uv_free(a->host);
	if (a->uv_src != 0)
		g_source_remove(a->uv_src);
	(void)clm_drain_loop(a->loop);
	clm_lua_cfg_free_str_list(a->volatile_tools);
	clm_lua_cfg_free(a->lcfg);
	free(a->host_block);
	free(a->spec_provider);
	g_free(a->provider);
	g_free(a->model);
	g_free(a->agent_name);
	g_free(a->shell_cmd);
}

/* SIGINT and SIGTERM quit cleanly, so an empty session log is removed. */
static gboolean
on_signal(gpointer user)
{
	g_application_quit(G_APPLICATION(user));
	return G_SOURCE_CONTINUE;
}

static void
usage(void)
{
	clm_dprintf(STDOUT_FILENO,
	    "usage: clm-gtk [options]\n"
	    "  -a, --agent NAME      load the profile agents/NAME.lua\n"
	    "  -m, --model SPEC      PROVIDER/MODEL-ID from config.lua\n"
	    "      --provider NAME   take the connection from this provider\n"
	    "      --effort LEVEL    none, low, medium, high, xhigh or max\n"
	    "  -p, --plugins DIR     plugin directory\n"
	    "  -P, --plugin NAME     also load the opt-in plugin NAME; "
	    "repeatable\n"
	    "  -r, --resume ID       open a saved session; ID may be part of "
	    "its name\n"
	    "      --allow-all-tools run every tool call without asking\n"
	    "      --auto            run tool calls without asking, unless a "
	    "pre_tool\n"
	    "                        hook plugin blocks or asks\n"
	    "  -V, --version         print the version and exit\n"
	    "  -h, --help            this text\n");
}

enum { OPT_PROVIDER = 256, OPT_EFFORT, OPT_ALLOW_ALL, OPT_AUTO };

/* Returns -1 to go on, else the exit status. */
static int
parse_args(struct app *a, int argc, char *argv[])
{
	static const struct option opts[] = {
	    {"agent", required_argument, NULL, 'a'},
	    {"model", required_argument, NULL, 'm'},
	    {"provider", required_argument, NULL, OPT_PROVIDER},
	    {"effort", required_argument, NULL, OPT_EFFORT},
	    {"plugins", required_argument, NULL, 'p'},
	    {"plugin", required_argument, NULL, 'P'},
	    {"resume", required_argument, NULL, 'r'},
	    {"allow-all-tools", no_argument, NULL, OPT_ALLOW_ALL},
	    {"auto", no_argument, NULL, OPT_AUTO},
	    {"version", no_argument, NULL, 'V'},
	    {"help", no_argument, NULL, 'h'},
	    {NULL, 0, NULL, 0},
	};
	int opt;

	while (
	    (opt = getopt_long(argc, argv, "a:m:p:P:r:Vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'a':
			a->opt_agent = optarg;
			break;
		case 'm':
			a->opt_model = optarg;
			break;
		case OPT_PROVIDER:
			a->opt_provider = optarg;
			break;
		case OPT_EFFORT:
			a->effort = optarg;
			break;
		case 'p':
			a->opt_plugins_dir = optarg;
			break;
		case 'P':
			if (a->opt_plugins == NULL)
				a->opt_plugins = g_ptr_array_new();
			/* Kept NULL-terminated, as the loader wants. */
			if (a->opt_plugins->len > 0)
				g_ptr_array_remove_index(
				    a->opt_plugins, a->opt_plugins->len - 1);
			g_ptr_array_add(a->opt_plugins, optarg);
			g_ptr_array_add(a->opt_plugins, NULL);
			break;
		case 'r':
			a->opt_resume = optarg;
			break;
		case OPT_ALLOW_ALL:
			a->allow_all = true;
			break;
		case OPT_AUTO:
			a->auto_mode = true;
			break;
		case 'V':
			clm_dprintf(STDOUT_FILENO, "clm-gtk %s\n", CLM_VERSION);
			return 0;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}
	return -1;
}

int
main(int argc, char *argv[])
{
	struct app a;
	AdwApplication *gapp;
	int status;

	memset(&a, 0, sizeof(a));
	status = parse_args(&a, argc, argv);
	if (status >= 0)
		return status;
	a.tools = g_ptr_array_new();
	a.stick = true;
	gapp =
	    adw_application_new("org.offblast.clm", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
	g_signal_connect(gapp, "shutdown", G_CALLBACK(on_shutdown), &a);
	(void)g_unix_signal_add(SIGINT, on_signal, gapp);
	(void)g_unix_signal_add(SIGTERM, on_signal, gapp);
	/* The options are ours; GApplication would reject them. */
	status = g_application_run(G_APPLICATION(gapp), 1, argv);
	g_object_unref(gapp);
	g_ptr_array_free(a.tools, TRUE);
	if (a.opt_plugins != NULL)
		g_ptr_array_free(a.opt_plugins, TRUE);
	return status;
}
