#!/bin/sh
# SPDX-License-Identifier: ISC
# Mirror every man/*.[1-9] page into ../docs/ as markdown, via mandoc's own
# native "-T markdown" mode (mdoc -> markdown, not the other direction --
# see the design discussion this came out of: pandoc's markdown -> man
# writer emits font escapes mandoc doesn't implement and can't be linted
# by check-docs.sh, so mdoc stays the one source of truth and this is
# purely a generated, read-only view of it).
set -e

MANDOC="${1:?usage: docs-sync.sh MANDOC MANDIR DOCSDIR}"
MANDIR="${2:?usage: docs-sync.sh MANDOC MANDIR DOCSDIR}"
DOCSDIR="${3:?usage: docs-sync.sh MANDOC MANDIR DOCSDIR}"

mkdir -p "$DOCSDIR"

for page in "$MANDIR"/*.[1-9]; do
	[ -f "$page" ] || continue
	base=$(basename "$page")
	name=${base%.*}
	out="$DOCSDIR/$name.md"
	"$MANDOC" -T markdown "$page" > "$out"
	echo "wrote $out"
done
