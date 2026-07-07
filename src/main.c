// SPDX-License-Identifier: ISC
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

#include "clm/clm.h"
#include "clm/host_uv.h"
#include "clm/provider.h"
#include "frontend.h"
#include "version.h"
#include "clm/lua_plugin.h"
#include "seed_plugins.h"
#include "clm/cleanup.h"
#include "mcp_setup.h"
#include "model_spec.h"

static void
usage(const char *prog)
{
	fprintf(stderr,
	    "usage: %s setup\n"
	    "       %s [-o|--oneshot PROMPT] [-f|--forever PROMPT] "
	    "[-H|--headless] [-u|--url BASE] "
	    "[-m|--model PROVIDER/MODEL-ID] [--provider NAME] "
	    "[-p|--plugins DIR] [-S|--no-stream] "
	    "[-V|--version] [-h|--help]\n",
	    prog, prog);
	fprintf(stderr,
	    "  setup                 write a starter config.lua and seed "
	    "builtin plugins\n"
	    "  -o, --oneshot PROMPT  run one prompt headless and exit\n"
	    "  -f, --forever PROMPT  TUI mode: submit PROMPT, then "
	    "auto-resubmit it\n"
	    "                        every time a turn completes with nothing "
	    "queued,\n"
	    "                        so the agent keeps going without a human "
	    "re-prompting it\n"
	    "  -H, --headless        force the plain stdio REPL\n"
	    "  -u, --url BASE        base API endpoint "
	    "(default http://127.0.0.1:8081/v1);\n"
	    "                        \"/messages\" is appended for an anthropic "
	    "provider,\n"
	    "                        \"/chat/completions\" otherwise\n"
	    "  -m, --model PROVIDER/MODEL-ID\n"
	    "                        a config.lua providers[PROVIDER].models"
	    "[MODEL-ID] entry,\n"
	    "                        or a literal model id (no \"/\") to request "
	    "on whatever\n"
	    "                        connection is otherwise active\n"
	    "  --provider NAME       name of an entry in config.lua's "
	    "`providers` table,\n"
	    "                        overriding the connection --model's "
	    "provider half names\n"
	    "  -p, --plugins DIR     plugin directory "
	    "(default $XDG_CONFIG_HOME/clm/plugins)\n"
	    "  -S, --no-stream       disable streamed (SSE) responses\n"
	    "  -V, --version         print version and exit\n"
	    "  -h, --help            show this help\n"
	    "\n"
	    "  With no options it runs the interactive ncurses UI on a "
	    "terminal.\n"
	    "  Set CLM_API_KEY in the environment to send a bearer token.\n");
}

struct cli_state {
	uv_loop_t *loop;
	struct clm_host *host;
	struct clm_agent *agent;
	struct clm_lua_env *lua_env;
	struct clm_lua_cfg *lua_cfg;
	struct clm_mcp_client **mcp_clients;
	size_t mcp_client_count;
	uv_pipe_t stdin_pipe;
	char prompt_line[1024];
	size_t prompt_len;
	int oneshot;
	int turn_done;
	int turn_status;
};

static void
cb_mcp_status(const char *msg, void *user)
{
	(void)user;
	fprintf(stderr, "%s\n", msg);
}

static void
cb_assistant_text(const char *text, void *user)
{
	struct cli_state *state = (struct cli_state *)user;
	if (state->oneshot)
		printf("%s", text);
	else
		printf("assistant> %s", text);
	fflush(stdout);
}

static void
cb_reasoning(const char *text, void *user)
{
	(void)user;
	printf("\033[2m%s\033[0m", text); /* dim "thinking" channel */
	fflush(stdout);
}

static void
cb_finish_reason(enum clm_finish_reason reason, void *user)
{
	(void)user;
	if (reason == CLM_FINISH_LENGTH)
		printf("\n\033[33m[truncated: hit token limit]\033[0m\n");
	else if (reason == CLM_FINISH_CONTENT_FILTER)
		printf("\n\033[31m[stopped: content filter]\033[0m\n");
	fflush(stdout);
}

