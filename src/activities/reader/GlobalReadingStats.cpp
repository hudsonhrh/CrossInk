#include "GlobalReadingStats.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <esp_mac.h>

#include <array>
#include <cstring>
#include <limits>
#include <string>

#include "BookReadingStats.h"
#include "DailyReadingStats.h"
#include "TwoSlotFile.h"
#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

namespace {
enum class StatsLoadResult : uint8_t { Ok, Missing, Invalid, NewerFormat };

struct StatsLoadOutcome {
  StatsLoadResult result = StatsLoadResult::Invalid;
  uint8_t version = 0;
  size_t fileSize = 0;
};

// Binary layout v1 (13 bytes):
//   [0]     version (= 1)
//   [1-4]   totalSessions       uint32_t LE
//   [5-8]   totalReadingSeconds uint32_t LE
//   [9-12]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (17 bytes):
//   [0]      version (= 2)
//   [1-4]    totalSessions       uint32_t LE
//   [5-8]    totalReadingSeconds uint32_t LE
//   [9-12]   totalPagesTurned    uint32_t LE
//   [13-16]  completedBooks      uint32_t LE
//
// Binary layout v3 (159 bytes):
//   [0]       version (= 3)
//   [1-4]     totalSessions             uint32_t LE
//   [5-8]     totalReadingSeconds       uint32_t LE
//   [9-12]    totalPagesTurned          uint32_t LE
//   [13-16]   completedBooks            uint32_t LE
//   [17-32]   timeOfDaySeconds[4]       uint32_t LE each
//   [33-60]   dayOfWeekSeconds[7]       uint32_t LE each
//   [61-64]   readingHistoryAnchorDay   uint32_t LE
//   [65-156]  readingHistoryBits[92]    uint8_t
//   [157-158] longestReadingStreak      uint16_t LE
// v4 appends the local counter ID (16 bytes); v5 appends completedArticles (LE u32)
// followed by the unassigned legacy book completions (LE u32) and article-marked flag (bit 0).
static constexpr uint8_t GLOBAL_STATS_VERSION = GlobalReadingStats::CURRENT_FILE_VERSION;
static constexpr uint8_t GLOBAL_STATS_VERSION_V1 = 1;
static constexpr int GLOBAL_STATS_FILE_SIZE_V1 = 13;
static constexpr uint8_t GLOBAL_STATS_VERSION_V2 = 2;
static constexpr int GLOBAL_STATS_FILE_SIZE_V2 = 17;
static constexpr int GLOBAL_STATS_FILE_SIZE_V3 = 159;
static constexpr int GLOBAL_STATS_FILE_SIZE = static_cast<int>(GlobalReadingStats::CURRENT_FILE_SIZE);
static constexpr size_t GLOBAL_STATS_FILE_SIZE_V4 = GlobalReadingStats::V4_FILE_SIZE;
static_assert(GlobalReadingStats::CURRENT_FILE_SIZE <= two_slot::MAX_PAYLOAD_BYTES);
static constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
static constexpr char SYNCED_STATS_DIR[] = "/.crosspoint/synced_stats";
static bool s_blockDestructiveSave = false;
static bool s_articleRecoveryPending = false;
static uint32_t s_localResetRevision = 0;

// Bind the cached calendar to the selected server/account without storing credentials.
std::string serverStreakPath() {
  MD5Builder md5;
  md5.begin();
  md5.add(KOREADER_STORE.getBaseUrl().c_str());
  md5.add("\n");
  md5.add(KOREADER_STORE.getUsername().c_str());
  md5.calculate();
  return std::string("/.crosspoint/server_streak_") + md5.toString().c_str() + ".bin";
}

bool loadServerStreak(GlobalReadingStats& out) {
  if (!KOREADER_STORE.hasCredentials() || !KOREADER_STORE.getSyncStats()) return false;
  SyncFiles::Reader reader(serverStreakPath().c_str());
  uint8_t version = 0;
  const bool loaded = reader.open() != SyncFiles::Reader::Source::None && reader.pod(version) && version == 1 &&
                      reader.pod(out.readingHistoryAnchorDay) &&
                      reader.read(out.readingHistoryBits.data(), READING_HISTORY_BYTES) &&
                      reader.pod(out.longestReadingStreak) && reader.atEnd();
  reader.close();
  return loaded;
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void loadCommonFields(const uint8_t* data, GlobalReadingStats& out) {
  out.totalSessions = readLe32(data, 1);
  out.totalReadingSeconds = readLe32(data, 5);
  out.totalPagesTurned = readLe32(data, 9);
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t addSaturated(const uint32_t a, const uint32_t b) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  return max - a < b ? max : a + b;
}

void addStats(GlobalReadingStats& target, const GlobalReadingStats& source) {
  target.totalSessions = addSaturated(target.totalSessions, source.totalSessions);
  target.totalReadingSeconds = addSaturated(target.totalReadingSeconds, source.totalReadingSeconds);
  target.totalPagesTurned = addSaturated(target.totalPagesTurned, source.totalPagesTurned);
  target.completedBooks = addSaturated(target.completedBooks, source.completedBooks);
  target.articleMarked |= source.hasArticles();
  target.completedArticles = addSaturated(target.completedArticles, source.completedArticles);
  for (size_t i = 0; i < target.timeOfDaySeconds.size(); ++i) {
    target.timeOfDaySeconds[i] = addSaturated(target.timeOfDaySeconds[i], source.timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < target.dayOfWeekSeconds.size(); ++i) {
    target.dayOfWeekSeconds[i] = addSaturated(target.dayOfWeekSeconds[i], source.dayOfWeekSeconds[i]);
  }
  mergeReadingHistory(target.readingHistoryAnchorDay, target.readingHistoryBits, source.readingHistoryAnchorDay,
                      source.readingHistoryBits);
  target.longestReadingStreak = std::max(target.longestReadingStreak, source.longestReadingStreak);
}

void serializeStats(const GlobalReadingStats& stats, uint8_t* data) {
  data[0] = GLOBAL_STATS_VERSION;
  data[1] = stats.totalSessions & 0xFF;
  data[2] = (stats.totalSessions >> 8) & 0xFF;
  data[3] = (stats.totalSessions >> 16) & 0xFF;
  data[4] = (stats.totalSessions >> 24) & 0xFF;
  data[5] = stats.totalReadingSeconds & 0xFF;
  data[6] = (stats.totalReadingSeconds >> 8) & 0xFF;
  data[7] = (stats.totalReadingSeconds >> 16) & 0xFF;
  data[8] = (stats.totalReadingSeconds >> 24) & 0xFF;
  data[9] = stats.totalPagesTurned & 0xFF;
  data[10] = (stats.totalPagesTurned >> 8) & 0xFF;
  data[11] = (stats.totalPagesTurned >> 16) & 0xFF;
  data[12] = (stats.totalPagesTurned >> 24) & 0xFF;
  data[13] = stats.completedBooks & 0xFF;
  data[14] = (stats.completedBooks >> 8) & 0xFF;
  data[15] = (stats.completedBooks >> 16) & 0xFF;
  data[16] = (stats.completedBooks >> 24) & 0xFF;
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    const uint32_t value = stats.timeOfDaySeconds[i];
    const int offset = 17 + static_cast<int>(i) * 4;
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    const uint32_t value = stats.dayOfWeekSeconds[i];
    const int offset = 33 + static_cast<int>(i) * 4;
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  }
  data[61] = stats.readingHistoryAnchorDay & 0xFF;
  data[62] = (stats.readingHistoryAnchorDay >> 8) & 0xFF;
  data[63] = (stats.readingHistoryAnchorDay >> 16) & 0xFF;
  data[64] = (stats.readingHistoryAnchorDay >> 24) & 0xFF;
  memcpy(data + 65, stats.readingHistoryBits.data(), stats.readingHistoryBits.size());
  data[157] = stats.longestReadingStreak & 0xFF;
  data[158] = (stats.longestReadingStreak >> 8) & 0xFF;
  memcpy(data + GLOBAL_STATS_FILE_SIZE_V3, stats.counterId.bytes.data(), stats.counterId.bytes.size());
  progress_record::writeU32(data + GLOBAL_STATS_FILE_SIZE_V4, stats.completedArticles);
  progress_record::writeU32(data + 179, stats.unassignedLegacyBooks);
  data[183] = stats.hasArticles() ? 1 : 0;
}

size_t slotPayloadSize(uint8_t version) {
  if (version == 3) return GLOBAL_STATS_FILE_SIZE_V3;
  if (version == 4) return GLOBAL_STATS_FILE_SIZE_V4;
  return GLOBAL_STATS_FILE_SIZE;
}

StatsLoadResult classifyGlobalPayload(uint8_t version, size_t size) {
  const size_t payloadSize = slotPayloadSize(version);
  // An interrupted slot upgrade can leave just part of the trailer.
  if (version >= 3 && version <= GLOBAL_STATS_VERSION && size > payloadSize &&
      size < payloadSize + two_slot::TRAILER_BYTES)
    return StatsLoadResult::Invalid;
  if (size > GLOBAL_STATS_FILE_SIZE || version > GLOBAL_STATS_VERSION) return StatsLoadResult::NewerFormat;
  if ((version == 1 && size == GLOBAL_STATS_FILE_SIZE_V1) || (version == 2 && size == GLOBAL_STATS_FILE_SIZE_V2) ||
      (version >= 3 && version <= GLOBAL_STATS_VERSION && size == payloadSize))
    return StatsLoadResult::Ok;
  return StatsLoadResult::Invalid;
}

void decodeGlobalPayload(const uint8_t* data, size_t n, GlobalReadingStats& out) {
  out = {};
  loadCommonFields(data, out);
  if (n == GLOBAL_STATS_FILE_SIZE_V1) return;
  out.completedBooks = readLe32(data, 13);
  if (n == GLOBAL_STATS_FILE_SIZE_V2) {
    out.unassignedLegacyBooks = out.completedBooks;
    return;
  }
  for (size_t i = 0; i < out.timeOfDaySeconds.size(); ++i) {
    out.timeOfDaySeconds[i] = readLe32(data, 17 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < out.dayOfWeekSeconds.size(); ++i) {
    out.dayOfWeekSeconds[i] = readLe32(data, 33 + static_cast<int>(i) * 4);
  }
  out.readingHistoryAnchorDay = readLe32(data, 61);
  memcpy(out.readingHistoryBits.data(), data + 65, out.readingHistoryBits.size());
  out.longestReadingStreak = readLe16(data, 157);
  if (n >= GLOBAL_STATS_FILE_SIZE_V4)
    memcpy(out.counterId.bytes.data(), data + GLOBAL_STATS_FILE_SIZE_V3, out.counterId.bytes.size());
  if (n == GLOBAL_STATS_FILE_SIZE) {
    out.completedArticles = readLe32(data, GLOBAL_STATS_FILE_SIZE_V4);
    out.unassignedLegacyBooks = readLe32(data, 179);
    out.articleMarked = (data[183] & 1u) || out.completedArticles > 0;
  } else {
    out.unassignedLegacyBooks = out.completedBooks;
  }
}

StatsLoadOutcome loadFromOpenFile(FsFile& f, GlobalReadingStats& out) {
  StatsLoadOutcome outcome;
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || !f.seekSet(0)) return outcome;
  outcome.fileSize = two_slot::payloadLength(f, slotPayloadSize(version));
  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  const size_t bytesToRead = std::min(outcome.fileSize, sizeof(data));
  const int n = f.read(data, bytesToRead);
  if (n <= 0 || static_cast<size_t>(n) != bytesToRead) return outcome;
  outcome.version = data[0];
  outcome.result = classifyGlobalPayload(outcome.version, outcome.fileSize);
  if (outcome.result == StatsLoadResult::Ok) decodeGlobalPayload(data, outcome.fileSize, out);
  return outcome;
}

std::string localSyncedStatsFileName() {
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) != 0) return {};

