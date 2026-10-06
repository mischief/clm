# clm for Lua

`require("clm")` drives clm agents from a Lua 5.4 script. It is for
experiments: orchestration, evaluations, several agents at once. Plugins
that run inside the `clm` program use the sandboxed API in clm-tool(5)
instead.

The build makes `clm.so` and installs it in `LIBDIR/lua/5.4`. Point
`LUA_CPATH` at it, for example
`LUA_CPATH="$HOME/.local/lib64/lua/5.4/?.so;;"`.

## Example

```lua
local clm = require("clm")

local a = clm.agent{
    url = "http://127.0.0.1:8091/v1",
    model = "qwen3.8-27b",
    on_text = function(s) io.write(s) end,
}
a:tool{
    name = "add", description = "add two numbers", no_prompt = true,
    params = { type = "object", properties = {
        x = { type = "number" }, y = { type = "number" } } },
    invoke = function(args) return tostring(args.x + args.y) end,
}

clm.run(function()
    local text, status = a:turn("what is 2 + 3? use the add tool")
    print(text, status)
end)
```

## Module

- `clm.agent(opts)` makes an agent. `opts`:
  - `url` (required): the base URL, such as `http://host/v1`.
  - `provider`: `openai` (default), `openai-responses`, `anthropic` or
    `ollama`.
  - `model`, `api_key`, `system_prompt`, `effort`, `context_size`,
    `max_iterations`.
  - `stream`: default true.
  - `tools`: built-in tools to add, from `"shell"`, `"bg"` and
    `"monitor"`.
  - `cache_system`: with the `openai-responses` provider, mark the end
    of the system prompt as a cache breakpoint, so agents that share
    it read it from the cache instead of writing it again.
  - `builtins`: `false` drops the file tools every agent has
    (`read_file`, `write_file`, `list_dir`, `read_image`); a list keeps
    only the ones named.
  - `permission`: `"allow"`, `"deny"` (default) or
    `function(name, args, reason)` that returns true to allow. `"allow"`
    still denies a call that a `pre_tool` hook asked about.
  - Callbacks: `on_text(s)`, `on_reasoning(s)`,
    `on_tool_begin(name, args)`, `on_tool(name, output, how)` where `how`
    is `ok`, `failed` or `timedout`, `on_notice(s)`, and `on_usage(t)`
    with `prompt`, `completion`, `cached` and `cache_written`.
- `clm.run(fn, ...)` runs `fn` in a coroutine and drives the event loop
  until `fn` returns. An error in `fn` is raised again.
- `clm.spawn(fn, ...)` starts `fn` in its own coroutine. Its errors are
  printed.
- `clm.sleep(ms)` waits inside a coroutine.
- `clm.post(url, body[, headers])` sends an HTTP POST from inside a
  coroutine and returns `status, body`, or `nil, err`. The content type
  is JSON unless `headers` (a name to value table) says otherwise.
- `clm.exec(cmd[, opts])` runs `cmd` with `/bin/sh -c` from inside a
  coroutine, while the loop keeps running. It returns `code, output`, or
  `nil, output, why` when the command timed out or died by a signal.
  `output` holds stdout and stderr together. `opts`: `cwd`, `stdin` (a
  string), `timeout_ms` (default 120000; 0 for none) and `max` (bytes of
  output kept, default 1 MiB). The command runs in a process group of its
  own; when it times out, or exits while a job it started still holds its
  output, the group is killed after a grace period (5 seconds, or
  `CLM_SHELL_KILL_GRACE_MS`). Commands still running when the Lua state
  closes are killed. Plugins cannot call it.
- `clm.step()` runs ready work once, without waiting.

## Agent

- `a:turn(prompt)` returns `text, 0`, or `nil, err, status`. Inside a
  coroutine it waits there while other coroutines run; outside one it
  runs the loop until the turn ends.
- `a:tool{name, description, params, invoke, no_prompt, timeout_ms}`.
  `invoke(args)` runs in a coroutine, so it may call `clm.sleep` or
  another agent's `turn`. Its return value is the result; an error fails
  the call.
- `a:on("pre_tool", fn)`: `fn(call)` with `call.name` and `call.args`
  returns nil, or a table with `allow`, `deny` or `ask` (a reason) and
  `args` (new arguments). It runs in a coroutine. One per agent.
- `a:on("turn_start", fn)` with `info.prompt`, and `a:on("turn_end", fn)`
  with `info.status` and `info.text`. These run as plain calls.
- `a:prompt_set(key, text)` sets a system prompt part; nil removes it.
- `a:notify(text)` delivers an event: it starts a turn when the agent is
  idle, or joins the running one.
- `a:clear()` starts a fresh conversation with the same settings. It
  fails while a turn runs.
- `a:cancel()`, `a:history()` (the messages as tables), `a:state()`,
  `a:error()`, `a:close()`. Closing ends a turn that waits on the agent.

All agents share one event loop, so call `clm.run` from the main chunk,
not from inside a coroutine.
