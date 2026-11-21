#pragma once

#include <HalClock.h>

#include <string>

#include "../Activity.h"
#include "AppCapabilities.h"
#include "BookReadingStats.h"
#include "BookStatsView.h"
#include "GlobalReadingStats.h"

class BookStatsActivity final : public Activity {
  enum class Page : uint8_t { PerBook, ThisDevice, AllDevices, History, EditDates };

  std::string bookTitle;
  std::string bookCachePath;
  std::string syncBookPath;  // the book, for the sync pending-push list; empty when unknown
  BookReadingStats stats;
  bool savedCompleted = false;  // finished state last written, to spot a change on save
  GlobalReadingStats globalStats;
  GlobalReadingStats allDevicesStats;
  DailyReadingStats::Summary dailySummary;
#if CROSSINK_APP_READING_HEATMAP
  // Owned by this activity so six months of daily counters never live on the render task stack.
  uint32_t historySeconds[DailyReadingStats::MAX_SIX_MONTH_DAYS]{};
  ReadingStatsDate historyToday;
  size_t historyDays = 0;
  void loadHistory();
#endif
  bool showAllDevicesStats = false;
  bool returnToHomeOnExit = false;
  float progressPercent = -1.0f;
  bool hasEstimatedTimeLeft = false;
  uint32_t estimatedTimeLeftSeconds = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
  Page page = Page::PerBook;
#if CROSSINK_APP_READING_HEATMAP
  bool cleanHistoryOnOpen = false;
#endif
  int selectedEditField = 0;
  bool didChangeStats = false;
  BookReadingStats dateEditStatsSnapshot;
  GlobalReadingStats dateEditGlobalStatsSnapshot;
  bool dateEditSnapshotValid = false;
  bool didChangeStatsBeforeDateEdit = false;
  bool ignoreInitialBackRelease = false;
  bool ignoreInitialConfirmRelease = false;
  bool ignoreInitialPowerRelease = false;

  bool hasEditableBook() const { return !bookCachePath.empty() && halClock.isAvailable(); }
  // Daily counters can use trusted system time when the X4 Pro RTC is unavailable.
  bool usesNoRtcSingleScreenLayout() const { return !SETTINGS.shouldShowReadingHeatmap() && !halClock.isAvailable(); }
  void refreshAllDevicesStats();
  void loadDailySummary();
  void saveStats();
  void beginDateEditing();
  void finishDateEditing(bool saveChanges);
  void cycleEditField();
  void adjustSelectedDateField(int delta);
  void applyCompletedState(bool completed);
  ReadingStatsDate defaultDateForField(bool finishedField) const;
  void clearEditedDate(bool finishedField);
  bool shouldClearDateOnAdjust(const ReadingStatsDate& date, bool finishedField, int fieldIndex, int delta) const;
  void normalizeEditedDates(const bool editedFinishedField);
  void exitStatsActivity();
  bool showPreviousStatsPage();
  bool showNextStatsPage();
  bool selectEditFieldFromTouchTarget(int touchTarget);
  // "This Device" totals sync from here; needs a configured sync account.
  bool canSyncReadingStats() const;
  void startSyncReadingStats();

 public:
#if CROSSINK_APP_READING_HEATMAP
  void openHistoryPage() {
    if (SETTINGS.shouldShowReadingHeatmap()) {
      page = Page::History;
      cleanHistoryOnOpen = true;
    }
  }
  void onFrontlightPanelClosed() override;
#endif
  // The book these stats belong to, so date edits are queued for the next sync.
  void setSyncBookPath(std::string path) { syncBookPath = std::move(path); }
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const std::string& bookCachePath, const BookReadingStats& stats, float progressPercent,
                    bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                    bool returnToHomeOnExit = false);
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const std::string& bookCachePath, const BookReadingStats& stats, float progressPercent,
                    bool hasEstimatedTimeLeft, uint32_t estimatedTimeLeftSeconds, const GlobalReadingStats& globalStats,
                    const GlobalReadingStats& allDevicesStats, bool returnToHomeOnExit = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
