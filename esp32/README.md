# clm on ESP32

A real, interactive, multi-turn LLM chat client running on ESP32-S3
hardware — no libuv, no libcurl, no Lua. It builds `libclm`'s portable
core (the agent loop, tool-calling, history) against a synchronous
`esp_http_client`-based transport instead of the desktop's libuv+libcurl
host, over WiFi, with a keyboard and on-device display for input/output
and a USB-serial console as a fallback/companion.

This works because `libclm`'s core has no direct dependency on libuv,
libcurl, or Lua — HTTP and timers come in through a small `struct
clm_host` vtable (`clm/host.h`) that the embedder supplies. The desktop
`clm` binary's host is a libuv+libcurl adapter; this port's is a blocking
`esp_http_client` one. See `components/libclm/CMakeLists.txt` for exactly
which files that excludes (the libuv/libcurl/Lua-dependent ones —
`http_async.c`, `host_uv.c`, `tool_shell.c`, `tool_bg.c`, `mcp_client.c`,
`lua_*.c`) versus what's compiled in unchanged from the desktop tree
(`agent.c`, `tools.c`, `history.c`, `provider*.c`, `llm.c`, `ratelimit.c`,
...).

## Supported hardware

Both are ESP32-S3, built via [PlatformIO](https://platformio.org/) +
ESP-IDF:

| Board | Chip | Flash | PSRAM | Board file |
|---|---|---|---|---|
| [LilyGo T-Deck](https://www.lilygo.cc/products/t-deck) | ESP32-S3-WROOM-1 N16R8 | 16 MB | 8 MB octal | `boards/t-deck.json` |
| [M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer) | ESP32-S3FN8 | 8 MB | none | `boards/cardputer.json` |

Both have a physical keyboard and a display; `firmware/board_tdeck.c` /
`firmware/board_cardputer.c` implement the per-board bring-up
(`board_init()`, `keyboard_read()`) behind the shared `board.h`
interface, selected at build time by a `-DBOARD_TDECK` /
`-DBOARD_CARDPUTER` flag (set per-environment in `platformio.ini`).

## Build & flash

```sh
cd esp32
pio run -e cardputer          # or -e t-deck
pio run -e cardputer -t upload
pio run -e cardputer -t monitor
```

Measured on a cardputer build: **RAM 13.8%** (45,376 / 327,680 bytes),
**flash 23.3%** (977,943 / 4,194,304 bytes) — plenty of headroom, but
expect these to drift as the core grows; re-check with `pio run -e
cardputer -t size` rather than trusting this number long-term.

`upload_port`/`monitor_port` in `platformio.ini` are hardcoded
(`/dev/ttyACM0` for t-deck, `/dev/ttyACM1` for cardputer) — adjust for
your setup, or drop them and pass `--upload-port`/`--monitor-port` on
the command line instead.

## Configuration

```sh
cp firmware/clm_config.h.example firmware/clm_config.h
```

Edit `firmware/clm_config.h`:

```c
#define WIFI_SSID    "your_ssid"
#define WIFI_PASS    "your_password"
#define CLM_BASE_URL "http://192.168.0.190:8080/v1/chat/completions"
#define CLM_API_KEY  ""
#define CLM_MODEL    "local-model"
```

`CLM_BASE_URL` is the **full** request URL (unlike the desktop CLI,
nothing here appends `/chat/completions` or `/messages` for you — see
the current limitation below).

**Current limitation: OpenAI-compatible dialect only.** `firmware/main.c`
hardcodes `cfg.provider = CLM_PROVIDER_OPENAI`. The underlying wire-dialect
translation code (`provider_anthropic.c`) *is* compiled into this port
unchanged — it's just not wired up to anything selectable yet. Pointing
`CLM_BASE_URL` at Anthropic's Messages API today would send an
OpenAI-shaped request to an endpoint that doesn't speak that dialect.
Making the provider configurable (a `CLM_PROVIDER` define, or a runtime
setting) is open work, not an architectural limitation.

## Using it

Flashing boots straight into an interactive chat REPL over the USB
serial console (and the on-device display, if `display_init()`
succeeds):

```
=== clm serial chat ===
model: local-model @ http://192.168.0.190:8080/v1/chat/completions
type a message and press enter. '/quit' resets the session.

you> what's 2+2?
clm> 4
```

The agent keeps its conversation history across turns (a real multi-turn
chat, not a one-shot demo). Two console-only commands:

- `/quit` or `/reset` — tear down and recreate the agent, clearing history
- `/shot` — dump the current display contents as a PPM image over the
  serial link (`console_screenshot()` in `firmware/display.c`)

Two tools are registered: the same built-in `read_file`/`write_file`
tools the desktop core ships (backed by whatever filesystem the ESP-IDF
build has mounted, if any), plus a device-specific `device_info` tool
returning chip and memory info (`firmware/main.c`'s `tool_device_info`).
No `shell_exec` — there's no subprocess concept on-device, which is
exactly why `tool_shell.c`/`tool_bg.c` are excluded from this build (see
above).
