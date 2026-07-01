project('clm', 'c',
  version: '0.1.1',
  license: 'ISC',
  meson_version: '>=1.1',
  default_options: [
    'c_std=c17',
    'warning_level=3',
    'werror=true',
  ])

cc = meson.get_compiler('c')

add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')
add_project_arguments('-fno-omit-frame-pointer', language: 'c')

# Hardening: warnings (from openbsd-gitea-runner)
hardening_args = [
  '-Wnull-dereference',
  '-Warray-bounds',
  '-Wsign-conversion',
  '-Wfloat-equal',
  '-Wformat',
  '-Wformat-security',
  '-Wuninitialized',
  '-Wshadow',
  '-Wimplicit-fallthrough',
  '-Wswitch',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wmissing-declarations',
  '-Wvla',
  '-Walloca',
  '-Wshift-overflow',
  '-Wframe-larger-than=2048',
  '-Wreturn-stack-address',
  '-Wuseless-cast',
  '-fsanitize-trap=all',
]

foreach a : hardening_args
  if cc.has_argument(a)
    add_project_arguments(a, language: 'c')
  endif
endforeach


# Suppress noisy warnings that don't apply cleanly across compilers
suppress_args = ['-Wno-unused-parameter', '-Wno-zero-length-array']

foreach a : suppress_args
  if cc.has_argument(a)
    add_project_arguments(a, language: 'c')
  endif
endforeach

clm_inc = include_directories('.')

version_h = configure_file(
  input: 'include/clm/version.h.in',
  output: 'version.h',
  configuration: {'VERSION': meson.project_version()},
  install: true,
  install_dir: get_option('includedir') / 'clm',
)

subdir('lib')
subdir('src')
subdir('man')

if get_option('tests')
  subdir('test')
  subdir('fuzz')
endif

cppcheck = find_program('cppcheck', required: false)
if cppcheck.found()
  run_target('cppcheck',
    command: [
      cppcheck,
      '--project=' + meson.project_build_root() / 'compile_commands.json',
      '--enable=warning,style,performance,portability',
      '--std=' + get_option('c_std'),
      '--suppress=normalCheckLevelMaxBranches',
      '--error-exitcode=1',
    ],
  )
endif
