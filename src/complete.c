// SPDX-License-Identifier: ISC
/*
 * TAB completion for the TUI's input line. Split out of tui.c since this
 * grew a real async path (live /model completion) worth keeping separate
 * from the curses/uv plumbing -- see tui_internal.h for the shared struct
 * ui this and tui.c both operate on.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <curses.h>

#include "clm/clm.h"
#include "clm/cleanup.h"
#include "clm/lua_plugin.h"
#include "xdg.h"
#include "complete.h"
#include "emoji.h"
#include "model_spec.h"
#include "tui_internal.h"

/* Generous but bounded: completion candidate sets are all small in
 * practice (the largest today, freellmapi's live catalog, is ~65). */
#define MAX_CANDIDATES 128

static int
strp_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Extract the "word" at the cursor position for completion.
 *
 * A word is the maximal span of non-space characters ending at input_pos
 * (or input_len if the cursor is at the end). Returns the start byte
 * offset in *wstart and the length in *wlen. If there is no word (cursor
 * sits right after a space, or at the very start), returns false.
 */
static bool
extract_word(const char *input, size_t input_len, size_t input_pos,
             size_t *wstart, size_t *wlen)
{
	size_t wend = input_pos;

	while (wend > 0 && input[wend - 1] != ' ')
		wend--;

	if (wend == input_pos)
		return false;

	size_t ws = wend;
	size_t we = input_pos;
	while (we < input_len && input[we] != ' ')
		we++;

	*wstart = ws;
	*wlen = we - ws;
	return true;
}

/* Longest common prefix shared by every candidate. 0 if n == 0. */
static size_t
longest_common_prefix(const char *const *candidates, size_t n)
{
	if (n == 0)
		return 0;
	size_t common = strlen(candidates[0]);
	for (size_t i = 1; i < n; i++) {
		size_t k = 0;
		while (k < common && candidates[i][k] == candidates[0][k])
			k++;
		common = k;
	}
	return common;
}

/*
 * Replace u->input[wstart+replace_off .. wstart+wlen) with the sole
 * candidate (ncandidates == 1), or the longest common prefix among
 * candidates if that's longer than what's already typed (typed_len bytes
 * counted from replace_off) -- a no-op otherwise (ambiguous with nothing
 * new to offer). suffix, if non-NUL, is appended after the inserted text
 * when there's exactly one candidate (e.g. '/' for a directory).
 *
 * Purely mechanical -- callers are responsible for showing an ambiguous
 * candidate list themselves, in whatever style suits the source (a
 * command list, a columnated file listing, a plain name-per-line list).
 */
static void
apply_insert(struct ui *u, size_t wstart, size_t wlen, size_t replace_off,
    const char *const *candidates, size_t ncandidates, size_t typed_len,
    char suffix)
{
	if (ncandidates == 0)
		return;

	size_t common = longest_common_prefix(candidates, ncandidates);
	if (ncandidates > 1 && common <= typed_len)
		return;

	size_t base_len = ncandidates == 1 ? strlen(candidates[0]) : common;
	bool has_suffix = ncandidates == 1 && suffix != '\0';
	size_t insert_len = base_len + (has_suffix ? 1 : 0);
	size_t replace_start = wstart + replace_off;
	size_t replace_end = wstart + wlen;
	size_t tail_len = u->input_len - replace_end;
	size_t new_len = replace_start + insert_len + tail_len;

	if (new_len >= sizeof(u->input))
		return;

	memmove(u->input + replace_start + insert_len,
	    u->input + replace_end, tail_len + 1);
	memcpy(u->input + replace_start, candidates[0], base_len);
	if (has_suffix)
		u->input[replace_start + base_len] = suffix;
	u->input_len = new_len;
	u->input_pos = replace_start + insert_len;
	u->dirty = true;
}

/* Show a plain, one-per-line candidate list, with an optional header
 * ("from config:", "from server:", ...). Heap-allocated, not a stack
 * buffer -- a live catalog can run to 60+ entries. */
static void
list_plain(struct ui *u, const char *header, const char *const *items, size_t n)
{
	autofree char *buf = malloc(4096);
	int wrote;
	size_t off;

	if (buf == NULL)
		return;

	wrote = header != NULL ? snprintf(buf, 4096, "\n%s\n", header)
	                       : snprintf(buf, 4096, "\n");
	off = wrote > 0 ? (size_t)wrote : 0;
	if (off > 4096)
		off = 4096;

	for (size_t i = 0; i < n && off < 4096; i++) {
		wrote = snprintf(buf + off, 4096 - off, "  %s\n", items[i]);
		off += wrote > 0 ? (size_t)wrote : 0;
		if (off > 4096)
			off = 4096;
	}
	ui_push(u, ST_META, buf);
}

