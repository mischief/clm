clm_lib_src = files('agent.c', 'http_async.c', 'llm.c', 'log.c', 'tools.c', 'history.c', 'props.c')

clm_lib_deps = [
  dependency('libcurl'),
  dependency('json-c'),
  dependency('libuv'),
]

lua_dep = dependency('lua5.4', required: get_option('lua'))
if lua_dep.found()
  clm_lib_src += files('lua_plugin.c', 'lua_json.c', 'lua_http.c')
  clm_lib_deps += lua_dep
  add_project_arguments('-DCLM_LUA=1', language: 'c')
endif

# Lib sources find "clm/..." via ../include, and "useful.h"/"banned.h" via
# ../include and the project root.
clm_lib_inc = include_directories('.', '..', '../include')

clm_lib = shared_library('clm', clm_lib_src + [version_h],
  version: meson.project_version(),
  # ABI version: bump only on ABI breaks, independent of the release version.
  soversion: '0',
  c_args: ['-DCLM_BUILDING'],
  gnu_symbol_visibility: 'hidden',
  dependencies: clm_lib_deps,
  include_directories: [clm_lib_inc, clm_inc],
  install: true,
  install_dir: get_option('libdir'),
  link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libclm.map'],
)

# Dependency for in-tree consumers (tests). Re-exports the lib's own deps so
# consumers get libuv/json-c include paths transitively.
clm_dep = declare_dependency(
  link_with: clm_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: clm_lib_deps,
)
