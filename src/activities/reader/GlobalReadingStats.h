#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "ReadingStatsUtils.h"
#include "StatsCounterId.h"

struct BookReadingStats;

// Cumulative reading statistics across all books, persisted to
// /.crosspoint/global_stats_<reader>.bin.
struct GlobalReadingStats {
  uint32_t totalSessions = 0;        // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;     // Total forward page turns after the dwell threshold
  uint32_t completedBooks = 0;       // Books manually marked as finished
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;
  StatsCounterId counterId;
  bool articleMarked = false;  // Stays set until this history is reset.
  uint32_t completedArticles = 0;
  uint32_t unassignedLegacyBooks = 0;  // Book completions that predate per-title credit records.

  static constexpr uint8_t CURRENT_FILE_VERSION = 5;
  static constexpr size_t V4_FILE_SIZE = 175;
  static constexpr size_t CURRENT_FILE_SIZE = 184;
  static constexpr size_t MIN_SUPPORTED_FILE_SIZE = 13;

  // Loads this reader's device-qualified stats. Only a genuinely new history
  // receives a fresh identity; unreadable existing slots protect future saves.
  static GlobalReadingStats load();

  bool hasArticles() const { return articleMarked || completedArticles > 0; }

  // Checked local export, never includes nearby-device snapshots.
  static bool exportLocal(uint8_t* data, size_t capacity);
  // Encode an exported local snapshot for a nearby reader's supported stats format.
  static size_t copyForNearby(const std::array<uint8_t, CURRENT_FILE_SIZE>& local, uint8_t peerVersion, uint8_t* out,
                              size_t capacity);
  static bool loadForUpload(GlobalReadingStats& stats);
  // A failed read is not an empty history. Only explicit backup recovery (or
  // restarting after repairing storage) may clear this write protection.
  static bool recoveryRequired();
  static bool readBackup(const char* path, GlobalReadingStats& stats);
  static bool restoreBackup(const char* path);

  // Returns true when the optional synced stats directory exists.
  static bool hasSyncedStats();

  // Loads this device's local stats plus one synced stats file per other device
  // from /.crosspoint/synced_stats/. A stale file matching this device's MAC is
  // skipped to avoid double counting.
  static GlobalReadingStats loadAggregated();

  // Adds synced device stats to an already-loaded local stats snapshot. Use this
  // when the local stats may include in-memory changes that are not saved yet.
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);

  // Update the local completion counters alongside the per-title record in memory.
  bool setCompleted(BookReadingStats& book, bool completed, bool track);
  // Update classification fields in an open reader while preserving its pending session.
  static bool recoverArticleTransfer();  // Called after mounting storage, before loading any title or counters.
  static bool toggleArticle(const std::string& cachePath, bool& article, BookReadingStats* live = nullptr,
                            const StatsCounterId* liveGlobalId = nullptr);

  // Saves the local snapshot alone when editing completion counts between sessions.
  bool saveSnapshot() const;
  // Saves global and pending daily stats. False means either save needs retrying.
  bool save() const;

  // Saves an empty newest snapshot through the same alternating-slot policy.
  static bool resetLocal();
  // Advances after each successful resetLocal(), so an open reader knows its
  // in-memory copy was replaced on disk. Any new path that replaces the file
  // while a reader may be open must advance it too.
  static uint32_t localResetRevision();

  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;

  // Separate server calendar; never included in loadForUpload() or nearby exports.
  static bool saveServerStreak(const GlobalReadingStats& streak);
};