static void
cb_usage(const struct clm_usage *usage, void *user)
{
	(void)user;
	printf("\033[2m[%d+%d tok", usage->prompt_tokens, usage->completion_tokens);
	if (usage->tokens_per_sec > 0)
		printf(", %.1f tok/s", usage->tokens_per_sec);
	printf("]\033[0m\n");
	fflush(stdout);
}

static void
cb_tool_begin(const char *name, const char *args, void *user)
{
	(void)user;
	printf("[tool: %s %s]\n", name, args ? args : "");
	fflush(stdout);
}

static void
cb_tool_result(const char *name, const char *content, enum clm_tool_outcome outcome, void *user)
{
	(void)user;
	switch (outcome) {
	case CLM_TOOL_OK:
		printf("[result: %s]\n%s\n", name, content ? content : "");
		break;
	case CLM_TOOL_FAILED:
		printf("\033[31m[x %s]\033[0m %s\n", name, content ? content : "");
		break;
	case CLM_TOOL_TIMEDOUT:
		printf("\033[33m[timeout %s]\033[0m %s\n", name, content ? content : "");
		break;
	}
	fflush(stdout);
}

static void
cb_tool_batch(size_t completed, size_t total, void *user)
{
	(void)user;
	printf("[tools %zu/%zu]\n", completed, total);
	fflush(stdout);
}

static void
cb_state(enum clm_agent_state state, void *user)
{
}

static void
cb_notice(const char *text, void *user)
{
	(void)user;
	fprintf(stderr, "notice: %s\n", text ? text : "");
}

static void
cb_turn_done(int status, void *user)
{
	struct cli_state *state = (struct cli_state *)user;
	if (status != 0) {
		fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
	}
	if (state->oneshot) {
		state->turn_done = 1;
		state->turn_status = status;
		return;
	}
	printf("\nuser> ");
	fflush(stdout);
}

/*
 * Auto-allow permission handler: preserves the CLI's pre-gate behaviour under
 * the library's default-deny. A future refinement could gate this behind an
 * explicit --yes/--allow flag for unattended runs.
 */
static void
cb_permission(const struct clm_permission_req *req, void *user)
{
	struct cli_state *state = (struct cli_state *)user;
	clm_tool_permission_respond(state->agent, req, CLM_PERM_ALLOW_ONCE);
}

static const struct clm_callbacks cli_callbacks = {
	.on_assistant_text = cb_assistant_text,
	.on_reasoning = cb_reasoning,
	.on_tool_begin = cb_tool_begin,
	.on_permission = cb_permission,
	.on_tool_result = cb_tool_result,
	.on_tool_batch = cb_tool_batch,
	.on_finish_reason = cb_finish_reason,
	.on_usage = cb_usage,
	.on_state = cb_state,
	.on_turn_done = cb_turn_done,
	.on_notice = cb_notice,
};

static void
on_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	buf->base = malloc(suggested_size);
	if (buf->base == NULL) {
		buf->len = 0;
		return;
	}
	buf->len = suggested_size;
}

static void
on_stdin_close(uv_handle_t *handle)
{
	struct cli_state *state = (struct cli_state *)handle->data;
	uv_stop(state->loop);
}

static void
on_stdin_read(uv_stream_t *stream, ssize_t n_read, const uv_buf_t *buf)
{
	struct cli_state *state = (struct cli_state *)stream->data;

	if (n_read < 0) {
		if (n_read == UV_ENOBUFS)
			return;
		if (n_read == UV_EOF) {
			free(buf->base);
			printf("\n");
			fflush(stdout);
			uv_close((uv_handle_t *)stream, on_stdin_close);
			return;
		}
		fprintf(stderr, "read error: %s\n", uv_err_name(n_read));
		uv_close((uv_handle_t *)stream, on_stdin_close);
		return;
	}

	if (n_read == 0) {
		free(buf->base);
		return;
	}

	for (ssize_t i = 0; i < n_read; i++) {
		if (state->prompt_len < sizeof(state->prompt_line) - 1) {
			state->prompt_line[state->prompt_len++] = buf->base[i];
		}
		if (buf->base[i] == '\n' || buf->base[i] == '\r') {
			if (state->prompt_len > 0 && state->prompt_line[state->prompt_len - 1] == '\r')
				state->prompt_len--;
			if (state->prompt_len > 0 && state->prompt_line[state->prompt_len - 1] == '\n')
				state->prompt_len--;
			state->prompt_line[state->prompt_len] = '\0';

			if (state->prompt_len > 0) {
				if (strcmp(state->prompt_line, "quit") == 0 || strcmp(state->prompt_line, "exit") == 0) {
					free(buf->base);
					uv_close((uv_handle_t *)stream, on_stdin_close);
					return;
				}

				int r = clm_agent_submit(state->agent, state->prompt_line);
				if (r < 0) {
					fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
				}
			}

			state->prompt_len = 0;
			break;
		}
	}

	free(buf->base);
}

