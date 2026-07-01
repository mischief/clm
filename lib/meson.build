clm_lib_src = files('agent.c', 'http_async.c', 'llm.c', 'log.c', 'tools.c', 'history.c', 'props.c')

clm_lib_deps = [
  dependency('libcurl'),
  dependency('json-c'),
  dependency('libuv'),
]

# Lib sources find "clm/..." via ../include, and "useful.h"/"banned.h" via
# ../include and the project root.
clm_lib_inc = include_directories('.', '..', '../include')

clm_lib = shared_library('clm', clm_lib_src,
  version: '0.1.0',
  soversion: '0',
  c_args: ['-DCLM_BUILDING'],
  gnu_symbol_visibility: 'hidden',
  dependencies: clm_lib_deps,
  include_directories: clm_lib_inc,
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
