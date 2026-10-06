# require("clm"): drive agents from host Lua. See README.md here.
if have_lua
  # The installed module finds libclm in a non-system prefix.
  clm_lua_rpath = get_option('prefix') == '/usr' ? '' : get_option('prefix') / get_option('libdir')
  clm_lua_module = shared_module('clm',
    files('clm_lua.c', '../libclmlua/lua_json.c'),
    name_prefix: '',
    c_args: clm_hardening_args,
    dependencies: [lua_dep, clm_dep, clmuv_dep, cjson_dep],
    include_directories: [clm_inc, include_directories('..')],
    install_rpath: clm_lua_rpath,
    install: true,
    install_dir: get_option('libdir') / 'lua' / '5.4',
  )
endif
