#include "TileTheme.h"

#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/tile/TileIcons.h"
#include "fontIds.h"
#include "util/BookProgress.h"
#include "util/ReadingStats.h"

namespace {
// Stock-style home geometry (528x792 portrait), measured off a straight-on photo
// of the stock firmware's home screen (±2 px): side margin 38, cover 176x260 at
// (40, 93), title column at x=243, divider at y=380, tiles 215x152 with a 20 px
// gap from y=407, hint text baseline at y=766.
constexpr int homeSide = 38;         // left/right margin of everything on the home screen
constexpr int cardTop = 37;          // cover top below the header band (y=93)
constexpr int coverInset = 2;        // cover sits 2 px inside the tile margin
constexpr int coverWidth = 176;
constexpr int coverBorder = 3;
constexpr int cardGap = 27;          // cover to text column
constexpr int titleMaxLines = 5;     // the stock card wraps up to five lines
constexpr int barHeight = 7;
constexpr int barBottomInset = 18;   // bar bottom above the cover bottom
constexpr int barToProgress = 5;     // progress line above the bar
constexpr int dividerThickness = 2;
constexpr int tileGap = 20;
constexpr int tileBottomGap = 21;  // tiles stop this far above the button hints
constexpr int tileMaxHeight = 155;
constexpr int tileBorder = 3;      // same weight as the cover border
constexpr int tileRadius = 20;
constexpr int tileIconTop = 33;    // icon box top below the tile top (stock glyph spans 40..82)
constexpr int tileLabelTop = 106;  // label line box top below the tile top (caps at 112)
constexpr int thumbDrawAttempts = 3;
constexpr int thumbRetryDelayMs = 50;
constexpr int hintSlotWidth = 120;   // 오른쪽으로 at 11pt is ~80 px wide; slots may spill past the 80px buttons
constexpr int hintBottomGap = 21;    // hint line box bottom above the screen edge

const uint8_t* tileIconFor(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return TileIcons::folder;
    case UIIcon::Library:
      return TileIcons::library;
    case UIIcon::Transfer:
      return TileIcons::wifi;
    case UIIcon::Settings:
      return TileIcons::settings;
    case UIIcon::Blocks:
      return TileIcons::blocks;
    case UIIcon::Book:
      return TileIcons::book;
    case UIIcon::Bookmark:
      return TileIcons::bookmark;
    default:
      return nullptr;
  }
}

// TileIcons are 2-bit (0 = paper .. 3 = ink), stored upright (not pre-rotated like the
// legacy Lyra icons), so they are plotted directly; `ink` picks black-on-white or
// white-on-black. The B/W pass paints the two darker levels as ink. Should a grayscale
// overlay pass ever draw the home again: those planes only set bits (1 = update this
// pixel to gray), the MSB plane marks both grays, the LSB plane the darker one, and on
// an inverted (black) tile a value-2 pixel is mostly white stroke, i.e. the lighter
// gray, so the LSB plane marks value 1 there instead.
void drawTileIcon(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const bool ink) {
  constexpr int size = TileIcons::SIZE;
  constexpr int rowBytes = size / 4;
  const auto mode = renderer.getRenderMode();
  uint8_t levels;  // bit n set: 2-bit value n is painted in this pass
  bool state;
  if (mode == GfxRenderer::BW) {
    levels = 0x0c;
    state = ink;
  } else if (mode == GfxRenderer::GRAYSCALE_MSB) {
    levels = 0x06;
    state = false;
  } else {
    levels = ink ? 0x04 : 0x02;
    state = false;
  }
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t value = (bitmap[row * rowBytes + (col >> 2)] >> (6 - (col & 3) * 2)) & 3;
      if (levels & (1u << value)) renderer.drawPixel(x + col, y + row, state);
    }
  }
}

