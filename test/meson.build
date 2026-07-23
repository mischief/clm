# -Wframe-larger-than (meson.build) is meant to catch a real library/frontend
# function with an unreasonably large stack frame. In test drivers it just
# fires on inlining artifacts instead: at -O2, GCC/clang can inline several
# independent, individually-small test helpers into one caller (every
# test_*() into main(), a canned-response builder into its one call site,
# etc.), and the combined frame trips the limit even though none of the
# actual functions involved are a problem. Not worth restructuring test-only
# code around (no attributes/pragmas needed either -- this is scoped to the
# test targets below, so it doesn't loosen the check anywhere real).
test_cflags = []
if cc.has_argument('-Wno-frame-larger-than')
  test_cflags += ['-Wno-frame-larger-than']
endif

# Uses the uv host (clm_host_uv) + shell_exec, so it links the uv layer.
test_agent = executable('test_agent',
  files('test_agent.c', 'canned.c', '../libclm/props.c', '../libclm/history.c', '../libclm/models_list.c'),
  dependencies: [clmuv_dep],
  include_directories: [include_directories('.'), clm_inc, include_directories('../include')],
  c_args: test_cflags,
)
test('agent', test_agent)

# Session persistence test: pure (no network, no loop). Compiles history.c
# and session.c directly, same pattern as test_agent, so internals are
# reachable without going through the shared library's export list.
test_session = executable('test_session',
  files('test_session.c', '../libclm/history.c', '../libclmsession/session.c'),
  dependencies: [cjson_dep],
  include_directories: [include_directories('.'), clm_inc,
    include_directories('../include'), include_directories('../libclmsession')],
  c_args: test_cflags,
)
test('session', test_session)

# Lua plugin test: loads plugins from ../plugins/ and verifies tool dispatch.
# Builds a clm_host_uv, so it links the uv layer too. Only built when
# have_lua (src/meson.build) -- clmlua_dep, needed for clm_lua_env_new etc,
# only exists in that case (../libclmlua's own lua_dep.found() guard). No more
# CLM_LUA-guarded skip-stub main() in the .c file: the target itself is now
# the seam, not a preprocessor branch inside it.
if have_lua
  test_lua = executable('test_lua_plugin',
    files('test_lua_plugin.c'),
    dependencies: [clmuv_dep, clmlua_dep],
    include_directories: [clm_inc, include_directories('../include')],
    c_args: test_cflags,
  )
  test('lua_plugin', test_lua, workdir: meson.project_source_root())

  # Subagent prototype test: agent.new()/:submit()/:free() (lua_agent.c),
  # driven through a real tool-call turn against a canned HTTP server so
  # both the parent's and child's async request/response paths run for
  # real on one uv loop, same pattern as test_agent.c.
  test_lua_agent = executable('test_lua_agent',
    files('test_lua_agent.c', 'canned.c'),
    dependencies: [clmuv_dep, clmlua_dep],
    include_directories: [include_directories('.'), clm_inc, include_directories('../include')],
    c_args: test_cflags,
  )
  test('lua_agent', test_lua_agent, workdir: meson.project_source_root())
endif

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
# installs anything. clm_exe only exists when have_lua (src/meson.build) --
# the desktop binary requires the clmlua archive -- so this test is skipped
# too under -Dlua=disabled rather than referencing a target that was never
# built.
pymod = import('python')
py = pymod.find_installation('python3', required: false)
have_pyte = py.found() and run_command(
  py, '-c', 'import pyte', check: false).returncode() == 0

if have_lua and have_pyte
  test('tui',
    py,
    args: [files('tui/test_tui.py')],
    env: {'CLM_BIN': clm_exe.full_path()},
    depends: clm_exe,
    suite: 'tui',
    timeout: 120,
  )
elif not have_lua
  message('lua disabled; skipping TUI test (needs the clm desktop binary)')
else
  message('python3+pyte not found; skipping TUI test')
endif
