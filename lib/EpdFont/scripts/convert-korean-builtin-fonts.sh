#!/bin/bash
# Regenerate the Korean built-in font headers (upstream's convert-builtin-fonts.sh
# covers NotoSerif/NotoSans/Ubuntu; this covers the fonts this fork actually links,
# see builtinFonts/all.h).
#
# my-theme: KoPub Batang is no longer built in (books are read with an SD-card
# .epdfont). The ~3 MB it freed pays for a larger UI font, the Tile theme's title
# font and the built-in Bookk Myungjo reader fonts.
#
# Requires freetype-py:  pip install freetype-py
#
# Usage:  lib/EpdFont/scripts/convert-korean-builtin-fonts.sh

set -e

cd "$(dirname "$0")"

PY="${PYTHON:-python3}"
SRC=../builtinFonts/source/Pretendard

# Unquoted on purpose: bash word-splits the ~3,200 tokens into array elements.
# (Not mapfile -- macOS still ships bash 3.2.)
KSX_INTERVALS=($($PY ks_x_1001_intervals.py))

# Pretendard 11pt — the UI font, and the reader font when no SD font is chosen.
# KS X 1001 Hangul; anything else (rare syllables, Hanja) falls back to the
# built-in Bookk Myungjo below at draw time.
echo "Generating pretendard_11_regular.h (KS X 1001 Hangul) ..."
$PY fontconvert.py pretendard_11_regular 11 \
  $SRC/Pretendard-Regular.ttf \
  --2bit \
  --additional-intervals 0x1100,0x11FF \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0x3130,0x318F \
  "${KSX_INTERVALS[@]}" > ../builtinFonts/pretendard_11_regular.h

# Bookk Myungjo 17pt (~36px) — built-in reader font: the KS X 1001 Hangul (2,350
# syllables) plus the 4,888 KS X 1001 Hanja, UNCOMPRESSED (~2.1 MB of glyphs), letter
# spacing -2 to match the SD .epdfont made with ~/tools/font-merge/make_epdfont.py
# --spacing -2. Not compressed on purpose: the X3 has no PSRAM, the reader never
# prewarms the FontDecompressor, and 64 KB DEFLATE groups sorted by code point mean
# nearly every Hangul glyph on a page is a group miss — three AA passes of
# inflate-64-KB-per-glyph made pages take many seconds. The 8,822 syllables outside
# KS X 1001 come from the SD 보조 글꼴 (Settings > Reader) when one is chosen.
# The TTF's redistribution terms are unverified, so the header is git-ignored: set
# BOOKK_TTF to the merged TTF to generate it locally.
if [ -n "$BOOKK_TTF" ]; then
  echo "Generating bookk_17_regular.h (KS X 1001 Hangul + Hanja, uncompressed) ..."
  HANJA_INTERVALS=($($PY ks_x_1001_hanja_intervals.py))
  $PY fontconvert.py bookk_17_regular 17 "$BOOKK_TTF" \
    --2bit --spacing -2 \
    --additional-intervals 0x1100,0x11FF \
    --additional-intervals 0x3000,0x303F \
    --additional-intervals 0x3130,0x318F \
    "${KSX_INTERVALS[@]}" \
    "${HANJA_INTERVALS[@]}" > ../builtinFonts/bookk_17_regular.h

  # The symbols bookk_17_regular.h leaves out, from the same TTF at the same size (Noto
  # Serif for what Bookk lacks: accented Latin, the rest of Greek), as a separate
  # COMPRESSED font (~215 KB of glyphs -> ~76 KB) that main.cpp wires as the reader
  # font's glyph fallback. These are a handful of glyphs per page at most, and the
  # reader prewarms them (FontCacheManager::prewarmFallbacks), so compression costs one
  # inflate per group per page here, not the per-page flood it was for Hangul.
  # --no-default-intervals: Latin, punctuation, math and arrows are already in
  # bookk_17_regular.h; without it they would be stored twice.
  echo "Generating bookk_17_symbols.h (symbols, compressed) ..."
  $PY fontconvert.py bookk_17_symbols 17 "$BOOKK_TTF" \
    ../builtinFonts/source/NotoSerif/NotoSerif-Regular.ttf \
    --2bit --compress --spacing -2 --no-default-intervals \
    --additional-intervals 0x00A0,0x017F \
    --additional-intervals 0x0370,0x03FF \
    --additional-intervals 0x2100,0x218F \
    --additional-intervals 0x2460,0x24FF \
    --additional-intervals 0x2500,0x257F \
    --additional-intervals 0x25A0,0x25FF \
    --additional-intervals 0x2600,0x26FF \
    --additional-intervals 0x3040,0x30FF \
    --additional-intervals 0xFF00,0xFFEF > ../builtinFonts/bookk_17_symbols.h
fi

# Pretendard Bold 14pt — the Tile theme's home book title. KS X 1001 Hangul
# (2,350 syllables); a rarer syllable falls back to the UI font. COMPRESSED
# (455 KB -> 126 KB), which is what keeps firmware.bin under the stock OTA limit on
# 2.0: unlike a reader page, the title is a few dozen glyphs drawn once per home
# visit, and TileTheme prewarms them so each DEFLATE group is inflated only once.
echo "Generating pretendard_14_bold.h (KS X 1001 Hangul, compressed) ..."
$PY fontconvert.py pretendard_14_bold 14 \
  $SRC/Pretendard-Bold.ttf \
  --2bit --compress \
  --additional-intervals 0x3130,0x318F \
  "${KSX_INTERVALS[@]}" > ../builtinFonts/pretendard_14_bold.h

# fontconvert.py records its whole argv in the header comment, which runs to tens
# of KB once the interval lists expand, so the headers get a pointer to this
# script instead.
$PY - <<'PYEOF'
import re

NOTES = {
    "../builtinFonts/pretendard_11_regular.h":
        " * Command used: lib/EpdFont/scripts/convert-korean-builtin-fonts.sh\n"
        " * Hangul coverage: KS X 1001 (2,350 syllables) via ks_x_1001_intervals.py.",
    "../builtinFonts/pretendard_14_bold.h":
        " * Command used: lib/EpdFont/scripts/convert-korean-builtin-fonts.sh\n"
        " * Hangul coverage: KS X 1001 (2,350 syllables) via ks_x_1001_intervals.py.",
}

for path, note in NOTES.items():
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    text = re.sub(r"^ \* Command used: .*$", note, text, count=1, flags=re.M)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
PYEOF

echo ""
echo "Done. Rebuild and confirm gh_release firmware.bin is still under 6,553,600 bytes."