/* Show a columnated candidate list: each item wrapped in prefix/suffix
 * (e.g. "" / "" for bare file names, ":" / ":" for emoji shortcodes),
 * packed as many per line as fit in ~60 display columns. For dense
 * listings (file completion's directories, emoji's ~1400-entry table) --
 * list_plain's one-per-line layout scrolls forever on those. */
static void
list_columns(struct ui *u, const char *const *items, size_t n,
    const char *prefix, const char *suffix)
{
	autofree char *buf = malloc(4096);
	if (buf == NULL)
		return;

	size_t plen = strlen(prefix), slen = strlen(suffix);
	size_t maxw = 0;
	for (size_t i = 0; i < n; i++) {
		size_t w = plen + strlen(items[i]) + slen;
		if (w > maxw)
			maxw = w;
	}
	maxw += 2; /* column gutter */
	/* COLS is ncurses' live terminal width (kept current across resize
	 * via resizeterm(), see tui.c) -- fall back to 60 if curses hasn't
	 * initialized a screen yet (shouldn't happen in practice: completion
	 * only ever runs from the input line, which requires one). */
	int width = COLS > 0 ? COLS : 60;
	size_t cols = (size_t)width / (maxw > 0 ? maxw : 1);
	if (cols < 1)
		cols = 1;

	int blen = snprintf(buf, 4096, "\n");
	for (size_t i = 0; i < n && (size_t)blen < 4096 - maxw - 4; i++) {
		autofree char *cell = malloc(maxw + 1);
		if (cell == NULL)
			break;
		snprintf(cell, maxw + 1, "%s%s%s", prefix, items[i], suffix);
		blen += snprintf(buf + blen, 4096 - (size_t)blen, "%-*s",
		    (int)maxw, cell);
		if ((i + 1) % cols == 0 || i + 1 == n)
			blen += snprintf(buf + blen, 4096 - (size_t)blen, "\n");
	}
	ui_push(u, ST_META, buf);
}

/* ---- first word: slash-command names ---- */

/*
 * Kept in sync by hand with run_command()'s CMD() dispatch chain in
 * tui.c; if you add a command there, add its name here too. Aliases
 * (h/?, cls, think, a, exit/q) are intentionally excluded -- completion
 * should offer the canonical name to type out, not the shortcuts that
 * exist for people who already know them.
 */
static const char *const command_names[] = {
	"help", "clear", "agent", "model", "provider",
	"reasoning", "output", "compact", "quit",
};
#define N_COMMAND_NAMES (sizeof(command_names) / sizeof(command_names[0]))

static void
complete_command_name(struct ui *u, size_t wstart, size_t wlen)
{
	const char *prefix = u->input + wstart + 1; /* skip '/' */
	size_t typed = wlen - 1;
	const char *candidates[N_COMMAND_NAMES];
	size_t ncandidates = 0;

	for (size_t i = 0; i < N_COMMAND_NAMES; i++) {
		if (strncmp(command_names[i], prefix, typed) == 0)
			candidates[ncandidates++] = command_names[i];
	}
	if (ncandidates == 0)
		return;

	if (ncandidates > 1) {
		autofree char *buf = malloc(512);
		if (buf != NULL) {
			int wrote = snprintf(buf, 512, "\n");
			size_t off = wrote > 0 ? (size_t)wrote : 0;
			for (size_t i = 0; i < ncandidates && off < 512; i++) {
				wrote = snprintf(buf + off, 512 - off, "  /%s\n",
				    candidates[i]);
				off += wrote > 0 ? (size_t)wrote : 0;
				if (off > 512)
					off = 512;
			}
			ui_push(u, ST_META, buf);
		}
	}

	/* A trailing space after an unambiguous match (ncandidates == 1,
	 * the only case apply_insert's suffix applies in) so the very next
	 * TAB goes straight to that command's argument completion instead
	 * of needing the space typed by hand first -- same idea as the
	 * trailing '/' file completion appends after a directory. */
	apply_insert(u, wstart, wlen, 1 /* skip '/' */, candidates, ncandidates,
	    typed, ' ');
}

