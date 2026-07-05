# clm

An embeddable LLM agent in C with a Lua plugin system.

Copyright (c) 2026 Nick Owens <mischief@offblast.org>  
ISC License — see [LICENSE](LICENSE)

## Features

- Interactive ncurses TUI and headless CLI modes
- OpenAI-compatible API (works with llama.cpp, Ollama, OpenAI, Anthropic)
- Built-in tools: shell exec, file read/write
- Lua 5.4 plugin system for custom tools (sandboxed, async HTTP)
- Per-tool permission prompts (allow once / always / deny / never)
- Token-bucket rate limiter on tool dispatch
- Streaming (SSE) and non-streaming response modes
- Portable core: transport + timers come in through a small host interface, so
  the agent engine itself depends only on json-c (no libuv/libcurl)

## Architecture

The library is split into a portable core and a desktop transport layer:

- **`libclm`** — the agent engine, tool registry, history, and
  OpenAI-compatible client. It has no libuv or libcurl dependency. HTTP and
  timers are provided by the embedder through a `struct clm_host` (see
  `clm/host.h`): four function pointers for `http_post` / `http_cancel` /
  `timer_set` / `timer_cancel`. `clm_agent_new()` takes a `clm_host *`. A host
  may leave `timer_set` NULL, in which case per-tool timeouts are disabled.
- **`libclmuv`** (static) — the desktop host: a libcurl + libuv implementation
  of `clm_host` (`clm_host_uv_new()`), plus the `shell_exec` tool (which needs
  `uv_spawn`, so it lives here rather than in the core:
  `clm_tools_register_shell()`). Link it alongside `libclm` for a full desktop
  agent.

An embedder targeting a different platform supplies its own `clm_host` and links
only `libclm`. For example, the ESP32 port implements `clm_host` over
`esp_http_client` (with SSE streaming) and never pulls in libuv or libcurl.

## Build

Requires: meson ≥ 1.1, a C17 compiler, libcurl, libuv, json-c, ncursesw,
md4c, and optionally Lua 5.4.

**OpenBSD:**

```sh
pkg_add lua%5.4 md4c libuv json-c curl
```

**Debian/Ubuntu:**

```sh
apt install meson libcurl4-openssl-dev libuv1-dev libjson-c-dev \
    libncursesw5-dev libmd4c-dev liblua5.4-dev
```

```sh
meson setup build
meson compile -C build
meson test -C build
```

Disable Lua plugins:

```sh
meson setup build -Dlua=disabled
```

## Usage

```sh
# Interactive TUI (default when on a terminal)
clm -u http://localhost:8080

# Headless oneshot
clm -u http://localhost:8080 -o "what time is it?"

# With a specific model
clm -u http://localhost:8080 -m qwen3-32b
```

Options:

| Flag | Description |
|------|-------------|
| `-u, --url BASE` | API endpoint (default `http://127.0.0.1:8081`) |
| `-m, --model NAME` | Model name to request |
| `-o, --oneshot PROMPT` | Run one prompt and exit |
| `-p, --plugins DIR` | Plugin directory (default `$XDG_CONFIG_HOME/clm/plugins`) |
| `-H, --headless` | Force plain stdio REPL |
| `-S, --no-stream` | Disable streaming |

The API key is read from the `CLM_API_KEY` environment variable.

## Plugins

Plugins are Lua scripts in `~/.config/clm/plugins/`. Each runs in a
sandboxed VM (no `os`, `io`, `require`) with an 8 MiB memory cap and a
CPU timeout.

```lua
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
```

Plugins can make async HTTP requests (the coroutine yields and resumes
on completion):

```lua
local resp, err = http.get("https://example.com/api")
local resp, err = http.post(url, body, {["Authorization"] = "Bearer ..."})
```

### Plugin config

Per-plugin configuration lives in `~/.config/clm/config.lua`:

```lua
return {
    tools = {
        web_search = { api_key = "tvly-..." },
        weather = { units = "metric" },
    },
}
```

Each plugin receives only its own section as `clm.config`.

### MCP servers

clm can also pull in tools from external [MCP](https://modelcontextprotocol.io)
servers, configured in the same `~/.config/clm/config.lua`:

```lua
return {
    mcp_servers = {
        -- Spawned as a subprocess; speaks JSON-RPC over its stdin/stdout.
        { name = "fs", transport = "stdio", command = {"mcp-server-fs", "/home/me/notes"} },

        -- A remote server; one JSON-RPC POST per call, no persistent connection.
        { name = "search", transport = "http", url = "https://example.com/mcp", api_key = "..." },
    },
}
```

`transport` defaults to `"stdio"` if omitted. `timeout_ms` is optional on
either kind (per-call deadline; defaults to 30s). Each remote tool is
registered as `mcp__<server name>__<tool name>` (matching the scheme Claude
Code uses for MCP-sourced tools), so identically-named tools from different
servers -- or from a built-in like `read_file` -- never collide.

A stdio server that crashes is automatically restarted (with a small backoff
budget, so a genuine crash loop doesn't turn into a fork/exec storm); its
tools disappear from the model's view while it's down and reappear once it's
back. The HTTP transport is newer and less exercised: it expects a plain JSON
response per call, not the SSE-streamed variant some MCP servers use.

## Platforms

- Linux (primary development)
- OpenBSD (tested, runs in production)

## Sanitizers

**Linux** — ASan + UBSan:

```sh
meson setup build-asan \
  -Db_sanitize=address,undefined \
  -Db_lundef=false \
  -Ddefault_library=static \
  -Dc_link_args='-static-libasan'
meson test -C build-asan
```

**OpenBSD** — trap-based UBSan:

```sh
meson setup build-ubsan \
  -Dc_args="-fsanitize=undefined -fsanitize-trap=undefined" \
  -Dc_link_args="-Wl,--no-execute-only"
meson test -C build-ubsan
```

## Tooling

```sh
ninja -C build clang-format       # format source
ninja -C build clang-tidy         # static analysis
ninja -C build cppcheck           # additional checks
meson test -C build --suite docs  # man page lint
```
