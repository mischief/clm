clm_bin_inc = include_directories('../include')

# md4c: streaming markdown parser. Confined to the md_render module below so
# the core libclm stays free of a markdown dependency.
md_dep = dependency('md4c', fallback: ['md4c', 'md4c_dep'], kwargs: static_kw)

# md_render: markdown -> styled runs, ncurses-free so it can be unit-tested in
# isolation. Static, internal, not installed. Independent of Lua (see below),
# so this stays unconditional -- test/meson.build's test_md links it without
# needing the clm desktop binary at all.
md_render_inc = include_directories('.')
md_render_lib = static_library('md_render',
  files('md_render.c'),
  dependencies: [md_dep],
  include_directories: [md_render_inc, clm_bin_inc, clm_inc],
  c_args: ['-D_XOPEN_SOURCE=700'] + clm_hardening_args, # wcwidth(3) is XSI
)

# Unlike the portable core (../libclm's `lua` feature, optional there for
# an eventual embedded/ESP32 build with no terminal), the clm desktop
# binary (this directory: main.c, tui.c, mcp_setup.c) has no embedded story
# at all -- it already hard-depends on ncursesw, getopt_long, XDG paths. So
# Lua plugin/agent-profile support is required for it, not optional: nothing
# in main.c/tui.c is #ifdef CLM_LUA'd (that define no longer exists anywhere
# -- lua_plugin.c et al now live in their own clmlua static archive, see
# ../libclmlua/meson.build, exactly like the ../libclmuv split; libclm
# itself never depended on Lua either way).
#
# But the binary still must not error out of a -Dlua=disabled configure
# entirely -- that flag is legitimate for embedding the portable core alone.
# So the whole binary -- and only the parts of the test suite that exercise
# it -- are skipped whenever libclmlua's lua feature (and so clmlua_dep)
# resolved to disabled. have_lua is checked again in test/meson.build for
# the same reason.
have_lua = lua_dep.found()

if have_lua
  clm_src = files('main.c')

  # No separate libuv dependency() here: clmuv_dep (used below on clm_exe)
  # already carries ../libclmuv's own libuv resolution transitively -- a
  # second independent dependency('libuv', ...) call here was dead weight,
  # never actually referenced by anything in this file.

  # ncurses (wide) is a hard dependency: the default frontend is the TUI.
  # Available in base/ports on the BSDs, Linux, and macOS. omnios/illumos
  # ships a single "ncurses" pkg-config module with wide-char support
  # built in (NCURSES_WIDECHAR baked into its .pc Cflags) rather than a
  # separate ncursesw -- so fall back to plain ncurses if ncursesw isn't
  # found.
  curses_dep = dependency('ncursesw', required: false, kwargs: static_kw)
  if not curses_dep.found()
    curses_dep = dependency('ncurses', kwargs: static_kw)
  endif

  # ncurses' own macros (NCURSES_CAST) trip -Wuseless-cast; not our code.
  curses_warn = cc.get_supported_arguments('-Wno-useless-cast')

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
  # clmlua_dep is listed explicitly (../libclmlua/meson.build only declares
  # it when have_lua, which this whole block is already gated on) since
  # lua_plugin.c et al no longer live inside libclm/clm_dep at all --
  # they're their own archive now, same shape as ../libclmuv.
  #
  # emoji_dep: :shortcode: -> glyph completion/expansion for the input
  # line, see ../libemoji.
  clm_exe = executable('clm', clm_src + files('tui.c', 'complete.c', 'mcp_setup.c') + seed_src + [version_h, templates_data],
    link_with: [md_render_lib],
    dependencies: [clmuv_dep, clmlua_dep, curses_dep, cjson_dep, emoji_dep],
    include_directories: [clm_bin_inc, md_render_inc, clm_inc],
    c_args: curses_warn + ['-D_XOPEN_SOURCE_EXTENDED'] + clm_hardening_args,
    install: true,
  )
endif
