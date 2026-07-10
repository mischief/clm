# Desktop uv/curl layer: the libuv+libcurl transport (host_uv + the async curl
# engine) plus the shell_exec tool. Kept OUT of the core so libclm itself pulls
# in no libuv/libcurl. A static archive: frontends that want the desktop host
# link it alongside libclm; an embedder supplying its own host just doesn't.
# http_async.c calls clm_debug (../libclm/log.h) -- that's CLM_API-exported
# from libclm itself (see log.h), so this archive just links clm_dep for it
# rather than compiling its own private copy of log.c.
clmuv_src = files('http_async.c', 'host_uv.c', 'tool_shell.c', 'tool_bg.c', 'mcp_client.c')

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
