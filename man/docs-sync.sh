#!/bin/sh
# SPDX-License-Identifier: ISC
# Mirror every man/*.[1-9] page into ../docs/ as markdown, via mandoc's own
# native "-T markdown" mode (mdoc -> markdown, not the other direction --
# see the design discussion this came out of: pandoc's markdown -> man
# writer emits font escapes mandoc doesn't implement and can't be linted
# by check-docs.sh, so mdoc stays the one source of truth and this is
# purely a generated, read-only view of it).
#
# mandoc's own docs call -T markdown lossy ("a very weak markup language,
# so all semantic markup is lost"), and two specific things it drops are
# ugly enough on GitHub to fix with a small sed pass, kept to real
# CommonMark syntax (not HTML -- an earlier version of this script
# switched to -T html -O fragment wrapped in a .md extension, which
# looked better on GitHub specifically but produces something that is
# not actually markdown at all; any real markdown renderer/linter/
# converter run over it would either choke or dump raw tags as literal
# text. reverted: better to keep these as genuine, portable markdown with
# two small known cosmetic gaps than something that only works in one
# specific renderer's HTML-passthrough leniency):
#
#  - .Xr cross-references render as inert "name(section)" text, since
#    mandoc has no way to know these pages are about to sit next to each
#    other as sibling files. rewritten below into a real markdown link
#    for any Xr target that matches another page in this same sync run;
#    a reference to something outside this project (errno(2), fnmatch(3),
#    etc) has nothing to link to and is correctly left as plain text.
#  - .Bl -tag list item bodies render as "> "-prefixed blockquotes (a
#    nested list, e.g. providers.*.api_key inside clm-config(5), comes
#    out doubly/triply nested as "> > "/"> > > "), which is a reasonable
#    one-to-one mapping in isolation but reads badly on GitHub for a
#    multi-paragraph body (nested-looking, not a real quotation). Since
#    nothing in these pages is an actual quotation, every leading run of
#    "> " is stripped from every such line, turning it back into a plain
#    paragraph regardless of nesting depth.
set -e

MANDOC="${1:?usage: docs-sync.sh MANDOC MANDIR DOCSDIR}"
MANDIR="${2:?usage: docs-sync.sh MANDOC MANDIR DOCSDIR}"
DOCSDIR="${3:?usage: docs-sync.sh MANDOC MANDIR DOCSDIR}"

mkdir -p "$DOCSDIR"

# Build the sed program: one Xr-linking substitution per page about to be
# generated in this run, plus the blanket "> " stripping rule.
sedprog=$(mktemp)
trap 'rm -f "$sedprog"' EXIT
: > "$sedprog"
for page in "$MANDIR"/*.[1-9]; do
	[ -f "$page" ] || continue
	base=$(basename "$page")
	name=${base%.*}
	section=${base##*.}
	# Escape sed/BRE metacharacters in the page name (clm-tool's '-' is
	# harmless as-is, but keep this robust for any future page name).
	esc=$(printf '%s' "$name" | sed 's/[.[\*^$/]/\\&/g')
	printf 's|%s(%s)|[%s(%s)](%s.md)|g\n' \
		"$esc" "$section" "$name" "$section" "$name" >> "$sedprog"
	# mandoc's markdown writer backslash-escapes every underscore in
	# running text (markdown's own italic-delimiter character), so
	# clm_agent(3) is literally "clm\_agent(3)" in the generated file.
	# Only add this second substitution when the name actually has an
	# underscore -- otherwise it's identical to the rule just above and
	# would double-substitute already-linked text (e.g. clm(1) has no
	# underscore, so without this guard both rules fire on the same
	# span, producing "[[clm(1)](clm.md)](clm.md)").
	case "$name" in
	*_*)
		escu=$(printf '%s' "$esc" | sed 's/_/\\\\_/g')
		printf 's|%s(%s)|[%s(%s)](%s.md)|g\n' \
			"$escu" "$section" "$name" "$section" "$name" >> "$sedprog"
		;;
	esac
done
# Strip every leading "> " (mandoc's rendering of a .Bl -tag item body --
# nested lists produce "> > ", "> > > ", etc, so repeat the substitution
# on the same line rather than stripping only the outermost level).
# Nothing in these pages is a real blockquote.
printf 's|^\(> \)*||\n' >> "$sedprog"

for page in "$MANDIR"/*.[1-9]; do
	[ -f "$page" ] || continue
	base=$(basename "$page")
	name=${base%.*}
	out="$DOCSDIR/$name.md"
	# -I os=clm: pin the .Os trailer instead of letting it default to
	# whatever uname()/os-release the machine running this happens to
	# report (Debian here, OpenBSD elsewhere, etc) -- otherwise every
	# re-sync from a different machine would diff on nothing but that,
	# with no real content change.
	"$MANDOC" -I os=clm -T markdown "$page" | sed -f "$sedprog" > "$out"
	echo "wrote $out"
done
