#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "QuickActions.h"
#include "ReaderRefreshScope.h"
#include "ReaderRuling.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
constexpr unsigned long LONG_PRESS_MENU_MS = 600;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 9;          // Increment when cache format changes
constexpr uint32_t MAX_CACHE_PAGES = ChunkedPageIndex<uint32_t>::kMaxEntries;

// Probe the same word and UTF-8 boundaries used for pagination. All longer
// prefixes already contain ink outside the viewport, so they can be skipped.
size_t wrappedLineLength(char* line, size_t length, const GfxRenderer& renderer, int fontId, int width,
                         int8_t characterSpacing) {
  size_t end = renderer.getTextOverflowPrefix(fontId, line, length, width, characterSpacing);
  if (!end) return length;
  const std::string_view text(line, length);
  while (end > 0) {
    const size_t space = text.rfind(' ', end - 1);
    if (space != std::string_view::npos && space > 0) {
      end = space;
    } else {
      do {
        --end;
      } while (end > 0 && (line[end] & 0xC0) == 0x80);
    }
    if (end == 0) break;
    const char next = line[end];
    line[end] = '\0';
    const int measured =
        renderer.getTextWidth(fontId, line, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, characterSpacing);
    line[end] = next;
    if (measured <= width) return end;
  }
  end = 1;
  while (end < length && (line[end] & 0xC0) == 0x80) ++end;
  return end;
}

// Up to three UTF-8 bytes or a CR awaiting its LF belong in the next window.
size_t unfinishedWindowSuffix(const uint8_t* buffer, size_t size) {
  if (!size) return 0;
  if (buffer[size - 1] == '\r') return 1;
  const int tail = static_cast<int>(std::min<size_t>(size, 4));
  return tail - utf8SafeTruncateBuffer(reinterpret_cast<const char*>(buffer + size - tail), tail);
}

// The caller owns a writable, NUL-terminated chunk. Temporary terminators
// let both measuring and ruled-row layout use it without copying paragraph tails.
size_t parseAndWrapLines(uint8_t* buffer, size_t chunkSize, size_t fileOffset, size_t fileSize, int linesPerPage,
                         GfxRenderer& renderer, int fontId, int vw, std::vector<std::string>* outLines,
                         int8_t characterSpacing, int ruledViewportHeight, int requestedHeight) {
  if (chunkSize < fileSize - fileOffset) chunkSize -= unfinishedWindowSuffix(buffer, chunkSize);
  size_t pos = 0;
  int lineCount = 0;
  while (pos < chunkSize && lineCount < linesPerPage) {
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') ++lineEnd;
    const bool complete = lineEnd < chunkSize || fileOffset + lineEnd >= fileSize;
    if (!complete && lineCount > 0) break;

    size_t contentEnd = lineEnd;
    if (complete && contentEnd > pos && buffer[contentEnd - 1] == '\r') --contentEnd;
    const uint8_t terminator = buffer[contentEnd];
    buffer[contentEnd] = 0;
    do {
      char* line = reinterpret_cast<char*>(buffer + pos);
      const size_t length = wrappedLineLength(line, contentEnd - pos, renderer, fontId, vw, characterSpacing);
      if (ruledViewportHeight >= 0) {
        const char next = line[length];
        line[length] = '\0';
        const int height = ReaderRuling::textRow(renderer, fontId, line, requestedHeight, characterSpacing).height;
        line[length] = next;
        if (lineCount > 0 && height > ruledViewportHeight) {
          buffer[contentEnd] = terminator;
          return pos;
        }
        ruledViewportHeight = std::max(0, ruledViewportHeight - height);
      }
      if (outLines) outLines->emplace_back(line, length);
      ++lineCount;
      pos += length;
      if (pos < contentEnd && buffer[pos] == ' ') ++pos;
    } while (pos < contentEnd && lineCount < linesPerPage);
    buffer[contentEnd] = terminator;
    if (pos < contentEnd) break;
    pos = lineEnd < chunkSize ? lineEnd + 1 : lineEnd;
  }
  return pos;
}

int getReaderLineHeight(const GfxRenderer& renderer, const int fontId) {
  return std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * SETTINGS.getReaderLineCompression() + 0.5f));
}

int lineOriginX(const GfxRenderer& renderer, const char* line, int fontId, int marginLeft, int width, uint8_t alignment,
                int8_t spacing) {
  if ((alignment == CrossPointSettings::LEFT_ALIGN || alignment == CrossPointSettings::JUSTIFIED) &&
      BidiUtils::startsWithRtl(line, BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
    alignment = CrossPointSettings::RIGHT_ALIGN;
  }
  if (alignment != CrossPointSettings::CENTER_ALIGN && alignment != CrossPointSettings::RIGHT_ALIGN) return marginLeft;
  const int remaining = width - renderer.getTextAdvanceX(fontId, line, EpdFontFamily::REGULAR, 0, spacing);
  return marginLeft + (alignment == CrossPointSettings::CENTER_ALIGN ? remaining / 2 : remaining);
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const bool toastBackgroundBlack = ReaderUtils::readerForegroundBlack();
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, toastBackgroundBlack);
  renderer.drawRect(toastX, toastY, toastW, toastH, !toastBackgroundBlack);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, !toastBackgroundBlack);
  renderer.displayBuffer();
}
}  // namespace

