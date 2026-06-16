#!/usr/bin/env bash
#
# Convert a raw bitmap font into src/font_data.c (the glyph table the kernel
# links in). The font file is 256 glyphs, each `height` bytes, one byte per row
# with the most-significant bit on the left (the .F08 / .F16 format). The glyph
# height is derived from the file size (size / 256), so the same tool handles an
# 8-pixel-tall .F08 or a 16-pixel-tall .F16.
#
# Usage:  tools/genfont.sh fonts/NAME.Fxx [src/font_data.c]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FONT="${1:?usage: genfont.sh fonts/NAME.Fxx [out.c]}"
OUT="${2:-$ROOT/src/font_data.c}"

[ -f "$FONT" ] || { echo "error: font '$FONT' not found"; exit 1; }

SIZE=$(stat -c %s "$FONT")
GLYPHS=256
if [ $(( SIZE % GLYPHS )) -ne 0 ]; then
    echo "error: '$FONT' is $SIZE bytes, not a multiple of $GLYPHS glyphs"; exit 1
fi
HEIGHT=$(( SIZE / GLYPHS ))

{
    echo "// Auto-generated from $(basename "$FONT") by tools/genfont.sh - do not edit."
    echo "// $GLYPHS glyphs, $HEIGHT bytes each (one byte per row, MSB = leftmost pixel)."
    echo
    echo '#include "font.h"'
    echo
    echo "unsigned char bbc_font[FONT_BYTES] = {"
    # Dump bytes as decimal, 12 per line, indented two spaces.
    od -An -v -tu1 "$FONT" \
        | awk '{ for (i = 1; i <= NF; i++) {
                     printf (n % 12 == 0 ? "  " : " ");
                     printf "%3d,", $i;
                     if (++n % 12 == 0) printf "\n";
                 } }
                END { if (n % 12 != 0) printf "\n" }'
    echo "};"
} > "$OUT"

echo "Generated $OUT from $FONT ($GLYPHS glyphs x $HEIGHT rows, ${SIZE} bytes)."
