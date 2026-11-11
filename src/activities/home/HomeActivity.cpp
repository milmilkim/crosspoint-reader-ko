#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/dos/DosTheme.h"
#include "fontIds.h"
#include "util/BookProgress.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Library, File transfer, Settings (tile home: its four tiles)
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasLibrarySlot() && !tileHome) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path) && !FsHelpers::hasTxtExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path) && !FsHelpers::hasTxtExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // Recovery for devices whose recent_books.json was written by an earlier
    // firmware that cleared coverBmpPath on XTC thumb-gen OOM (before the
    // sleep-validation fix stopped deleting 1-bit thumbs on every sleep).
    // Rebuild the cache-path template from the book's extension so the
    // normal generation path below gets another chance.
    if (book.coverBmpPath.empty() && !book.path.empty()) {
      const auto h = std::to_string(std::hash<std::string>{}(book.path));
      if (FsHelpers::hasXtcExtension(book.path)) {
        book.coverBmpPath = "/.crosspoint/xtc_" + h + "/thumb_[HEIGHT].bmp";
      } else if (FsHelpers::hasEpubExtension(book.path)) {
        book.coverBmpPath = "/.crosspoint/epub_" + h + "/thumb_[HEIGHT].bmp";
      }
    }

    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub/txt/md, try to load the metadata for title/author and cover
        if (FsHelpers::hasReflowableBookExtension(book.path) && !FsHelpers::hasTxtExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          const bool cacheLoaded = epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          // Korean: a failed generation (often a transient OOM) must not erase the persisted
          // cover path; the next visit retries instead of losing the thumbnail for good.
          // With the book cache gone (reader menu: delete cache) there is no cached metadata,
          // so read the cover location straight from the EPUB.
          const bool thumbReady =
              cacheLoaded ? epub.generateThumbBmp(thumbHeight) : epub.generateThumbBmpFromSource(thumbHeight);
          if (!thumbReady) {
            LOG_ERR("HOME", "EPUB thumb generation failed: %s", book.path.c_str());
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            // Korean: keep the cover path on failure (see the EPUB branch above).
            if (!xtc.generateThumbBmp(thumbHeight)) {
              LOG_ERR("HOME", "XTC thumb generation failed: %s", book.path.c_str());
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  // Plugin System is the global opt-in. Once enabled, expose the Plugins entry
  // even when no SD plugins are installed yet so Plugin Hub can be installed
  // from inside the Plugins screen (or users can sideload plugins manually).
  hasPlugins = SETTINGS.pluginsEnabled != 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  const bool dosTheme = SETTINGS.uiTheme == CrossPointSettings::DOS;
  const int recentBookLimit = dosTheme ? DosTheme::recentBooksLimit(renderer) : metrics.homeRecentBooksCount;
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : recentBookLimit);
  hasContinueReading = !recentBooks.empty();
  recentBookProgress.fill(-1);
  if (!coverGridUi && SETTINGS.uiTheme == CrossPointSettings::DOS) {
    for (size_t i = 0; i < recentBooks.size() && i < recentBookProgress.size(); ++i) {
      recentBookProgress[i] = loadBookProgress(recentBooks[i].path);
    }
  }
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasLibrarySlot(), hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  tileHome = !coverGridUi && UITheme::hasTileHome();
  if (tileHome) {
    // The book card is not selectable: start on a tile.
    selectorIndex = base + (initialMenuItem == HomeMenuItem::NONE ? 0 : tileMenuItemToIndex(initialMenuItem));
  } else {
    selectorIndex = initialMenuItem == HomeMenuItem::NONE ? (dosTheme ? base : 0)
                                                          : base + menuItemToIndex(initialMenuItem, hasLibrarySlot());
  }

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  recentPopup.dismiss();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  if (recentPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onRecentBookSelected(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (tileHome ? tileIndexToMenuItem(menuIndex) : indexToMenuItem(menuIndex, hasLibrarySlot())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:  // the library slot
        hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        if (tileHome && hasLibrarySlot()) {
          showTransferPopup();
        } else {
          onFileTransferOpen();
        }
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      case HomeMenuItem::CONTINUE_READING:
        // Tile home's Read tile: the last book, or the file browser when there is none yet.
        if (!recentBooks.empty()) {
          onRecentBookSelected(recentBooks[0].path);
        } else {
          onFileBrowserOpen();
        }
        break;
      default:
        break;
    }
  };

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  // Tile home cycles through the tiles only (the book card is not selectable).
  const int firstSelectable = tileHome ? static_cast<int>(recentBooks.size()) : 0;
  const auto stepSelection = [this, menuCount, firstSelectable](const bool forward) {
    const int count = menuCount - firstSelectable;
    const int index = selectorIndex - firstSelectable;
    selectorIndex = firstSelectable + (forward ? ButtonNavigator::nextIndex(index, count)
                                               : ButtonNavigator::previousIndex(index, count));
    requestUpdate();
  };
  if (!coverGridUi) {
    buttonNavigator.onNext([&stepSelection] { stepSelection(true); });
    buttonNavigator.onPrevious([&stepSelection] { stepSelection(false); });
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    stepSelection(true);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    stepSelection(false);
    return;
  }

  // Back is otherwise unused on the home menu. Tile home: a popup of the recent
  // books (the library is not on its tiles); other themes open the most recently
  // read book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    if (tileHome) {
      showRecentPopup();
    } else {
      onRecentBookSelected(recentBooks[0].path);
    }
    return;
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, bookCount] { cycleBand(0, bookCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
    return;
  }

  // Match the rendered cover grid, which lays tiles inside getContentArea()
  // (origin content.x + contentSidePadding, span content.width). Using
  // screen-relative getScreenWidth()/contentSidePadding here offset the touch
  // targets from the drawn covers by content.x on bezel-inset panels (EEGO A4).
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = [&] {
    if (SETTINGS.uiTheme == CrossPointSettings::DOS) {
      const DosHomeLayout layout = DosTheme::homeLayout(renderer, recentCount);
      return mappedInput.rowTouch(touchedBook, layout.recentRowsTop, layout.recentRowStep, recentCount, layout.contentX,
                                  renderer.getScreenWidth() - layout.contentX, layout.recentRowStep);
    }
    return mappedInput.colTouch(touchedBook, content.x + metrics.contentSidePadding, coverColumnWidth, recentCount,
                                metrics.homeTopPadding, metrics.homeTopPadding + metrics.homeCoverTileHeight,
                                coverColumnWidth);
  }();
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  const bool dosTheme = SETTINGS.uiTheme == CrossPointSettings::DOS;
  const DosHomeLayout dosLayout =
      dosTheme ? DosTheme::homeLayout(renderer, static_cast<int>(recentBooks.size())) : DosHomeLayout{};
  const int menuTop = dosTheme ? dosLayout.menuRowsTop
                               : metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight =
      dosTheme ? DosTheme::homeMenuRowHeight(renderer, renderedMenuCount, static_cast<int>(recentBooks.size()))
               : GUI.getMenuRowHeight(renderer);
  const int menuRowStep =
      dosTheme ? DosTheme::homeMenuRowStep(renderer, renderedMenuCount, static_cast<int>(recentBooks.size()))
               : menuRowHeight + metrics.menuSpacing;
  const auto menuTouch =
      mappedInput.rowTouch(menuRow, menuTop, menuRowStep, renderedMenuCount, 0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // Content columns clear the bezel insets on rounded panels. The header
  // self-insets in drawHeader, so only the cover/menu need it here.
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const bool dosTheme = SETTINGS.uiTheme == CrossPointSettings::DOS;
  const DosHomeLayout dosLayout =
      dosTheme ? DosTheme::homeLayout(renderer, static_cast<int>(recentBooks.size())) : DosHomeLayout{};

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverRendered && coverBufferStored && restoreCoverBuffer();

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};

  if (hasLibrarySlot()) {
    menuItems.insert(menuItems.begin() + 2, hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Plugins);
  }

  if (tileHome) {
    // Stock tile order and wording; must match TILE_ORDER.
    menuItems = {tr(STR_TILE_READ), tr(STR_TILE_FOLDER), tr(STR_SETTINGS_TITLE), tr(STR_FILE_TRANSFER)};
    menuIcons = {Bookmark, Folder, Settings, Transfer};
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  const Rect menuRect =
      dosTheme ? dosLayout.menu
               : Rect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset,
                      content.width,
                      pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                    metrics.homeMenuTopOffset + metrics.buttonHintsHeight)};
  const int menuSelected =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - static_cast<int>(recentBooks.size());

  // Tile home: stock-style labels for the same actions (Back lists the recent books).
  const char* backLabel = tileHome ? tr(STR_HINT_GO_BACK) : tr(STR_RESUME);
  const char* prevLabel = tileHome ? tr(STR_HINT_TO_LEFT) : tr(STR_DIR_UP);
  const char* nextLabel = tileHome ? tr(STR_HINT_TO_RIGHT) : tr(STR_DIR_DOWN);
  const auto labels =
      mappedInput.mapLabels(recentBooks.empty() ? "" : backLabel, tr(STR_SELECT), prevLabel, nextLabel);

  const auto drawHome = [&](const bool withHints) {
    // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
    // homeTopPadding, so the height must shrink by topPadding or the band (and a
    // centered title, e.g. RoundedRaff's book title) sinks into the tile.
    // Home is the stack root: no back button in its header.
    const Rect headerRect = dosTheme ? dosLayout.header
                                     : Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding};
    GUI.drawHeader(renderer, headerRect,
                   metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                   nullptr, false);

    // Record the tile rect so storeCoverBuffer (called from the theme) knows
    // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
    // instead of the 48 KB full framebuffer the previous bind captured.
    const Rect recentBooksRect = dosTheme ? dosLayout.recent
                                          : Rect{content.x, metrics.homeTopPadding, content.width,
                                                 metrics.homeCoverTileHeight};
    coverRectX = recentBooksRect.x;
    coverRectY = recentBooksRect.y;
    coverRectW = recentBooksRect.width;
    coverRectH = recentBooksRect.height;

    if (dosTheme) {
      GUI.drawHomeRecentBooks(renderer, recentBooksRect, recentBooks, selectorIndex, recentBookProgress.data(),
                              recentBooks.size());
    } else {
      GUI.drawRecentBookCover(renderer, recentBooksRect, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                              bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this));
    }

    GUI.drawButtonMenu(
        renderer, menuRect, static_cast<int>(menuItems.size()), menuSelected,
        [&menuItems](int index) { return std::string(menuItems[index]); },
        [&menuIcons](int index) { return menuIcons[index]; });

    if (withHints) GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  };

  drawHome(!recentPopup.isActive());
  if (recentPopup.isActive()) {
    // The popup paints its own hints and displays the buffer over the home just drawn.
    recentPopup.processRender(renderer, mappedInput);
    return;
  }

  // 1-bit only: a grayscale anti-aliasing pass (tile corners, icons) was tried and made
  // every home paint far too slow on the X3 (three waveforms plus a full redraw).
  const auto refreshMode =
      cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  renderer.displayBuffer(refreshMode);

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    if (!GUI.usesHomeCoverImages()) {
      recentsLoaded = true;
    } else {
      recentsLoading = true;
      const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
      loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
    }
  }
}