// Version 9 is a packed, little-endian record. Both supported chips and the
// simulator are little-endian. Reading it in one call also detects a short read.
struct __attribute__((packed)) TxtReaderActivity::IndexHeader {
  uint32_t magic;
  uint8_t version;
  uint32_t fileSize;
  int8_t spacing;
  int32_t viewportWidth, linesPerPage, fontId, verticalMargin, horizontalMargin;
  uint8_t alignment;
  uint32_t pages;
  int32_t ruledViewport, rowHeight;

  uint32_t readPageCount(FsFile& file) const {
    static_assert(sizeof(IndexHeader) == 43);
    IndexHeader stored{};
    if (!serialization::tryReadPod(file, stored) || !stored.pages || stored.pages > MAX_CACHE_PAGES ||
        (stored.pages > 1 && stored.pages > fileSize) ||
        file.size() != sizeof(IndexHeader) + stored.pages * sizeof(uint32_t)) {
      return 0;
    }
    IndexHeader expected = *this;
    expected.pages = stored.pages;
    return memcmp(&stored, &expected, sizeof(stored)) == 0 ? stored.pages : 0;
  }
};

void TxtReaderActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  showOpeningFeedback();

  if (!txt) {
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  showOpeningFeedback();

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  showOpeningFeedback();
  SleepCoverAssets::prepareTxt(*txt);
  const std::string coverBmpPath = Storage.exists(txt->getCoverBmpPath().c_str()) ? txt->getCoverBmpPath() : "";
  if (!skipRecentBookUpdateOnEntry) {
    RECENT_BOOKS.addOrUpdateBook(filePath, fileName, "", coverBmpPath);
  }

  showOpeningFeedback();
  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  if (!flushQueuedProgress()) {
    LOG_ERR("TRS", "Failed to flush debounced reader progress on exit");
  }
  pluginEvents.exit(getCurrentBookPath(), getScreenshotInfo().progressPercent);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

bool TxtReaderActivity::handleFrontlightPanelResult(const FrontlightPanelResult& result) {
  if (result.action != FrontlightPanelAction::SendNearbyBook || !txt) return false;
  saveProgressBeforeRestart();
  return activityManager.goToNearbyBookSend(txt->getPath(), true);
}

void TxtReaderActivity::loop() {
  pluginEvents.pollWhenIdle(getCurrentBookPath());
  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
#if CROSSINK_APP_CAP_TOUCH
  // TXT has no reader drawer: the bottom edge exits directly on every touch device.
  if (mappedInput.wasBottomEdgeUpSwipe()) {
    onGoHome();
    return;
  }
  if (handlePinchFontResize()) return;
#endif
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  if (SETTINGS.tapToHideStatusBar && touch.tapped &&
      (ReaderUtils::isBottomStatusBarTap(renderer, touch.y, UITheme::getInstance().getStatusBarHeight(renderer)) ||
       ReaderUtils::isTopStatusBarTap(renderer, touch.y,
                                      UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Top, renderer)))) {
    statusBarVisible = !statusBarVisible;
    requestUpdate();
    return;
  }
  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executePowerButtonAction()) {
    return;
  }

  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (longPressMenuHandled) {
    if (confirmReleased || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressMenuHandled = false;
    }
    return;
  }

  if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_ROTATE_FLIP &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    longPressMenuHandled = true;
    applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
    return;
  }

  if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_CHANGE_FONT &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    longPressMenuHandled = true;
    cycleReaderFont();
    return;
  }

  if ((SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_LIBRARY ||
       ReaderUtils::isNavigationLongPressAction(SETTINGS.longPressMenuAction)) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS &&
      (mappedInput.isPressed(MappedInputManager::Button::Confirm) || confirmReleased)) {
    longPressMenuHandled = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    mappedInput.suppressNextConfirmRelease();
    if (!ReaderUtils::dispatchNavigationLongPressAction(SETTINGS.longPressMenuAction)) activityManager.goToLibrary();
    return;
  }

  if (longPressBackHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      longPressBackHandled = false;
    }
    return;
  }

  if (!longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    longPressBackHandled = true;
    mappedInput.suppressNextBackRelease();
    executeLongPressBackAction();
    return;
  }

  // Short press BACK goes directly to home
  if (!touch.prev && !touch.next && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto side = readerButtonShortcuts.update(mappedInput, mappedInput.buttonSampleTime());
  if (ReaderButtonShortcuts::shouldConsume(side, mappedInput)) {
    if (side.triggered) {
      if (side.longPress) {
        if (side.front)
          mappedInput.suppressNextFrontRelease(side.up ? MappedInputManager::Button::Left
                                                       : MappedInputManager::Button::Right);
        else
          mappedInput.suppressNextSideRelease(side.up ? MappedInputManager::Button::Up
                                                      : MappedInputManager::Button::Down);
      }
      switch (side.action) {
        case CrossPointSettings::PAGE_TURN:
          requestPageTurn(false, true);
          break;
        case CrossPointSettings::SIDE_INCREASE_FONT:
        case CrossPointSettings::SIDE_DECREASE_FONT:
          changeReaderFontSize(side.action == CrossPointSettings::SIDE_INCREASE_FONT);
          break;
        case CrossPointSettings::SIDE_ROTATE_COUNTERCLOCKWISE:
        case CrossPointSettings::SIDE_ROTATE_CLOCKWISE:
          handleTwoFingerRotation(side.action == CrossPointSettings::SIDE_ROTATE_CLOCKWISE);
          break;
        case CrossPointSettings::SIDE_ROTATE_FLIP:
          applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
          break;
        case CrossPointSettings::SIDE_PREVIOUS_CHAPTER:
        case CrossPointSettings::SIDE_NEXT_CHAPTER:
        case CrossPointSettings::IGNORE:
          // Plain text has no chapter model.
          break;
        default: {
          const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(side.action);
          if (action == CrossPointSettings::QUICK_LOCK)
            handleGlobalPowerButtonAction(action, ReaderButtonShortcuts::quickLockTrigger(side));
          else if (!handleShortcutAction(action))
            handleGlobalPowerButtonAction(action);
          break;
        }
      }
    }
    return;
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  (void)fromSideBtn;
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  requestPageTurn(prevTriggered, nextTriggered);
}

void TxtReaderActivity::requestPageTurn(const bool previous, const bool next) {
  {
    // Indexing publishes the page count on the render task. Retain this turn
    // until that work finishes, then check bounds while holding its lock.
    RenderLock lock(*this);
    if ((!initialized && totalPages == 0) || pageReadFailed) {
      // Retry the interrupted index or the unread page before moving past it.
      requestUpdate();
      return;
    }
    if (previous && currentPage > 0) {
      --currentPage;
      pluginEvents.noteTurn(false);
    } else if (next && currentPage < totalPages - 1) {
      ++currentPage;
      pluginEvents.noteTurn(true);
    } else {
      return;
    }
  }
  requestUpdate();
}

bool TxtReaderActivity::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  RenderLock lock(*this);
  if (!sdFontSystem.changeReaderFontSize(larger, mode)) return false;
  rebuildTextLayout();
  return true;
}

