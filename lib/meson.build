# Portable core: the agent engine, tool registry, and OpenAI-compatible client.
# Deliberately free of libuv and libcurl -- transport and timers come in through
# a struct clm_host (see clm/host.h). The only hard dependency is cJSON (ESP-IDF
# ships cJSON built-in, so this avoids vendoring json-c into the esp32 tree).
clm_core_src = files('agent.c', 'http.c', 'llm.c', 'log.c', 'tools.c', 'history.c', 'props.c', 'models_list.c', 'ratelimit.c', 'provider.c', 'provider_openai.c', 'provider_anthropic.c')

cjson_dep = dependency('libcjson', fallback: ['cjson', 'libcjson_dep'], kwargs: static_kw)

clm_core_deps = [
  cjson_dep,
]

# lib/'s own optional feature: whether Lua plugin/agent-profile support is
# built at all. Independent of whether any particular consumer (the clm
# desktop binary, tests) wants it -- see clmlua_dep below, which mirrors
# clmuv_dep's pattern: an always-buildable-when-available static archive,
# kept OUT of libclm itself, that a consumer either links or doesn't. This
# is what actually lets an embedded build (e.g. a future esp32 target) omit
# Lua/liblua entirely without any #ifdef CLM_LUA anywhere in C source --
# the seam is a link-time decision, not a preprocessor one.
lua_dep = dependency('lua5.4', required: get_option('lua'),
  fallback: ['lua', 'lua_dep'], kwargs: static_kw)

# Lib sources find "clm/..." via ../include, and "useful.h"/"banned.h" via
# ../include and the project root.
clm_lib_inc = include_directories('.', '..', '../include')

# -Dstatic=true forces a static archive here regardless of
# default_library, so a plain `meson setup -Dstatic=true` is enough (no
# need to also pass -Ddefault_library=static --prefer-static). Meson has
# no function references, so the call is duplicated rather than picked
# via a variable.
clm_lib_common_args = {
  'c_args': ['-DCLM_BUILDING'] + clm_hardening_args,
  'gnu_symbol_visibility': 'hidden',
  'dependencies': clm_core_deps,
  'include_directories': [clm_lib_inc, clm_inc],
  'install': true,
  'install_dir': get_option('libdir'),
}
if want_static
  clm_lib = static_library('clm', clm_core_src + [version_h],
    kwargs: clm_lib_common_args)
else
  clm_lib_link_args = []
  if cc.has_link_argument('-Wl,--version-script=/dev/null')
    clm_lib_link_args += ['-Wl,--version-script=' + meson.current_source_dir() / 'libclm.map']
  endif
  clm_lib = library('clm', clm_core_src + [version_h],
    # ABI version: bump only on ABI breaks, independent of the release version.
    version: meson.project_version(),
    soversion: '0',
    link_args: clm_lib_link_args,
    kwargs: clm_lib_common_args)
endif

# Core dependency for in-tree consumers (tests, frontends).
clm_dep = declare_dependency(
  link_with: clm_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: clm_core_deps,
)

# Lua plugin support: registers Lua-backed tools through the same clm_tool_fn
# callback every other tool provider uses (see tools.c/clm.h) -- lua_plugin.c
# only ever calls INTO the core (clm_agent_*, clm_debug), never the reverse,
# so this can live entirely outside libclm as its own static archive, exactly
# like clmuv_lib below. That keeps libclm itself free of a liblua dependency
# unconditionally, while a consumer that wants Lua support links clmlua_dep.
# log.c is duplicated here for the same reason clmuv_src duplicates it:
# clm_debug is library-internal (clm/log.h is explicitly "not part of the
# public contract and not installed"), so an out-of-tree consumer linking
# this archive against an already-built libclm has no other way to get it.
clmlua_src = files('lua_plugin.c', 'lua_json.c', 'lua_http.c', 'log.c')

if lua_dep.found()
  clm_lua_lib = static_library('clmlua', clmlua_src,
    c_args: ['-DCLM_BUILDING'] + clm_hardening_args,
    dependencies: [lua_dep, clm_core_deps],
    include_directories: [clm_lib_inc, clm_inc],
    install: true,
  )

  clmlua_dep = declare_dependency(
    link_with: clm_lua_lib,
    include_directories: include_directories('..', '../include'),
    dependencies: [lua_dep, clm_dep],
  )
endif

# Desktop uv/curl layer: the libuv+libcurl transport (host_uv + the async curl
# engine) plus the shell_exec tool. Kept OUT of the core so libclm itself pulls
# in no libuv/libcurl. A static archive: frontends that want the desktop host
# link it alongside libclm; an embedder supplying its own host just doesn't.
# log.c is included here too: clm_debug is a library-internal helper (hidden in
# libclm.so, so not visible across the .so boundary), and http_async.c uses it.
# A private copy in the archive keeps clm_debug off the public ABI.
clmuv_src = files('http_async.c', 'host_uv.c', 'tool_shell.c', 'tool_bg.c', 'mcp_client.c', 'log.c')

