# template

Copyright (c) 2026 Nick Owens <mischief@offblast.org>  
ISC License — see [LICENSE](LICENSE)

## Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

To skip the test suite:

```sh
meson setup build -Dtests=false
```

## Sanitizers

**Linux** — full ASan + UBSan:

```sh
meson setup build-asan \
  -Db_sanitize=address,undefined \
  -Db_lundef=false \
  -Ddefault_library=static \
  -Dc_link_args='-static-libasan'
meson test -C build-asan
```

`-Ddefault_library=static` avoids ASan runtime ordering issues with shared libraries.
`-static-libasan` is needed if anything else is preloaded ahead of `libasan.so`.

**OpenBSD** — ASan and the UBSan runtime library are not in base clang.
Trap-based UBSan instruments the binary without a runtime; violations crash
with SIGILL. `-Wl,--no-execute-only` opts out of W^X for this build only:

```sh
meson setup build-ubsan \
  -Dc_args="-fsanitize=undefined -fsanitize-trap=undefined" \
  -Dc_link_args="-Wl,--no-execute-only"
meson test -C build-ubsan
```

## Tooling

Format source files in place:

```sh
ninja -C build clang-format
```

Static analysis:

```sh
ninja -C build clang-tidy
ninja -C build cppcheck
```

Man page lint and doc coverage:

```sh
meson test -C build --suite docs
```

## Fuzzing

Fuzz targets are built alongside tests but not run by `meson test`.
To run with AFL:

```sh
meson compile -C build fuzz_tmpl
afl-fuzz -i fuzz/corpus -o fuzz/out_tmpl -- ./build/fuzz/fuzz_tmpl
```