void TxtReaderActivity::cycleReaderFont() {
  RenderLock lock(*this);
  const CrossPointSettings::FONT_SIZE effectiveSize = SETTINGS.getEffectiveReaderFontSize();
  SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % CrossPointSettings::FONT_FAMILY_COUNT;
  SETTINGS.sdFontFamilyName[0] = '\0';
  SETTINGS.readerFontPointSize = CrossPointSettings::getReaderFontPointSize(effectiveSize);
  rebuildTextLayout();
}

void TxtReaderActivity::rebuildTextLayout() {
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("TXT", "Failed to save reader font setting");
  }
  sdFontSystem.ensureLoaded(renderer);
  invalidateTextLayout();
  requestUpdate();
}

#if CROSSINK_APP_CAP_TOUCH
bool TxtReaderActivity::handlePinchFontResize() {
  if (!SETTINGS.pinchFontResizeEnabled || !mappedInput.supportsMultiTouch()) {
    resetPinchFontGesture();
    return false;
  }

  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  if (!mappedInput.getTwoFingerTouch(x1, y1, x2, y2)) {
    resetPinchFontGesture();
    return false;
  }

  const auto action = pinchFontGesture.update(x1, y1, x2, y2);
  if (action == ReaderPinchGesture::Action::None) return true;

  mappedInput.suppressCurrentTouchContact();
  changeReaderFontSize(action == ReaderPinchGesture::Action::Increase, FontSizeStepMode::Clamp);
  return true;
}

void TxtReaderActivity::resetPinchFontGesture() { pinchFontGesture.reset(); }
#endif

bool TxtReaderActivity::handleTwoFingerSwipeAction(const CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) {
  if (action != CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE &&
      action != CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_FONT_SIZE) {
    // TXT has no chapter model; ActivityManager still consumes configured
    // chapter swipes so they cannot fall through as one-finger navigation.
    return true;
  }

  changeReaderFontSize(action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE, FontSizeStepMode::Clamp);
  return true;
}

bool TxtReaderActivity::handleTwoFingerRotation(const bool clockwise) {
  return applyReaderOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, clockwise));
}

bool TxtReaderActivity::applyReaderOrientation(const uint8_t orientation) {
  {
    RenderLock lock(*this);
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    invalidateTextLayout();
  }
  requestUpdate();
  return true;
}

void TxtReaderActivity::toggleDarkMode() {
  SETTINGS.screenInverted = !SETTINGS.screenInverted;
  SETTINGS.saveToFile();
  requestUpdate();
}

bool TxtReaderActivity::consumeLongPowerButtonRelease() {
  if (!longPowerButtonHandled) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      !mappedInput.isPressed(MappedInputManager::Button::Power)) {
    longPowerButtonHandled = false;
    return true;
  }

  return false;
}

bool TxtReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  longPowerButtonHandled = true;
  return true;
}

bool TxtReaderActivity::supportsQuickAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::ROTATE_FLIP:
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
    case CrossPointSettings::SHORT_PWRBTN::BACK_HOME:
    case CrossPointSettings::SHORT_PWRBTN::HOME_READER:
      return true;
    default:
      return false;
  }
}

bool TxtReaderActivity::executeReaderShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::ROTATE_FLIP:
      return applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
      requestPageTurn(true, false);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      cycleReaderFont();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      activityManager.goToFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      saveProgressBeforeRestart();
      activityManager.goToCalibreWireless(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      saveProgressBeforeRestart();
      activityManager.goToJoinNetworkFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      saveProgressBeforeRestart();
      activityManager.goToHotspotFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      toggleDarkMode();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      activityManager.goToFileBrowser(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    default:
      return false;
  }
}

