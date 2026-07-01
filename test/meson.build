test_agent = executable('test_agent',
  files('test_agent.c', 'canned.c'),
  dependencies: [clm_dep],
  include_directories: include_directories('.'),
)
test('agent', test_agent)

# md_render unit test: pure, links only the internal md_render static library
# (no ncurses, no libclm), so the markdown-to-runs logic is tested in isolation.
test_md = executable('test_md',
  files('test_md.c'),
  link_with: [md_render_lib],
  include_directories: [md_render_inc],
  c_args: ['-D_XOPEN_SOURCE=700'], # wcwidth(3) is XSI
)
test('md', test_md, suite: 'md')

# End-to-end TUI test: drives the real clm binary on a pty against a canned
# mock server via pyte. Uses only an already-installed python3 + pyte (global
# or venv); if pyte is not importable the test is skipped. The build never
# installs anything.
pymod = import('python')
py = pymod.find_installation('python3', required: false)
have_pyte = py.found() and run_command(
  py, '-c', 'import pyte', check: false).returncode() == 0

if have_pyte
  test('tui',
    py,
    args: [files('tui/test_tui.py')],
    env: {'CLM_BIN': clm_exe.full_path()},
    depends: clm_exe,
    suite: 'tui',
    timeout: 120,
  )
else
  message('python3+pyte not found; skipping TUI test')
endif
