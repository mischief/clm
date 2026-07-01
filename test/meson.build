test_agent = executable('test_agent',
  files('test_agent.c', 'canned.c'),
  dependencies: [clm_dep],
  include_directories: include_directories('.'),
)
test('agent', test_agent)

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