/* ---- emoji shortcode names (":sh" -> ":shrug") ---- */

static void
complete_emoji(struct ui *u, size_t wstart, size_t wlen)
{
	const char *prefix = u->input + wstart + 1; /* skip ':' */
	size_t typed = wlen - 1;
	const char *candidates[MAX_CANDIDATES];
	size_t ncandidates = 0;

	for (size_t i = 0;
	    i < clm_emoji_table_len && ncandidates < MAX_CANDIDATES; i++) {
		const char *name = clm_emoji_entry_name(i);
		if (strncmp(name, prefix, typed) == 0)
			candidates[ncandidates++] = name;
	}
	if (ncandidates == 0)
		return;

	if (ncandidates > 1)
		list_columns(u, candidates, ncandidates, ":", ":");

	/* Unambiguous match gets its closing ':' appended, same trick
	 * complete_command_name uses with a trailing space -- and that ':'
	 * is exactly what tui_expand_emoji_at_cursor looks for, so a single
	 * match completes straight to the glyph. */
	apply_insert(u, wstart, wlen, 1 /* skip ':' */, candidates, ncandidates,
	    typed, ':');
	if (ncandidates == 1)
		tui_expand_emoji_at_cursor(u);
}

/* ---- command arguments: config.lua names, live /model catalog ---- */

/*
 * Collect config.lua's <table> entry names that start with prefix into
 * matches (capped at MAX_CANDIDATES, borrowed pointers into *names_out),
 * sorted. Returns the count (0 if lcfg is NULL, the table doesn't exist,
 * or nothing matches). On any non-zero return, *names_out is the
 * clm_lua_cfg_list_names() result matches[] borrows from -- the caller
 * must keep it alive for as long as matches[] is used and free it
 * (clm_lua_cfg_free_str_list) afterward. On a zero return, *names_out is
 * NULL and there is nothing to free.
 */
static size_t
match_config_names(struct clm_lua_cfg *lcfg, const char *table,
    const char *prefix, size_t typed_len, const char *matches[MAX_CANDIDATES],
    char ***names_out)
{
	size_t nmatches = 0;

	*names_out = NULL;
	if (lcfg == NULL)
		return 0;

	char **names = clm_lua_cfg_list_names(lcfg, table);
	if (names == NULL)
		return 0;

	for (size_t i = 0; names[i] != NULL && nmatches < MAX_CANDIDATES; i++) {
		if (strncmp(names[i], prefix, typed_len) == 0)
			matches[nmatches++] = names[i];
	}
	if (nmatches == 0) {
		clm_lua_cfg_free_str_list(names);
		return 0;
	}
	qsort(matches, nmatches, sizeof(*matches), strp_cmp);
	*names_out = names;
	return nmatches;
}

static void
source_provider_names(struct ui *u, uint64_t generation, size_t wstart, size_t wlen)
{
	(void)generation; /* fully synchronous; nothing to go stale */
	const char *prefix = u->input + wstart;
	size_t typed = wlen;
	const char *matches[MAX_CANDIDATES];
	char **names = NULL;

	size_t n = match_config_names(u->lcfg, "providers", prefix, typed,
	    matches, &names);
	if (n == 0)
		return;

	if (n > 1)
		list_plain(u, NULL, matches, n);
	apply_insert(u, wstart, wlen, 0, matches, n, typed, '\0');

	clm_lua_cfg_free_str_list(names);
}

static void
source_agent_names(struct ui *u, uint64_t generation, size_t wstart, size_t wlen)
{
	(void)generation;
	const char *prefix = u->input + wstart;
	size_t typed = wlen;
	const char *matches[MAX_CANDIDATES];
	size_t nmatches = 0;

	char *agents_dir = xdg_config_path("clm/agents");

	if (agents_dir == NULL)
		return;

	DIR *d = opendir(agents_dir);
	if (d == NULL) {
		free(agents_dir);
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && nmatches < MAX_CANDIDATES) {
		size_t namelen = strlen(ent->d_name);
		if (namelen <= 4 || strcmp(ent->d_name + namelen - 4, ".lua") != 0)
			continue;

		if (strncmp(ent->d_name, prefix, typed) == 0) {
			char *name = malloc(namelen - 3);
			if (name != NULL) {
				memcpy(name, ent->d_name, namelen - 4);
				name[namelen - 4] = '\0';
				matches[nmatches++] = name;
			}
		}
	}
	closedir(d);
	free(agents_dir);

	if (nmatches == 0)
		return;

	qsort(matches, nmatches, sizeof(matches[0]), strp_cmp);

	if (nmatches > 1)
		list_plain(u, NULL, matches, nmatches);
	apply_insert(u, wstart, wlen, 0, matches, nmatches, typed, '\0');

	for (size_t i = 0; i < nmatches; i++)
		free((char *)matches[i]);
}