  char name[32];
  snprintf(name, sizeof(name), "device_%02x%02x%02x%02x%02x%02x.bin", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return name;
}

bool saveToFile(const GlobalReadingStats& stats, const char* path, const char* backupPath) {
  uint8_t data[GLOBAL_STATS_FILE_SIZE];
  serializeStats(stats, data);

  return two_slot::write(
      "GSTATS", path, backupPath, data, sizeof(data),
      [](const uint8_t* bytes, size_t size) {
        return size != 0 && classifyGlobalPayload(bytes[0], size) == StatsLoadResult::Ok;
      },
      {GLOBAL_STATS_FILE_SIZE_V4, GLOBAL_STATS_FILE_SIZE_V3}, ".new");
}

}  // namespace

// exists() alone also returns false on FAT I/O errors. Only a complete parent
// scan can establish first use after open fails. This is a cold failure path;
// the bounded filename buffer avoids allocating a directory listing.
static bool confirmedMissingFile(const char* path, const bool recovering = false) {
  const std::string full(path);
  const auto slash = full.find_last_of('/');
  if (slash == std::string::npos) return false;
  const std::string parent = full.substr(0, slash);
  const char* wanted = path + slash + 1;
  const std::string marker = std::string(wanted) + ".recovery";
  FsFile dir = Storage.open(parent.c_str());
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return false;
  }
  bool absent = true;
  char name[128];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool named = file.getName(name, sizeof(name)) != 0;
    const bool closed = file.close();
    if (!named || !closed || strcmp(name, wanted) == 0 || (!recovering && strcmp(name, marker.c_str()) == 0)) {
      absent = false;
      break;
    }
  }
  const bool scanned = !FsHelpers::directoryIterationFailed(dir);
  const bool closed = dir.close();
  return absent && scanned && closed;
}

