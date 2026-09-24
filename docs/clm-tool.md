CLM-TOOL(5) - File Formats Manual

# NAME

**clm-tool** - clm Lua plugin and tool API

# DESCRIPTION

[clm(1)](clm.md)
loads every
*\*.lua*
file in
*~/.config/clm/plugins/*
(or a directory given with
**--plugins**,
or an agent-specific
*~/.config/clm/agents/*&zwnj;*agent*&zwnj;*/*
directory) as a plugin.
Files in the
*opt/*
subdirectory are opt-in plugins: one loads only when its name, without
*.lua*,
appears in the
*plugins*
list of
[clm-config(5)](clm-config.md)
or is given with
**-P**.
The seeded copies of the builtin plugins are the ones that run.
[clm(1)](clm.md)'s
setup step copies them once and never touches them again, so a later
change to the installed source has no effect until the copy is replaced
by hand.
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
*clm.read\_file*,
*clm.write\_file*,
*clm.spawn*
and
*clm.exec*
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
[clm_agent(3)](clm_agent.md)'s
*on\_permission*
callback); set
*no\_prompt = true*
for a tool with no side effects worth confirming.
The prompt lists arguments in the order
*params\_schema.required*
names them, then any remaining ones, so put the parameters a human needs
to read first, in that order, in
*required*.
*hidden = true*
omits the tool from the schema advertised to the model entirely, for
a tool meant to be invoked only by other plugin code, not by the
model itself.

**clm.tool\_remove**(*name*)
removes a tool the same plugin registered, and returns true, or false
when the plugin has no such tool.
It is safe to call from inside that tool's own
*invoke*,
for example to offer a one-time setup tool only until it has run.
Removing a tool changes the tool list sent with each request, so the
server's prompt cache misses once.

## The ctx object

**ctx:complete**(*result*)

Ends the invocation successfully.
*result*
is a string; the model sees it as the tool's output.

**ctx:fail**(*message*)

Ends the invocation with a failure the model sees as
"\[tool failed: X]"
(where
'X'
is
*message*).

**ctx:args\_raw**()

Returns the raw, undecoded JSON arguments string
("{}"
if the model supplied none)
.
Most plugins use the decoded
*args*
table passed to
*invoke*
instead; this is for a plugin that wants to reparse or forward the
arguments verbatim.

**ctx:log**(*message*)

Writes
*message*
to the debug log (see
`CLM_DEBUG_LOG`
in
[clm(1)](clm.md)).
A no-op, at negligible cost, when that variable is unset.

**ctx:http\_get**(*url*, *headers*)

**ctx:http\_post**(*url*, *body*, *headers*)

Described under
*HTTP requests*;
*headers*
is optional in both.

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

Read or write a file, without the sandbox's normal lack of filesystem
access.
Available at any point in a plugin, not just from within
*invoke*.

**clm.sleep**(*ms*)

Yield the calling coroutine for
*ms*
milliseconds without blocking the event loop.
Same coroutine restriction as
*HTTP requests*
above.

**clm.spawn**(*argv*, *opts*)

Start the program
*argv\[1]*
with the arguments in the table
*argv*,
and return a handle.
Callable at any point, also at load time.
The child gets the environment of
**clm**
and its own process group.
*opts*
is an optional table:

*stdin*

a string written to the standard input of the child, which is then
closed.
Without it the child reads end of file.

*on\_line*

called with each line of standard output, without the newline.

*on\_stderr*

the same for standard error.

*on\_exit*

called once, after the child exits, with
*code*,
*signal*
and
*stderr*.
*code*
is
`nil`
when a signal killed the child.
*stderr*
holds the first 64 KiB of standard error when there is no
*on\_stderr*.

Callbacks run from the event loop with a 2 s deadline each.
**handle:kill**(*sig*)
sends
*sig*
(a number, or
"TERM",
"KILL",
"INT",
"HUP";
default
"TERM")
to the process group of the child.
**handle:running**()
is true until the child exits.
When the plugin is unloaded, running children get SIGTERM, then SIGKILL
after 5 s.

**clm.exec**(*argv*, *opts*)

Run
*argv*
like
**clm.spawn**(),
wait for it to exit, and return a table with
*code*,
*signal*,
*stdout*,
*stderr*
and
*truncated*.
Each output stream keeps its first 1 MiB.
*opts*
takes only
*stdin*.
Same coroutine restriction as
**clm.sleep**().
Cancelling the tool call stops the child.

**clm.after**(*ms*, *fn*)

Call
*fn*
once, after
*ms*
milliseconds, and return a handle.
**handle:cancel**()
stops a timer that has not fired.

**clm.notify**(*text*)

Deliver
*text*
to the agent as a new message.
It starts a turn when the agent is idle, or joins the running turn.
Delivery happens later, from the event loop.

**clm.getenv**(*name*)

The value of environment variable
*name*,
or
`nil`.

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
[clm-config(5)](clm-config.md)),
that subtable is available inside the plugin as
*clm.config*.
An agent file can have its own
*tools*
table; its entry for a plugin replaces the top-level entry.
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

[clm(1)](clm.md),
[clm_agent(3)](clm_agent.md),
[clm-config(5)](clm-config.md),
[clm-session(5)](clm-session.md)

clm - July 6, 2026
