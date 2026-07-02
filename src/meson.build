clm_src = files('main.c')

clm_bin_inc = include_directories('../include')

clm_bin_deps = [
  dependency('libuv'),
]

# ncurses (wide) is a hard dependency: the default frontend is the TUI.
# Available in base/ports on the BSDs, Linux, and macOS.
curses_dep = dependency('ncursesw')

# md4c: streaming markdown parser. Confined to the md_render module below so
# the core libclm stays free of a markdown dependency.
md_dep = dependency('md4c')

# json-c: the TUI parses tool-call arguments to summarize them for display.
jsonc_dep = dependency('json-c')

# md_render: markdown -> styled runs, ncurses-free so it can be unit-tested in
# isolation. Static, internal, not installed.
md_render_inc = include_directories('.')
md_render_lib = static_library('md_render',
  files('md_render.c'),
  dependencies: [md_dep],
  include_directories: [md_render_inc, clm_bin_inc, clm_inc],
  c_args: ['-D_XOPEN_SOURCE=700'], # wcwidth(3) is XSI
)

# ncurses' own macros (NCURSES_CAST) trip -Wuseless-cast; not our code.
curses_warn = cc.get_supported_arguments('-Wno-useless-cast')

# clmuv_dep pulls in the core (libclm) plus the uv/curl host layer and its
# libuv/libcurl deps, so the binary gets clm_host_uv_new + shell_exec.
clm_exe = executable('clm', clm_src + files('tui.c') + [version_h],
  link_with: [md_render_lib],
  dependencies: [clmuv_dep, curses_dep, jsonc_dep],
  include_directories: [clm_bin_inc, md_render_inc, clm_inc],
  c_args: curses_warn + ['-D_XOPEN_SOURCE_EXTENDED'],
  install: true,
)