static StatsLoadOutcome loadFromFile(const char* path, GlobalReadingStats& out, bool primary = false) {
  StatsLoadOutcome outcome;
  FsFile f;
  if (!Storage.openFileForRead("GSTATS", path, f)) {
    if (confirmedMissingFile(path)) outcome.result = StatsLoadResult::Missing;
    return outcome;
  }
  outcome = loadFromOpenFile(f, out);
  if (!f.close()) outcome.result = StatsLoadResult::Invalid;
  if (primary && outcome.result != StatsLoadResult::NewerFormat) {
    const std::string selected = two_slot::preferredPath("GSTATS", path, GLOBAL_STATS_FILE_SIZE, false,
                                                         {GLOBAL_STATS_FILE_SIZE_V4, GLOBAL_STATS_FILE_SIZE_V3});
    if (selected != path && Storage.openFileForRead("GSTATS", selected, f)) {
      outcome = loadFromOpenFile(f, out);
      if (!f.close()) outcome.result = StatsLoadResult::Invalid;
    }
  }
  return outcome;
}

static bool readLocalHistory(GlobalReadingStats& stats) {
  stats = {};
  if (s_blockDestructiveSave) return false;
  const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH);
  if (path.empty()) return false;
  if (Storage.exists((path + ".recovery").c_str())) {
    s_blockDestructiveSave = true;
    return false;
  }
  const auto primary = loadFromFile(path.c_str(), stats, true);
  if (primary.result == StatsLoadResult::Ok) return true;
  if (primary.result == StatsLoadResult::NewerFormat) return false;
  const auto backup = loadFromFile((path + ".bak").c_str(), stats);
  return backup.result == StatsLoadResult::Ok;
}