/*
 * Build a path under the XDG config dir: $XDG_CONFIG_HOME/<suffix> or
 * ~/.config/<suffix>. Returns a malloc'd string, or NULL.
 */
static char *
xdg_config_path(const char *suffix)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *out = NULL;

	if (xdg != NULL && xdg[0] != '\0') {
		size_t n = strlen(xdg) + 1 + strlen(suffix) + 1;
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/%s", xdg, suffix);
	} else if (home != NULL && home[0] != '\0') {
		size_t n = strlen(home) + sizeof("/.config/") + strlen(suffix);
		out = malloc(n);
		if (out != NULL)
			(void)snprintf(out, n, "%s/.config/%s", home, suffix);
	}
	return out;
}

/* Writes content to path unless it already exists. Returns 0 if written,
 * 1 if it already existed (left untouched), or -errno on failure. */
static int
write_new_file(const char *path, const char *content, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
	if (fd < 0)
		return errno == EEXIST ? 1 : -errno;

	size_t len = strlen(content);
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, content + off, len - off);
		if (n < 0) {
			int err = -errno;
			close(fd);
			(void)unlink(path);
			return err;
		}
		off += (size_t)n;
	}
	close(fd);
	return 0;
}

static const char config_template[] =
    "-- clm configuration\n"
    "--\n"
    "-- Uncomment and edit the sections you need. See clm-config(5) for\n"
    "-- the full schema (providers, models, agent profiles, per-plugin\n"
    "-- tool config).\n"
    "return {\n"
    "    -- Default agent profile: an inline entry in an `agents` table\n"
    "    -- here, or a file at ~/.config/clm/agents/<name>.lua.\n"
    "    -- agent = \"default\",\n"
    "\n"
    "    -- Default model, used when no agent profile sets its own.\n"
    "    -- \"provider/model-id\": which providers[] entry, and which of\n"
    "    -- its models{} it's found under (see below).\n"
    "    -- model = \"ollama/qwen3-32b\",\n"
    "\n"
    "    -- providers = {\n"
    "    --     ollama = {\n"
    "    --         kind = \"ollama\", -- openai | ollama | anthropic\n"
    "    --         url = \"http://127.0.0.1:8081/v1\",\n"
    "    --         -- Prefer clm.secrets.* (see secrets.lua) over a\n"
    "    --         -- literal key here, since this file often ends up\n"
    "    --         -- checked into dotfiles.\n"
    "    --         api_key = clm.secrets.ollama,\n"
    "    --\n"
    "    --         -- Models nest under their provider, keyed by the\n"
    "    --         -- literal wire model id -- which provider a model\n"
    "    --         -- uses is which provider's `models` table it's\n"
    "    --         -- listed in, not a field you set on the model\n"
    "    --         -- itself.\n"
    "    --         models = {\n"
    "    --             [\"qwen3-32b\"] = {\n"
    "    --                 -- context_size = 32768,\n"
    "    --                 -- autocompact_pct = 70,\n"
    "    --             },\n"
    "    --         },\n"
    "    --     },\n"
    "    -- },\n"
    "\n"
    "    -- system_prompt = \"You are a helpful assistant.\",\n"
    "\n"
    "    -- Per-plugin config: each plugin sees only its own section as\n"
    "    -- clm.config.\n"
    "    tools = {\n"
    "        -- web_search = { api_key = clm.secrets.tavily },\n"
    "        -- weather = { units = \"metric\" },\n"
    "    },\n"
    "}\n";

