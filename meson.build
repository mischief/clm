project('clm', 'c',
  version: '0.1.1',
  license: 'ISC',
  meson_version: '>=1.1',
  default_options: [
    'c_std=c17',
    'warning_level=3',
    'werror=false',  # applied only to clm targets, not subprojects
    # debugoptimized (-O2 -g, asserts on), not plain debug (-O0): several
    # of our own -W hardening flags below (-Wframe-larger-than via
    # inlining, -Wnull-dereference's interprocedural analysis) and
    # -D_FORTIFY_SOURCE=3 further down only actually do anything at -O1+.
    # At -O0 they're silently inert, so real bugs they'd catch (two found
    # this way: libclmuv/tool_bg.c's asprintf-failure handling, a
    # provable-null path in libclm/provider_anthropic.c) would otherwise
    # go untested by the default build every contributor actually runs.
    # -O2 over -O3: this
    # project is I/O/string-handling bound, not compute-bound, and -O3
    # buys nothing here beyond -O2 while pulling in extra inlining that
    # otherwise just makes the frame-size/backtrace tradeoffs below worse
    # for no benefit.
    'buildtype=debugoptimized',
    # PIE hardening via meson's built-in option rather than hand-rolled
    # -fPIE/-pie: b_pie correctly scopes -pie to executable() targets only
    # and leaves shared_library() targets (libclm.so) alone -- unlike
    # add_project_link_arguments, which applies everywhere and breaks
    # linking a shared lib with "-pie" (it forces executable-style linking,
    # which then wants a main()). Overridable with -Db_pie=false.
    'b_pie=true',
  ])

cc = meson.get_compiler('c')

# -Dstatic=true is a one-flag substitute for
# -Ddefault_library=static --prefer-static: it forces libclm itself to a
# static archive regardless of default_library, and forces every
# pkg-config dependency lookup below to prefer .a over .so. Read early (not
# just where static_kw is used below) since the PIE hardening block further
# down needs to know it too -- -fPIE/-pie and a fully static link don't mix
# without -static-pie, which this project doesn't ask for.
want_static = get_option('static')
static_kw = want_static ? {'static': true} : {}

# Feature test macros to expose POSIX/GNU extensions (strdup, asprintf,
# getline, strsignal, struct addrinfo, pthread_rwlock_t, ...) without
# breaking the platform's own headers. illumos/Solaris gates all of this
# behind __EXTENSIONS__ instead of glibc's _GNU_SOURCE/_DEFAULT_SOURCE --
# defining _GNU_SOURCE there does nothing useful, and conversely
# __EXTENSIONS__ alone does nothing on glibc (this was tried and broke
# strdup/asprintf/pthread_rwlock_t/struct addrinfo declarations in
# libclmuv/tool_bg.c, libclmuv/mcp_client.c, libclmuv/http_async.c on Linux).
if host_machine.system() == 'sunos'
  add_project_arguments('-D__EXTENSIONS__', language: 'c')
else
  add_project_arguments('-D_GNU_SOURCE', language: 'c')
endif
add_project_arguments('-fno-omit-frame-pointer', language: 'c')

# Now that the default buildtype optimizes (see debugoptimized above): a
# tail call at -O1+ can get turned into a jump that reuses the caller's
# stack frame instead of a real call, which quietly erases that caller from
# every future backtrace/coredump -- the sharpest debuggability cost of
# turning optimization on by default. Keep real call frames instead; this
# project's few genuinely tail-recursive spots aren't hot enough to need it.
if cc.has_argument('-fno-optimize-sibling-calls')
  add_project_arguments('-fno-optimize-sibling-calls', language: 'c')
endif

