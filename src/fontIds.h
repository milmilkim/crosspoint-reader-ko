// Font IDs for CrossPoint Korean version
#pragma once

// --- Korean build primary fonts ---
// UI font (Pretendard 11pt, KS X 1001 Hangul) - single font for all UI sizes. Hanja and rarer
// syllables are drawn from the built-in reader font (glyph fallback, see main.cpp).
#define UI_FONT_ID (1983644244)
#define SMALL_FONT_ID UI_FONT_ID
#define UI_10_FONT_ID UI_FONT_ID
#define UI_12_FONT_ID UI_FONT_ID

// Book-title font for the Tile theme home (Pretendard Bold 14pt, KS X 1001 Hangul;
// other glyphs fall back to UI_FONT_ID)
#define TITLE_FONT_ID (1983644320)

// Built-in reader font: Bookk Myungjo 17pt (KS X 1001 Hangul + Hanja, uncompressed: compressed
// glyph groups thrash on the X3, see convert-korean-builtin-fonts.sh). Other syllables come from
// the SD 보조 글꼴.
// Generated locally (lib/EpdFont/builtinFonts/bookk_17_regular.h, git-ignored: the TTF's
// redistribution terms are unverified); a build without it reads in the UI font instead.
#if __has_include(<builtinFonts/bookk_17_regular.h>)
#define HAS_BUILTIN_BOOKK 1
#define BUILTIN_READER_FONT_ID (1983644330)
#else
#define HAS_BUILTIN_BOOKK 0
#define BUILTIN_READER_FONT_ID UI_FONT_ID
#endif
// Symbols the built-in reader font lacks (Greek, accented Latin, circled numbers, shapes,
// hearts and stars, full-width punctuation, kana, box drawing), from the same Bookk TTF
// (Noto Serif where Bookk has no glyph).
// A small DEFLATE-compressed font wired as the reader font's glyph fallback, so these
// characters need no SD font. Generated locally next to bookk_17_regular.h.
#if HAS_BUILTIN_BOOKK && __has_include(<builtinFonts/bookk_17_symbols.h>)
#define HAS_BUILTIN_SYMBOLS 1
#else
#define HAS_BUILTIN_SYMBOLS 0
#endif
#define READER_SYMBOLS_FONT_ID (1983644340)

// Upstream code names the built-in reader font READER_FONT_ID.
#define READER_FONT_ID BUILTIN_READER_FONT_ID

// Optional SD font that only supplies glyphs the built-in reader fonts lack (rare Hangul
// syllables, rare Hanja, Chinese). Chosen in Settings > Reader > 보조 글꼴; never the
// whole-page font. Last in the chain: reader font -> symbols -> this.
#define READER_FALLBACK_FONT_ID (-666666)

// Custom reader font loaded from SD card
#define CUSTOM_FONT_ID (-999999)

// User-selectable UI "system font" loaded from SD card.
// When set, it becomes the primary UI font (the whole UI renders with it) and Pretendard
// is kept as its glyph-level fallback for codepoints the SD font lacks. When unset, the UI
// uses Pretendard only.
#define SYSTEM_FONT_ID (-888888)

// Reserved id. The Korean system-font path keeps Pretendard registered under UI_FONT_ID and
// wires it as the SD system font's glyph fallback via setGlyphFallback(SYSTEM_FONT_ID, UI_FONT_ID),
// so this dedicated fallback slot is currently unused but kept reserved (and asserted non-zero).
#define UI_FALLBACK_FONT_ID (-777777)

// --- Upstream built-in reader font IDs (kept so upstream render/settings code compiles).
// The Korean build does not load these font objects; the reader uses Pretendard 10pt / a custom
// SD font instead (see CrossPointSettings::getReaderFontId and src/main.cpp font loading).
// UI_10/UI_12/SMALL are intentionally NOT redefined here — the Korean Pretendard IDs above win.
#define NOTOSERIF_12_FONT_ID (85340443)
#define NOTOSERIF_14_FONT_ID (-1367885987)
#define NOTOSERIF_16_FONT_ID (1428909134)
#define NOTOSERIF_18_FONT_ID (-501438527)
#define NOTOSANS_12_FONT_ID (2057568286)
#define NOTOSANS_14_FONT_ID (-1589315735)
#define NOTOSANS_16_FONT_ID (1669013660)
#define NOTOSANS_18_FONT_ID (37077304)

// Font ID 0 is reserved as the "not found" sentinel. Guard against any hash producing 0.
static_assert(NOTOSERIF_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSERIF_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSERIF_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSERIF_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_12_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_14_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_16_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(NOTOSANS_18_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(UI_FONT_ID != 0, "Font ID collision with sentinel");
static_assert(UI_FALLBACK_FONT_ID != 0, "Font ID collision with sentinel");
