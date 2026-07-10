# Lua plugin support: registers Lua-backed tools through the same clm_tool_fn
# callback every other tool provider uses (see tools.c/clm.h) -- lua_plugin.c
# only ever calls INTO the core (clm_agent_*, clm_debug), never the reverse,
# so this can live entirely outside libclm as its own static archive, exactly
# like libclmuv. That keeps libclm itself free of a liblua dependency
# unconditionally, while a consumer that wants Lua support links clmlua_dep.
# clm_debug (../libclm/log.h) is CLM_API-exported from libclm itself, so
# this archive just links clm_dep for it rather than compiling its own
# private copy of log.c.
clmlua_src = files('lua_plugin.c', 'lua_json.c', 'lua_http.c')

if lua_dep.found()
  clm_lua_lib = static_library('clmlua', clmlua_src,
    c_args: ['-DCLM_BUILDING'] + clm_hardening_args,
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