bool TxtReaderActivity::executePowerButtonAction() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn));
  }

  const auto longPowerAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (longPowerAction == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }

  if (executeReaderShortcutAction(longPowerAction)) {
    // Reader long-press actions execute while Power is still held. Consume its
    // later release so the app-wide shortcut dispatcher cannot run it again.
    mappedInput.suppressNextPowerRelease();
    return true;
  }

  return false;
}

void TxtReaderActivity::toggleHomeButtonInReader() {
  if (!mappedInput.hasHomeKey()) return;
  SETTINGS.homeButtonInReaderEnabled = SETTINGS.homeButtonInReaderEnabled ? 0 : 1;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("TXT", "Failed to save Home button reader setting");
  }
  mappedInput.clearDeferredHomeGesture();
  drawToast(renderer, SETTINGS.homeButtonInReaderEnabled ? tr(STR_HOME_BUTTON_ENABLED) : tr(STR_HOME_BUTTON_DISABLED));
  delay(1000);
  requestUpdate();
}

bool TxtReaderActivity::executeLongPressBackAction() {
  if (ReaderUtils::dispatchNavigationLongPressAction(SETTINGS.longPressBackAction)) return true;
  switch (static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressBackAction)) {
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_ROTATE_FLIP:
      return applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CHANGE_FONT:
      cycleReaderFont();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_REFRESH_SCREEN:
      prepareManualRefresh();
      requestUpdate();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_TRANSFER:
      activityManager.goToFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CALIBRE_WIRELESS:
      saveProgressBeforeRestart();
      activityManager.goToCalibreWireless(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_JOIN_NETWORK:
      saveProgressBeforeRestart();
      activityManager.goToJoinNetworkFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_HOTSPOT:
      saveProgressBeforeRestart();
      activityManager.goToHotspotFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_TOGGLE_DARK_MODE:
      toggleDarkMode();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_CLIPPING:
      return false;
    default:
      return false;
  }
}

bool TxtReaderActivity::handleShortcutAction(const uint8_t action) {
  return executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(action));
}

bool TxtReaderActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::HOME_READER || action == CrossPointSettings::SHORT_PWRBTN::BACK_HOME)
    return dispatchShortcutAction(action);
  if (action == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) {
    QuickActions::showConfiguredPopup(
        quickActionsPopup, [this] { requestUpdate(); },
        [this](const auto quickAction) {
          mappedInput.setReaderTouchscreenOverride(false);
          dispatchShortcutAction(quickAction);
        },
        [](const auto quickAction) { return supportsQuickAction(quickAction); });
    if (quickActionsPopup.isActive()) {
      mappedInput.setReaderTouchscreenOverride(true);
      quickActionsPopup.setCancelCallback([this] { mappedInput.setReaderTouchscreenOverride(false); });
    }
    return true;
  }
  return executeReaderShortcutAction(action);
}

void TxtReaderActivity::invalidateTextLayout() {
  if (initialized && currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size()) &&
      (!relayoutOffset || currentPage != relayoutPage)) {
    // An unreadable page has never been shown. Keep the last displayed anchor.
    if (!pageReadFailed)
      relayoutOffset = pageOffsets[currentPage];
    else if (progressSaveDebouncer.hasObserved())
      relayoutOffset = progressSaveDebouncer.lastObservedMetadata();
  }
  initialized = false;
}

int TxtReaderActivity::pageAtOffset(const uint32_t offset) const {
  size_t first = 0, last = pageOffsets.size();
  while (first < last) {
    const size_t mid = first + (last - first) / 2;
    if (pageOffsets[mid] <= offset)
      first = mid + 1;
    else
      last = mid;
  }
  return first ? static_cast<int>(first - 1) : 0;
}

bool TxtReaderActivity::appendPageOffset(const uint32_t offset) {
  const auto result = pageOffsets.prepareAppend();
  if (result != decltype(pageOffsets)::PrepareResult::Ready) {
    LOG_ERR("TRS", "TXT index %s at %zu pages",
            result == decltype(pageOffsets)::PrepareResult::EntryLimit ? "limit" : "allocation failed",
            pageOffsets.size());
    return false;
  }
  pageOffsets.appendPrepared(offset);
  return true;
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedCharacterSpacing = SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing);
  cachedVerticalMargin = SETTINGS.screenMarginVertical;
  cachedHorizontalMargin = SETTINGS.screenMarginHorizontal;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedTopStatusBarHeight = ReaderUtils::getTopStatusBarReservedHeight(renderer);
  cachedFooterReservedHeight = ReaderUtils::getReaderFooterReservedHeight(renderer, false);
  cachedViewableInsets = renderer.getViewableInsets();

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginLeft += cachedHorizontalMargin;
  cachedOrientedMarginRight += cachedHorizontalMargin;
  const int topStatusBarReservedHeight = cachedTopStatusBarHeight;
  if (topStatusBarReservedHeight > 0) {
    cachedOrientedMarginTop += std::max(static_cast<int>(cachedVerticalMargin),
                                        topStatusBarReservedHeight + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING);
  } else {
    cachedOrientedMarginTop += cachedVerticalMargin;
  }
  cachedOrientedMarginBottom += cachedFooterReservedHeight;

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  cachedRowHeight = getReaderLineHeight(renderer, cachedFontId);

  cachedRuledViewport = SETTINGS.ruledLines ? viewportHeight : -1;
  linesPerPage = viewportHeight / cachedRowHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Try to load cached page index first
  const IndexHeader layout{CACHE_MAGIC,
                           CACHE_VERSION,
                           static_cast<uint32_t>(txt->getFileSize()),
                           cachedCharacterSpacing,
                           viewportWidth,
                           linesPerPage,
                           cachedFontId,
                           cachedVerticalMargin,
                           cachedHorizontalMargin,
                           cachedParagraphAlignment,
                           0,
                           cachedRuledViewport,
                           cachedRuledViewport < 0 ? 0 : cachedRowHeight};
  if (!loadPageIndexCache(layout)) {
    if (!buildPageIndex()) {
      pageOffsets.clear();
      totalPages = 0;
      return;
    }
    savePageIndexCache(layout);
  }

  if (relayoutOffset) {
    currentPage = pageAtOffset(*relayoutOffset);
    relayoutPage = currentPage;
  } else if (!loadProgress()) {
    totalPages = 0;
    return;
  }

  initialized = true;
}