/*
 * Per-request context for the async half of /model completion, carrying
 * only what's needed to check staleness and re-locate the word -- see
 * complete_generation's doc comment in tui_internal.h. Heap-allocated,
 * freed exactly once by whichever of the two callbacks below fires.
 *
 * wstart/wlen are snapshotted (not re-derived from u->input_pos at
 * delivery time via extract_word) because extract_word treats "cursor
 * right after a space, nothing typed" as no word at all -- exactly the
 * empty-prefix "/model <TAB>" case this needs to handle, where wlen == 0
 * is a real, valid request, not "nothing to complete". If the sync
 * config-name pass in source_model_names already inserted something
 * (single match / unambiguous common prefix) before this request was
 * built, wlen reflects that -- see the comment where it's set.
 */
struct model_complete_req {
	struct ui *u;
	uint64_t generation;
	size_t wstart, wlen;
	char *provider_name; /* the provider this live catalog was probed
	                       * against -- not necessarily u->provider_name,
	                       * see source_model_names(). Owned by the req,
	                       * freed alongside it. */
};

static void
model_complete_req_free(struct model_complete_req *req)
{
	if (req != NULL)
		free(req->provider_name);
	free(req);
}

static void
model_live_result(char **ids, void *user)
{
	struct model_complete_req *req = user;
	struct ui *u = req->u;

	/* Stale: the user has pressed another key (typed on, TAB again,
	 * Enter, ...) since this request started. Drop it silently rather
	 * than reaching back into an input line that's moved on. */
	if (u->complete_generation == req->generation) {
		const char *prefix = u->input + req->wstart;
		size_t typed = req->wlen;
		/* The server's ids are bare (no provider prefix); /model
		 * expects "provider/model-id" (see src/model_spec.h), so
		 * reprefix each with the provider this catalog was probed
		 * against before matching/offering it -- a bare id here
		 * wouldn't line up with what the user is typing.
		 *
		 * Heap, not two more MAX_CANDIDATES stack arrays alongside
		 * everything else already declared here: that combination
		 * trips -Wframe-larger-than, and this callback isn't a hot
		 * path worth a bigger stack budget for. */
		char **prefixed = malloc(MAX_CANDIDATES * sizeof(*prefixed));
		const char **matches = malloc(MAX_CANDIDATES * sizeof(*matches));
		size_t nprefixed = 0, n = 0;

		if (prefixed != NULL && matches != NULL) {
			for (size_t i = 0; ids[i] != NULL && nprefixed < MAX_CANDIDATES; i++) {
				size_t need = strlen(req->provider_name) + 1 + strlen(ids[i]) + 1;
				char *spec = malloc(need);
				if (spec == NULL)
					continue;
				(void)snprintf(spec, need, "%s/%s", req->provider_name, ids[i]);
				prefixed[nprefixed++] = spec;
			}

			for (size_t i = 0; i < nprefixed && n < MAX_CANDIDATES; i++) {
				if (strncmp(prefixed[i], prefix, typed) == 0)
					matches[n++] = prefixed[i];
			}
			if (n > 0) {
				qsort(matches, n, sizeof(*matches), strp_cmp);
				if (n > 1)
					list_plain(u, "from server:", matches, n);
				apply_insert(u, req->wstart, req->wlen, 0, matches, n,
				    typed, '\0');
			}
		}
		for (size_t i = 0; i < nprefixed; i++)
			free(prefixed[i]);
		free(prefixed);
		free(matches);
	}
	model_complete_req_free(req);
}

static void
model_live_error(const char *msg, void *user)
{
	struct model_complete_req *req = user;
	struct ui *u = req->u;

	if (u->complete_generation == req->generation) {
		autofree char *buf = malloc(256);
		if (buf != NULL) {
			(void)snprintf(buf, 256,
			    "\n(live model fetch failed: %s)\n", msg);
			ui_push(u, ST_META, buf);
		}
	}
	model_complete_req_free(req);
}

