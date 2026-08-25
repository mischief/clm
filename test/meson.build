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

# Tiny TAP 13 harness shared by C test binaries. It intentionally has no
# production dependency or install target.
tap_inc = include_directories('../libtap')
tap_src = files('../libtap/tap.c')

test_tap = executable('test_tap',
  files('test_tap.c') + tap_src,
  include_directories: [tap_inc],
  c_args: test_cflags,
)
test('tap', test_tap, protocol: 'tap')

# Uses the uv host (clm_host_uv) + shell_exec, so it links the uv layer.
test_agent = executable('test_agent',
  files('test_agent.c', 'canned.c', '../libclm/props.c', '../libclm/history.c', '../libclm/models_list.c', '../libclm/text_clean.c') + tap_src,
  dependencies: [clmuv_dep],
  include_directories: [tap_inc, include_directories('.'), clm_inc, include_directories('../include')],
  c_args: test_cflags,
)
test('agent', test_agent, protocol: 'tap')

# mcp transport regressions use the uv host plus the local canned http server.
test_mcp = executable('test_mcp',
  files('test_mcp.c', 'canned.c') + tap_src,
  dependencies: [clmuv_dep],
  include_directories: [tap_inc, include_directories('.'), clm_inc, include_directories('../include')],
  c_args: test_cflags,
)
test('mcp', test_mcp, protocol: 'tap')

# compile the mcp client against a delayed http stub so teardown can settle
# each cancelled request after clm_mcp_client_free returns.
test_mcp_client = executable('test_mcp_client',
  files('test_mcp_client.c', '../libclmuv/mcp_client.c') + tap_src + clm_core_src + [version_h],
  dependencies: clmuv_deps,
  include_directories: [tap_inc, clm_lib_inc, clm_inc, include_directories('../include')],
  c_args: ['-DCLM_BUILDING'] + clm_hardening_args + test_cflags,
)
test('mcp_client', test_mcp_client, protocol: 'tap')

# the callback lifetime test runs itself as a stdio server, lets that server
# exit after one handshake, and verifies status callbacks across the restart.
test_mcp_ready = executable('test_mcp_ready',
  files('test_mcp_ready.c') + tap_src,
  dependencies: [clmuv_dep],
  include_directories: [tap_inc, clm_inc, include_directories('../include')],
  c_args: test_cflags,
)
test('mcp_ready', test_mcp_ready, protocol: 'tap', timeout: 15)

# malformed urls complete in curl's initial socket action. this checks that
# completion does not publish a dead cancellation handle.
test_http_async = executable('test_http_async',
  files('test_http_async.c') + tap_src,
  dependencies: [clmuv_dep],
  include_directories: [tap_inc, clm_inc, include_directories('../include')],
  c_args: test_cflags,
)
test('http_async', test_http_async, protocol: 'tap')

# response accounting is pure and uses tiny limits so overflow and cap tests
# do not need a server or large allocations.
test_http_response_buffer = executable('test_http_response_buffer',
  files('test_http_response_buffer.c',
    '../libclmuv/http_response_buffer.c') + tap_src,
  include_directories: [tap_inc, include_directories('../libclmuv'),
    include_directories('..')],
  c_args: test_cflags,
)
test('http_response_buffer', test_http_response_buffer, protocol: 'tap')

# Session persistence test: pure (no network, no loop). Compiles history.c
# and session.c directly, same pattern as test_agent, so internals are
# reachable without going through the shared library's export list.
test_session = executable('test_session',
  files('test_session.c', '../libclm/history.c', '../libclmsession/session.c') + tap_src,
  dependencies: [cjson_dep],
  include_directories: [tap_inc, include_directories('.'), clm_inc,
    include_directories('../include'), include_directories('../libclmsession')],
  c_args: test_cflags,
)
test('session', test_session, protocol: 'tap')

# Lua plugin test: loads plugins from ../plugins/ and verifies tool dispatch.
# Builds a clm_host_uv, so it links the uv layer too. Only built when
# have_lua (src/meson.build) -- clmlua_dep, needed for clm_lua_env_new etc,
# only exists in that case (../libclmlua's own lua_dep.found() guard). No more
# CLM_LUA-guarded skip-stub main() in the .c file: the target itself is now
# the seam, not a preprocessor branch inside it.
if have_lua
  test_lua = executable('test_lua_plugin',
    files('test_lua_plugin.c', 'canned.c') + tap_src,
    dependencies: [clmuv_dep, clmlua_dep],
    include_directories: [tap_inc, clm_inc, include_directories('../include')],
    c_args: test_cflags,
  )
  test('lua_plugin', test_lua, protocol: 'tap', workdir: meson.project_source_root())
endif

# md_render unit test: pure, links only the internal md_render static library
# (no ncurses, no libclm), so the markdown-to-runs logic is tested in isolation.
test_md = executable('test_md',
  files('test_md.c') + tap_src,
  link_with: [md_render_lib],
  include_directories: [tap_inc, md_render_inc],
  c_args: ['-D_XOPEN_SOURCE=700'], # wcwidth(3) is XSI
)
test('md', test_md, protocol: 'tap', suite: 'md')

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
  test('tui_basic',
    py,
    args: [files('tui/test_tui.py'), 'connection', 'offline', 'agent',
           'markdown', 'resize', 'margin', 'editing', 'history', 'paste'],
    env: {'CLM_BIN': clm_exe.full_path()},
    depends: clm_exe,
    suite: 'tui',
    timeout: 60,
  )
  test('tui_scrollback',
    py,
    args: [files('tui/test_tui.py'), 'scrollback', 'scroll_stable', 'end_key'],
    env: {'CLM_BIN': clm_exe.full_path()},
    depends: clm_exe,
    suite: 'tui',
    timeout: 60,
  )
  test('tui_interaction',
    py,
    args: [files('tui/test_tui.py'), 'commands', 'effort', 'queueing',
      'permission', 'allow_all', 'cancel', 'cancel_tools',
      'tool_escapes'],
    env: {'CLM_BIN': clm_exe.full_path()},
    depends: clm_exe,
    suite: 'tui',
    timeout: 60,
  )
  test('tui_session',
    py,
    args: [files('tui/test_tui.py'), 'session', 'session_compact',
      'scratch', 'peers'],
    env: {'CLM_BIN': clm_exe.full_path()},
    depends: clm_exe,
    suite: 'tui',
    timeout: 60,
  )
elif not have_lua
  message('lua disabled; skipping TUI test (needs the clm desktop binary)')
else
  message('python3+pyte not found; skipping TUI test')
endif
