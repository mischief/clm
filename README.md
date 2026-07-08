# clm

An embeddable LLM agent in C with a Lua plugin system.

Copyright (c) 2026 Nick Owens <mischief@offblast.org>  
ISC License — see [LICENSE](LICENSE)

## Features

- Interactive ncurses TUI and headless CLI modes
- Two wire dialects: OpenAI-compatible (works with llama.cpp, Ollama, OpenAI,
  Groq, and most other hosted/self-hosted APIs) and Anthropic's native
  Messages API, translated transparently at the request/response layer —
  see [`clm-config(5)`](docs/clm-config.md)'s `kind` field
- Built-in tools: shell exec, file read/write
- Lua 5.4 plugin system for custom tools (sandboxed, async HTTP)
- [MCP](https://modelcontextprotocol.io) client: pull in tools from external
  stdio or HTTP servers
- Per-tool permission prompts (allow once / always / deny / never)
- Two independent token-bucket rate limiters: a small fixed one pacing tool
  dispatch, and a configurable one (`rate_tokens_per_sec`/`rate_burst` per
  provider) pacing outgoing LLM requests against a backend's real quota
- Streaming (SSE) and non-streaming response modes
- Portable core: transport + timers come in through a small host interface, so
  the agent engine itself depends only on cJSON (no libuv/libcurl) — see
  [`esp32/README.md`](esp32/README.md) for a from-scratch embedder example

## Build

Requires: meson ≥ 1.1, a C17 compiler, libcurl, libuv, cJSON, ncursesw,
md4c, and Lua 5.4.

**OpenBSD:**

```sh
pkg_add lua%5.4 md4c libuv cjson curl
```

**Debian/Ubuntu:**

```sh
apt install meson libcurl4-openssl-dev libuv1-dev libcjson-dev \
    libncursesw5-dev libmd4c-dev liblua5.4-dev
```

```sh
meson setup build
meson compile -C build
meson test -C build
```

`-Dlua=disabled` builds `libclm` (the portable core) without Lua support at
all -- for embedding the core into something with no terminal and no Lua
(the ESP32 port, for instance). It also skips `libclmlua` and, with it, the
`clm` binary itself: the ncurses/CLI frontend always requires Lua (agent
profiles, plugins), so there's nothing for this flag to build there. Use it
when you only want `libclm` as a library, not when building `clm` the
program:

```sh
meson setup build -Dlua=disabled
```

## Usage

```sh
# First run: writes a starter config.lua under $XDG_CONFIG_HOME/clm/ --
# nine ready-to-use provider connections (Anthropic, plus eight
# OpenAI-compatible ones with a free tier: Groq, Cerebras, NVIDIA,
# OpenRouter, GitHub Models, Ollama Cloud, LLM7, Google) -- and a
# matching secrets.lua with a blank slot for each key. Also seeds the
# builtin plugins. Safe to re-run -- never overwrites existing files.
clm setup
```

The only step left is a key. Open `~/.config/clm/secrets.lua` and fill
in one entry -- each provider in `config.lua` has a comment right above
it linking to that service's free-key signup page, e.g.:

```lua
-- ~/.config/clm/secrets.lua
return {
    groq = "gsk_...",   -- from https://console.groq.com/keys
    ...
}
```

A provider with no key just sits inert (`nil`, not an error) until you
pick it via `-m`/`--model` or `config.lua`'s top-level `model` field, so
there's no need to touch the ones you don't use. Then:

```sh
# Interactive TUI (default when on a terminal); "provider/model-id"
# picks the config.lua connection and model to use
clm -m groq/llama-3.3-70b-versatile

# Headless oneshot -- same spec form
clm -m anthropic/claude-sonnet-5 -o "what time is it?"

# No config.lua at all -- point straight at an endpoint (e.g. a local
# llama.cpp server). No wire dialect to resolve without a config, so
# this always speaks OpenAI-compatible (/chat/completions).
clm -u http://localhost:8080/v1
```

Free-tier keys are real but rate-limited for light use, not heavy
agentic sessions -- see the comments in the canned `config.lua` (and
[`clm-config(5)`](docs/clm-config.md)'s `rate_tokens_per_sec` /
`rate_burst`) before relying on one as a daily driver. Anthropic has no
free tier but is the most dependable option once you have a key.

See [`clm-config(5)`](docs/clm-config.md) for the full `config.lua`
schema (providers, per-model overrides, agent profiles, MCP servers,
per-plugin tool config) and [`clm(1)`](docs/clm.md) for the complete
CLI reference. Options:

| Flag | Description |
|------|-------------|
| `-m, --model PROVIDER/MODEL-ID` | A `config.lua` `providers[PROVIDER].models[MODEL-ID]` entry, or a literal model id (no `/`) on whatever connection is otherwise active |
| `--provider NAME` | Override which `config.lua` `providers[]` entry supplies the connection, without changing the requested model |
| `-u, --url BASE` | Base API endpoint (default `http://127.0.0.1:8081/v1`); the request path appended depends on the wire dialect (`/messages` for Anthropic, `/chat/completions` otherwise) |
| `-a, --agent NAME` | Load an agent profile from `~/.config/clm/agents/<name>.lua` |
| `-o, --oneshot PROMPT` | Run one prompt headless and exit |
| `-f, --forever PROMPT` | TUI mode: submit `PROMPT`, then auto-resubmit it whenever a turn completes with nothing queued |
| `-p, --plugins DIR` | Plugin directory (default `$XDG_CONFIG_HOME/clm/plugins`) |
| `-H, --headless` | Force the plain stdio REPL |
| `-S, --no-stream` | Disable streamed (SSE) responses |
| `-V, --version` | Print version and exit |

With no options, `clm` runs the interactive ncurses UI on a terminal.

API keys are usually set once in `secrets.lua` and referenced from
`config.lua` as `clm.secrets.<name>` (see [Secrets](#secrets) below,
and `clm setup`'s canned files) — this is the normal path. The
`CLM_API_KEY` environment variable, when set, overrides whatever
`config.lua` would otherwise use for the active connection.

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

### Secrets

API keys and other secrets live in a separate `~/.config/clm/secrets.lua`
(mode `600`), not in `config.lua` itself, so `config.lua` can be shared
or checked into dotfiles without leaking anything:

```lua
-- ~/.config/clm/secrets.lua
return {
    tavily = "tvly-...",
}
```

`clm.secrets` itself (the live lookup table) is only ever visible where
it's resolved: `config.lua` and per-agent profile files under
`~/.config/clm/agents/`, which share one Lua state for exactly this
reason:

```lua
return {
    tools = {
        web_search = { api_key = clm.secrets.tavily },
    },
}
```

Each plugin runs in its own separate, sandboxed Lua state (see
[Plugins](#plugins) above) with no access to `clm.secrets` or to any
other plugin's config — but the *value* a secret resolved to still
reaches the plugin it's configured for, as plain data: `config.lua` is
evaluated once (substituting `clm.secrets.tavily` for its real string),
and each plugin's own slice of the resulting `tools` table is handed to
it as `clm.config`. So `web_search`'s `invoke` function can read
`clm.config.api_key` and get the real key, without the plugin sandbox
ever holding a reference to `clm.secrets` or seeing any other tool's
configuration.

`clm` warns (via `CLM_DEBUG_LOG`) if `secrets.lua` is readable by group
or other. `clm setup` writes a starter `secrets.lua` with the right
permissions.

### Volatile tools

Tools whose output is a state snapshot (a map read, a status query) leave
stale copies in the conversation as they are re-called; the history grows
without bound and the model can act on out-of-date data. Declaring them
volatile keeps only the newest result: when such a tool completes, every
prior result from it is replaced in place with a short
`[superseded by newer <tool>]` stub. Stubbed entries never change again,
so the request prefix stays byte-stable for backend prompt caching, and
call/result pairing is preserved (results are stubbed, never removed).

`volatile_tools` is a list of `fnmatch(3)` patterns, set in `config.lua`
or per agent profile:

```lua
return {
    volatile_tools = { "local_map", "character_status" },
}
```

## Platforms

- Linux (primary development)
- OpenBSD (tested, runs in production)
- ESP32-S3 — an embedder example for the portable core, no libuv/libcurl/Lua:
  see [`esp32/README.md`](esp32/README.md)

## Static builds

`-Dstatic=true` links libclm and every third-party dependency (cJSON,
lua5.4, libcurl, libuv, ncursesw, md4c, and curl's own chain) as static
archives, in one flag:

```sh
meson setup build-static -Dstatic=true -Dtests=false
meson compile -C build-static
```

On glibc this still leaves libc itself dynamic (`ldd` shows only
`libc`/`libm`/the loader) — glibc's static linking has NSS/`dlopen`
caveats that don't matter for most builds but are worth knowing about.
Every third-party lib needs a static archive on disk (`.a`, not just
`.so`) for this to work; on distros that don't ship one by default
(e.g. Gentoo's `static-libs` USE flag, or a `-static` / `-dev` package
elsewhere), you'll need to install or rebuild it first.

For a truly portable binary with **zero** dynamic dependencies —
including libc — build against musl instead, with a cross-file:

```sh
meson setup build-musl --cross-file cross/x86_64-linux-musl \
  -Dstatic=true -Dtests=false
meson compile -C build-musl
```

This needs a musl cross-toolchain (e.g. via Gentoo's `crossdev
--target x86_64-linux-musl`) with the same dependency stack built
static into its sysroot. `cross/x86_64-linux-musl` assumes the sysroot
lives at `/usr/x86_64-linux-musl`; adjust `sys_root` and the `x86_64-
linux-musl-*` tool names in that file for a different toolchain layout.
musl has no `<sys/queue.h>`; `compat/sys/queue.h` (vendored from glibc,
BSD-3-Clause) is picked up automatically as a fallback via `-idirafter`
only when the system doesn't provide one.

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