bool TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  if (!appendPageOffset(0)) return false;
  totalPages = 1;
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) return true;
  openingFeedback.showIndexing(renderer);

  // Keep the original full window at each page start. Carry its unread tail
  // forward so long paragraphs break at the same offsets as existing indexes.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(std::min(CHUNK_SIZE, fileSize) + 1);
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate TXT indexing scratch");
    return false;
  }
  HalFile file;
  // SD font measurement can open the font for missing glyphs. Close the book
  // before measuring in that case, retaining the rolling window on both paths.
  const bool needsFontFile = SETTINGS.sdFontFamilyName[0] != 0;
  if (!needsFontFile && !Storage.openFileForRead("TRS", txt->getPath(), file)) return false;

  size_t offset = 0;
  size_t retained = 0;
  while (offset < fileSize) {
    const size_t window = std::min(CHUNK_SIZE, fileSize - offset);
    const size_t needed = window - retained;
    if (needed && !(needsFontFile ? txt->readContent(buffer.get() + retained, offset + retained, needed)
                                  : file.read(buffer.get() + retained, needed) == static_cast<int>(needed))) {
      LOG_ERR("TRS", "Incomplete TXT index read at %zu", offset + retained);
      break;
    }
    const size_t consumed = wrapPage(buffer.get(), window, offset, nullptr);
    if (!consumed) {
      LOG_ERR("TRS", "TXT index made no progress at %zu", offset);
      break;
    }
    offset += consumed;
    if (offset == fileSize) break;
    if (!appendPageOffset(offset)) break;
    retained = window - consumed;
    memmove(buffer.get(), buffer.get() + consumed, retained);
    if (pageOffsets.size() % 20 == 0) vTaskDelay(1);
  }
  file.close();
  totalPages = pageOffsets.size();
  return offset == fileSize;
}

size_t TxtReaderActivity::wrapPage(uint8_t* buffer, size_t length, size_t offset, std::vector<std::string>* lines) {
  buffer[length] = '\0';
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }
  const size_t consumed =
      parseAndWrapLines(buffer, length, offset, txt->getFileSize(), linesPerPage, renderer, cachedFontId, viewportWidth,
                        lines, cachedCharacterSpacing, cachedRuledViewport, cachedRowHeight);
  return consumed;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset) {
  currentPageLines.clear();
  if (offset >= txt->getFileSize()) return false;
  const size_t length = std::min(CHUNK_SIZE, txt->getFileSize() - offset);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(length + 1);
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", length + 1);
    return false;
  }
  return txt->readContent(buffer.get(), offset, length) && wrapPage(buffer.get(), length, offset, &currentPageLines);
}

void TxtReaderActivity::render(RenderLock&&) {
  showOpeningFeedback();
  if (!txt) {
    return;
  }
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  if (initialized && currentPage != relayoutPage) relayoutOffset.reset();
  if (initialized && ((cachedRuledViewport >= 0) != (SETTINGS.ruledLines != 0) ||
                      cachedRowHeight != getReaderLineHeight(renderer, cachedFontId) ||
                      cachedViewableInsets != renderer.getViewableInsets() ||
                      cachedTopStatusBarHeight != ReaderUtils::getTopStatusBarReservedHeight(renderer) ||
                      cachedFooterReservedHeight != ReaderUtils::getReaderFooterReservedHeight(renderer, false))) {
    invalidateTextLayout();
  }
  if (!initialized) initializeReader();

  showOpeningFeedback();
  const bool emptyBook = txt->getFileSize() == 0;
  if (initialized) currentPage = std::clamp(currentPage, 0, totalPages - 1);
  pageReadFailed = initialized && !emptyBook && !loadPageAtOffset(pageOffsets[currentPage]);
  if (!initialized || emptyBook || pageReadFailed) {
    openingFeedback.finish();
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              emptyBook ? tr(STR_EMPTY_FILE) : tr(STR_PAGE_LOAD_ERROR),
                              ReaderUtils::readerForegroundBlack(), EpdFontFamily::BOLD);
    renderer.displayBuffer(pageToneHistory.monochromeScreenMode());
    return;
  }
  showOpeningFeedback();

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  renderPage();

  if (!queueProgressSave()) {
    LOG_ERR("TRS", "Failed to save debounced reader progress");
  }
}