/*
 * /model's argument: config.lua's nested providers[*].models{} entries
 * complete immediately as "provider/model-id" compound strings
 * (synchronous, always correct -- no network dependency; see
 * clm_lua_cfg_list_names(cfg, "models") and src/model_spec.h), then a
 * fresh GET /v1/models against whatever connection is currently active is
 * kicked off every time (no cache -- a cached list from a since-switched
 * provider would be actively misleading, worse than a moment's wait) to
 * extend/refine the candidates a moment later if the user pauses.
 */
static void
source_model_names(struct ui *u, uint64_t generation, size_t wstart, size_t wlen)
{
	const char *prefix = u->input + wstart;
	size_t typed = wlen;
	const char *matches[MAX_CANDIDATES];
	char **names = NULL;
	struct model_complete_req *req;

	/* No '/' typed yet: complete provider names first (same table
	 * /provider offers), not "provider/model-id" specs -- mixing those
	 * in here biases completion toward whichever provider the live
	 * server query below happens to be connected to, and that query
	 * only ever knows the currently active provider's catalog, so it
	 * would go on to silently narrow the offered specs down to just
	 * that one provider. Suffix with '/' on an unambiguous match so
	 * the next TAB moves straight on to that provider's models. */
	if (memchr(prefix, '/', typed) == NULL) {
		size_t pn = match_config_names(u->lcfg, "providers", prefix, typed,
		    matches, &names);
		if (pn > 0) {
			if (pn > 1)
				list_plain(u, NULL, matches, pn);
			apply_insert(u, wstart, wlen, 0, matches, pn, typed, '/');
			clm_lua_cfg_free_str_list(names);
		}
		return;
	}

	size_t n = match_config_names(u->lcfg, "models", prefix, typed,
	    matches, &names);
	if (n > 0) {
		if (n > 1)
			list_plain(u, "from config:", matches, n);
		apply_insert(u, wstart, wlen, 0, matches, n, typed, '\0');
		clm_lua_cfg_free_str_list(names);
	}

	if (u->agent == NULL)
		return;

	/* A provider name is already typed (that's how we got past the
	 * no-'/' branch above) -- resolve it and probe *that* provider's
	 * live catalog, not necessarily whatever the agent is currently
	 * connected to (see clm_agent_probe_models's doc comment: /model
	 * completion for a provider you're not currently on needs its own
	 * probe, since the agent's own connection can't tell us anything
	 * about it). typed/prefix have already moved past any sync insert
	 * above (see the wlen recompute below), so re-find the slash. */
	{
		prefix = u->input + wstart;
		typed = u->input_pos - wstart;
		const char *slash = memchr(prefix, '/', typed);
		if (slash == NULL)
			return; /* shouldn't happen: we're past the no-'/' branch */

		autofree char *spec_provider = strndup(prefix, (size_t)(slash - prefix));
		if (spec_provider == NULL)
			return;

		req = malloc(sizeof(*req));
		if (req == NULL)
			return;
		req->u = u;
		req->generation = generation;
		req->wstart = wstart;
		/* Not the original wlen parameter: the sync pass above may have
		 * just inserted text (a single config match, or an unambiguous
		 * common prefix), moving u->input_pos past what was originally
		 * typed. Since apply_insert never moves the word's start, only
		 * extends it, input_pos - wstart is the word's current length
		 * either way -- with or without an insert. */
		req->wlen = typed;
		req->provider_name = strdup(spec_provider);
		if (req->provider_name == NULL) {
			free(req);
			return;
		}

		const char *purl = u->lcfg != NULL
		    ? clm_lua_cfg_provider_str(u->lcfg, spec_provider, "url") : NULL;

		if (purl != NULL) {
			enum clm_provider provider = clm_provider_from_str(
			    clm_lua_cfg_provider_str(u->lcfg, spec_provider, "kind"));
			const char *api_key =
			    clm_lua_cfg_provider_str(u->lcfg, spec_provider, "api_key");
			char url_buf[512];

			clm_provider_build_url(url_buf, sizeof(url_buf), purl, provider);
			if (clm_agent_probe_models(u->agent, url_buf, provider, api_key,
			    model_live_result, model_live_error, req) != 0)
				model_complete_req_free(req);
		} else if (u->provider_name != NULL &&
		    strcmp(spec_provider, u->provider_name) == 0) {
			/* Not in config, but it's the connection we're already
			 * on (e.g. a literal ad hoc connection) -- probe that
			 * live, same as before this function learned to look
			 * beyond the active provider. */
			if (clm_agent_list_models(u->agent, model_live_result,
			    model_live_error, req) != 0)
				model_complete_req_free(req);
		} else {
			/* Unknown provider name and it's not the active
			 * connection either -- nothing to probe. */
			model_complete_req_free(req);
		}
	}
}

