CLM-CONFIG(5) - File Formats Manual

# NAME

**clm-config** - clm configuration file format

# DESCRIPTION

[clm(1)](clm.md)
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
([clm-tool(5)](clm-tool.md)).

Top-level keys:

*agent*

Name of the default agent profile to load from
*~/.config/clm/agents/*&zwnj;*name*&zwnj;*.lua*.
Overridden by
[clm(1)](clm.md)'s
**--agent**
flag.
If neither is set, the top-level
*model*
and
*system\_prompt*
below apply directly with no named profile.

*model*

A
"*provider*/*model-id*"
spec: which
*providers*
(below) entry to connect through, and the literal wire model id to
request, which doubles as the key into that provider's own
*models*
subtable (if it has a matching entry there -- see below).
Overridden by
[clm(1)](clm.md)'s
**--model**
flag, which takes the same
"*provider*/*model-id*"
form.
A bare id with no
"/"
is also accepted, meaning "request this literal wire id on whatever
connection is otherwise active" -- useful for an id a server reports
live that was never added to any provider's
*models*
table.
There is no separate top-level
*provider*
key: failing over to a different connection for the current model,
without changing which wire model id is requested or editing
*providers*,
is done at runtime only, via
[clm(1)](clm.md)'s
**--provider**
flag or the TUI's
**/provider**
command -- both read directly from
*providers*
(below) by name.

*providers*

A table of provider (connection) definitions, keyed by name.
Each entry may set:

*kind*

Wire dialect to speak:
"openai",
"anthropic",
or
"ollama".
Defaults to
"openai"
if unset.
"ollama"
is the same wire dialect as
"openai"
(every OpenAI-compatible server, Ollama included, speaks it directly);
it exists as a separate value only to name the connection for its own
sake.
"anthropic"
speaks Anthropic's own Messages API instead -- a structurally
different request, response, and streaming-event shape, translated
internally -- and gets Anthropic's
*x-api-key*/*anthropic-version*
auth headers instead of a bearer token, and prompt caching enabled on
every request automatically (a no-op below the model's minimum
cacheable prefix, so this is free to leave on).

*url*

Base API endpoint, e.g.
"http://127.0.0.1:8081/v1".
Include any path prefix the endpoint itself needs (e.g. a gateway that
routes multiple wire dialects under
"/anthropic/v1"
or
"/openai/v1")
up through the segment just before the request path proper, which is
appended automatically and depends on
*kind*:
*/messages*
for
"anthropic",
*/chat/completions*
otherwise.

*api\_key*

Bearer token
(*x-api-key* for "anthropic").
Prefer
*clm.secrets.\*&zwnj;*
(see [clm-tool(5)](clm-tool.md))
over a literal key here, since
*config.lua*
often ends up shared or checked into dotfiles.
Overridden by the
`CLM_API_KEY`
environment variable if set.

*rate\_tokens\_per\_sec*, *rate\_burst*

Token-bucket rate limiter for outgoing LLM requests, paced by an
estimate of each request's size (roughly one token per four bytes of
the full serialized body -- the whole conversation history resent
every turn, not just the newest message).
Guards against bursting past a backend's requests- or
tokens-per-minute limit when a single logical turn chains several
tool-calling round-trips back to back; unrelated to tool
*dispatch*
itself, which has its own small, fixed, unconfigurable limiter.
Unset, or either left at
0,
falls back to a rate high enough to never bind in normal use -- lower
these only for a backend with a real, tight quota (a free-tier proxy,
typically).

*models*

A table of per-model overrides nested under this provider, keyed by
the literal wire model id (the same string sent on the wire and used
as the second half of a
*model*
spec -- there is no separate field repeating it).
An entry here is optional: a provider can be requested with any model
id, configured or not, and only gets these overrides applied when one
happens to match.
Each entry may set:

*context\_size*

Override the context window size (tokens) the agent assumes, instead
of learning it from the backend.

*autocompact\_pct*