bool GlobalReadingStats::loadForUpload(GlobalReadingStats& stats) {
  return !s_articleRecoveryPending && readLocalHistory(stats);
}

bool GlobalReadingStats::exportLocal(uint8_t* data, size_t capacity) {
  GlobalReadingStats stats;
  if (!data || capacity < CURRENT_FILE_SIZE || !loadForUpload(stats)) return false;
  serializeStats(stats, data);
  return true;
}

size_t GlobalReadingStats::copyForNearby(const std::array<uint8_t, CURRENT_FILE_SIZE>& local, const uint8_t peerVersion,
                                         uint8_t* out, const size_t capacity) {
  static constexpr size_t sizes[] = {0,
                                     GLOBAL_STATS_FILE_SIZE_V1,
                                     GLOBAL_STATS_FILE_SIZE_V2,
                                     GLOBAL_STATS_FILE_SIZE_V3,
                                     V4_FILE_SIZE,
                                     CURRENT_FILE_SIZE};
  static_assert(std::size(sizes) == CURRENT_FILE_VERSION + 1);
  const uint8_t version = std::min(peerVersion, CURRENT_FILE_VERSION);
  const size_t size = sizes[version];
  if (!out || !size || size > capacity || local[0] != CURRENT_FILE_VERSION) return 0;
  memcpy(out, local.data(), size);
  out[0] = version;
  if (version >= 2 && version < CURRENT_FILE_VERSION) {
    // Older readers have a single completion count. Their remaining fields are a prefix of v5.
    progress_record::writeU32(out + 13, addSaturated(readLe32(local.data(), 13), readLe32(local.data(), V4_FILE_SIZE)));
  }
  return size;
}

static GlobalReadingStats loadLocalStats() {
  GlobalReadingStats stats;
  if (s_articleRecoveryPending) return stats;
  const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH);
  if (path.empty()) {
    s_blockDestructiveSave = true;
    return stats;
  }
  if (Storage.exists((path + ".recovery").c_str())) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "Interrupted history recovery; preserving all snapshots");
    return stats;
  }
  const StatsLoadOutcome primary = loadFromFile(path.c_str(), stats, true);
  if (primary.result == StatsLoadResult::Ok) return stats;
  if (primary.result == StatsLoadResult::NewerFormat) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "On-disk stats are from a newer build (v%u, %u bytes); refusing to overwrite", primary.version,
            static_cast<unsigned>(primary.fileSize));
    return stats;
  }

  const StatsLoadOutcome backup = loadFromFile((path + ".bak").c_str(), stats);
  if (backup.result == StatsLoadResult::Ok) {
    LOG_DBG("GSTATS", "Recovered global stats from backup");
    return stats;
  }
  if (backup.result == StatsLoadResult::NewerFormat) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "Backup stats are from a newer build (v%u, %u bytes); refusing to overwrite", backup.version,
            static_cast<unsigned>(backup.fileSize));
    return stats;
  }

  if (primary.result != StatsLoadResult::Missing || backup.result != StatsLoadResult::Missing) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "Cannot read saved history; preserving stats and backups for recovery");
    return {};
  }
  LOG_DBG("GSTATS", "No global stats yet, starting fresh");
  stats.counterId = StatsCounterId::create();
  return stats;
}

GlobalReadingStats GlobalReadingStats::load() { return loadLocalStats(); }

bool GlobalReadingStats::recoveryRequired() { return s_blockDestructiveSave || s_articleRecoveryPending; }

bool GlobalReadingStats::readBackup(const char* path, GlobalReadingStats& stats) {
  stats = {};
  if (!path || !*path) return false;
  return loadFromFile(path, stats, true).result == StatsLoadResult::Ok ||
         loadFromFile((std::string(path) + ".bak").c_str(), stats).result == StatsLoadResult::Ok;
}

