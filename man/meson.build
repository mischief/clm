mandoc = find_program('mandoc', required: false)

install_man(files(
  'clm.1',
  'clm_agent.3',
  'clm-config.5',
  'clm-tool.5',
))

# Doc coverage: every exported clm_* symbol in libclm should have a .Fn/.Fo
# entry in some man/*.3 page. A run_target (`ninja -C build check-docs`),
# not a test() -- this intentionally doesn't gate a plain `meson test`,
# since 100% API coverage is an ongoing target to check on demand, not a
# pass/fail bar every test run should enforce. Requires the real
# libclm.so (not a static archive) since it greps `nm -D` output.
if mandoc.found() and not want_static
  run_target('check-docs',
    command: [find_program('check-docs.sh'),
      clm_lib.full_path(), meson.current_source_dir()],
  )
endif
