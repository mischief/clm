#!/bin/sh
# Build static ncursesw for an aarch64 musl target with zig cc, for use
# with cross/aarch64-linux-musl-zig. ncurses has no WrapDB wrap, so the
# TUI's one hard dependency has to come from a sysroot.
#
#   cross/build-ncurses.sh [tarball] [sysroot]
#
# Defaults: ./ncurses-6.5.tar.gz, $HOME/aarch64-musl-sysroot. The sysroot
# must match the sys_root and pkg_config_libdir in the cross file.
set -eu

TARBALL=${1:-ncurses-6.5.tar.gz}
SYSROOT=${2:-$HOME/aarch64-musl-sysroot}
TARGET=${TARGET:-aarch64-linux-musl}
BUILD=${BUILD:-$(cc -dumpmachine)}
WORK=$(mktemp -d)

trap 'rm -rf "$WORK"' EXIT

command -v zig >/dev/null || { echo "zig not on PATH" >&2; exit 1; }
TARBALL=$(cd "$(dirname "$TARBALL")" && pwd)/$(basename "$TARBALL")
SYSROOT=$(mkdir -p "$SYSROOT" && cd "$SYSROOT" && pwd)

tar xf "$TARBALL" -C "$WORK"
cd "$WORK"/ncurses-*

# TERMINFO/TERMINFO_DIRS unset: configure otherwise bakes whatever
# terminfo tree the calling terminal uses into the library.
#
# --with-pic: the static PIE link rejects non-PIC relocations.
# --with-fallbacks: a target with no terminfo database still needs an
#   entry, or the TUI dies at startup whatever TERM says.
env -u TERMINFO -u TERMINFO_DIRS ./configure \
	--host="$TARGET" \
	--build="$BUILD" \
	--prefix="$SYSROOT" \
	CC="zig cc -target $TARGET" \
	AR="zig ar" \
	RANLIB="zig ranlib" \
	CFLAGS="-fPIC -O2" \
	--with-build-cc=cc \
	--without-shared \
	--with-normal \
	--with-pic \
	--enable-widec \
	--without-cxx \
	--without-cxx-binding \
	--without-ada \
	--without-manpages \
	--without-progs \
	--without-tests \
	--disable-stripping \
	--enable-pc-files \
	--with-default-terminfo-dir=/usr/share/terminfo \
	--with-pkg-config-libdir="$SYSROOT/lib/pkgconfig" \
	--with-fallbacks=ansi,dumb,linux,screen,screen-256color,vt100,vt220,xterm,xterm-color,xterm-256color

env -u TERMINFO make -j"$(nproc 2>/dev/null || echo 4)"

# install.libs, not install: the full install target writes the terminfo
# database to the *host* system's /usr/share/terminfo and fails.
env -u TERMINFO make install.libs

echo "installed libncursesw.a and ncursesw.pc under $SYSROOT"