void TxtReaderActivity::renderPage() {
  const GfxRenderer::TextDarknessScope darkness(renderer, SETTINGS.textDarkness);
  const int lineHeight = cachedRowHeight;
  const int contentWidth = viewportWidth;

  const ReaderRuling ruling{SETTINGS.ruledLines, cachedOrientedMarginLeft, contentWidth,
                            renderer.getScreenHeight() - cachedOrientedMarginBottom, SETTINGS.lineHeightPercent};
  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      const auto row = SETTINGS.ruledLines ? ReaderRuling::textRow(renderer, cachedFontId, line.c_str(), lineHeight,
                                                                   cachedCharacterSpacing)
                                           : RuledLineLayout{lineHeight, 0};
      if (!line.empty()) {
        const int x = lineOriginX(renderer, line.c_str(), cachedFontId, cachedOrientedMarginLeft, contentWidth,
                                  cachedParagraphAlignment, cachedCharacterSpacing);
        renderer.drawText(cachedFontId, x, y + row.offset, line.c_str(), ReaderUtils::readerForegroundBlack(),
                          EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                          SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
      }
      y += row.height;
      if (!line.empty() && ruling.enabled(renderer)) ruling.draw(renderer, y);
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  // Scan mode records the shaped glyphs without drawing. Position and row
  // metrics are only needed after the glyphs have been prepared.
  for (const auto& line : currentPageLines) {
    renderer.drawText(cachedFontId, 0, 0, line.c_str());
  }
  scope.endScanAndPrewarm();
  if (showOpeningFeedback()) renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  openingFeedback.finish();

  const ReaderUtils::ReaderRefreshScope readerRefresh(renderer,
                                                      ReaderUtils::ReaderRefreshScope::text(SETTINGS.twoToneTextAA));

  // BW rendering
  renderLines();
  renderStatusBar();

  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && renderer.supportsTextGrayscale();
  if (pageToneHistory.advanceTo(needsTextGrayscale ? ReaderUtils::PageTone::GrayText
                                                       : ReaderUtils::PageTone::Monochrome)) {
    pagesUntilFullRefresh = std::min(pagesUntilFullRefresh, 1);
  }
  const bool deferred = needsTextGrayscale && ReaderUtils::displayDeferredGrayscale(renderer, pagesUntilFullRefresh,
                                                                                    deferredGrayscalePlanes, [&]() {
                                                                                      renderLines();
                                                                                      return true;
                                                                                    });
  if (!deferred) {
    const auto request =
        needsTextGrayscale ? ReaderUtils::PageRefresh::GrayscaleBase : ReaderUtils::PageRefresh::Blocking;
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, request);
  }

  if (needsTextGrayscale && !deferred) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  pluginEvents.rendered(totalPages > 0 ? static_cast<int>((currentPage + 1LL) * 10000 / totalPages) : 0);
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  if (!statusBarVisible) {
    return;
  }

  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  const auto needsTitle = [](const ReaderStatusBarConfig& bar) {
    return bar.contains(ReaderStatusBarItem::TitleBook) || bar.contains(ReaderStatusBarItem::TitleChapter);
  };
  std::string title;
  if (needsTitle(SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top)) ||
      needsTitle(SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom))) {
    title = txt->getTitle();
  }
  ReaderStatusBarContent content;
  if (SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top).contains(ReaderStatusBarItem::Battery) ||
      SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom).contains(ReaderStatusBarItem::Battery)) {
    content.batteryPercentage = powerManager.getBatteryPercentage();
  }
  content.bookProgress = progress;
  content.chapterPage = currentPage + 1;
  content.chapterPageCount = totalPages;
  content.bookTitle = title.c_str();
  // TXT has no chapter metadata; preserve its existing title fallback.
  content.chapterTitle = content.bookTitle;
  content.darkMode = ReaderUtils::readerDarkModeEnabled();
  GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content);
  GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Bottom, content);
}

bool TxtReaderActivity::getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) {
  RenderLock lock(*this);
  if (!txt) return false;

  details.title = txt->getTitle();
  details.author.clear();
  details.chapter.clear();
  if (!initialized || totalPages <= 0) {
    details.progressPercent = 0;
    return true;
  }

  const int page = std::clamp(currentPage, 0, totalPages - 1);
  details.progressPercent = (page + 1) * 100 / totalPages;
  return true;
}

bool TxtReaderActivity::saveProgress(const uint32_t page, const uint32_t offset) {
  if (!txt) {
    return false;
  }
  // 6-byte format: page(2 bytes LE) + file offset(4 bytes LE)
  // The offset lets drawCurrentPageToBuffer render without requiring index.bin.
  uint8_t data[6];
  data[0] = page & 0xFF;
  data[1] = (page >> 8) & 0xFF;
  data[2] = offset & 0xFF;
  data[3] = (offset >> 8) & 0xFF;
  data[4] = (offset >> 16) & 0xFF;
  data[5] = (offset >> 24) & 0xFF;
  if (!txt->writeProgress(data, sizeof(data))) return false;
  progressSaveDebouncer.markPersisted(page, offset);
  return true;
}

bool TxtReaderActivity::queueProgressSave() {
  const uint32_t offset = pageOffsets[currentPage];
  if (!progressSaveDebouncer.observe(static_cast<uint32_t>(currentPage), offset)) {
    return true;
  }
  return saveProgress(currentPage, offset);
}

