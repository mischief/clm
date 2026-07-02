#!/usr/bin/env python3
#
# Generate the T-Deck console font header from a monospaced BDF bitmap font.
# Single purpose: BDF in, C header out. Emits:
#   - font8x16[256][H]   glyph bitmaps indexed by codepoint 0x00..0xFF
#   - font_symbols[]     sparse codepoint -> glyph-index map for a curated set
#                        of >ASCII symbols LLMs emit (rendered as real glyphs;
#                        anything absent is transliterated by the console)
#
# The renderer packs one byte per row (bit 7 = leftmost pixel), so the font
# width must be <= 8. BDF stores each bitmap row left-justified in whole bytes,
# which matches; we keep the high byte of each row verbatim.
import os, sys

_HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(_HERE, "spleen-6x12.bdf")
OUT = os.path.join(_HERE, os.pardir, "font8x16.h")

# Attribution / license of the SOURCE font, emitted into the generated header.
FONT_NAME = "Spleen 6x12"
FONT_LICENSE = "BSD-2-Clause"
FONT_ORIGIN = "https://github.com/fcambus/spleen"

# Codepoints worth rendering as real glyphs when the font has them.
WANT = [
    0x00A0, 0x00A3, 0x00A9, 0x00AE, 0x00B0, 0x00B1, 0x00B5, 0x00B7,
    0x00BC, 0x00BD, 0x00BE, 0x00D7, 0x00F7, 0x20AC,
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
    0x2032, 0x2033, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212,
    0x2248, 0x2260, 0x2264, 0x2265,
]


def parse_bdf(path):
    """Return (width, height, {codepoint: [row_byte, ...]})."""
    w = h = None
    glyphs = {}
    cp = None
    reading = False
    rows = []
    for ln in open(path):
        p = ln.split()
        if not p:
            continue
        k = p[0]
        if k == "FONTBOUNDINGBOX":
            w, h = int(p[1]), int(p[2])
        elif k == "ENCODING":
            cp = int(p[1])
        elif k == "BITMAP":
            reading, rows = True, []
        elif k == "ENDCHAR":
            reading = False
            if cp is not None and cp >= 0:
                glyphs[cp] = [int(r[:2], 16) for r in rows]  # high byte/row
            cp = None
        elif reading:
            rows.append(k)
    return w, h, glyphs


w, h, glyphs = parse_bdf(SRC)
assert w and w <= 8, "renderer packs one byte per row; width must be <= 8"


def fit(rows):
    """Pad/truncate a glyph's row list to exactly H rows."""
    return (rows + [0] * h)[:h]


base = [fit(glyphs.get(i, [])) for i in range(256)]

# Assign symbols. Those <= 0xFF already live at their own index in base; higher
# codepoints borrow an unused control slot whose bitmap we overwrite.
free = iter(list(range(0x01, 0x20)) + [0x7F] + list(range(0x80, 0xA0)))
symbols, missing = [], []
for cp in WANT:
    if cp not in glyphs:
        missing.append(cp)
        continue
    if cp <= 0xFF:
        slot = cp
    else:
        slot = next(free)
        base[slot] = fit(glyphs[cp])
    symbols.append((cp, slot))
symbols.sort()

out = []
out.append("/*")
out.append(" * GENERATED FILE - DO NOT EDIT.")
out.append(" * Produced by %s from %s" % (
    os.path.basename(sys.argv[0] or "bdf2font.py"), os.path.basename(SRC)))
out.append(" * (a %dx%d BDF bitmap font); re-run the generator to regenerate." % (w, h))
out.append(" *")
out.append(" * Embedded glyph bitmaps of: %s" % FONT_NAME)
out.append(" * Source font license: %s" % FONT_LICENSE)
out.append(" * Source font origin:  %s" % FONT_ORIGIN)
out.append(" * The font's own license (above) governs these glyph bitmaps; see the")
out.append(" * accompanying font LICENSE file. This header is data, not clm source.")
out.append(" */")
out.append("#ifndef CLM_FONT8X16_H")
out.append("#define CLM_FONT8X16_H")
out.append("#include <stdint.h>")
out.append("#define FONT_W %d" % w)
out.append("#define FONT_H %d" % h)
out.append("")
out.append("static const uint8_t font8x16[256][%d] = {" % h)
for g in base:
    out.append("    {" + ",".join("0x%02x" % b for b in g) + "},")
out.append("};")
out.append("")
out.append("/* Curated codepoint -> glyph index, sorted by codepoint (binary search). */")
out.append("struct clm_glyph_map { uint16_t cp; uint8_t idx; };")
out.append("static const struct clm_glyph_map font_symbols[] = {")
for cp, idx in symbols:
    out.append("    {0x%04x, %d}," % (cp, idx))
out.append("};")
out.append("#define FONT_NSYMBOLS %d" % len(symbols))
out.append("")
out.append("#endif /* CLM_FONT8X16_H */")

open(OUT, "w").write("\n".join(out) + "\n")
print("wrote %s: %dx%d, 256 glyphs + %d symbols" % (OUT, w, h, len(symbols)))
if missing:
    print("  not in font (transliterated):",
          ", ".join("U+%04X" % c for c in missing))