// Reading-cache directory of a book, without loading it (the path is a hash of
// the file path), or "" for formats that have none.
std::string cachePathFor(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    return epub ? epub->getCachePath() : "";
  }
  if (FsHelpers::hasXtcExtension(path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
    return xtc ? xtc->getCachePath() : "";
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return "";
}
}  // namespace

void TileTheme::loadBookStats(const std::string& path) const {
  if (path == statsPath) return;
  statsPath = path;
  statsPercent = loadBookProgress(path);
  statsSeconds = 0;
  const std::string cachePath = cachePathFor(path);
  ReadingStats stats;
  if (!cachePath.empty() && ReadingStats::load(cachePath, stats)) statsSeconds = stats.totalSeconds;
}

void TileTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }
  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Same button anchors as Lyra (80px wide), keyed to the portrait panel width.
  constexpr int buttonWidth = 80;
  constexpr int narrowButtonPositions[] = {58, 146, 254, 342};
  constexpr int wideButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int textY = renderer.getScreenHeight() - hintBottomGap - renderer.getLineHeight(UI_FONT_ID);

  for (int i = 0; i < 4; i++) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    const std::string label = renderer.truncatedText(UI_FONT_ID, labels[i], hintSlotWidth);
    const int textWidth = renderer.getTextWidth(UI_FONT_ID, label.c_str());
    renderer.drawText(UI_FONT_ID, buttonPositions[i] + (buttonWidth - textWidth) / 2, textY, label.c_str(), true);
  }

  renderer.setOrientation(origOrientation);
}

int TileTheme::homeCoverThumbHeight(const GfxRenderer&) const {
  // Thumbs fill a (0.6*h, h) box; ask for one at least as wide as the cover slot
  // and let the draw crop the extra height.
  return std::max(TileMetrics::values.homeCoverHeight, coverWidth * 5 / 3 + 2);
}

void TileTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int /*selectorIndex*/, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const auto& m = TileMetrics::values;
  const int right = rect.x + rect.width - homeSide;
  // The divider closes the card band whether or not there is a book.
  renderer.fillRect(rect.x + homeSide, rect.y + rect.height - dividerThickness, right - rect.x - homeSide,
                    dividerThickness, true);
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const RecentBook& book = recentBooks[0];
  const int coverX = rect.x + homeSide + coverInset;
  const int coverY = rect.y + cardTop;
  const int coverHeight = m.homeCoverHeight;

  // The card (cover from the SD card, title, progress) is painted once; later passes
  // restore it from the stored buffer.
  if (coverRendered && bufferRestored) return;

  {
    const Rect coverRect{coverX, coverY, coverWidth, coverHeight};
    bool hasCover = false;
    bool coverFailed = false;
    if (!book.coverBmpPath.empty()) {
      const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, homeCoverThumbHeight(renderer));
      // A thumb generated just before this pass can read short on a slow card
      // (the write has not settled yet): drawBitmap then stops partway and
      // returns false. Retry briefly instead of keeping a half-drawn cover.
      for (int attempt = 0; attempt < thumbDrawAttempts && !hasCover; attempt++) {
        if (attempt > 0) delay(thumbRetryDelayMs);
        HalFile file;
        if (!Storage.openFileForRead("HOME", thumbPath, file)) break;  // no thumb (yet)
        // The slot may hold the restored placeholder (its lower two thirds are solid
        // black) or a previous partial draw; the 1-bit bitmap draw only paints black
        // pixels, so clear it first or the old ink shows through below the art.
        renderer.fillRect(coverX, coverY, coverWidth, coverHeight, false);
        Bitmap bitmap(file);
        hasCover = bitmap.parseHeaders() == BmpReaderError::Ok && drawCoverThumbFill(renderer, bitmap, coverRect);
        file.close();
        coverFailed = !hasCover;
      }
    }
    if (!hasCover) {
      renderer.fillRect(coverX, coverY, coverWidth, coverHeight, false);  // clear any partial draw
      drawCoverPlaceholder(renderer, coverRect);
    }
    renderer.drawRect(coverX, coverY, coverWidth, coverHeight, coverBorder, true);
    // A cover that exists but would not draw stays "not rendered", so the next
    // pass (or the next visit) tries the file again.
    coverRendered = !coverFailed;
  }

  loadBookStats(book.path);

  const int textX = coverX + coverWidth + cardGap;
  const int textWidth = right - textX;

  // Title: large bold, up to five lines from the top of the cover. The title font is
  // DEFLATE-compressed in flash (see convert-korean-builtin-fonts.sh): inflate each
  // group its glyphs need once up front instead of once per glyph, and drop the
  // decompressed copies as soon as the lines are drawn.
  FontCacheManager* const fontCache = renderer.getFontCacheManager();
  if (fontCache) fontCache->prewarmCache(TITLE_FONT_ID, book.title.c_str(), 0x01);
  const int titleLineHeight = renderer.getLineHeight(TITLE_FONT_ID);
  int y = coverY;
  for (const auto& line : renderer.wrappedText(TITLE_FONT_ID, book.title.c_str(), textWidth, titleMaxLines)) {
    renderer.drawText(TITLE_FONT_ID, textX, y, line.c_str(), true);
    y += titleLineHeight;
  }
  if (fontCache) fontCache->clearCache();

  // Progress line and bar sit just above the cover's bottom edge.
  const int barY = coverY + coverHeight - barBottomInset;
  const int progressY = barY - barToProgress - renderer.getLineHeight(UI_10_FONT_ID);
  if (statsPercent >= 0) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", statsPercent);
    renderer.drawText(UI_10_FONT_ID, textX, progressY, percent, true);
  }
  if (statsSeconds > 0) {
    char elapsed[16];
    ReadingStats::format(statsSeconds, elapsed, sizeof(elapsed));
    renderer.drawText(UI_10_FONT_ID, right - renderer.getTextWidth(UI_10_FONT_ID, elapsed), progressY, elapsed, true);
  }
  // Borderless like stock: a light dotted track with the read part solid.
  if (statsPercent >= 0) {
    renderer.fillRectDither(textX, barY, textWidth, barHeight, Color::LightGray);
    renderer.fillRect(textX, barY, textWidth * statsPercent / 100, barHeight, true);
  }

  coverBufferStored = storeCoverBuffer();
  coverRendered = coverRendered && coverBufferStored;
}

void TileTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& m = TileMetrics::values;
  if (buttonCount <= 0) return;

  // Tiles fill the space down to the button hints; a fifth item (OPDS) adds a third row.
  const int rows = (buttonCount + 1) / 2;
  const int bottom = renderer.getScreenHeight() - m.buttonHintsHeight - tileBottomGap;
  const int tileWidth = (rect.width - 2 * homeSide - tileGap) / 2;
  const int tileHeight = std::min(tileMaxHeight, (bottom - rect.y - tileGap * (rows - 1)) / rows);

  for (int i = 0; i < buttonCount; ++i) {
    const int x = rect.x + homeSide + (i % 2) * (tileWidth + tileGap);
    const int y = rect.y + (i / 2) * (tileHeight + tileGap);
    const bool selected = i == selectedIndex;


    if (selected) {
      renderer.fillRoundedRect(x, y, tileWidth, tileHeight, tileRadius, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, tileWidth, tileHeight, tileBorder, tileRadius, true);
    }

    if (rowIcon != nullptr) {
      if (const uint8_t* icon = tileIconFor(rowIcon(i))) {
        drawTileIcon(renderer, icon, x + (tileWidth - TileIcons::SIZE) / 2, y + tileIconTop, !selected);
      }
    }

    const std::string labelStr = buttonLabel(i);
    const std::string label = renderer.truncatedText(UI_12_FONT_ID, labelStr.c_str(), tileWidth - 16);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label.c_str());
    renderer.drawText(UI_12_FONT_ID, x + (tileWidth - labelWidth) / 2, y + tileLabelTop, label.c_str(), !selected);
  }
}