# -finline-functions-called-once (on by default at -O1+) force-inlines any
# static function with exactly one call site, regardless of size -- which
# is most of test/test_agent.c's test_*() helpers, each called once from
# main(). At -O2 that merges every inlined helper's own locals (a ~1KB
# struct tstate apiece) into main()'s single frame, tripping
# -Wframe-larger-than for no benefit (these aren't hot paths, and main()
# calling them normally rather than having them flattened in is *better*
# for reading a crash backtrace, not worse). Disabling just this heuristic
# leaves the general inliner's normal cost-based decisions untouched.
if cc.has_argument('-fno-inline-functions-called-once')
  add_project_arguments('-fno-inline-functions-called-once', language: 'c')
endif

# musl has no <sys/queue.h> (a BSD/glibc extension we use for TAILQ_*).
# compat/sys/queue.h is a vendored copy (BSD-3-Clause, from glibc, itself
# lifted from the original CSRG sources). -idirafter puts it after the
# normal system search path, so glibc/BSD's real header always wins and
# this is only consulted where the system genuinely has none.
add_project_arguments('-idirafter', meson.project_source_root() / 'compat',
  language: 'c')

# Suppress noisy warnings that don't apply cleanly across compilers.
# -Wno-zero-length-array: OpenBSD's own libuv package (system /usr/local/
# include/uv/unix.h) computes a padding array as
# sizeof(pthread_barrier_t) - sizeof(struct _uv_barrier*), which is 0 on
# OpenBSD's pthread_barrier_t layout -- a system header we don't control,
# not our own code.
suppress_args = ['-Wno-unused-parameter', '-Wno-useless-cast', '-Wno-zero-length-array']

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

# Build a list of hardening flags that are supported by this compiler.
# Applied to clm targets only, not to subprojects (via c_args in each target).
clm_hardening_args = ['-Werror']  # treat warnings as errors, but only for clm
foreach a : hardening_args
  if cc.has_argument(a)
    clm_hardening_args += [a]
  endif
endforeach
# Add suppression flags only if supported (unrecognized flags break header processing)
foreach a : suppress_args
  if cc.has_argument(a)
    clm_hardening_args += [a]
  endif
endforeach

# Hardening: exploit mitigations (FORTIFY_SOURCE, RELRO/BIND_NOW; PIE is
# 'b_pie=true' in default_options above instead, see the comment there).
# Unlike hardening_args above, none of this is known-portable yet -- it's
# only been observed working by accident of this machine's Gentoo toolchain
# defaulting PIE/RELRO/BIND_NOW on already (confirmed via readelf on a build
# here; -D_FORTIFY_SOURCE wasn't on at all). RELRO/BIND_NOW below is gated
# behind an actual link check rather than "gcc or clang", since OpenBSD's
# ld and other non-GNU linkers may reject or no-op it differently than
# Debian's does -- verify on both before trusting this section.

# _FORTIFY_SOURCE needs at least -O1 to do anything; glibc warns (a hard
# error under -Werror) if it's defined at -O0 -- still reachable via an
# explicit -Dbuildtype=debug/plain override even though debugoptimized is
# the default now. -U first: some distro gcc specs already predefine it
# (Debian/Gentoo do, at -O1+) and redefining without undefining first is
# itself a hard error under -Werror.
if get_option('optimization') not in ['0', 'g']
  add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3', language: 'c')
endif

# RELRO + BIND_NOW: make the GOT read-only after startup instead of leaving
# it writable for the process's whole lifetime. Safe on a shared library
# too (unlike -pie), so this one is a plain add_project_link_arguments.
foreach a : ['-Wl,-z,relro', '-Wl,-z,now']
  if cc.has_link_argument(a)
    add_project_link_arguments(a, language: 'c')
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

subdir('libclm')
subdir('libclmlua')
subdir('libclmuv')
subdir('libclmsession')
subdir('libemoji')
subdir('src')
subdir('man')

if get_option('tests')
  subdir('test')
  if cc.has_argument('-fsanitize=fuzzer') and cc.has_argument('-fsanitize=address')
    subdir('fuzz')
  endif
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
