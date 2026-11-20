#pragma once
#include "ReadingStatsUtils.h"
#include "StatsCounterId.h"

// Separate, device-local cumulative counters. No historical totals are backfilled.
namespace DailyReadingStats {
struct Counter {
  uint32_t seconds = 0;
  uint32_t uploaded = 0;
};
// Legacy directory; storage resolves it to daily_reading_<reader>.
constexpr const char* DIRECTORY = "/.crosspoint/daily_reading";
bool read(uint32_t day, Counter& counter);
bool write(uint32_t day, const Counter& counter);
bool record(uint32_t day, uint32_t seconds, bool allowCheckpoint = true);
bool checkpointIfDue();
bool flush();
bool counterId(StatsCounterId& id);
// Streams cumulative daily totals; acknowledges only successful callbacks. No file stays open during upload.
bool uploadPending(bool (*upload)(void*, uint32_t day, uint32_t seconds), void* context);
// Call after successful sync. Keep the current month, this many prior months, and all future or uncertain data.
bool pruneSyncedMonths(const ReadingStatsDate& today, uint8_t priorMonthsToKeep = 1);
// Seconds for one day, including accepted seconds not yet checkpointed to SD.
bool secondsFor(uint32_t day, uint32_t& seconds);

constexpr size_t MAX_SIX_MONTH_DAYS = 184;
// Number of dates after the same day six months ago (clamped to the shorter month), through today.
size_t sixMonthDayCount(const ReadingStatsDate& today);
// Load device-local seconds oldest first, including pending time. Use the output only on success.
bool loadWindow(const ReadingStatsDate& today, uint32_t* seconds, size_t count);
struct WindowSummary {
  uint32_t totalSeconds = 0;
  uint16_t activeDays = 0;
  uint32_t peakSeconds = 0;
};
WindowSummary summarizeWindow(const uint32_t* seconds, size_t count);

// Today plus a 7-calendar-day average. Days without reading count as zero.
struct Summary {
  bool hasToday = false;
  bool hasSevenDayAverage = false;
  uint32_t todaySeconds = 0;
  uint32_t sevenDayAverageSeconds = 0;
};
Summary summarize(uint32_t today);

// Clock capture is paired with the existing page timer, including its pauses.
// At most nine one-second dates can precede the existing 10-second session floor.
class Session {
  struct Day {
    uint32_t day = 0;
    uint32_t seconds = 0;
  };
  Day pending[10]{};
  uint8_t pendingCount = 0;
  ReadingStatsDateTime startTime{};
  uint8_t offset = 255;

 public:
  void reset() { pendingCount = 0; }
  void start(const ReadingStatsDateTime& local, uint8_t utcOffset) {
    startTime = local;
    offset = utcOffset;
  }
  bool accept(const ReadingStatsDateTime& end, uint8_t utcOffset, uint32_t seconds, uint32_t sessionSeconds,
              bool allowCheckpoint = true);
};
}  // namespace DailyReadingStats