clmuv_deps = [
  # Trimmed to the protocols/features clm actually uses (HTTP/HTTPS) --
  # cuts LDAP/mail/RTSP/telnet/tftp/gopher/mqtt/dict/smb/rtmp/FTP/file
  # support and curl's own CLI tool + test/unittest suites out of a
  # from-source build. ssl/openssl enabled, but ssl-default-backend left
  # on its 'implicit' default: curl's option choice literal ('openssl')
  # doesn't match its own internal backend-list literal ('openssl_opt'),
  # so setting it explicitly here just errors out -- 'implicit' correctly
  # auto-selects the only backend actually enabled anyway. openssl is the
  # only TLS backend with its own WrapDB wrap (no mbedtls/wolfssl/bearssl
  # wrap exists); openssl.wrap alongside this one is what curl's own
  # internal dependency('openssl') falls back to.
  dependency('libcurl', fallback: ['curl', 'libcurl_dep'],
    default_options: [
      'tool=disabled',
      'tests=disabled',
      'unittests=disabled',
      'ldap=disabled',
      'ldaps=disabled',
      'rtsp=disabled',
      'smtp=disabled',
      'pop3=disabled',
      'imap=disabled',
      'telnet=disabled',
      'tftp=disabled',
      'gopher=disabled',
      'mqtt=disabled',
      'dict=disabled',
      'smb=disabled',
      'rtmp=disabled',
      'ftp=disabled',
      'file=disabled',
      # ssh disabled: libssh2 links its own crypto against the *system*
      # OpenSSL, which then ends up loaded in the same process alongside
      # our vendored one -- two different libcrypto builds mapped in at
      # once, exactly the outcome the openssl.wrap was added to avoid.
      'ssh=disabled',
      # curl has its own optional libuv dependency (debug-only, event-based
      # test harness) that we never asked for -- but since it's 'auto',
      # forcefallback resolves it too, and it does so *before* our own
      # dependency('libuv', ...) call below gets to add build_tests=false
      # to the shared subprojects/libuv-v1.51.0 tree, so libuv's own test
      # suite (broken on illumos, see below) got built anyway. Disabling
      # outright since tests=disabled above means curl never uses it.
      'libuv=disabled',
      # brotli/psl/idn/http2/zstd are all 'auto' -- clm's own use of curl
      # (JSON APIs over plain HTTP/1.1 HTTPS, not general-purpose
      # fetching) needs none of brotli/zstd response compression, PSL-
      # based cookie domain checks, IDN host names, or HTTP/2. Disabling
      # all five outright rather than depending on whichever optional
      # libs (and, for a --prefer-static build, static archives
      # specifically -- not guaranteed even when the shared lib is
      # present, e.g. libbrotlidec/libpsl on a stock Gentoo box) a given
      # system happens to have lying around.
      'brotli=disabled',
      'psl=disabled',
      'idn=disabled',
      'http2=disabled',
      'zstd=disabled',
      'http=enabled',
      'ssl=enabled',
      'openssl=enabled',
      # Neither curl's nor openssl's own project() sets its own c_std, so
      # both yield to our root's 'c_std=c17' (strict ISO C17, no GNU
      # extensions). That breaks plain POSIX calls glibc only exposes
      # when __STRICT_ANSI__ is off -- fileno()/fdopen() in curl's own
      # lib/fopen.c, usleep() in openssl's e_os.h -- plus openssl's
      # legacy inline-asm bignum code, which needs the GNU `asm` keyword
      # outright (asm=disabled below sidesteps that one). Override c_std
      # for just these two subprojects rather than loosening our own
      # root -std for no reason of our own code's.
      'c_std=gnu17',
      'openssl:c_std=gnu17',
      'openssl:asm=disabled',
    ],
    kwargs: static_kw),
  # build_tests/build_benchmarks off: as a fallback subproject we only want
  # libuv_dep, and libuv's own test suite doesn't build clean on illumos
  # (test-pipe-sendmsg.c assumes Linux's struct msghdr layout, unrelated to
  # anything clm actually uses).
  dependency('libuv', fallback: ['libuv', 'libuv_dep'],
    default_options: ['build_tests=false', 'build_benchmarks=false'],
    kwargs: static_kw),
  cjson_dep,
]

clm_uv_lib = static_library('clmuv', clmuv_src,
  c_args: ['-DCLM_BUILDING'] + clm_hardening_args,
  dependencies: clmuv_deps,
  include_directories: [clm_lib_inc, clm_inc],
  install: true,
)

# Consumers link the uv layer plus the core it calls into.
clmuv_dep = declare_dependency(
  link_with: clm_uv_lib,
  include_directories: include_directories('..', '../include'),
  dependencies: [clmuv_deps, clm_dep],
)