bool GlobalReadingStats::restoreBackup(const char* backupPath) {
  GlobalReadingStats restored;
  if (!backupPath || strncmp(backupPath, "/.crossink-stats-backup/", 24) != 0 || !readBackup(backupPath, restored)) {
    LOG_ERR("GSTATS", "Cannot validate recovery snapshot");
    return false;
  }
  const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH);
  if (path.empty()) return false;
  s_blockDestructiveSave = true;
  // Presence protects a recovery interrupted after the originals were archived
  // but before the replacement reached disk. Even an empty marker fails closed.
  const std::string marker = path + ".recovery";
  const uint8_t recovering = 1;
  if (!Storage.exists(marker.c_str()) && !writeFileInPlace("GSTATS", marker.c_str(), &recovering, 1)) return false;
  // Keep the original bytes, including unreadable slots, rather than turning a
  // failed recovery write into permanent loss. These are cold, user-requested
  // path allocations, independent of book size.
  for (const char* suffix : {"", ".bak", ".tmp", ".article"}) {
    const std::string source = path + suffix;
    if (!Storage.exists(source.c_str()) && confirmedMissingFile(source.c_str(), true)) continue;
    bool archived = false;
    for (unsigned i = 0; i < 16; ++i) {
      const std::string archive = source + ".damaged." + std::to_string(i);
      if (Storage.exists(archive.c_str())) continue;
      archived = Storage.rename(source.c_str(), archive.c_str());
      break;
    }
    if (!archived) {
      LOG_ERR("GSTATS", "Cannot preserve original stats: %s", source.c_str());
      return false;
    }
  }
  if (!saveToFile(restored, path.c_str(), (path + ".bak").c_str())) return false;
  GlobalReadingStats verified;
  if (loadFromFile(path.c_str(), verified, true).result != StatsLoadResult::Ok) {
    LOG_ERR("GSTATS", "Recovered stats could not be read back");
    return false;
  }
  // Two bounded serialized snapshots on this cold recovery path verify every
  // persisted field without depending on struct padding or another allocation.
  std::array<uint8_t, CURRENT_FILE_SIZE> intended{}, actual{};
  serializeStats(restored, intended.data());
  serializeStats(verified, actual.data());
  if (intended != actual) {
    LOG_ERR("GSTATS", "Recovered history differs from the selected backup");
    return false;
  }
  if (!Storage.remove(marker.c_str())) {
    LOG_ERR("GSTATS", "Cannot complete history recovery marker");
    return false;
  }
  s_blockDestructiveSave = false;
  s_articleRecoveryPending = false;
  // An open reader must replace its live counters after explicit restoration.
  // resetLocal() and recovery share this revision (defined below).
  ++s_localResetRevision;
  return true;
}

GlobalReadingStats GlobalReadingStats::loadAggregated() { return loadAggregated(load()); }

bool GlobalReadingStats::hasSyncedStats() {
  FsFile dir = Storage.open(SYNCED_STATS_DIR);
  if (!dir) return false;

  const bool exists = dir.isDirectory();
  dir.close();
  return exists;
}

GlobalReadingStats GlobalReadingStats::loadAggregated(const GlobalReadingStats& localStats) {
  GlobalReadingStats stats = localStats;
  FsFile dir = Storage.open(SYNCED_STATS_DIR);
  if (!dir) return stats;

  if (!dir.isDirectory()) {
    dir.close();
    return stats;
  }

  char name[128];
  const std::string localFileName = localSyncedStatsFileName();
  uint16_t loadedCount = 0;
  uint16_t skippedCount = 0;
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));

    // Older firmware or manual copies may leave this device's own file here.
    // Skip it because local stats are already included from global_stats.bin.
    if (!isDirectory && nameLen > 0 && (localFileName.empty() || strcmp(name, localFileName.c_str()) != 0)) {
      GlobalReadingStats syncedStats;
      const StatsLoadOutcome outcome = loadFromOpenFile(file, syncedStats);
      if (outcome.result == StatsLoadResult::Ok) {
        addStats(stats, syncedStats);
        loadedCount++;
      } else if (outcome.result == StatsLoadResult::NewerFormat) {
        skippedCount++;
        LOG_DBG("GSTATS", "Skipping newer-format synced stats file: %s (v%u, %u bytes)", name, outcome.version,
                static_cast<unsigned>(outcome.fileSize));
      } else {
        skippedCount++;
        LOG_DBG("GSTATS", "Skipping invalid synced stats file: %s", name);
      }
    }

    file.close();
  }
  dir.close();

  if (loadedCount > 0 || skippedCount > 0) {
    LOG_DBG("GSTATS", "Aggregated %u synced stats file(s), skipped %u", static_cast<unsigned>(loadedCount),
            static_cast<unsigned>(skippedCount));
  }
  return stats;
}

