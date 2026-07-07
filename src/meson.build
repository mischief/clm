clm_bin_inc = include_directories('../include')

# md4c: streaming markdown parser. Confined to the md_render module below so
# the core libclm stays free of a markdown dependency.
md_dep = dependency('md4c', kwargs: static_kw)

# md_render: markdown -> styled runs, ncurses-free so it can be unit-tested in
# isolation. Static, internal, not installed. Independent of Lua (see below),
# so this stays unconditional -- test/meson.build's test_md links it without
# needing the clm desktop binary at all.
md_render_inc = include_directories('.')
md_render_lib = static_library('md_render',
  files('md_render.c'),
  dependencies: [md_dep],
  include_directories: [md_render_inc, clm_bin_inc, clm_inc],
  c_args: ['-D_XOPEN_SOURCE=700'], # wcwidth(3) is XSI
)

# Unlike the portable core (lib/meson.build's `lua` feature, optional there
# for an eventual embedded/ESP32 build with no terminal), the clm desktop
# binary (this directory: main.c, tui.c, mcp_setup.c) has no embedded story
# at all -- it already hard-depends on ncursesw, getopt_long, XDG paths. So
# Lua plugin/agent-profile support is required for it, not optional: nothing
# in main.c/tui.c is #ifdef CLM_LUA'd (that define no longer exists anywhere
# -- lua_plugin.c et al now live in their own clmlua static archive, see
# lib/meson.build, exactly like the existing clmuv split; libclm itself
# never depended on Lua either way).
#
# But the binary still must not error out of a -Dlua=disabled configure
# entirely -- that flag is legitimate for embedding the portable core alone.
# So the whole binary -- and only the parts of the test suite that exercise
# it -- are skipped whenever lib/'s lua feature (and so clmlua_dep) resolved
# to disabled. have_lua is checked again in test/meson.build for the same
# reason.
have_lua = lua_dep.found()

if have_lua
  clm_src = files('main.c')

  clm_bin_deps = [
    dependency('libuv', kwargs: static_kw),
  ]

  # ncurses (wide) is a hard dependency: the default frontend is the TUI.
  # Available in base/ports on the BSDs, Linux, and macOS.
  curses_dep = dependency('ncursesw', kwargs: static_kw)

  # ncurses' own macros (NCURSES_CAST) trip -Wuseless-cast; not our code.
  curses_warn = cc.get_supported_arguments('-Wno-useless-cast')

  # Builtin plugin seeds: embed plugins/*.lua into the binary so a fresh
  # XDG plugin dir gets a usable starter set on first run (see
  # seed_plugins.h).
  # Runs at build time on the build machine to generate a source file;
  # under a cross build this must not resolve to the target's lua.
  lua_bin = find_program('lua5.4', 'lua', required: true, native: true)
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
  seed_src = [files('seed_plugins.c'), seed_plugins_data]

  # `clm setup`'s canned config.lua/secrets.lua: embedded the same way (see
  # gen_templates.lua's comment for why -- a plain string literal blew past
  # ISO C99's mandatory-support minimum for a single string literal once
  # the provider list grew past a handful of entries).
  template_files = files('config.lua.tpl', 'secrets.lua.tpl')
  templates_data = custom_target('templates_data.c',
    input: template_files,
    output: 'templates_data.c',
    command: [lua_bin, files('gen_templates.lua'), '@OUTPUT@', '@INPUT@'],
  )

  # clmuv_dep pulls in the core (libclm) plus the uv/curl host layer and its
  # libuv/libcurl deps, so the binary gets clm_host_uv_new + shell_exec.
  # clmlua_dep is listed explicitly (lib/meson.build only declares it when
  # have_lua, which this whole block is already gated on) since lua_plugin.c
  # et al no longer live inside libclm/clm_dep at all -- they're their own
  # archive now, same shape as clmuv.
  clm_exe = executable('clm', clm_src + files('tui.c', 'complete.c', 'mcp_setup.c') + seed_src + [version_h, templates_data],
    link_with: [md_render_lib],
    dependencies: [clmuv_dep, clmlua_dep, curses_dep, cjson_dep],
    include_directories: [clm_bin_inc, md_render_inc, clm_inc],
    c_args: curses_warn + ['-D_XOPEN_SOURCE_EXTENDED'],
    install: true,
  )
endif
