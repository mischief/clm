# Portable core: the agent engine, tool registry, and OpenAI-compatible client.
# Deliberately free of libuv and libcurl -- transport and timers come in through
# a struct clm_host (see clm/host.h). The only hard dependency is cJSON (ESP-IDF
# ships cJSON built-in, so this avoids vendoring json-c into the esp32 tree).
clm_core_src = files('agent.c', 'http.c', 'llm.c', 'log.c', 'tools.c', 'history.c', 'props.c', 'models_list.c', 'ratelimit.c', 'provider.c', 'provider_openai.c', 'provider_anthropic.c', 'provider_responses.c')

cjson_dep = dependency('libcjson', fallback: ['cjson', 'libcjson_dep'], kwargs: static_kw)

clm_core_deps = [
  cjson_dep,
]

# libclmlua/ and libclmuv/'s own optional feature: whether Lua plugin/agent-
# profile support is built at all. Independent of whether any particular
# consumer (the clm desktop binary, tests) wants it -- see clmlua_dep in
# ../libclmlua/meson.build, which mirrors clmuv_dep's pattern: an always-
# buildable-when-available static archive, kept OUT of libclm itself, that a
# consumer either links or doesn't. This is what actually lets an embedded
# build (e.g. a future esp32 target) omit Lua/liblua entirely without any
# #ifdef CLM_LUA anywhere in C source -- the seam is a link-time decision,
# not a preprocessor one.
lua_dep = dependency('lua5.4', required: get_option('lua'),
  fallback: ['lua', 'lua_dep'], kwargs: static_kw)

# libclm sources find "clm/..." via ../include, and "useful.h"/"banned.h" via
# ../include and the project root. libclmlua/libclmuv also use clm_lib_inc
# (this variable, shared via meson's subdir() scoping) for the same reason
# plus to reach log.c here -- see their own meson.build.
clm_lib_inc = include_directories('.', '..', '../include')

# -Dstatic=true forces a static archive here regardless of
# default_library, so a plain `meson setup -Dstatic=true` is enough (no
# need to also pass -Ddefault_library=static --prefer-static). Meson has
# no function references, so the call is duplicated rather than picked
# via a variable.
clm_lib_common_args = {
  'c_args': ['-DCLM_BUILDING'] + clm_hardening_args,
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
  clm_lib_link_args = []
  if cc.has_link_argument('-Wl,--version-script=/dev/null')
    clm_lib_link_args += ['-Wl,--version-script=' + meson.current_source_dir() / 'libclm.map']
  endif
  clm_lib = library('clm', clm_core_src + [version_h],
    # ABI version: bump only on ABI breaks, independent of the release version.
    version: meson.project_version(),
    soversion: '0',
    link_args: clm_lib_link_args,
    kwargs: clm_lib_common_args)
endif

# Core dependency for in-tree consumers (tests, frontends).
clm_dep = declare_dependency(
  link_with: clm_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: clm_core_deps,
)
