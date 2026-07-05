# Portable core: the agent engine, tool registry, and OpenAI-compatible client.
# Deliberately free of libuv and libcurl -- transport and timers come in through
# a struct clm_host (see clm/host.h). The only hard dependency is json-c.
clm_core_src = files('agent.c', 'http.c', 'llm.c', 'log.c', 'tools.c', 'history.c', 'props.c', 'ratelimit.c')

clm_core_deps = [
  dependency('json-c'),
]

lua_dep = dependency('lua5.4', required: get_option('lua'))
if lua_dep.found()
  clm_core_src += files('lua_plugin.c', 'lua_json.c', 'lua_http.c')
  clm_core_deps += lua_dep
  add_project_arguments('-DCLM_LUA=1', language: 'c')
endif

# Lib sources find "clm/..." via ../include, and "useful.h"/"banned.h" via
# ../include and the project root.
clm_lib_inc = include_directories('.', '..', '../include')

clm_lib = shared_library('clm', clm_core_src + [version_h],
  version: meson.project_version(),
  # ABI version: bump only on ABI breaks, independent of the release version.
  soversion: '0',
  c_args: ['-DCLM_BUILDING'],
  gnu_symbol_visibility: 'hidden',
  dependencies: clm_core_deps,
  include_directories: [clm_lib_inc, clm_inc],
  install: true,
  install_dir: get_option('libdir'),
  link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libclm.map'],
)

# Core dependency for in-tree consumers (tests, frontends).
clm_dep = declare_dependency(
  link_with: clm_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: clm_core_deps,
)

# Desktop uv/curl layer: the libuv+libcurl transport (host_uv + the async curl
# engine) plus the shell_exec tool. Kept OUT of the core so libclm itself pulls
# in no libuv/libcurl. A static archive: frontends that want the desktop host
# link it alongside libclm; an embedder supplying its own host just doesn't.
# log.c is included here too: clm_debug is a library-internal helper (hidden in
# libclm.so, so not visible across the .so boundary), and http_async.c uses it.
# A private copy in the archive keeps clm_debug off the public ABI.
clmuv_src = files('http_async.c', 'host_uv.c', 'tool_shell.c', 'mcp_client.c', 'log.c')

clmuv_deps = [
  dependency('libcurl'),
  dependency('libuv'),
  dependency('json-c'),
]

clm_uv_lib = static_library('clmuv', clmuv_src,
  c_args: ['-DCLM_BUILDING'],
  dependencies: clmuv_deps,
  include_directories: [clm_lib_inc, clm_inc],
  install: true,
)

# Consumers link the uv layer plus the core it calls into.
clmuv_dep = declare_dependency(
  link_with: clm_uv_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: [clmuv_deps, clm_dep],
)