namespace {
bool countedHere(const BookReadingStats& book, const GlobalReadingStats& global) {
  if (!book.isCompleted || !book.completionCredited) return false;
  return book.legacyCompletion ? global.unassignedLegacyBooks > 0
                               : book.completionCounterId.bytes == global.counterId.bytes;
}

void reclassify(BookReadingStats& book, GlobalReadingStats& global, const bool desired) {
  const bool counted = countedHere(book, global);
  uint32_t& source = book.isArticle ? global.completedArticles : global.completedBooks;
  book.isArticle = desired;
  if (book.legacyCompletion) {
    book.legacyCompletion = false;
    book.completionCredited = counted;
    if (counted) {
      book.completionCounterId = global.counterId;
      --global.unassignedLegacyBooks;
    }
  }
  if (!counted || source == 0) return;
  --source;
  uint32_t& destination = desired ? global.completedArticles : global.completedBooks;
  destination = addSaturated(destination, 1);
}
}  // namespace

bool GlobalReadingStats::setCompleted(BookReadingStats& book, const bool completed, const bool track) {
  if (book.isCompleted == completed) return false;
  uint32_t& count = book.isArticle ? completedArticles : completedBooks;
  const uint32_t before = count;
  if (book.isArticle && (before || (completed && track))) articleMarked = true;
  if (completed && track) count = addSaturated(count, 1);
  if (!completed && countedHere(book, *this) && count > 0) {
    --count;
    if (book.legacyCompletion) --unassignedLegacyBooks;
  }
  book.legacyCompletion = false;
  book.isCompleted = completed;
  book.completionCredited = completed && track;
  book.completionCounterId = book.completionCredited ? counterId : StatsCounterId{};
  return count != before;
}

namespace {
struct CompletionCounts {
  uint32_t books;
  uint32_t articles;
  uint32_t legacy;

  static CompletionCounts from(const GlobalReadingStats& g) {
    return {g.completedBooks, g.completedArticles, g.unassignedLegacyBooks};
  }
  bool matches(const GlobalReadingStats& g) const {
    return books == g.completedBooks && articles == g.completedArticles && legacy == g.unassignedLegacyBooks;
  }
  void apply(GlobalReadingStats& g) const {
    g.completedBooks = books;
    g.completedArticles = articles;
    g.unassignedLegacyBooks = legacy;
  }
};
static_assert(sizeof(CompletionCounts) == 12);

struct ArticleTransfer {
  StatsCounterId globalId;
  StatsCounterId bookId;
  CompletionCounts before;
  CompletionCounts after;
  uint8_t flags;
  static constexpr uint8_t TO_ARTICLE = 1;
  static constexpr uint8_t PREVIOUSLY_MARKED = 2;

  bool marked(bool committed) const { return (flags & PREVIOUSLY_MARKED) || (committed && (flags & TO_ARTICLE)); }

