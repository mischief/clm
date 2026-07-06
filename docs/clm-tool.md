CLM-TOOL(5) - File Formats Manual

# NAME

**clm-tool** - clm Lua plugin and tool API

# DESCRIPTION

clm(1)
loads every
*\*.lua*
file in
*~/.config/clm/plugins/*
(or a directory given with
**--plugins**,
or an agent-specific
*~/.config/clm/agents/*&zwnj;*agent*&zwnj;*/*
directory) as a plugin.
Each plugin file gets its own sandboxed
lua(1)
state and is expected to call
*clm.tool\_register*
at least once, at load time, to register the tools it provides.

## Sandbox

Only the
*string*, *table*, *math*,
and
*utf8*
standard libraries are loaded;
*os*
and
*io*
are never loaded at all, and
*dofile*, *loadfile*, *load*, *require*,
and
*collectgarbage*
are removed from
*\_G*
after loading the safe libraries.
There is no filesystem or process access except through
*clm.read\_file*
and
*clm.write\_file*
(below), no dynamic code loading, and no way to reach any other
plugin's state.

A plugin has an 8 MiB private memory cap, a 500 ms deadline to
finish executing its top-level (load-time) code, and a separate,
per-tool-call CPU deadline (the
*timeout\_ms*
field of
*clm.tool\_register*,
below; 30 s if unset), checked every 10000 VM instructions so a
tight infinite loop cannot dodge it.
Exceeding either deadline aborts execution of that plugin or call;
a load-time failure is logged and that one plugin's tools are simply
not registered; the rest of the plugin directory still loads.

## Registering a tool

	clm.tool_register(name, {
	    description = "...",       -- optional
	    params_schema = { ... },   -- optional, a JSON-Schema-shaped table
	    invoke = function(args, ctx)
	        ...
	        ctx:complete(result)   -- or ctx:fail(message)
	    end,
	    timeout_ms = 30000,        -- optional, overrides the default
	    no_prompt = false,         -- optional, skip the permission prompt
	    hidden = false,            -- optional, hide from the model's schema
	})

*name*
must be unique across every plugin and builtin tool loaded into the
agent.
*invoke*
is called once per tool invocation with
*args*
(the decoded JSON arguments the model supplied, following
*params\_schema*)
and
*ctx*,
described below.
Exactly one of
*ctx:complete*
or
*ctx:fail*
must be called, from
*invoke*
or from a coroutine it yields into (see
*HTTP requests*
below); calling either a second time is an error.

By default a tool is gated behind an interactive allow/deny prompt the
first time it is called in a session (see
clm\_agent(3)'s
*on\_permission*
callback); set
*no\_prompt = true*
for a tool with no side effects worth confirming.
*hidden = true*
omits the tool from the schema advertised to the model entirely, for
a tool meant to be invoked only by other plugin code, not by the
model itself.

## The ctx object

**ctx:complete**(*result*)

> Ends the invocation successfully.
> *result*
> is a string; the model sees it as the tool's output.

**ctx:fail**(*message*)

> Ends the invocation with a failure the model sees as
> "\[tool failed: X]"
> (where
> 'X'
> is
> *message*).

**ctx:args\_raw**()

> Returns the raw, undecoded JSON arguments string
> ("{}"
> if the model supplied none)
> .
> Most plugins use the decoded
> *args*
> table passed to
> *invoke*
> instead; this is for a plugin that wants to reparse or forward the
> arguments verbatim.

**ctx:log**(*message*)

> Writes
> *message*
> to the debug log (see
> `CLM_DEBUG_LOG`
> in
> clm(1)).
> A no-op, at negligible cost, when that variable is unset.

**ctx:http\_get**(*url*, *headers*)

**ctx:http\_post**(*url*, *body*, *headers*)

> Described under
> *HTTP requests*;
> *headers*
> is optional in both.

## HTTP requests

	local resp, err = ctx:http_get(url)
	local resp, err = ctx:http_get(url, {["Authorization"] = "Bearer ..."})
	local resp, err = ctx:http_post(url, body)
	local resp, err = ctx:http_post(url, body, {["Content-Type"] = "text/plain"})

Both yield the calling coroutine and resume it once the request
completes, so
*invoke*
does not block the agent's event loop while a request is in flight.
Only callable directly from a tool invocation's own coroutine, not
from a nested coroutine or at plugin load time.
*headers*
is an optional table of header-name to header-value strings;
**ctx:http\_post**()
seeds
"Content-Type: application/json"
by default unless
*headers*
overrides it.

On success,
*resp*
is a table with
*resp.status*
(the HTTP status code) and
*resp.body*
(the response body as a string);
*err*
is
`nil`.
On failure,
*resp*
is
`nil`
and
*err*
is a string describing what went wrong.
Up to 8 requests per plugin may be in flight concurrently, and a
single tool call may make at most 128 requests in total; exceeding
either limit fails the request rather than queuing it.

## Other clm module functions

**clm.read\_file**(*path*)

**clm.write\_file**(*path*, *content*)

> Read or write a file, without the sandbox's normal lack of filesystem
> access.
> Available at any point in a plugin, not just from within
> *invoke*.

**clm.sleep**(*ms*)

> Yield the calling coroutine for
> *ms*
> milliseconds without blocking the event loop.
> Same coroutine restriction as
> *HTTP requests*
> above.

## The json module

	local text = json.encode(value)
	local value, err = json.decode(text)

A global
*json*
table over
json-c(3).
*json.null*
is a sentinel light userdata value representing JSON
`null`,
distinct from Lua
*nil*.
*json.decode*
rejects input larger than 2 MiB.

## Per-tool configuration

If the plugin's file or tool name appears as a key under
*tools*
in
*config.lua*
(see
clm-config(5)),
that subtable is available inside the plugin as
*clm.config*.
A plugin with no matching
*tools*
entry sees
*clm.config*
as an empty table, not
`nil`.

# EXAMPLES

	-- ~/.config/clm/plugins/hello.lua
	clm.tool_register("hello", {
	    description = "Say hello to someone",
	    params_schema = {
	        type = "object",
	        properties = {
	            name = { type = "string", description = "who to greet" },
	        },
	        required = { "name" },
	    },
	    invoke = function(args, ctx)
	        ctx:complete("Hello, " .. args.name .. "!")
	    end,
	})

# SEE ALSO

clm(1),
clm\_agent(3),
clm-config(5)

clm - July 6, 2026