bool TxtReaderActivity::flushQueuedProgress() {
  return !progressSaveDebouncer.hasPending() ||
         saveProgress(progressSaveDebouncer.lastObservedPosition(), progressSaveDebouncer.lastObservedMetadata());
}

bool TxtReaderActivity::loadProgress() {
  uint8_t data[6]{};
  const int bytes = txt->readProgress("TRS", data, sizeof(data));
  if (bytes < 0) return false;
  if (bytes < 2) return true;
  currentPage = data[0] + (data[1] << 8);
  // Page numbers change when layout settings change while the book is closed.
  // A zero offset for a nonzero page was written by some older firmware.
  if (bytes == 6 && !pageOffsets.empty()) {
    const uint32_t offset =
        uint32_t{data[2]} | (uint32_t{data[3]} << 8) | (uint32_t{data[4]} << 16) | (uint32_t{data[5]} << 24);
    if (offset < txt->getFileSize() && (offset != 0 || currentPage == 0)) currentPage = pageAtOffset(offset);
  }
  currentPage = std::clamp(currentPage, 0, totalPages - 1);
  return true;
}

bool TxtReaderActivity::loadPageIndexCache(const IndexHeader& layout) {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  const size_t numPages = layout.readPageCount(f);
  if (numPages == 0) {
    LOG_DBG("TRS", "TXT index incomplete or layout changed, rebuilding");
    f.close();
    return false;
  }
  pageOffsets.clear();
  uint32_t offsets[32];
  bool hasFullWindows = false;
  while (pageOffsets.size() < numPages) {
    const size_t count = std::min(std::size(offsets), numPages - pageOffsets.size());
    const int bytes = count * sizeof(uint32_t);
    if (f.read(offsets, bytes) != bytes) {
      LOG_ERR("TRS", "Incomplete TXT page index");
      f.close();
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const size_t offset = offsets[i];
      // Older indexes could skip one byte when an entire 8 KiB window fit
      // without a newline. Only those books need a new index.
      if (pageOffsets.empty() ? offset != 0
                              : offset <= pageOffsets[pageOffsets.size() - 1] || offset >= txt->getFileSize() ||
                                    offset - pageOffsets[pageOffsets.size() - 1] > CHUNK_SIZE) {
        LOG_DBG("TRS", "Invalid TXT page offset %zu, rebuilding", offset);
        f.close();
        return false;
      }
      if (!pageOffsets.empty() && offset - pageOffsets[pageOffsets.size() - 1] == CHUNK_SIZE) hasFullWindows = true;
      if (!appendPageOffset(offset)) {
        f.close();
        return false;
      }
    }
  }

  f.close();
  if (txt->getFileSize() - pageOffsets[pageOffsets.size() - 1] > CHUNK_SIZE) return false;
  // Ordinary pages end well inside the read window. Only an old index with a
  // full 8 KiB page could have split a character or CRLF at the window edge.
  if (hasFullWindows) {
    HalFile source;
    if (!Storage.openFileForRead("TRS", txt->getPath(), source)) return false;
    bool valid = true;
    for (size_t i = 1; i < pageOffsets.size(); ++i) {
      if (pageOffsets[i] - pageOffsets[i - 1] != CHUNK_SIZE) continue;
      uint8_t tail[4];
      if (!source.seek(pageOffsets[i] - sizeof(tail)) || source.read(tail, sizeof(tail)) != sizeof(tail) ||
          unfinishedWindowSuffix(tail, sizeof(tail))) {
        LOG_DBG("TRS", "Unverified TXT window boundary at %u, rebuilding", pageOffsets[i]);
        valid = false;
        break;
      }
    }
    source.close();
    if (!valid) return false;
  }
  totalPages = pageOffsets.size();
  return true;
}

void TxtReaderActivity::savePageIndexCache(IndexHeader layout) const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  layout.pages = pageOffsets.size();
  if (!serialization::tryWritePod(f, layout)) {
    LOG_ERR("TRS", "Incomplete TXT index header write");
    f.close();
    return;
  }
  uint32_t offsets[32];
  for (size_t i = 0; i < pageOffsets.size();) {
    const size_t count = std::min(std::size(offsets), pageOffsets.size() - i);
    for (size_t j = 0; j < count; ++j) offsets[j] = pageOffsets[i + j];
    if (f.write(reinterpret_cast<const uint8_t*>(offsets), count * sizeof(uint32_t)) != count * sizeof(uint32_t)) {
      LOG_ERR("TRS", "Incomplete TXT page index write");
      break;
    }
    i += count;
  }
  f.close();
}

