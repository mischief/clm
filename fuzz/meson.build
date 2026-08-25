# Fuzz targets for AFL++ persistent mode. These fuzz how clm handles parsed
# data, not upstream libraries. Built alongside the tests but never run by
# `meson test`; see README.md for how to drive them.
#
# Build:  CC=afl-clang-fast meson setup build-fuzz -Dtests=true
# Run:    afl-fuzz -i fuzz/corpus/session -o out -- ./build-fuzz/fuzz/fuzz_session

fuzz_inc = include_directories('.')

fuzz_lib_src = files(
  '../libclm/agent.c', '../libclm/tools.c', '../libclm/history.c',
  '../libclm/llm.c', '../libclm/ratelimit.c', '../libclm/log.c',
  '../libclm/http.c', '../libclm/props.c', '../libclm/provider.c',
  '../libclm/provider_anthropic.c', '../libclm/provider_openai.c',
  '../libclm/provider_responses.c', '../libclm/models_list.c',
  '../libclm/text_clean.c',
)

# fuzz_tool_dispatch: fuzz clm_tools_dispatch() with adversarial JSON tool-calls.
# Exercises: registry lookup, permission gating, rate-limit deferral,
# invocation limit computation, history mirroring, batch teardown.
fuzz_tool_dispatch_exe = executable(
  'fuzz_tool_dispatch',
  files('fuzz_tool_dispatch.c') + fuzz_lib_src,
  include_directories: [fuzz_inc, clm_inc, include_directories('../include')],
  dependencies: clm_core_deps,
  c_args: ['-DCLM_BUILDING'],
  build_by_default: false,
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
    include_directories: [fuzz_inc, clm_inc, include_directories('../include')],
    dependencies: [clm_core_deps, clmlua_dep],
    c_args: ['-DCLM_BUILDING'],
    build_by_default: false,
    install: false,
  )
endif

# fuzz_session: fuzz the session JSONL line parser (external input: session
# files are read back from disk and must never crash the loader).
fuzz_session_exe = executable(
  'fuzz_session',
  files('fuzz_session.c', '../libclm/history.c', '../libclmsession/session.c'),
  include_directories: [fuzz_inc, clm_inc, include_directories('../include'),
    include_directories('../libclmsession')],
  dependencies: [cjson_dep],
  c_args: ['-DCLM_BUILDING'],
  build_by_default: false,
  install: false,
)

# fuzz_text_clean: fuzz the tool-output cleaner (external input: a command
# prints whatever it likes, including half-formed escape sequences).
fuzz_text_clean_exe = executable(
  'fuzz_text_clean',
  files('fuzz_text_clean.c', '../libclm/text_clean.c'),
  include_directories: [fuzz_inc, clm_inc, include_directories('../include')],
  dependencies: [cjson_dep],
  c_args: ['-DCLM_BUILDING'],
  build_by_default: false,
  install: false,
)

# fuzz_md: fuzz md_render() with arbitrary markdown input. It compiles
# md_render.c rather than linking md_render_lib, so the parser is built with
# the same instrumentation as the harness.
fuzz_md_exe = executable(
  'fuzz_md',
  files('fuzz_md.c', '../src/md_render.c'),
  dependencies: [md_dep],
  include_directories: [fuzz_inc, md_render_inc, clm_bin_inc, clm_inc],
  c_args: ['-D_XOPEN_SOURCE=700'],
  build_by_default: false,
  install: false,
)