void HomeActivity::showRecentPopup() {
  std::vector<std::string> titles;
  recentPopupPaths.clear();
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    titles.push_back(book.title.empty() ? book.path : book.title);
    recentPopupPaths.push_back(book.path);
  }
  if (titles.empty()) return;
  recentPopup.show(StrId::STR_MENU_RECENT_BOOKS, titles, 0, [this](const int idx) {
    if (idx >= 0 && idx < static_cast<int>(recentPopupPaths.size())) onRecentBookSelected(recentPopupPaths[idx]);
  });
  requestUpdate();
}

void HomeActivity::showTransferPopup() {
  // The library slot (Plugins, else OPDS) has no tile of its own; it shares the File
  // transfer tile. A single OPDS server shows under its own name (e.g. the user's
  // library); several go through the server list behind the generic OPDS entry.
  const auto& servers = OPDS_STORE.getServers();
  std::string libraryLabel;
  if (hasPlugins) {
    libraryLabel = tr(STR_PLUGINS);
  } else if (servers.size() == 1 && !servers[0].name.empty()) {
    libraryLabel = servers[0].name;
  } else {
    libraryLabel = tr(STR_OPDS_BROWSER);
  }
  std::vector<std::string> options = {libraryLabel, tr(STR_FILE_TRANSFER)};
  recentPopup.show(StrId::STR_FILE_TRANSFER, options, 0, [this](const int idx) {
    if (idx == 0) {
      hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
    } else if (idx == 1) {
      onFileTransferOpen();
    }
  });
  requestUpdate();
}

void HomeActivity::onRecentBookSelected(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(hasOpdsServers); }
