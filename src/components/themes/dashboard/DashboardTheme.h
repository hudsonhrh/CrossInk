#pragma once

#include <AppCapabilities.h>

#include <array>
#include <cstdint>

#include "components/themes/minimal/MinimalTheme.h"
#if CROSSINK_APP_READING_HEATMAP
#include "components/ReadingHeatmap.h"

struct DashboardHistory {
  std::array<uint32_t, ReadingHeatmap::MonthDays> seconds{};
  uint32_t totalSeconds = 0;
  uint16_t streak = 0;
  bool available = false;
  void load(const GlobalReadingStats* globalStats);
};
#endif

namespace DashboardMetrics {
constexpr ThemeMetrics makeValues() {
  ThemeMetrics v = MinimalMetrics::values;
  v.homeTopPadding = 50;
  v.homeCoverHeight = 445;
  v.homeCoverTileHeight = 690;
  v.homeRecentBooksCount = 1;
  v.homeContinueReadingInMenu = false;
  v.homeMenuTopOffset = 0;
  return v;
}

constexpr ThemeMetrics values = makeValues();
constexpr int homeCoverImageWidth = 296;
constexpr int homeCoverImageHeight = 444;
}  // namespace DashboardMetrics

class DashboardTheme : public MinimalTheme {
 public:
  static Rect getHomeCoverRect(const GfxRenderer& renderer);
  static Rect getSleepCoverRect(const GfxRenderer& renderer);
#if CROSSINK_APP_READING_HEATMAP
  // Outside the range of the four standard Home navigation items.
  static constexpr int historyTouchId = 4;
  static void drawHomeHistory(const GfxRenderer& renderer, const Rect& tile, const DashboardHistory& history);
#endif

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  void drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                       const GlobalReadingStats* globalStats, float progressPercent = -1.0f,
                       const char* currentChapterTitle = nullptr, bool inverted = false) const;
};
