CLM-CONFIG(5) - File Formats Manual

# NAME

**clm-config** - clm configuration file format

# DESCRIPTION

clm(1)
reads its configuration from
*~/.config/clm/config.lua*
(or
`XDG_CONFIG_HOME`*/clm/config.lua*
if that variable is set).
The file is plain Lua: it must
**return**
a table, evaluated once at startup by a long-lived interpreter that
also backs agent profile loading and the
*clm.secrets*
table
(clm-tool(5)).

Top-level keys:

*agent*

> Name of the default agent profile to load from
> *~/.config/clm/agents/*&zwnj;*name*&zwnj;*.lua*.
> Overridden by
> clm(1)'s
> **--agent**
> flag.
> If neither is set, the top-level
> *provider*
> and
> *system\_prompt*
> below apply directly with no named profile.

*provider*

> Name of the entry in
> *providers*
> (below) to use.

*providers*

> A table of provider definitions, keyed by name.
> Each entry may set:

> *url*

> > Base API endpoint, e.g.
> > "http://127.0.0.1:8081/v1".
> > */chat/completions*
> > is appended automatically.

> *model*

> > Model name to request.

> *api\_key*

> > Bearer token.
> > Prefer
> > *clm.secrets.\*&zwnj;*
> > (see clm-tool(5))
> > over a literal key here, since
> > *config.lua*
> > often ends up shared or checked into dotfiles.
> > Overridden by the
> > `CLM_API_KEY`
> > environment variable if set.

> *context\_size*

> > Override the context window size (tokens) the agent assumes, instead
> > of learning it from the backend.

> *autocompact\_pct*

> > Override the percentage of the context window that triggers automatic
> > conversation summarization.

> *rate\_tokens\_per\_sec*, *rate\_burst*

> > Token-bucket rate limiter parameters for tool dispatch.

*system\_prompt*

> The system message sent to the model.
> A built-in default is used if unset.

*tools*

> Per-plugin configuration, keyed by tool/plugin name.
> Each plugin sees only its own subtable, as
> *clm.config*
> (see
> clm-tool(5)).

*volatile\_tools*

> A list of
> fnmatch(3)
> patterns naming tools whose results go stale as soon as a newer
> result exists (a map read, a status query).
> When a matching tool completes, every prior result from that same
> tool is replaced in place with a short stub of the form
> "\[superseded by newer X]"
> (where
> 'X'
> is the tool's name)
> before the new result is recorded, keeping the request prefix
> byte-stable for backend prompt caching while bounding history growth.

*mcp\_servers*

> A list of
> [MCP](https://modelcontextprotocol.io)
> server definitions to connect to at startup.
> Each entry is a table with:

> *name*

> > Used to namespace the server's tools as
> > "mcp\_\_X\_\_Y"
> > (where
> > 'X'
> > is this
> > *name*
> > and
> > 'Y'
> > is the tool's own name),
> > matching the scheme Claude Code uses for MCP-sourced tools, so
> > identically-named tools from different servers, or from a builtin
> > tool, never collide.

> *transport*

> > Either
> > "stdio"
> > (the default if omitted) or
> > "http".

> *command*

> > For
> > "stdio":
> > an argv array; the server is spawned as a subprocess and speaks
> > JSON-RPC over its stdin/stdout.
> > A crashing stdio server is automatically restarted, with a small
> > backoff budget so a genuine crash loop does not turn into a
> > fork/exec storm; its tools disappear from the model's view while it
> > is down and reappear once it is back.

> *url*

> > For
> > "http":
> > the server's endpoint.
> > One JSON-RPC POST per call, no persistent connection.
> > This transport is newer and less exercised than
> > "stdio";
> > it expects a plain JSON response per call, not an SSE-streamed one.

> *api\_key*

> > Optional bearer token for an
> > "http"
> > server.

> *timeout\_ms*

> > Optional per-call deadline for either transport.
> > Defaults to 30000 (30s).

Agent profile files under
*~/.config/clm/agents/*
share the same schema as the top level (they are merged over it,
profile values winning) and may set any of the same keys except
*agent*
itself.

# SECRETS

*~/.config/clm/secrets.lua*,
mode 0600, is loaded automatically (if present) alongside
*config.lua*
and exposed as the global
*clm.secrets*
table to
*config.lua*
and to agent profile files, letting them write
*api\_key = clm.secrets.provider\_name*
instead of a literal key.
It must also
**return**
a table; there is no required schema beyond that, since keys are
referenced by whatever name the rest of the configuration expects.
clm(1)
warns, via
`CLM_DEBUG_LOG`,
if this file is readable by group or other.

# EXAMPLES

A minimal single-provider configuration:

	return {
	    provider = "ollama",
	    providers = {
	        ollama = {
	            url = "http://127.0.0.1:8081/v1",
	            model = "qwen3-32b",
	            api_key = clm.secrets.ollama,
	        },
	    },
	    tools = {
	        web_search = { api_key = clm.secrets.tavily },
	        weather = { units = "metric" },
	    },
	    volatile_tools = { "local_map", "character_status" },
	}

The corresponding
*secrets.lua*:

	return {
	    tavily = "tvly-...",
	    ollama = "...",
	}

# SEE ALSO

clm(1),
clm-tool(5)

Debian - July 6, 2026