static const char secrets_template[] =
    "-- clm secrets: kept separate from config.lua so the latter can be\n"
    "-- shared/checked in without leaking keys. Exposed as clm.secrets in\n"
    "-- config.lua and in per-agent profile files (~/.config/clm/agents/).\n"
    "--\n"
    "-- chmod 600 this file; clm warns (via CLM_DEBUG_LOG) if it's\n"
    "-- readable by group or other.\n"
    "return {\n"
    "    -- tavily = \"tvly-...\",\n"
    "    -- ollama = \"...\",\n"
    "}\n";

/* `clm setup`: writes a starter config.lua and secrets.lua, and seeds the
 * builtin plugins, without silently doing any of that on a normal run.
 * Safe to re-run: never overwrites a config, secrets, or plugin file
 * that's already there. */
static int
run_setup(void)
{
	autofree char *cfg_path = xdg_config_path("clm/config.lua");
	autofree char *secrets_path = xdg_config_path("clm/secrets.lua");
	autofree char *agents_dir = xdg_config_path("clm/agents");
	autofree char *plugins_dir = xdg_config_path("clm/plugins");

	if (cfg_path == NULL || secrets_path == NULL || agents_dir == NULL ||
	    plugins_dir == NULL) {
		fprintf(stderr, "setup: could not determine config path "
		    "($XDG_CONFIG_HOME or $HOME not set)\n");
		return 1;
	}

	/* Also creates the parent clm/ dir. */
	int sr = clm_seed_default_plugins(plugins_dir);
	if (sr < 0) {
		fprintf(stderr, "setup: seeding %s: %s\n", plugins_dir,
		    strerror(-sr));
		return 1;
	}
	printf("plugins in %s\n", plugins_dir);

	if (mkdir(agents_dir, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "setup: mkdir %s: %s\n", agents_dir,
		    strerror(errno));
		return 1;
	}
	printf("agent profiles in %s\n", agents_dir);

	/* 0600: secrets.lua holds API keys, unlike config.lua. */
	int xr = write_new_file(secrets_path, secrets_template, 0600);
	if (xr < 0) {
		fprintf(stderr, "setup: writing %s: %s\n", secrets_path,
		    strerror(-xr));
		return 1;
	}
	printf("%s %s\n", xr == 1 ? "kept existing" : "wrote", secrets_path);

	int cr = write_new_file(cfg_path, config_template, 0644);
	if (cr < 0) {
		fprintf(stderr, "setup: writing %s: %s\n", cfg_path,
		    strerror(-cr));
		return 1;
	}
	printf("%s %s\n", cr == 1 ? "kept existing" : "wrote", cfg_path);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *api_base = NULL;
	const char *model_name = NULL; /* -m/--model: a "provider/model-id" spec, or a literal wire id */
	const char *provider_name = NULL; /* --provider: a config providers[] name override */
	/* Owned copy of a "provider/model-id" spec's provider half (see
	 * split_provider_model() in model_spec.h), process-lifetime like
	 * plenty else in this function -- cfg.provider_name below may alias
	 * it, and cfg is still read as late as tui_run()'s call at the very
	 * end of main(), well past any block-scoped cleanup. */
	char *spec_provider = NULL;
	const char *plugin_dir = NULL;
	const char *agent_name = NULL;
	char *oneshot = NULL;
	char *forever_prompt = NULL;
	int stream = 1;
	int headless = 0;
	struct clm_cfg cfg = {0};
	struct cli_state *state;
	uv_loop_t *loop;
	char endpoint[256];
	int opt, r;
	struct clm_lua_cfg *lcfg = NULL;

	if (argc >= 2 && strcmp(argv[1], "setup") == 0)
		return run_setup();

	enum { OPT_PROVIDER = 256 };
	const struct option opts[] = {
		{"oneshot", required_argument, NULL, 'o'},
		{"forever", required_argument, NULL, 'f'},
		{"url", required_argument, NULL, 'u'},
		{"model", required_argument, NULL, 'm'},
		{"provider", required_argument, NULL, OPT_PROVIDER},
		{"plugins", required_argument, NULL, 'p'},
		{"agent", required_argument, NULL, 'a'},
		{"headless", no_argument, NULL, 'H'},
		{"no-stream", no_argument, NULL, 'S'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	while ((opt = getopt_long(argc, argv, "a:o:f:u:m:p:HSVh", opts, NULL)) != -1) {
		switch (opt) {
		case 'a': agent_name = optarg; break;
		case 'o': oneshot = optarg; break;
		case 'f': forever_prompt = optarg; break;
		case 'u': api_base = optarg; break;
		case 'm': model_name = optarg; break;
		case OPT_PROVIDER: provider_name = optarg; break;
		case 'p': plugin_dir = optarg; break;
		case 'H': headless = 1; break;
		case 'V': printf("clm %s\n", CLM_VERSION); return 0;
		case 'S': stream = 0; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}

	/* Load config early so agent profile + model/provider inform cfg. */
	{
		autofree char *cpath = xdg_config_path("clm/config.lua");
		if (cpath != NULL)
			lcfg = clm_lua_cfg_load(cpath);
	}
	if (lcfg != NULL) {
		autofree char *adir = xdg_config_path("clm/agents");
		clm_lua_cfg_load_agent(lcfg, adir, agent_name);

		/* Resolve the model (agent profile, or top-level default,
		 * unless -m gave one directly) -- a "provider/model-id" spec,
		 * see src/model_spec.h -- down to a provider connection.
		 * --provider overrides which provider entry supplies the
		 * connection (e.g. failing over to a mirror endpoint)
		 * without changing which wire model id is requested. */
		if (model_name == NULL)
			model_name = clm_lua_cfg_get_str(lcfg, "model");

		const char *spec_model = NULL;
		split_provider_model(model_name, &spec_provider, &spec_model);

		const char *prov_name = provider_name != NULL ? provider_name
		    : spec_provider;

		if (prov_name != NULL) {
			const char *purl = clm_lua_cfg_provider_str(lcfg, prov_name, "url");
			const char *pkey = clm_lua_cfg_provider_str(lcfg, prov_name, "api_key");
			const char *pkind = clm_lua_cfg_provider_str(lcfg, prov_name, "kind");
			if (api_base == NULL && purl != NULL)
				api_base = purl;
			if (pkey != NULL && getenv("CLM_API_KEY") == NULL)
				cfg.api_key = pkey;
			cfg.provider = clm_provider_from_str(pkind);
			cfg.provider_name = prov_name;
			cfg.rate_tokens_per_sec = clm_lua_cfg_provider_int(lcfg, prov_name, "rate_tokens_per_sec", 0);
			cfg.rate_burst = clm_lua_cfg_provider_int(lcfg, prov_name, "rate_burst", 0);
		}
		if (spec_provider != NULL && spec_model != NULL) {
			cfg.context_size = clm_lua_cfg_provider_model_int(lcfg,
			    spec_provider, spec_model, "context_size", 0);
			cfg.autocompact_pct = (int)clm_lua_cfg_provider_model_int(lcfg,
			    spec_provider, spec_model, "autocompact_pct", 0);
		}
		if (spec_model != NULL)
			model_name = spec_model;
	}

	/* Defaults for anything not set by config or CLI. */
	if (api_base == NULL)
		api_base = "http://127.0.0.1:8081/v1";
	if (model_name == NULL)
		model_name = "local-model";

	/* -u takes the base endpoint; the provider-specific path is appended
	 * by clm_provider_build_url() -- see clm/provider.h's endpoint_path
	 * for why this must be the one place that logic lives. */
	clm_provider_build_url(endpoint, sizeof(endpoint), api_base, cfg.provider);

	/* API key: env > config > placeholder. */
	if (cfg.api_key == NULL) {
		const char *key = getenv("CLM_API_KEY");
		cfg.api_key = (key != NULL && key[0] != '\0') ? key
		                                              : "sk-no-key-required";
	}
	cfg.base_url = endpoint;
	cfg.model = model_name;
	cfg.max_iterations = 0;
	cfg.stream = stream;
	if (lcfg != NULL) {
		cfg.system_prompt = clm_lua_cfg_get_str(lcfg, "system_prompt");
		/* Agent policy: fnmatch patterns for tools whose old results
		 * get stubbed once a newer one lands (see clm_cfg). Never
		 * freed: the agent borrows it for the process lifetime, same
		 * as lcfg itself. */
		cfg.volatile_tools = (const char *const *)
		    clm_lua_cfg_get_str_list(lcfg, "volatile_tools");
	}

	/*
	 * Default to the ncurses UI when we're interactive: no oneshot, not
	 * forced headless, and stdin+stdout are both a terminal. Otherwise fall
	 * through to the plain stdio path (works for pipes and --oneshot).
	 */
	if (oneshot == NULL && !headless && isatty(STDIN_FILENO) &&
	    isatty(STDOUT_FILENO))
		return tui_run(&cfg, plugin_dir, lcfg, forever_prompt);

	/* Heap-allocated so the 1 KB input buffer stays off main's stack frame. */
	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		fprintf(stderr, "error: out of memory\n");
		return 1;
	}

	loop = uv_default_loop();
	state->loop = loop;

	state->oneshot = (oneshot != NULL);

	r = clm_host_uv_new(loop, &state->host);
	if (r < 0) {
		fprintf(stderr, "error: failed to create host (%d)\n", r);
		free(state);
		return 1;
	}

	r = clm_agent_new(&cfg, state->host, &cli_callbacks, state, &state->agent);
	if (r < 0) {
		fprintf(stderr, "error: failed to create agent (%d)\n", r);
		clm_host_uv_free(state->host);
		free(state);
		return 1;
	}
	/* Desktop uv layer: add the shell_exec/bg_exec tools (not in the portable core). */
	clm_tools_register_shell(state->agent);
	clm_tools_register_bg(state->agent);

	if (clm_lua_env_new(state->agent, &state->lua_env) == 0) {
		if (lcfg != NULL)
			clm_lua_env_set_config_from(state->lua_env, lcfg);
		if (plugin_dir != NULL) {
			clm_lua_load_plugins(state->lua_env, plugin_dir);
		} else {
			autofree char *ppath = xdg_config_path("clm/plugins");
			if (ppath != NULL) {
				clm_seed_default_plugins(ppath);
				clm_lua_load_plugins(state->lua_env, ppath);
			}
		}
	}
	state->mcp_clients = clm_cli_connect_mcp_servers(state->agent, loop, lcfg,
	    cb_mcp_status, state, &state->mcp_client_count);

	if (oneshot != NULL) {
		r = clm_agent_submit(state->agent, oneshot);
		if (r < 0) {
			fprintf(stderr, "error: %s\n", clm_agent_get_last_error(state->agent));
			clm_lua_env_free(state->lua_env);
			clm_cli_free_mcp_servers(state->mcp_clients, state->mcp_client_count);
			clm_agent_free(state->agent);
			clm_host_uv_free(state->host);
			free(state);
			return 1;
		}

		while (!state->turn_done)
			uv_run(loop, UV_RUN_ONCE);

		printf("\n");
		clm_lua_env_free(state->lua_env);
		clm_cli_free_mcp_servers(state->mcp_clients, state->mcp_client_count);
		clm_agent_free(state->agent);
		clm_host_uv_free(state->host);
		r = state->turn_status;
		free(state);
		return r == 0 ? 0 : 1;
	}

	printf("clm agent. api: %s\n", endpoint);
	printf("type 'quit' or 'exit' to stop.\n\n");
	printf("user> ");
	fflush(stdout);

	uv_pipe_init(loop, &state->stdin_pipe, 0);
	uv_pipe_open(&state->stdin_pipe, fileno(stdin));
	state->stdin_pipe.data = (uv_handle_t *)state;
	uv_read_start((uv_stream_t *)&state->stdin_pipe, on_alloc_buffer, on_stdin_read);

	uv_run(loop, UV_RUN_DEFAULT);

	clm_lua_env_free(state->lua_env);
	clm_cli_free_mcp_servers(state->mcp_clients, state->mcp_client_count);
	clm_agent_free(state->agent);
	clm_host_uv_free(state->host);
	free(state);

	return 0;
}