bool TxtReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Txt txt(filePath, "/.crosspoint");
  if (!txt.load()) {
    LOG_DBG("SLP", "TXT: failed to load %s", filePath.c_str());
    return false;
  }

  // Apply the reader orientation so margins match what the reader would produce
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  // Compute layout values that match what initializeReader() produces
  const int fontId = SETTINGS.getReaderFontId();
  const uint8_t verticalMargin = SETTINGS.screenMarginVertical;
  const uint8_t horizontalMargin = SETTINGS.screenMarginHorizontal;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginLeft += horizontalMargin;
  marginRight += horizontalMargin;
  const int topStatusBarReservedHeight = ReaderUtils::getTopStatusBarReservedHeight(renderer);
  if (topStatusBarReservedHeight > 0) {
    marginTop += std::max(static_cast<int>(verticalMargin),
                          topStatusBarReservedHeight + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING);
  } else {
    marginTop += verticalMargin;
  }
  marginBottom += ReaderUtils::getReaderFooterReservedHeight(renderer, false);

  const int vw = renderer.getScreenWidth() - marginLeft - marginRight;
  const int vh = renderer.getScreenHeight() - marginTop - marginBottom;
  const int lineHeight = getReaderLineHeight(renderer, fontId);
  const int linesPerPage = std::max(1, vh / lineHeight);

  // Step 1: Try to read the saved page and its file offset from progress.bin.
  // The 6-byte format (written by saveProgress) stores: page(2) + offset(4).
  // This lets us skip index.bin entirely, so the overlay works even when the
  // page index cache is missing or stale (e.g. after a firmware update).
  int savedPage = 0;
  size_t savedOffset = 0;
  bool offsetKnown = false;
  {
    uint8_t data[6]{};
    const int n = txt.readProgress("SLP", data, sizeof(data));
    if (n < 0) return false;
    if (n > 0) {
      if (n >= 2) {
        savedPage = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      }
      if (n >= 6) {
        const uint32_t off =
            (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
        if (off < txt.getFileSize() && (off != 0 || savedPage == 0)) {
          savedOffset = off;
          offsetKnown = true;
        }
      }
    }
  }

  // Step 2: If progress.bin didn't provide the offset, fall back to index.bin.
  if (!offsetKnown) {
    std::string cachePath = txt.getCachePath() + "/index.bin";
    FsFile cacheFile;
    if (Storage.openFileForRead("SLP", cachePath, cacheFile)) {
      const IndexHeader layout{CACHE_MAGIC,
                               CACHE_VERSION,
                               static_cast<uint32_t>(txt.getFileSize()),
                               SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing),
                               vw,
                               linesPerPage,
                               fontId,
                               verticalMargin,
                               horizontalMargin,
                               paragraphAlignment,
                               0,
                               SETTINGS.ruledLines ? vh : -1,
                               SETTINGS.ruledLines ? lineHeight : 0};
      const uint32_t numPages = layout.readPageCount(cacheFile);
      if (numPages) {
        if (savedPage < 0 || savedPage >= static_cast<int>(numPages)) savedPage = 0;
        uint32_t off = 0;
        if (cacheFile.seek(sizeof(IndexHeader) + savedPage * sizeof(off)) &&
            serialization::tryReadPod(cacheFile, off) && off < txt.getFileSize()) {
          savedOffset = off;
          offsetKnown = true;
        }
      } else {
        LOG_DBG("SLP", "TXT: index cache invalid or stale");
      }
      cacheFile.close();
    }

    // Step 3: No valid cache at all; render from the start of the file as a last resort.
    // This shows page 1 rather than a blank screen, which is always preferable.
    if (!offsetKnown) {
      LOG_DBG("SLP", "TXT: no valid cache, falling back to start of file");
      savedOffset = 0;
    }
  }

  // Load the page lines from file
  std::vector<std::string> pageLines;
  const size_t fileSize = txt.getFileSize();
  size_t offset = savedOffset;
  if (offset >= fileSize) {
    LOG_DBG("SLP", "TXT: page offset out of bounds");
    return false;
  }

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto buffer = makeUniqueNoThrow<uint8_t[]>(chunkSize + 1);
  if (!buffer) {
    LOG_ERR("SLP", "Failed to allocate TXT page scratch");
    return false;
  }
  if (!txt.readContent(buffer.get(), offset, chunkSize)) return false;
  buffer[chunkSize] = '\0';
  if (renderer.isSdCardFont(fontId))
    renderer.ensureSdCardFontReady(fontId, reinterpret_cast<const char*>(buffer.get()), /*styleMask=*/0x01);

  parseAndWrapLines(buffer.get(), chunkSize, offset, fileSize, linesPerPage, renderer, fontId, vw, &pageLines,
                    SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing), SETTINGS.ruledLines ? vh : -1,
                    lineHeight);
  buffer.reset();

  if (pageLines.empty()) return false;

  // Render the same foreground, alignment and text darkness as the visible page.
  const GfxRenderer::TextDarknessScope darkness(renderer, SETTINGS.textDarkness);
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  const ReaderRuling ruling{SETTINGS.ruledLines, marginLeft, vw, marginTop + vh, SETTINGS.lineHeightPercent};
  int y = marginTop;
  for (const auto& line : pageLines) {
    const auto row = SETTINGS.ruledLines
                         ? ReaderRuling::textRow(renderer, fontId, line.c_str(), lineHeight,
                                                 SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing))
                         : RuledLineLayout{lineHeight, 0};
    if (!line.empty()) {
      const int x = lineOriginX(renderer, line.c_str(), fontId, marginLeft, vw, paragraphAlignment,
                                SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
      renderer.drawText(fontId, x, y + row.offset, line.c_str(), ReaderUtils::readerForegroundBlack(),
                        EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                        SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
    }
    y += row.height;
    if (!line.empty() && ruling.enabled(renderer)) ruling.draw(renderer, y);
  }
  return true;
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}

void TxtReaderActivity::saveProgressBeforeRestart() {
  // Silent network-mode restarts skip onExit(); coordinate with the render task.
  RenderLock lock(*this);
  if (!(flushQueuedProgress())) {
    LOG_ERR("READER", "Failed to save progress before restart");
  }
}
