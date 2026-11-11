#!/bin/sh
# SPDX-License-Identifier: ISC
# Runs the clm-gtk test on a private session bus and runtime directory, so
# the accessibility bus it starts stays off the real one. $1 is the test
# harness, the rest its arguments.
set -eu
# A desktop with accessibility off exports these. The test needs it on.
unset GTK_A11Y NO_AT_BRIDGE
dir=$(mktemp -d "${TMPDIR:-/tmp}/clm-gtk-run.XXXXXX")
cleanup() {
	fusermount3 -uz "$dir/doc" 2>/dev/null || true
	rm -rf "$dir" 2>/dev/null || { sleep 1; rm -rf "$dir"; }
}
trap cleanup EXIT
# Services the bus starts write to stdout; keep that off the test's output.
XDG_RUNTIME_DIR=$dir ADW_DISABLE_PORTAL=1 GDK_DEBUG=no-portals \
    dbus-run-session -- "$@" >&2
