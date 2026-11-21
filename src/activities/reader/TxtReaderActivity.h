#pragma once
#include <ChunkedPageIndex.h>
#include <Txt.h>

#include <optional>
#include <vector>

#include "BookLoadingFeedback.h"
#include "CrossPointSettings.h"
#include "DeferredGrayscale.h"
#include "PluginReaderEvents.h"
#include "ReaderButtonShortcuts.h"
#include "ReaderGrayscale.h"
#include "ReaderProgressSaveDebouncer.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#if CROSSINK_APP_CAP_TOUCH
#include "activities/reader/ReaderPinchGesture.h"
#endif

class TxtReaderActivity final : public Activity {
  PluginReaderEvents pluginEvents;
  ReaderUtils::DeferredGrayscalePlanes deferredGrayscalePlanes;
#ifdef SIMULATOR
  friend struct ReaderMenuOrientationSmokeTest;
  friend struct StatusBarFeatureSmokeTest;
  friend struct ScreenCalibrationSmokeTest;
#endif
  OptionPopup quickActionsPopup;
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  ReaderUtils::PageToneHistory pageToneHistory;
  // Session-only display toggle; cached page layout remains unchanged.
  bool statusBarVisible = true;
  ReaderButtonShortcuts readerButtonShortcuts;
  bool longPowerButtonHandled = false;
  bool longPressBackHandled = false;
  bool longPressMenuHandled = false;
  bool skipRecentBookUpdateOnEntry = false;
  ReaderProgressSaveDebouncer progressSaveDebouncer;
#if CROSSINK_APP_CAP_TOUCH
  ReaderPinchGesture pinchFontGesture;
#endif

  // Streaming text reader - stores file offsets for each page
  ChunkedPageIndex<uint32_t> pageOffsets;
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;
  bool pageReadFailed = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int8_t cachedCharacterSpacing = 0;
  int cachedRuledViewport = -1;
  int cachedRowHeight = 0;
  int cachedFontId = 0;
  uint8_t cachedVerticalMargin = 0;
  uint8_t cachedHorizontalMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedTopStatusBarHeight = 0;
  int cachedFooterReservedHeight = 0;
  ScreenInsets cachedViewableInsets;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;
  // Keep the same byte visible through repeated layout edits and failed retries.
  std::optional<uint32_t> relayoutOffset;
  int relayoutPage = -1;

  void renderPage();
  void renderStatusBar() const;
  void requestPageTurn(bool previous, bool next);

  void initializeReader();
  bool loadPageAtOffset(size_t offset);
  size_t wrapPage(uint8_t* buffer, size_t length, size_t offset, std::vector<std::string>* lines);
  bool buildPageIndex();
  struct IndexHeader;
  bool loadPageIndexCache(const IndexHeader& layout);
  void savePageIndexCache(IndexHeader layout) const;
  bool saveProgress(uint32_t page, uint32_t offset);
  bool appendPageOffset(uint32_t offset);
  int pageAtOffset(uint32_t offset) const;
  void invalidateTextLayout();
  bool queueProgressSave();
  bool flushQueuedProgress();
  void saveProgressBeforeRestart();
  bool loadProgress();
  void toggleDarkMode();
  void toggleHomeButtonInReader();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  static bool supportsQuickAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executeReaderShortcutAction(CrossPointSettings::SHORT_PWRBTN action);
  bool executePowerButtonAction();
  bool executeLongPressBackAction();
  bool changeReaderFontSize(bool larger, FontSizeStepMode mode = FontSizeStepMode::Wrap);
  void cycleReaderFont();
  // Font changes and layout invalidation share the caller's RenderLock.
  void rebuildTextLayout();
  bool applyReaderOrientation(uint8_t orientation);
#if CROSSINK_APP_CAP_TOUCH
  bool handlePinchFontResize();
  void resetPinchFontGesture();
#endif

  BookLoadingFeedback openingFeedback;
  bool showOpeningFeedback() {
    if (!openingFeedback.showIfPending(renderer)) return false;
    pagesUntilFullRefresh = 0;
    return true;
  }

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown, bool skipRecentBookUpdateOnEntry = false,
                             BookLoadingFeedback openingFeedback = {})
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown),
        skipRecentBookUpdateOnEntry(skipRecentBookUpdateOnEntry),
        openingFeedback(openingFeedback) {}
  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override {
    if (!flushQueuedProgress()) LOG_ERR("PEVT", "Reader progress save failed before sleep");
    pluginEvents.poll(getCurrentBookPath());
    pluginEvents.flush(getCurrentBookPath());
  }
  void loop() override;
  void render(RenderLock&&) override;
  bool handleTwoFingerSwipeAction(CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) override;
  bool handleTwoFingerRotation(bool clockwise) override;
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = -1;
    return true;
  }
  bool isReaderActivity() const override { return true; }
  bool isBookReaderActivity() const override { return true; }
  bool usesMenuButtonOrientation() const override { return quickActionsPopup.isActive(); }
  bool usesFullScreenReaderVerticalSwipes() const override {
    return mappedInput.hasTouchHardware() && !mappedInput.hasHomeKey();
  }
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(uint8_t action) override;
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  std::string getCurrentBookPath() const override { return txt ? txt->getPath() : std::string{}; }
  std::string getCurrentBookTitle() const override { return txt ? txt->getTitle() : std::string{}; }
  bool getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) override;
  void onFrontlightPanelOpened() override { saveProgressBeforeRestart(); }
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  ScreenshotInfo getScreenshotInfo() const override;
};
