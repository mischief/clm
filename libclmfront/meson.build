# What the frontends share: the tui, the headless cli and clm-gtk. Paths,
# host facts for the prompt, builtin plugin seeds, plugin and mcp loading,
# and the small headers that turn a config.lua spec into a connection.
# A static archive, internal and not installed. Nothing here draws or reads
# input; that stays in each frontend.
#
# The headers are available without lua, for the tests that only need
# loop_drain.h. The archive needs the lua plugin layer and is skipped
# without it, as the frontends are.
clmfront_inc = include_directories('.')

if lua_dep.found()
  # Builtin plugin seeds: embed plugins/*.lua into the binary so a fresh
  # XDG plugin dir gets a usable starter set on first run (see
  # seed_plugins.h).
  # Runs at build time on the build machine to generate a source file;
  # under a cross build this must not resolve to the target's lua.
  # Falls back to the lua-wrap subproject's own interpreter executable
  # when the system has no lua CLI installed at all (omnios: lua_dep came
  # from the wrap fallback above, but WrapDB's lua.wrap doesn't register
  # its lua_exe target via meson.override_find_program, so find_program()
  # alone never sees it).
  # 'lua54' is OpenBSD's package binary name (its lang/lua54 port installs
  # /usr/local/bin/lua54, not lua5.4 or plain lua).
  lua_bin = find_program('lua5.4', 'lua54', 'lua', required: false, native: true)
  if not lua_bin.found()
    lua_bin = subproject('lua').get_variable('lua_exe')
  endif
  seed_plugin_files = files(
    '../plugins/edit_file.lua',
    '../plugins/reverse.lua',
    '../plugins/tasks.lua',
    '../plugins/weather.lua',
    '../plugins/web_search.lua',
  )
  seed_plugins_data = custom_target('seed_plugins_data.c',
    input: seed_plugin_files,
    output: 'seed_plugins_data.c',
    command: [lua_bin, files('gen_seed_plugins.lua'), '@OUTPUT@', '@INPUT@'],
  )

  # Host facts for the system prompt: the portable half plus one per-OS
  # file. Selecting the file here keeps the OS knowledge out of the C source
  # as a link-time choice; a host with no file of its own gets the generic
  # one, which reports uname and core count and nothing it would have to
  # guess at.
  host_os = host_machine.system()
  if host_os == 'linux'
    sysinfo_src = files('sysinfo.c', 'sysinfo_linux.c')
  elif host_os == 'openbsd'
    sysinfo_src = files('sysinfo.c', 'sysinfo_openbsd.c')
  else
    sysinfo_src = files('sysinfo.c', 'sysinfo_generic.c')
  endif

  clm_front_lib = static_library('clmfront',
    files('xdg.c', 'mcp_setup.c', 'plugin_setup.c', 'seed_plugins.c') +
      sysinfo_src + [seed_plugins_data],
    c_args: clm_hardening_args,
    dependencies: [clmuv_dep, clmlua_dep, clmsession_dep, cjson_dep],
    include_directories: [clmfront_inc, clm_inc],
  )

  clmfront_dep = declare_dependency(
    link_with: clm_front_lib,
    include_directories: clmfront_inc,
    dependencies: [clmuv_dep, clmlua_dep, clmsession_dep, cjson_dep],
  )
endif
