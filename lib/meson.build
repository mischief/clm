# Portable core: the agent engine, tool registry, and OpenAI-compatible client.
# Deliberately free of libuv and libcurl -- transport and timers come in through
# a struct clm_host (see clm/host.h). The only hard dependency is json-c.
clm_core_src = files('agent.c', 'http.c', 'llm.c', 'log.c', 'tools.c', 'history.c', 'props.c', 'ratelimit.c')

clm_core_deps = [
  dependency('json-c', kwargs: static_kw),
]

# lib/'s own optional feature: whether Lua plugin/agent-profile support is
# built at all. Independent of whether any particular consumer (the clm
# desktop binary, tests) wants it -- see clmlua_dep below, which mirrors
# clmuv_dep's pattern: an always-buildable-when-available static archive,
# kept OUT of libclm itself, that a consumer either links or doesn't. This
# is what actually lets an embedded build (e.g. a future esp32 target) omit
# Lua/liblua entirely without any #ifdef CLM_LUA anywhere in C source --
# the seam is a link-time decision, not a preprocessor one.
lua_dep = dependency('lua5.4', required: get_option('lua'), kwargs: static_kw)

# Lib sources find "clm/..." via ../include, and "useful.h"/"banned.h" via
# ../include and the project root.
clm_lib_inc = include_directories('.', '..', '../include')

# -Dstatic=true forces a static archive here regardless of
# default_library, so a plain `meson setup -Dstatic=true` is enough (no
# need to also pass -Ddefault_library=static --prefer-static). Meson has
# no function references, so the call is duplicated rather than picked
# via a variable.
clm_lib_common_args = {
  'c_args': ['-DCLM_BUILDING'],
  'gnu_symbol_visibility': 'hidden',
  'dependencies': clm_core_deps,
  'include_directories': [clm_lib_inc, clm_inc],
  'install': true,
  'install_dir': get_option('libdir'),
}
if want_static
  clm_lib = static_library('clm', clm_core_src + [version_h],
    kwargs: clm_lib_common_args)
else
  clm_lib = library('clm', clm_core_src + [version_h],
    # ABI version: bump only on ABI breaks, independent of the release version.
    version: meson.project_version(),
    soversion: '0',
    link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libclm.map'],
    kwargs: clm_lib_common_args)
endif

# Core dependency for in-tree consumers (tests, frontends).
clm_dep = declare_dependency(
  link_with: clm_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: clm_core_deps,
)

# Lua plugin support: registers Lua-backed tools through the same clm_tool_fn
# callback every other tool provider uses (see tools.c/clm.h) -- lua_plugin.c
# only ever calls INTO the core (clm_agent_*, clm_debug), never the reverse,
# so this can live entirely outside libclm as its own static archive, exactly
# like clmuv_lib below. That keeps libclm itself free of a liblua dependency
# unconditionally, while a consumer that wants Lua support links clmlua_dep.
# log.c is duplicated here for the same reason clmuv_src duplicates it:
# clm_debug is library-internal (clm/log.h is explicitly "not part of the
# public contract and not installed"), so an out-of-tree consumer linking
# this archive against an already-built libclm has no other way to get it.
clmlua_src = files('lua_plugin.c', 'lua_json.c', 'lua_http.c', 'log.c')

if lua_dep.found()
  clm_lua_lib = static_library('clmlua', clmlua_src,
    c_args: ['-DCLM_BUILDING'],
    dependencies: [lua_dep, clm_core_deps],
    include_directories: [clm_lib_inc, clm_inc],
    install: true,
  )

  clmlua_dep = declare_dependency(
    link_with: clm_lua_lib,
    include_directories: include_directories('..', '../include'),
    dependencies: [lua_dep, clm_dep],
  )
endif

# Desktop uv/curl layer: the libuv+libcurl transport (host_uv + the async curl
# engine) plus the shell_exec tool. Kept OUT of the core so libclm itself pulls
# in no libuv/libcurl. A static archive: frontends that want the desktop host
# link it alongside libclm; an embedder supplying its own host just doesn't.
# log.c is included here too: clm_debug is a library-internal helper (hidden in
# libclm.so, so not visible across the .so boundary), and http_async.c uses it.
# A private copy in the archive keeps clm_debug off the public ABI.
clmuv_src = files('http_async.c', 'host_uv.c', 'tool_shell.c', 'mcp_client.c', 'log.c')

clmuv_deps = [
  dependency('libcurl', kwargs: static_kw),
  dependency('libuv', kwargs: static_kw),
  dependency('json-c', kwargs: static_kw),
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
