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
exactly one of
*ctx:complete*
or
*ctx:fail*
must be called, from
*invoke*
or from a child started with
**clm.spawn**()
(see
*HTTP requests*
below); calling either a second time is an error.

By default a tool is gated behind an interactive allow/deny prompt the
first time it is called in a session (see
[clm_agent(3)](clm_agent.md)'s
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

## HTTP requests

	local resp, err = http.get(url)
	local resp, err = http.get(url, {["Authorization"] = "Bearer ..."})
	local resp, err = http.post(url, body)
	local resp, err = http.post(url, body, {["Content-Type"] = "text/plain"})

both yield the calling coroutine and resume it once the request
completes, so
*invoke*
does not block the agent's event loop while a request is in flight.
they may be called from the main tool coroutine or from a child started
with
**clm.spawn**(),
but not at plugin load time.
*headers*
is an optional table of header-name to header-value strings;
**http.post**()
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

yield the calling coroutine for
*ms*
milliseconds without blocking the event loop.
same coroutine restriction as
*HTTP requests*
above.

**clm.spawn**(*function*)

create and return a task handle for
*function*.
the function is scheduled for a later event-loop iteration and does not
execute before
**clm.spawn**()
returns.
the task belongs to the current tool invocation and may call
**http.get**(),
**http.post**(),
and
**clm.sleep**().
only the main tool coroutine may spawn tasks; a task may not spawn another
task.
values returned by
*function*
become the task result.

**task:wait**()

wait until
*task*
succeeds, fails, or is cancelled, then return
`true`.
waiting does not read or interpret the task result.

**task:result**()

return
`nil`
if
*task*
is pending.
a terminal task returns a stable outcome table.
a successful outcome has
*ok*
set to
`true`
and one normalized
*value*
field: one returned value is stored directly, multiple returned values are
stored in an array, and zero returned values omit
*value*.
a failed outcome has
*ok*
set to
`false`
and an
*error*
string.
a cancelled outcome also has
*cancelled*
set to
`true`.
reading a failed outcome marks its error as observed.

**task:cancel**(*reason*)

cancel a pending task and its asynchronous operation.
*reason*
is optional and defaults to
"task cancelled".
return
`true`
when the task was cancelled or was already cancelled, and
`false`
when it had already succeeded or failed.

**clm.wait\_any**(*tasks*, *timeout\_ms*)

wait until any task in the array table
*tasks*
is terminal and return its one-based input index.
*timeout\_ms*
is optional; expiration returns
`nil`
plus
"timeout"
without cancelling any task.
all tasks must be unique handles from the current invocation, and at most 128
tasks may be waited on at once.

**clm.await\_all**(*tasks*)

wait for every task and return an ordered array of outcome tables as described
under
**task:result**().
task failure does not cancel or otherwise affect another task.

**clm.try\_all**(*tasks*)

wait for tasks until all succeed or one fails.
on success, return normalized task values in input order.
on failure, cancel every unfinished task in this set and return
`nil`
plus the observed error string.

only the main tool coroutine may wait, read results, cancel tasks, or complete
or fail the tool.
returning from the main
*invoke*
function with live tasks cancels those tasks and fails the tool.
successfully completing a tool with live tasks or unobserved task errors also
fails the tool;
**clm.spawn**()
does not create detached background work.
the invocation remains alive while its main coroutine or child tasks are
suspended; tool cancellation, timeout, and plugin teardown cancel the whole
family and its pending asynchronous work.

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

	local tasks = {
	    clm.spawn(function()
	        local resp, err = http.get(url_a)
	        if resp == nil then error(err) end
	        return resp.body
	    end),
	    clm.spawn(function()
	        local resp, err = http.get(url_b)
	        if resp == nil then error(err) end
	        return resp.body
	    end),
	}
	local outcomes = clm.await_all(tasks)
	for _, outcome in ipairs(outcomes) do
	    if not outcome.ok then
	        ctx:fail(outcome.error)
	        return
	    end
	end
	ctx:complete(outcomes[1].value .. outcomes[2].value)

# SEE ALSO

[clm(1)](clm.md),
[clm_agent(3)](clm_agent.md),
[clm-config(5)](clm-config.md)

clm - July 6, 2026
