# Fuzz targets using libFuzzer (CC=clang required)
# These fuzz how clm handles parsed data, not upstream libraries.
#
# Build:  CC=clang meson setup build-fuzz -Dtests=true
# Run:    ./build-fuzz/fuzz/fuzz_tool_dispatch

fuzz_lib_src = files(
  '../lib/agent.c', '../lib/tools.c', '../lib/history.c',
  '../lib/llm.c', '../lib/ratelimit.c', '../lib/log.c',
  '../lib/http.c', '../lib/props.c',
)

# fuzz_tool_dispatch: fuzz clm_tools_dispatch() with adversarial JSON tool-calls.
# Exercises: registry lookup, permission gating, rate-limit deferral,
# invocation limit computation, history mirroring, batch teardown.
fuzz_tool_dispatch_exe = executable(
  'fuzz_tool_dispatch',
  files('fuzz_tool_dispatch.c') + fuzz_lib_src,
  include_directories: [clm_inc, include_directories('../include')],
  dependencies: clm_core_deps,
  c_args: ['-fsanitize=address', '-DCLM_BUILDING', '-DCLM_LUA=1'],
  link_args: ['-fsanitize=fuzzer,address'],
  install: false,
)

# fuzz_tool_dispatch_lua: same but with Lua plugin support compiled in.
if lua_dep.found()
  fuzz_tool_dispatch_lua_exe = executable(
    'fuzz_tool_dispatch_lua',
    files('fuzz_tool_dispatch.c') + fuzz_lib_src + files(
      '../lib/lua_plugin.c', '../lib/lua_json.c', '../lib/lua_http.c'),
    include_directories: [clm_inc, include_directories('../include')],
    dependencies: [clm_core_deps, lua_dep],
    c_args: ['-fsanitize=address', '-DCLM_BUILDING', '-DCLM_LUA=1'],
    link_args: ['-fsanitize=fuzzer,address'],
    install: false,
  )
endif

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