typedef void (*arg_complete_fn)(struct ui *u, uint64_t generation,
    size_t wstart, size_t wlen);

struct arg_source {
	const char *command; /* without the leading '/' */
	arg_complete_fn fn;
};

/*
 * Which commands have a completable argument, and what completes it.
 */
static const struct arg_source arg_sources[] = {
	{ "agent",    source_agent_names },
	{ "provider", source_provider_names },
	{ "model",    source_model_names },
};
#define N_ARG_SOURCES (sizeof(arg_sources) / sizeof(arg_sources[0]))

/* ---- fallback: filesystem paths ---- */

/* Check if a string looks like a file path: contains '/' or starts with '~'. */
static bool
looks_like_path(const char *s, size_t len)
{
	if (len == 0)
		return false;
	if (s[0] == '~')
		return true;
	for (size_t i = 0; i < len; i++)
		if (s[i] == '/')
			return true;
	return false;
}

/* Expand leading ~ to $HOME. Returns a malloc'd string or NULL. */
static char *
expand_tilde(const char *s, size_t len)
{
	if (len > 0 && s[0] == '~') {
		const char *home = getenv("HOME");
		if (home) {
			size_t hlen = strlen(home);
			size_t n = hlen + len - 1;
			char *out = malloc(n + 1);
			if (out) {
				memcpy(out, home, hlen);
				memcpy(out + hlen, s + 1, len - 1);
				out[n] = '\0';
			}
			return out;
		}
	}
	char *out = malloc(len + 1);
	if (out) {
		memcpy(out, s, len);
		out[len] = '\0';
	}
	return out;
}

