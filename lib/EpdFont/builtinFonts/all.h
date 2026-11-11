#pragma once

// Korean build (my-theme): Pretendard for the UI, Bookk Myungjo as the built-in reader font.
// Upstream's NotoSerif/NotoSans/Ubuntu built-ins are intentionally NOT included here;
// src/main.cpp loads only the fonts below. The NOTOSERIF_*/NOTOSANS_* font IDs in fontIds.h
// remain defined so upstream code compiles, but their font data is absent.

// UI font (Pretendard 11pt, KS X 1001 Hangul) - Regular only, synthetic bold used when needed
#include <builtinFonts/pretendard_11_regular.h>

// Book-title font (Pretendard Bold 14pt, KS X 1001 Hangul, compressed) - Tile theme home
#include <builtinFonts/pretendard_14_bold.h>

// Reader font (Bookk Myungjo 17pt, uncompressed) - generated locally, never committed
#if __has_include(<builtinFonts/bookk_17_regular.h>)
#include <builtinFonts/bookk_17_regular.h>
// Symbols the reader font lacks (same TTF, compressed) - generated locally, never committed
#if __has_include(<builtinFonts/bookk_17_symbols.h>)
#include <builtinFonts/bookk_17_symbols.h>
#endif
#endif
