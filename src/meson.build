clm_src = files('main.c')

clm_bin_inc = include_directories('../include')

clm_bin_deps = [
  dependency('libuv'),
]

# ncurses (wide) is a hard dependency: the default frontend is the TUI.
# Available in base/ports on the BSDs, Linux, and macOS.
curses_dep = dependency('ncursesw')

# lowdown renders the assistant's markdown for the transcript.
lowdown_dep = dependency('lowdown')

# json-c: the TUI parses tool-call arguments to summarize them for display.
jsonc_dep = dependency('json-c')

# ncurses' own macros (NCURSES_CAST) trip -Wuseless-cast; not our code.
curses_warn = cc.get_supported_arguments('-Wno-useless-cast')

clm_exe = executable('clm', clm_src + files('tui.c'),
  link_with: [clm_lib],
  dependencies: clm_bin_deps + [curses_dep, lowdown_dep, jsonc_dep],
  include_directories: clm_bin_inc,
  c_args: curses_warn,
  install: true,
)