Override the percentage of the context window that triggers automatic
conversation summarization.

*system\_prompt*

The system message sent to the model.
A built-in default is used if unset.

*tools*

Per-plugin configuration, keyed by tool/plugin name.
Each plugin sees only its own subtable, as
*clm.config*
(see
[clm-tool(5)](clm-tool.md)).

*volatile\_tools*

A list of
fnmatch(3)
patterns naming tools whose results go stale as soon as a newer
result exists (a map read, a status query).
When a matching tool completes, every prior result from that same
tool is replaced in place with a short stub of the form
"\[superseded by newer X]"
(where
'X'
is the tool's name)
before the new result is recorded, keeping the request prefix
byte-stable for backend prompt caching while bounding history growth.

*mcp\_servers*

A list of
[MCP](https://modelcontextprotocol.io)
server definitions to connect to at startup.
Each entry is a table with:

*name*

Used to namespace the server's tools as
"mcp\_\_X\_\_Y"
(where
'X'
is this
*name*
and
'Y'
is the tool's own name),
matching the scheme Claude Code uses for MCP-sourced tools, so
identically-named tools from different servers, or from a builtin
tool, never collide.

*transport*

Either
"stdio"
(the default if omitted) or
"http".

*command*

For
"stdio":
an argv array; the server is spawned as a subprocess and speaks
JSON-RPC over its stdin/stdout.
A crashing stdio server is automatically restarted, with a small
backoff budget so a genuine crash loop does not turn into a
fork/exec storm; its tools disappear from the model's view while it
is down and reappear once it is back.

*url*

For
"http":
the server's endpoint.
One JSON-RPC POST per call, no persistent connection.
This transport is newer and less exercised than
"stdio";
it expects a plain JSON response per call, not an SSE-streamed one.

*api\_key*

Optional bearer token for an
"http"
server.

*timeout\_ms*

Optional per-call deadline for either transport.
Defaults to 30000 (30s).

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
[clm(1)](clm.md)
warns, via
`CLM_DEBUG_LOG`,
if this file is readable by group or other.

# EXAMPLES

A minimal single-provider, single-model configuration:

	return {
	    model = "ollama/qwen3-32b",
	    providers = {
	        ollama = {
	            kind = "ollama",
	            url = "http://127.0.0.1:8081/v1",
	            api_key = clm.secrets.ollama,
	        },
	    },
	    tools = {
	        web_search = { api_key = clm.secrets.tavily },
	        weather = { units = "metric" },
	    },
	    volatile_tools = { "local_map", "character_status" },
	}

A second model sharing the same connection, with per-model context
overrides, plus a backup provider for failover
([clm(1)](clm.md)'s **--provider**, or the TUI's **/provider command**):

	return {
	    model = "ollama/qwen3-8b",
	    providers = {
	        ollama = {
	            kind = "ollama",
	            url = "http://127.0.0.1:8081/v1",
	            api_key = clm.secrets.ollama,
	            models = {
	                ["qwen3-8b"] = {
	                    context_size = 32768,
	                },
	                ["qwen3-32b"] = {
	                    context_size = 131072,
	                    autocompact_pct = 80,
	                },
	            },
	        },
	        ollama_backup = {
	            kind = "ollama",
	            url = "http://10.0.0.2:8081/v1",
	            api_key = clm.secrets.ollama,
	        },
	    },
	}

Direct first-party Anthropic API access
(*kind* = "anthropic"):

	return {
	    model = "anthropic/claude-sonnet-5",
	    providers = {
	        anthropic = {
	            kind = "anthropic",
	            url = "https://api.anthropic.com/v1",
	            api_key = clm.secrets.anthropic,
	        },
	    },
	}

The corresponding
*secrets.lua*:

	return {
	    tavily = "tvly-...",
	    ollama = "...",
	    anthropic = "sk-ant-...",
	}

# SEE ALSO

[clm(1)](clm.md),
[clm-tool(5)](clm-tool.md)

clm - July 6, 2026
