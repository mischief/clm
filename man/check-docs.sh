#!/bin/sh
# SPDX-License-Identifier: ISC
# Verify all exported tmpl_* symbols are documented in man pages.
# Exits non-zero if any symbols are undocumented.
set -e

LIBSO="$1"
MANDIR="$2"

if [ ! -f "$LIBSO" ]; then
	echo "error: $LIBSO not found" >&2
	exit 1
fi

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

nm -D --defined-only "$LIBSO" | awk '/( T | D | B )/{print $3}' \
	| grep '^tmpl_' | sed 's/@@.*//' | sort -u > "$TMPD/exported.txt"

grep -h '\.\(Fn\|Fo\) ' "$MANDIR"/*.3 2>/dev/null \
	| awk '{print $2}' | sort -u > "$TMPD/documented.txt"

TOTAL=$(wc -l < "$TMPD/exported.txt")
DOCUMENTED=$(comm -12 "$TMPD/exported.txt" "$TMPD/documented.txt" | wc -l)
UNDOCUMENTED=$(comm -23 "$TMPD/exported.txt" "$TMPD/documented.txt" | wc -l)

echo "doc coverage: $DOCUMENTED/$TOTAL symbols documented"

if [ "$UNDOCUMENTED" -gt 0 ]; then
	echo "undocumented:"
	comm -23 "$TMPD/exported.txt" "$TMPD/documented.txt" | sed 's/^/  /'
fi

[ "$UNDOCUMENTED" -eq 0 ]
