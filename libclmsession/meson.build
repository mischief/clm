# Session persistence: append-only JSONL transcript logs for save/resume
# (see include/clm/session.h). Kept out of the core for the same reason as
# libclmuv -- libclm itself stays IO-free (no filesystem paths, no XDG
# policy); frontends that want persistent sessions link this archive, an
# embedder that doesn't just doesn't.
clmsession_src = files('session.c')

clm_session_lib = static_library('clmsession', clmsession_src,
  c_args: ['-DCLM_BUILDING'] + clm_hardening_args,
  dependencies: [cjson_dep],
  include_directories: [clm_lib_inc, clm_inc],
  install: true,
)

clmsession_dep = declare_dependency(
  link_with: clm_session_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: [cjson_dep, clm_dep],
)