  bool publish(const std::string& path, const std::string& cachePath) const {
    SyncFiles::Writer writer(path.c_str());
    return cachePath.size() <= 64 && writer.open() && writer.pod(uint8_t{2}) && writer.pod(globalId.bytes) &&
           writer.pod(bookId.bytes) && writer.pod(before) && writer.pod(after) && writer.pod(flags) &&
           writer.string16(cachePath) && writer.commit();
  }
  bool read(const std::string& path, std::string& cachePath) {
    SyncFiles::Reader reader(path.c_str());
    uint8_t version = 0;
    const bool valid =
        reader.open() == SyncFiles::Reader::Source::Main && reader.pod(version) && (version == 1 || version == 2) &&
        reader.pod(globalId.bytes) && reader.pod(bookId.bytes) && reader.pod(before) && reader.pod(after) &&
        reader.pod(flags) && flags <= (version == 1 ? TO_ARTICLE : (TO_ARTICLE | PREVIOUSLY_MARKED)) &&
        reader.string16(cachePath, 64) && reader.atEnd() && cachePath.rfind("/.crosspoint/epub_", 0) == 0 &&
        cachePath.find('/', 13) == std::string::npos && cachePath.find('\0') == std::string::npos;
    if (version == 1 && before.articles > 0) flags |= PREVIOUSLY_MARKED;
    return valid;
  }
};

// An unreadable slot could hold newer totals. Decode each successfully read
// snapshot directly, retaining the newest valid one without reopening its file.
bool readGlobalForArticleEdit(GlobalReadingStats& stats, bool* newHistory = nullptr) {
  if (newHistory) *newHistory = false;
  const auto path = StatsStorage::localPath(GLOBAL_STATS_PATH);
  if (s_blockDestructiveSave) return false;
  if (path.empty() || Storage.exists((path + ".recovery").c_str())) {
    s_blockDestructiveSave = true;
    return false;
  }
  progress_record::Slot best;
  bool present = false;
  uint8_t data[two_slot::MAX_FILE_BYTES];
  for (const auto& candidate : {path, path + ".bak"}) {
    const auto slot = two_slot::readSlot("GSTATS", candidate.c_str(), GLOBAL_STATS_FILE_SIZE, data,
                                         {GLOBAL_STATS_FILE_SIZE_V4, GLOBAL_STATS_FILE_SIZE_V3});
    if (slot.ioError) return false;
    if (!slot.present) {
      if (!confirmedMissingFile(candidate.c_str())) return false;
      continue;
    }
    present = true;
    if (!slot.size) continue;
    const auto format = classifyGlobalPayload(data[0], slot.size);
    if (format == StatsLoadResult::NewerFormat) {
      s_blockDestructiveSave = true;
      LOG_ERR("GSTATS", "Cannot classify title with newer global history: %s", candidate.c_str());
      return false;
    }
    if (format == StatsLoadResult::Ok && progress_record::newest(best, slot.slot) == progress_record::BACKUP) {
      decodeGlobalPayload(data, slot.size, stats);
      best = slot.slot;
    }
  }
  if (best.kind != progress_record::SlotKind::Invalid) return true;
  if (present) {
    s_blockDestructiveSave = true;
    LOG_ERR("GSTATS", "Cannot classify title with invalid global history");
    return false;
  }
  if (!newHistory) return false;
  stats = {};
  stats.counterId = StatsCounterId::create();
  *newHistory = true;
  return true;
}

bool readExistingTransferBook(const std::string& cachePath, BookReadingStats& book) {
  bool newHistory;
  return BookReadingStats::loadForEdit(cachePath, book, nullptr, newHistory) && !newHistory;
}

bool reconcileArticleTransfer(const std::string& globalPath) {
  const std::string marker = globalPath + ".article";
  if (!Storage.exists(marker.c_str())) return true;  // An unpublished .tmp precedes both snapshot writes.
  ArticleTransfer txn{};
  std::string cachePath;
  BookReadingStats book;
  GlobalReadingStats global;
  if (!txn.read(marker, cachePath) || !readExistingTransferBook(cachePath, book) || !readGlobalForArticleEdit(global) ||
      book.counterId.bytes != txn.bookId.bytes || global.counterId.bytes != txn.globalId.bytes ||
      (!txn.before.matches(global) && !txn.after.matches(global))) {
    LOG_ERR("GSTATS", "Cannot verify pending article transfer; preserving both histories");
    return false;
  }
  // The title is written last. Its durable category decides which absolute counts should survive a restart.
  const bool committed = book.isArticle == bool(txn.flags & ArticleTransfer::TO_ARTICLE);
  const CompletionCounts& counts = committed ? txn.after : txn.before;
  if (!counts.matches(global) || global.hasArticles() != txn.marked(committed)) {
    global.articleMarked = txn.marked(committed);
    counts.apply(global);
    if (!saveToFile(global, globalPath.c_str(), (globalPath + ".bak").c_str())) return false;
  }
  return Storage.remove(marker.c_str());
}
}  // namespace

bool GlobalReadingStats::recoverArticleTransfer() {
  const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH, false, true);
  s_articleRecoveryPending = path.empty() || !reconcileArticleTransfer(path);
  if (s_articleRecoveryPending) LOG_ERR("GSTATS", "Article transfer still pending");
  return !s_articleRecoveryPending;
}

bool GlobalReadingStats::toggleArticle(const std::string& cachePath, bool& article, BookReadingStats* live,
                                       const StatsCounterId* liveGlobalId) {
  if (s_articleRecoveryPending && !recoverArticleTransfer()) return false;
  if (cachePath.empty() || (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str()))) {
    LOG_ERR("BSTAT", "Cannot open book cache: %s", cachePath.c_str());
    return false;
  }
  BookReadingStats book;
  bool newBook, newGlobal;
  if (!BookReadingStats::loadForEdit(cachePath, book, live ? &live->counterId : nullptr, newBook)) return false;
  GlobalReadingStats global;
  if (!readGlobalForArticleEdit(global, &newGlobal)) return false;
  const bool desired = !book.isArticle;
  const bool hadArticles = global.hasArticles();
  // The first mark can precede both histories. Publish their original identities and categories
  // before the journal references them, so a reboot can verify either outcome.
  if (newGlobal && desired) {
    if (liveGlobalId) global.counterId = *liveGlobalId;
    if (!global.saveSnapshot()) return false;
  }
  if (newBook && !hadArticles && !book.save(cachePath)) return false;
  const CompletionCounts before = CompletionCounts::from(global);
  reclassify(book, global, desired);
  global.articleMarked = hadArticles || desired;
  const bool updateGlobal = !before.matches(global) || global.articleMarked != hadArticles;
  std::string marker;
  if (updateGlobal) {
    const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH);
    if (path.empty()) return false;
    marker = path + ".article";
    const uint8_t flags =
        (desired ? ArticleTransfer::TO_ARTICLE : 0) | (hadArticles ? ArticleTransfer::PREVIOUSLY_MARKED : 0);
    const ArticleTransfer txn{global.counterId, book.counterId, before, CompletionCounts::from(global), flags};
    if (Storage.exists(marker.c_str()) || !txn.publish(marker, cachePath)) {
      LOG_ERR("GSTATS", "Could not publish article transfer");
      s_articleRecoveryPending = Storage.exists(marker.c_str());
      return false;
    }
    if (!global.saveSnapshot()) {
      // A sync/close error can still leave a complete, checksummed slot.
      if (!readGlobalForArticleEdit(global) || !txn.after.matches(global) || global.hasArticles() != txn.marked(true) ||
          global.counterId.bytes != txn.globalId.bytes) {
        recoverArticleTransfer();
        return false;
      }
    }
  }

  if (!book.save(cachePath) && (!readExistingTransferBook(cachePath, book) || book.isArticle != desired)) {
    if (updateGlobal) recoverArticleTransfer();
    return false;
  }
  if (updateGlobal && !Storage.remove(marker.c_str())) recoverArticleTransfer();
  if (live) {
    live->isArticle = desired;
    live->completionCounterId = book.completionCounterId;
    live->completionCredited = book.completionCredited;
    live->legacyCompletion = book.legacyCompletion;
  }
  article = desired;
  return true;
}

