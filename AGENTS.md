# AGENTS.md

## clm - Embeddable LLM Agent in C

`clm` is an embeddable LLM agent library and CLI written in C. It provides a
core agent engine with tool execution capabilities (shell commands, file
read/write) and integrates with OpenAI-compatible chat completion APIs.

### Features

- Core agent engine with state management (idle, thinking, calling tool, complete, error)
- Tool execution: shell commands, file read, file write
- OpenAI-compatible API integration (ollama, openai, anthropic providers)
- libuv event loop integration for async I/O
- libcurl for HTTP requests
- json-c for JSON parsing
- Meson build system with hardening flags and UBSan
- Shared library (`libclm`) with versioned symbol map and pkg-config
- CLI binary (`clm`) with interactive REPL and oneshot mode

### Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

For ASan + UBSan (Linux):

```sh
meson setup build-asan \
  -Db_sanitize=address,undefined \
  -Db_lundef=false \
  -Ddefault_library=static \
  -Dc_link_args='-static-libasan'
meson test -C build-asan
```

ASan is not available on OpenBSD. See README.md for the OpenBSD trap-based
UBSan alternative.

### Tooling

These require clang-tools-extra and cppcheck (ports on OpenBSD, standard
packages on Linux). The CI runner installs llvm-21, clang-tools-extra, and
cppcheck.

| Command | What it does |
|---|---|
| `ninja -C build clang-format` | Format all source files in place (BSD KNF via `.clang-format`) |
| `ninja -C build clang-format-check` | Verify formatting without modifying files |
| `ninja -C build clang-tidy` | Static analysis via clang-tidy (bugprone, clang-analyzer) |
| `ninja -C build cppcheck` | Static analysis via cppcheck |
| `meson test -C build --suite docs` | Man page lint and symbol/doc coverage check |

### Conventions

See [CODING_STYLE.md](CODING_STYLE.md) for the full coding standard.

Tests live in `test/`, fuzz corpora in `fuzz/corpus/`.
