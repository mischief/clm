# libemoji: :shortcode: -> UTF-8 glyph lookup table, generated at build
# time from emoji_data.tsv (see gen_emoji.lua for the relocation-free
# name-blob + packed-codepoint layout this produces). Deliberately
# self-contained -- no clm/ or curses dependency at all -- so it's a
# standalone static lib any frontend (or a future non-C consumer via a thin
# wrapper) can link against, not just the tui. Finds its own lua rather
# than reusing src/meson.build's lua_bin so this directory has no ordering
# dependency on subdir('src') and could be lifted out on its own.
lua_bin = find_program('lua5.4', 'lua54', 'lua', required: false, native: true)
if not lua_bin.found()
  lua_bin = subproject('lua').get_variable('lua_exe')
endif

emoji_inc = include_directories('.')

emoji_data = custom_target('emoji_data.c',
  input: files('emoji_data.tsv'),
  output: 'emoji_data.c',
  command: [lua_bin, files('gen_emoji.lua'), '@OUTPUT@', '@INPUT@'],
)

emoji_lib = static_library('emoji',
  files('emoji.c'), emoji_data,
  include_directories: emoji_inc,
  c_args: clm_hardening_args,
)

emoji_dep = declare_dependency(
  link_with: emoji_lib,
  include_directories: emoji_inc,
)