/* Return true if path exists and is a directory. */
static bool
is_directory(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/*
 * Tab completion for file paths.
 *
 * If the word under the cursor looks like a file path:
 *   1. Gather matching entries from the parent directory.
 *   2. If there is exactly one match, insert it. If it is a directory,
 *      append '/' so the next TAB can complete inside it.
 *   3. If ambiguous but there is a common prefix, insert that prefix.
 *   4. Otherwise just print the candidate list (no change to the input).
 */
static void
complete_path(struct ui *u, size_t wstart, size_t wlen)
{
	const char *word = u->input + wstart;

	char *expanded = expand_tilde(word, wlen);
	if (!expanded)
		return;

	char *slash = strrchr(expanded, '/');
	char *dir, *prefix;
	size_t dir_offset; /* how much of the original word is the dir part */

	if (slash) {
		*slash = '\0';
		dir = expanded[0] != '\0' ? expanded : "/";
		prefix = slash + 1;
		const char *orig_slash = NULL;
		for (size_t i = wlen; i > 0; i--) {
			if (word[i - 1] == '/') {
				orig_slash = word + i;
				break;
			}
		}
		dir_offset = orig_slash ? (size_t)(orig_slash - word) : 0;
	} else {
		dir = ".";
		prefix = expanded;
		dir_offset = 0;
	}

	DIR *d = opendir(dir);
	if (!d) {
		free(expanded);
		return;
	}

	char *candidates[MAX_CANDIDATES];
	size_t ncandidates = 0;
	struct dirent *ent;
	size_t plen = strlen(prefix);

	while (ncandidates < MAX_CANDIDATES && (ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (strncmp(ent->d_name, prefix, plen) == 0) {
			char *c = strdup(ent->d_name);
			if (c)
				candidates[ncandidates++] = c;
		}
	}
	closedir(d);

	if (ncandidates == 0) {
		free(expanded);
		return;
	}

	qsort(candidates, ncandidates, sizeof(*candidates), strp_cmp);

	bool append_slash = false;
	if (ncandidates == 1) {
		autofree char *full = malloc(1024);
		if (full) {
			(void)snprintf(full, 1024, "%s/%s", dir, candidates[0]);
			if (is_directory(full))
				append_slash = true;
		}
	}

	/* Ambiguous match: columnated, not list_plain's one-per-line -- file
	 * listings can be wide and dense. */
	if (ncandidates > 1)
		list_columns(u, (const char *const *)candidates, ncandidates, "",
		    "");

	apply_insert(u, wstart, wlen, dir_offset, (const char *const *)candidates,
	    ncandidates, plen, append_slash ? '/' : '\0');

	for (size_t i = 0; i < ncandidates; i++)
		free(candidates[i]);
	free(expanded);
}

/* ---- entry point ---- */

void
complete_input(struct ui *u, uint64_t generation)
{
	/* Zero-initialized to pacify -Wmaybe-uninitialized: every real read
	 * below is guarded by have_word (extract_word() always sets both
	 * whenever it returns true), but GCC's flow analysis can't correlate
	 * the bool with these output params across that path. */
	size_t wstart = 0, wlen = 0;
	bool have_word = extract_word(u->input, u->input_len, u->input_pos,
	    &wstart, &wlen);

	/* First word starting with '/' is a slash command, not a path --
	 * checked before looks_like_path, which would otherwise treat the
	 * leading '/' itself as "absolute path" and try to opendir("/"). */
	if (have_word && wstart == 0 && wlen > 0 && u->input[0] == '/') {
		complete_command_name(u, wstart, wlen);
		return;
	}

	/*
	 * A later word of a slash-command line: dispatch to that command's
	 * registered argument source instead of falling through to
	 * file-path completion. Two shapes, both valid:
	 *   - a word is already partially typed (have_word && wstart > 0)
	 *   - the cursor sits right after the command's trailing space with
	 *     nothing typed yet for the argument -- extract_word returns
	 *     false here (by design: "cursor right after a space" has no
	 *     word), but this is exactly the "/model <TAB>" case that
	 *     should list everything, not do nothing. Treat it as an empty
	 *     word positioned at the cursor.
	 */
	if (u->input_len > 0 && u->input[0] == '/') {
		size_t cmd_len = 1; /* end of the command word, index of the
		                     * space if any, else input_len */
		while (cmd_len < u->input_len && u->input[cmd_len] != ' ')
			cmd_len++;
		bool past_command = cmd_len < u->input_len &&
		    u->input_pos > cmd_len;

		if (past_command) {
			size_t use_wstart, use_wlen;
			if (have_word && wstart > 0) {
				use_wstart = wstart;
				use_wlen = wlen;
			} else {
				use_wstart = u->input_pos;
				use_wlen = 0;
			}

			const char *cmd = u->input + 1; /* skip '/' */
			size_t cmd_name_len = cmd_len - 1;

			for (size_t i = 0; i < N_ARG_SOURCES; i++) {
				if (strlen(arg_sources[i].command) == cmd_name_len &&
				    strncmp(arg_sources[i].command, cmd,
				        cmd_name_len) == 0) {
					arg_sources[i].fn(u, generation,
					    use_wstart, use_wlen);
					return;
				}
			}
			/* A recognized slash command with no argument source
			 * (/agent, /reasoning, /compact, ...) -- nothing to
			 * offer, and certainly not a file path. */
			return;
		}
	}

	/*
	 * ":sh<TAB>" -> ":shrug", Slack/Discord-style. Scanned back from the
	 * cursor independently of extract_word()'s space-delimited notion of
	 * "word": that would miss a shortcode typed right after an already-
	 * expanded glyph with no space in between (e.g. "🦴:bon<TAB>"), since
	 * the glyph's bytes and ":bon" are one "word" together and its start
	 * isn't ':'. Mirrors tui_expand_emoji_at_cursor's backward scan.
	 * Requires at least one name char typed so a lone ':' doesn't dump
	 * the whole ~1400-entry table.
	 */
	{
		size_t cpos = u->input_pos;
		size_t i = cpos;
		bool found_colon = false;
		while (i > 0) {
			i--;
			char c = u->input[i];
			if (c == ':') {
				found_colon = true;
				break;
			}
			if (!(isalnum((unsigned char)c) || c == '_' || c == '+' ||
			        c == '-'))
				break;
			if (cpos - i > 64) /* no shortcode is this long; bail */
				break;
		}
		if (found_colon && cpos - i >= 2) {
			complete_emoji(u, i, cpos - i);
			return;
		}
	}

	if (!have_word)
		return;

	const char *word = u->input + wstart;
	if (!looks_like_path(word, wlen))
		return;

	complete_path(u, wstart, wlen);
}
