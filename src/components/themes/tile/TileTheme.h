#pragma once

#include <cstdint>
#include <string>

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Tile theme: Lyra styling everywhere except the home screen and the front
// button hints, which follow the stock Xteink firmware look — one large book
// card (cover, title, progress and reading time) above a 2x2 grid of rounded
// menu tiles, and borderless text-only button hints in a smaller font. Only the
// tiles are selectable; Back opens the last book.
namespace TileMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeRecentBooksCount = 1;
  v.homeCoverHeight = 260;
  // Card (37 + 260) plus breathing room down to the 2px divider at its bottom edge (y=380).
  v.homeCoverTileHeight = 326;
  v.homeMenuTopOffset = 25;
  return v;
}();
}  // namespace TileMetrics

class TileTheme : public LyraTheme {
 public:
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;

 private:
  // Progress and reading time of the book on the card. Reading them touches the
  // SD card (and parses EPUB metadata), so they are cached per path.
  void loadBookStats(const std::string& path) const;

  mutable std::string statsPath;
  mutable int statsPercent = -1;
  mutable uint32_t statsSeconds = 0;
};
