# Fuzz targets using libFuzzer (CC=clang required)
# These fuzz how clm handles parsed data, not upstream libraries.
#
# Build:  CC=clang meson setup build-fuzz -Dtests=true
# Run:    ./build-fuzz/fuzz/fuzz_tool_dispatch

fuzz_lib_src = files(
  '../libclm/agent.c', '../libclm/tools.c', '../libclm/history.c',
  '../libclm/llm.c', '../libclm/ratelimit.c', '../libclm/log.c',
  '../libclm/http.c', '../libclm/props.c', '../libclm/provider.c',
  '../libclm/provider_anthropic.c', '../libclm/provider_openai.c',
  '../libclm/models_list.c',
)

# fuzz_tool_dispatch: fuzz clm_tools_dispatch() with adversarial JSON tool-calls.
# Exercises: registry lookup, permission gating, rate-limit deferral,
# invocation limit computation, history mirroring, batch teardown.
fuzz_tool_dispatch_exe = executable(
  'fuzz_tool_dispatch',
  files('fuzz_tool_dispatch.c') + fuzz_lib_src,
  include_directories: [clm_inc, include_directories('../include')],
  dependencies: clm_core_deps,
  c_args: ['-fsanitize=address', '-DCLM_BUILDING'],
  link_args: ['-fsanitize=fuzzer,address'],
  install: false,
)

# fuzz_tool_dispatch_lua: same but with Lua plugin support compiled in --
# links clmlua_dep (../libclmlua/meson.build) rather than respecifying
# lua_plugin.c/lua_json.c/lua_http.c as their own source list, now that
# they're a proper static archive shared with the clm desktop binary.
if lua_dep.found()
  fuzz_tool_dispatch_lua_exe = executable(
    'fuzz_tool_dispatch_lua',
    files('fuzz_tool_dispatch.c') + fuzz_lib_src,
    include_directories: [clm_inc, include_directories('../include')],
    dependencies: [clm_core_deps, clmlua_dep],
    c_args: ['-fsanitize=address', '-DCLM_BUILDING'],
    link_args: ['-fsanitize=fuzzer,address'],
    install: false,
  )
endif

# fuzz_session: fuzz the session JSONL line parser (external input: session
# files are read back from disk and must never crash the loader).
fuzz_session_exe = executable(
  'fuzz_session',
  files('fuzz_session.c', '../libclm/history.c', '../libclmsession/session.c'),
  include_directories: [clm_inc, include_directories('../include'),
    include_directories('../libclmsession')],
  dependencies: [cjson_dep],
  c_args: ['-fsanitize=address', '-DCLM_BUILDING'],
  link_args: ['-fsanitize=fuzzer,address'],
  install: false,
)

# fuzz_md: fuzz md_render() with arbitrary markdown input.
fuzz_md_exe = executable(
  'fuzz_md',
  files('fuzz_md.c'),
  link_with: [md_render_lib],
  include_directories: [md_render_inc],
  c_args: ['-fsanitize=address', '-D_XOPEN_SOURCE=700'],
  link_args: ['-fsanitize=fuzzer,address'],
  install: false,
)