bool GlobalReadingStats::save() const {
  const bool dailySaved = DailyReadingStats::flush();
  if (!dailySaved) LOG_ERR("GSTATS", "Failed to flush daily reading counters");
  // Keep the global snapshot even if the independent daily flush needs retrying.
  return saveSnapshot() && dailySaved;
}

bool GlobalReadingStats::saveSnapshot() const {
  if (recoveryRequired()) {
    LOG_ERR("GSTATS", "Refusing to overwrite protected reading history");
    return false;
  }
  const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH);
  return !path.empty() && saveToFile(*this, path.c_str(), (path + ".bak").c_str());
}

bool GlobalReadingStats::resetLocal() {
  if (recoveryRequired()) {
    LOG_ERR("GSTATS", "Recover unreadable history before resetting counters");
    return false;
  }
  const std::string path = StatsStorage::localPath(GLOBAL_STATS_PATH);
  GlobalReadingStats fresh;
  fresh.counterId = StatsCounterId::create();
  if (path.empty() || !saveToFile(fresh, path.c_str(), (path + ".bak").c_str())) return false;
  s_localResetRevision++;
  return true;
}

uint32_t GlobalReadingStats::localResetRevision() { return s_localResetRevision; }

#ifdef STATS_STORAGE_TEST
void resetGlobalReadingStatsForTest() {
  s_articleRecoveryPending = false;
  s_blockDestructiveSave = false;
  s_localResetRevision = 0;
}
#endif

void GlobalReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
  recordReadingSpanIntoHistory(readingHistoryAnchorDay, readingHistoryBits, localStart, seconds);

  const uint16_t historyLongest = computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits);
  if (historyLongest > longestReadingStreak) {
    longestReadingStreak = historyLongest;
  }
}

uint16_t GlobalReadingStats::currentReadingStreak(const ReadingStatsDate* today) const {
  GlobalReadingStats server;
  if (loadServerStreak(server)) {
    mergeReadingHistory(server.readingHistoryAnchorDay, server.readingHistoryBits, readingHistoryAnchorDay,
                        readingHistoryBits);
    return computeReadingHistoryCurrentStreak(server.readingHistoryAnchorDay, server.readingHistoryBits, today);
  }
  return computeReadingHistoryCurrentStreak(readingHistoryAnchorDay, readingHistoryBits, today);
}

uint16_t GlobalReadingStats::displayLongestReadingStreak() const {
  GlobalReadingStats server;
  if (loadServerStreak(server)) {
    mergeReadingHistory(server.readingHistoryAnchorDay, server.readingHistoryBits, readingHistoryAnchorDay,
                        readingHistoryBits);
    return std::max(std::max(longestReadingStreak, server.longestReadingStreak),
                    computeReadingHistoryLongestStreak(server.readingHistoryAnchorDay, server.readingHistoryBits));
  }
  return std::max(longestReadingStreak,
                  computeReadingHistoryLongestStreak(readingHistoryAnchorDay, readingHistoryBits));
}

bool GlobalReadingStats::saveServerStreak(const GlobalReadingStats& streak) {
  SyncFiles::Writer writer(serverStreakPath().c_str());
  constexpr uint8_t version = 1;
  const bool saved = writer.open() && writer.pod(version) && writer.pod(streak.readingHistoryAnchorDay) &&
                     writer.write(streak.readingHistoryBits.data(), READING_HISTORY_BYTES) &&
                     writer.pod(streak.longestReadingStreak) && writer.commit();
  if (!saved) LOG_ERR("GSTATS", "Cannot save server reading calendar");
  return saved;
}
