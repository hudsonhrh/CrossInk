#include <HalStorage.h>
#include <esp_mac.h>
#include <gtest/gtest.h>

#include "BookReadingStats.h"
#include "DailyReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingSessionJournal.h"
#include "StatsBackup.h"
#include "TwoSlotFile.h"
#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

void resetStatsStorageForTest();
void resetGlobalReadingStatsForTest();
namespace DailyReadingStats {
void resetForTest();
}

namespace {
constexpr const char* CACHE = "/.crosspoint/epub_book";
constexpr const char* LEGACY_BOOK = "/.crosspoint/epub_book/stats_v6.bin";
constexpr const char* ROOT_OWNER = "/.crosspoint/stats_origin.bin";

void switchReader(uint8_t device) {
  testMac[5] = device;
  failMac = false;
  resetStatsStorageForTest();
  resetGlobalReadingStatsForTest();
  DailyReadingStats::resetForTest();
}
void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  FsFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
  ASSERT_EQ(file.write(bytes.data(), bytes.size()), bytes.size());
  ASSERT_TRUE(file.close());
}
void legacyBook(uint32_t seconds) {
  std::vector<uint8_t> bytes(75);
  bytes[0] = 6;
  for (size_t i = 0; i < 4; ++i) bytes[3 + i] = seconds >> (i * 8);
  writeBytes(LEGACY_BOOK, bytes);
}
ReadingSessionJournal::Record session() {
  ReadingSessionJournal::Record r;
  strcpy(r.id, "0123456789abcdef0123456789abcdef");
  strcpy(r.document, "abcdef0123456789abcdef0123456789");
  r.startedAt = 1793491200;
  r.endedAt = r.startedAt + 60;
  r.seconds = 60;
  return r;
}
bool collectSession(void* context, const ReadingSessionJournal::Record& r) {
  static_cast<std::vector<ReadingSessionJournal::Record>*>(context)->push_back(r);
  return true;
}
bool collectDay(void* context, uint32_t, uint32_t seconds) {
  *static_cast<uint32_t*>(context) += seconds;
  return true;
}
}  // namespace
class StatsStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    switchReader(5);
    ASSERT_TRUE(Storage.mkdir(CACHE));
  }
};

TEST_F(StatsStorageTest, CorruptGlobalSlotsCannotBecomeANewSavedHistoryOrBackup) {
  auto history = GlobalReadingStats::load();
  history.totalReadingSeconds = 97000;
  ASSERT_TRUE(history.save());
  history.totalReadingSeconds += 60;
  ASSERT_TRUE(history.save());
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  for (const auto& slot : {path, path + ".bak"}) Storage.files.at(slot)->bytes.back() ^= 1;
  const auto original = Storage.files.at(path)->bytes;
  const auto previous = Storage.files.at(path + ".bak")->bytes;
  auto unreadable = GlobalReadingStats::load();
  EXPECT_TRUE(GlobalReadingStats::recoveryRequired());
  EXPECT_EQ(unreadable.counterId.bytes, StatsCounterId{}.bytes);
  unreadable.totalReadingSeconds = 60;
  EXPECT_FALSE(unreadable.save());
  EXPECT_FALSE(backupGlobalStats(false));
  EXPECT_FALSE(GlobalReadingStats::loadForUpload(unreadable));
  EXPECT_EQ(Storage.files.at(path)->bytes, original);
  EXPECT_EQ(Storage.files.at(path + ".bak")->bytes, previous);
}

TEST_F(StatsStorageTest, ReadOpenFailurePreservesTheExistingHistory) {
  auto history = GlobalReadingStats::load();
  history.totalReadingSeconds = 97000;
  ASSERT_TRUE(history.save());
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  const auto bytes = Storage.files.at(path)->bytes;
  faults.failReadPath = path;
  auto unreadable = GlobalReadingStats::load();
  EXPECT_TRUE(GlobalReadingStats::recoveryRequired());
  faults.failReadPath.clear();
  EXPECT_FALSE(unreadable.save());
  EXPECT_EQ(Storage.files.at(path)->bytes, bytes);
}

TEST_F(StatsStorageTest, FailedDirectoryReadIsNotTreatedAsANewHistory) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  faults.failScan = true;
  auto unknown = GlobalReadingStats::load();
  EXPECT_TRUE(GlobalReadingStats::recoveryRequired());
  EXPECT_EQ(unknown.counterId.bytes, StatsCounterId{}.bytes);
  faults.failScan = false;
  EXPECT_FALSE(unknown.save());
}

TEST_F(StatsStorageTest, DatedBackupHasAChecksumAndRecoversTheOriginalCounterIdentity) {
  auto history = GlobalReadingStats::load();
  history.totalReadingSeconds = 97000;
  ASSERT_TRUE(history.save());
  char name[64];
  ASSERT_TRUE(backupGlobalStats(false, name, sizeof(name)));
  const auto backup = std::string("/.crossink-stats-backup/") + name;
  ASSERT_EQ(Storage.files.at(backup)->bytes.size(), GlobalReadingStats::CURRENT_FILE_SIZE + two_slot::TRAILER_BYTES);
  GlobalReadingStats snapshot;
  ASSERT_TRUE(GlobalReadingStats::readBackup(backup.c_str(), snapshot));
  EXPECT_EQ(snapshot.counterId.bytes, history.counterId.bytes);
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  Storage.files.at(path)->bytes.back() ^= 1;
  const auto damaged = Storage.files.at(path)->bytes;
  GlobalReadingStats::load();
  ASSERT_TRUE(GlobalReadingStats::recoveryRequired());
  ASSERT_TRUE(GlobalReadingStats::restoreBackup(backup.c_str()));
  EXPECT_FALSE(GlobalReadingStats::recoveryRequired());
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 97000u);
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, history.counterId.bytes);
  EXPECT_EQ(Storage.files.at(path + ".damaged.0")->bytes, damaged);
  EXPECT_EQ(GlobalReadingStats::localResetRevision(), 1u);
}

TEST_F(StatsStorageTest, InterruptedBackupUpdatesRetainThePreviousCompleteSnapshot) {
  auto history = GlobalReadingStats::load();
  history.totalReadingSeconds = 97000;
  ASSERT_TRUE(history.save());
  char name[64];
  ASSERT_TRUE(backupGlobalStats(false, name, sizeof(name)));
  const auto backup = std::string("/.crossink-stats-backup/") + name;
  history.totalReadingSeconds += 60;
  ASSERT_TRUE(history.save());
  for (int cut = 0; cut < static_cast<int>(GlobalReadingStats::CURRENT_FILE_SIZE + two_slot::TRAILER_BYTES); ++cut) {
    faults.bytesUntilFailure = cut;
    EXPECT_FALSE(backupGlobalStats(false)) << "byte " << cut;
    faults.bytesUntilFailure = -1;
    GlobalReadingStats snapshot;
    ASSERT_TRUE(GlobalReadingStats::readBackup(backup.c_str(), snapshot)) << "byte " << cut;
    EXPECT_EQ(snapshot.totalReadingSeconds, 97000u) << "byte " << cut;
    EXPECT_EQ(snapshot.counterId.bytes, history.counterId.bytes);
  }
  ASSERT_TRUE(backupGlobalStats(false));
  GlobalReadingStats snapshot;
  ASSERT_TRUE(GlobalReadingStats::readBackup(backup.c_str(), snapshot));
  EXPECT_EQ(snapshot.totalReadingSeconds, 97060u);
}

TEST_F(StatsStorageTest, InterruptedExplicitRestoreCannotAppearAsFirstUseAfterRestart) {
  auto history = GlobalReadingStats::load();
  history.totalReadingSeconds = 97000;
  ASSERT_TRUE(history.save());
  char name[64];
  ASSERT_TRUE(backupGlobalStats(false, name, sizeof(name)));
  const auto backup = std::string("/.crossink-stats-backup/") + name;
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  Storage.files.at(path)->bytes.back() ^= 1;
  GlobalReadingStats::load();
  faults.failRenameFrom = path;
  EXPECT_FALSE(GlobalReadingStats::restoreBackup(backup.c_str()));
  switchReader(5);  // boot after the failed recovery
  auto unreadable = GlobalReadingStats::load();
  EXPECT_TRUE(GlobalReadingStats::recoveryRequired());
  EXPECT_FALSE(unreadable.save());
  ASSERT_TRUE(GlobalReadingStats::restoreBackup(backup.c_str()));
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 97000u);
}

TEST_F(StatsStorageTest, LegacyRawBackupsRemainReadableForExplicitRecovery) {
  std::vector<uint8_t> raw(159);
  raw[0] = 3;
  progress_record::writeU32(raw.data() + 5, 97000);
  const char* path = "/.crossink-stats-backup/stats_2026-09-01.bin";
  ASSERT_TRUE(Storage.mkdir("/.crossink-stats-backup"));
  writeBytes(path, raw);
  GlobalReadingStats snapshot;
  ASSERT_TRUE(GlobalReadingStats::readBackup(path, snapshot));
  EXPECT_EQ(snapshot.totalReadingSeconds, 97000u);
  EXPECT_EQ(snapshot.counterId.bytes, StatsCounterId{}.bytes);
  char name[64];
  ASSERT_TRUE(findLatestStatsBackup(name, sizeof(name)));
  EXPECT_STREQ(name, "stats_2026-09-01.bin");
}

TEST_F(StatsStorageTest, RecoverySelectionOrdersClocklessSequenceNumbersNumerically) {
  ASSERT_TRUE(Storage.mkdir("/.crossink-stats-backup"));
  std::vector<uint8_t> raw(159);
  raw[0] = 3;
  writeBytes("/.crossink-stats-backup/stats_backup_999.bin", raw);
  writeBytes("/.crossink-stats-backup/stats_backup_1000.bin", raw);
  char name[64];
  ASSERT_TRUE(findLatestStatsBackup(name, sizeof(name)));
  EXPECT_STREQ(name, "stats_backup_1000.bin");
  writeBytes("/.crossink-stats-backup/stats_2026-11-13.bin", raw);
  ASSERT_TRUE(findLatestStatsBackup(name, sizeof(name)));
  EXPECT_STREQ(name, "stats_2026-11-13.bin");
}

TEST_F(StatsStorageTest, RecoverySelectionIncludesASoleSurvivingBackupSlot) {
  ASSERT_TRUE(Storage.mkdir("/.crossink-stats-backup"));
  std::vector<uint8_t> raw(159);
  raw[0] = 3;
  writeBytes("/.crossink-stats-backup/stats_2026-11-13.bin.bak", raw);
  char name[64];
  ASSERT_TRUE(findLatestStatsBackup(name, sizeof(name)));
  EXPECT_STREQ(name, "stats_2026-11-13.bin");
}

TEST_F(StatsStorageTest, PowerLossAfterArchivingHistoryKeepsRecoveryProtectionAcrossBoots) {
  auto history = GlobalReadingStats::load();
  history.totalReadingSeconds = 97000;
  ASSERT_TRUE(history.save());
  char name[64];
  ASSERT_TRUE(backupGlobalStats(false, name, sizeof(name)));
  const auto backup = std::string("/.crossink-stats-backup/") + name;
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  Storage.files.at(path)->bytes.back() ^= 1;
  GlobalReadingStats::load();
  faults.bytesUntilFailure = 1;  // marker persists, restored stats cannot be written
  EXPECT_FALSE(GlobalReadingStats::restoreBackup(backup.c_str()));
  EXPECT_TRUE(Storage.exists((path + ".damaged.0").c_str()));
  faults.bytesUntilFailure = -1;
  switchReader(5);
  auto interrupted = GlobalReadingStats::load();
  EXPECT_TRUE(GlobalReadingStats::recoveryRequired());
  EXPECT_FALSE(interrupted.save());
  ASSERT_TRUE(GlobalReadingStats::restoreBackup(backup.c_str()));
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, history.counterId.bytes);
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 97000u);
}

TEST_F(StatsStorageTest, CopiedBookStartsSeparateCountersAndSourceResumes) {
  BookReadingStats a;
  a.totalReadingSeconds = 3600;
  a.isCompleted = true;
  ASSERT_TRUE(a.save(CACHE));
  switchReader(6);
  BookReadingStats b;
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE, b));
  EXPECT_EQ(b.totalReadingSeconds, 0u);
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  b.totalReadingSeconds = 60;
  ASSERT_TRUE(b.save(CACHE));
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 60u);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
}
TEST_F(StatsStorageTest, BookFolderCopiedWithoutRootMarkerRetainsItsOrigin) {
  BookReadingStats a;
  a.totalReadingSeconds = 3600;
  ASSERT_TRUE(a.save(CACHE));
  ASSERT_TRUE(Storage.remove(ROOT_OWNER));
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  a.totalReadingSeconds = 60;
  ASSERT_TRUE(a.save(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, LegacyStatsAreClaimedOnceAndNotReclaimedOnAnotherReader) {
  legacyBook(3600);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
  EXPECT_FALSE(Storage.exists(LEGACY_BOOK));
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, RootClaimProtectsPreviouslyUnopenedLegacyBooks) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  legacyBook(3600);
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, ReadOnlyCompletionDoesNotWriteOrMigrate) {
  legacyBook(3600);
  const auto writes = faults.writes;
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  EXPECT_FALSE(Storage.exists(ROOT_OWNER));
}
TEST_F(StatsStorageTest, CompletionSkipsAbsentStatsAndFindsOlderFilesOnTheirOwningReader) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  writeBytes(std::string(CACHE) + "/progress.bin", {1});
  faults = {};
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_LE(faults.existsChecks, 7u);
  EXPECT_EQ(faults.readOpens, 1u);  // Card origin for the current format.
  EXPECT_EQ(faults.directoryOpens, 1u);
  EXPECT_EQ(faults.directorySteps, 2u);
  legacyBook(3600);
  Storage.files.at(LEGACY_BOOK)->bytes[11] = 1;
  faults = {};
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_LE(faults.existsChecks, 16u);
  EXPECT_EQ(faults.readOpens, 3u);  // Two origin checks and the legacy payload.
  EXPECT_EQ(faults.directoryOpens, 1u);
  EXPECT_EQ(faults.directorySteps, 3u);
  EXPECT_EQ(faults.writes, 0u);
  switchReader(6);
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}

TEST_F(StatsStorageTest, CurrentCompletionUsesNewestSlotWithoutWalkingImageFiles) {
  for (int i = 0; i < 40; ++i) writeBytes(std::string(CACHE) + "/image_" + std::to_string(i), {0});
  BookReadingStats book;
  ASSERT_TRUE(book.save(CACHE));
  book.isCompleted = true;
  ASSERT_TRUE(book.save(CACHE));
  faults = {};
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.directoryOpens, 0u);
  EXPECT_EQ(faults.directorySteps, 0u);
  EXPECT_LE(faults.existsChecks, 7u);
  EXPECT_LE(faults.readOpens, 3u);
  EXPECT_EQ(faults.writes, 0u);
}

TEST_F(StatsStorageTest, CompletionFallsBackWhenDirectoryWalkIsIncomplete) {
  legacyBook(3600);
  Storage.files.at(LEGACY_BOOK)->bytes[11] = 1;
  faults.failScan = true;
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  faults.failScan = false;
  for (int i = 0; i < 40; ++i) writeBytes(std::string(CACHE) + "/image_" + std::to_string(i), {0});
  faults = {};
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_LE(faults.directorySteps, 32u);
  EXPECT_EQ(faults.writes, 0u);
}

TEST_F(StatsStorageTest, CompletionUsesLegacyRecoveryWithoutMixingReadersOrWriting) {
  legacyBook(3600);
  Storage.files.at(LEGACY_BOOK)->bytes[11] = 1;
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  ASSERT_TRUE(Storage.rename(LEGACY_BOOK, (std::string(LEGACY_BOOK) + ".bak").c_str()));
  writeBytes(std::string(LEGACY_BOOK) + ".tmp", {6});
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  switchReader(6);
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  BookReadingStats own;
  ASSERT_TRUE(own.save(CACHE));
  switchReader(5);
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_TRUE(Storage.exists((std::string(LEGACY_BOOK) + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists(LEGACY_BOOK));
}

TEST_F(StatsStorageTest, PendingSessionsAreNotUploadedByTheReceivingReader) {
  const auto record = session();
  ASSERT_TRUE(ReadingSessionJournal::save(record));
  switchReader(6);
  std::vector<ReadingSessionJournal::Record> uploaded;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  EXPECT_TRUE(uploaded.empty());
  // Even the same session ID remains scoped to the original saved record.
  switchReader(5);
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_STREQ(uploaded[0].id, record.id);
  EXPECT_EQ(uploaded[0].seconds, 60u);
}
TEST_F(StatsStorageTest, BothReadersCanKeepIndependentPendingSessions) {
  ASSERT_TRUE(ReadingSessionJournal::save(session()));
  switchReader(6);
  auto b = session();
  b.seconds = 30;
  ASSERT_TRUE(ReadingSessionJournal::save(b));
  std::vector<ReadingSessionJournal::Record> uploaded;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_EQ(uploaded[0].seconds, 30u);
  uploaded.clear();
  switchReader(5);
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_EQ(uploaded[0].seconds, 60u);
}
TEST_F(StatsStorageTest, DailyHistoryAndAcknowledgementsAreDeviceLocal) {
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 3600));
  ASSERT_TRUE(DailyReadingStats::flush());
  switchReader(6);
  uint32_t uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 0u);
  ASSERT_TRUE(DailyReadingStats::record(day, 60));
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 60u);
  switchReader(5);
  uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 3600u);
  uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 0u);
}
TEST_F(StatsStorageTest, GlobalMigrationRetainsBothSlotsAndInterruptedSaves) {
  for (const char* suffix : {"", ".bak", ".tmp"})
    writeBytes(std::string("/.crosspoint/global_stats.bin") + suffix, {1, 2, 3});
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  ASSERT_FALSE(path.empty());
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    EXPECT_TRUE(Storage.exists((path + suffix).c_str()));
    EXPECT_FALSE(Storage.exists((std::string("/.crosspoint/global_stats.bin") + suffix).c_str()));
  }
  switchReader(6);
  EXPECT_NE(StatsStorage::localPath("/.crosspoint/global_stats.bin"), path);
}
TEST_F(StatsStorageTest, InterruptedMigrationCannotOverwriteRecoveredHistory) {
  legacyBook(3600);
  faults.failRenameFrom = LEGACY_BOOK;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  BookReadingStats empty;
  EXPECT_FALSE(empty.save(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, FailedClaimDoesNotMoveOrOverwriteHistory) {
  legacyBook(3600);
  faults.failOpenWrite = true;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  faults.failOpenWrite = false;
  EXPECT_FALSE(BookReadingStats{}.save(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, CorruptOwnershipFailsClosed) {
  legacyBook(3600);
  writeBytes(ROOT_OWNER, {1, 2, 3});
  EXPECT_TRUE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  EXPECT_FALSE(BookReadingStats{}.save(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}
TEST_F(StatsStorageTest, IdentityFailureNeverUsesGenericStats) {
  legacyBook(3600);
  failMac = true;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_FALSE(BookReadingStats{}.save(CACHE));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}
TEST_F(StatsStorageTest, ResetBookStatsPreservesOtherReadersHistory) {
  BookReadingStats a;
  a.totalReadingSeconds = 3600;
  ASSERT_TRUE(a.save(CACHE));
  switchReader(6);
  a.totalReadingSeconds = 60;
  ASSERT_TRUE(a.save(CACHE));
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, LegacyDirectoryMovePreservesPendingSessions) {
  const auto owned = StatsStorage::localPath("/.crosspoint/reading_sessions", true);
  ASSERT_TRUE(ReadingSessionJournal::save(session()));
  ASSERT_TRUE(Storage.rename(owned.c_str(), "/.crosspoint/reading_sessions"));
  std::vector<ReadingSessionJournal::Record> uploaded;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &uploaded));
  ASSERT_EQ(uploaded.size(), 1u);
  EXPECT_EQ(uploaded[0].seconds, 60u);
  EXPECT_FALSE(Storage.exists("/.crosspoint/reading_sessions"));
}

TEST_F(StatsStorageTest, LifetimeTotalsUseOnlyTheCurrentReaderAndResetIsScoped) {
  GlobalReadingStats a;
  a.totalReadingSeconds = 3600;
  a.totalSessions = 10;
  ASSERT_TRUE(a.save());
  switchReader(6);
  GlobalReadingStats b;
  EXPECT_FALSE(GlobalReadingStats::loadForUpload(b));
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 0u);
  b.totalReadingSeconds = 60;
  b.totalSessions = 1;
  ASSERT_TRUE(b.save());
  ASSERT_TRUE(GlobalReadingStats::loadForUpload(b));
  EXPECT_EQ(b.totalReadingSeconds, 60u);
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 0u);
  switchReader(5);
  ASSERT_TRUE(GlobalReadingStats::loadForUpload(a));
  EXPECT_EQ(a.totalReadingSeconds, 3600u);
  EXPECT_EQ(a.totalSessions, 10u);
}

TEST_F(StatsStorageTest, ReadOnlyCompletionDoesNotPromoteAnInterruptedOriginClaim) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  ASSERT_TRUE(Storage.rename(ROOT_OWNER, "/.crosspoint/stats_origin.bin.tmp"));
  legacyBook(3600);
  const auto writes = faults.writes;
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_FALSE(Storage.exists(ROOT_OWNER));
  EXPECT_TRUE(Storage.exists("/.crosspoint/stats_origin.bin.tmp"));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
}

TEST_F(StatsStorageTest, TornInitialRootClaimRetriesWithoutLosingLegacyStats) {
  legacyBook(3600);
  writeBytes("/.crosspoint/stats_origin.bin.tmp", {1, 2, 3});
  const auto writes = faults.writes;
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_EQ(faults.writes, writes);
  EXPECT_TRUE(Storage.exists("/.crosspoint/stats_origin.bin.tmp"));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
  EXPECT_TRUE(Storage.exists(ROOT_OWNER));
  EXPECT_FALSE(Storage.exists("/.crosspoint/stats_origin.bin.tmp"));
}
TEST_F(StatsStorageTest, TornInitialBookClaimRetriesUsingTheCardOrigin) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  legacyBook(3600);
  writeBytes(std::string(CACHE) + "/stats_origin.bin.tmp", {1, 2, 3});
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, UnreadableInitialClaimIsRetainedRatherThanTreatedAsTorn) {
  legacyBook(3600);
  const std::string temporary = "/.crosspoint/stats_origin.bin.tmp";
  writeBytes(temporary, {1, 2, 3});
  faults.failReadPath = temporary;
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(temporary.c_str()));
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  faults.failReadPath.clear();
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}

TEST_F(StatsStorageTest, EmptyInitialClaimCanRetryAfterPowerLossBeforeTheFirstByte) {
  legacyBook(3600);
  writeBytes("/.crosspoint/stats_origin.bin.tmp", {});
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 3600u);
}
TEST_F(StatsStorageTest, UnsupportedCompleteTemporaryClaimIsNeverReplaced) {
  ASSERT_FALSE(StatsStorage::localPath("/.crosspoint/global_stats.bin").empty());
  auto bytes = Storage.files.at(ROOT_OWNER)->bytes;
  bytes[0] = 2;
  const uint32_t crc = SyncFiles::crc(bytes.data(), 33, 0);
  for (size_t i = 0; i < 4; ++i) bytes[33 + i] = crc >> (8 * i);
  ASSERT_TRUE(Storage.remove(ROOT_OWNER));
  writeBytes("/.crosspoint/stats_origin.bin.tmp", bytes);
  legacyBook(3600);
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
  EXPECT_EQ(Storage.files.at("/.crosspoint/stats_origin.bin.tmp")->bytes, bytes);
  EXPECT_FALSE(Storage.exists(ROOT_OWNER));
}

TEST_F(StatsStorageTest, LegacyBookRecreatedByOlderFirmwareKeepsLocalHistoryWritable) {
  BookReadingStats saved;
  saved.totalReadingSeconds = 565;
  saved.startDate = {2026, 11, 5};
  saved.avgSecondsPerForwardPage = 30;
  saved.paceSampleCount = 18;
  ASSERT_TRUE(saved.save(CACHE));
  legacyBook(95);
  const auto legacy = Storage.files.at(LEGACY_BOOK)->bytes;
  switchReader(5);
  auto loaded = BookReadingStats::load(CACHE);
  ASSERT_EQ(loaded.totalReadingSeconds, 565u);
  EXPECT_EQ(loaded.startDate.year, 2026);
  EXPECT_EQ(loaded.paceSampleCount, 18u);
  loaded.totalReadingSeconds += 120;
  ASSERT_TRUE(loaded.save(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 685u);
  EXPECT_EQ(Storage.files.at(LEGACY_BOOK)->bytes, legacy);
}

TEST_F(StatsStorageTest, LegacyGlobalCollisionDoesNotDisableBookStats) {
  GlobalReadingStats global;
  global.totalReadingSeconds = 3600;
  ASSERT_TRUE(global.save());
  BookReadingStats book;
  book.totalReadingSeconds = 565;
  ASSERT_TRUE(book.save(CACHE));
  writeBytes("/.crosspoint/global_stats.bin", {1, 2, 3});
  switchReader(5);
  ASSERT_TRUE(GlobalReadingStats::loadForUpload(global));
  EXPECT_EQ(global.totalReadingSeconds, 3600u);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 565u);
  global.totalReadingSeconds += 60;
  EXPECT_TRUE(global.save());
}

TEST_F(StatsStorageTest, ExistingLocalRecoverySlotKeepsLegacySlotsSeparate) {
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    Storage.reset();
    switchReader(5);
    const auto local = StatsStorage::localPath(LEGACY_BOOK);
    writeBytes(local + suffix, {6, 7, 8});
    legacyBook(95);
    writeBytes(std::string(LEGACY_BOOK) + ".bak", {1, 2, 3});
    writeBytes(std::string(LEGACY_BOOK) + ".tmp", {4, 5, 6});
    EXPECT_EQ(StatsStorage::localPath(LEGACY_BOOK), local);
    EXPECT_EQ(StatsStorage::localPath(LEGACY_BOOK, false, true), local);
    EXPECT_TRUE(Storage.exists(LEGACY_BOOK));
    EXPECT_TRUE(Storage.exists((std::string(LEGACY_BOOK) + ".bak").c_str()));
    EXPECT_TRUE(Storage.exists((std::string(LEGACY_BOOK) + ".tmp").c_str()));
  }
}

TEST_F(StatsStorageTest, RecreatedLegacyDailyAndSessionDirectoriesDoNotBlockLocalWrites) {
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 3600));
  ASSERT_TRUE(DailyReadingStats::flush());
  ASSERT_TRUE(ReadingSessionJournal::save(session()));
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/daily_reading"));
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/reading_sessions"));
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::record(day, 60));
  ASSERT_TRUE(DailyReadingStats::flush());
  uint32_t uploaded = 0;
  ASSERT_TRUE(DailyReadingStats::uploadPending(collectDay, &uploaded));
  EXPECT_EQ(uploaded, 3660u);
  auto record = session();
  record.seconds = 120;
  record.endedAt = record.startedAt + record.seconds;
  ASSERT_TRUE(ReadingSessionJournal::save(record));
  std::vector<ReadingSessionJournal::Record> sessions;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collectSession, &sessions));
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(sessions[0].seconds, 120u);
  EXPECT_TRUE(Storage.exists("/.crosspoint/daily_reading"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/reading_sessions"));
}

TEST_F(StatsStorageTest, InterruptedMigrationStillRecoversTheNewerLegacyBackup) {
  BookReadingStats book;
  book.totalReadingSeconds = 100;
  ASSERT_TRUE(book.save(CACHE));
  book.totalReadingSeconds = 200;
  book.isCompleted = true;
  ASSERT_TRUE(book.save(CACHE));
  const std::string generic = std::string(CACHE) + "/stats_v8.bin";
  const auto local = StatsStorage::localPath(generic);
  ASSERT_TRUE(Storage.rename(local.c_str(), generic.c_str()));
  ASSERT_TRUE(Storage.rename((local + ".bak").c_str(), (generic + ".bak").c_str()));
  faults.failRenameFrom = generic + ".bak";
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(local.c_str()));
  EXPECT_TRUE(Storage.exists((generic + ".bak").c_str()));
  switchReader(6);
  EXPECT_FALSE(BookReadingStats::peekCompleted(CACHE));
  switchReader(5);
  EXPECT_TRUE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_TRUE(Storage.exists((generic + ".bak").c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 200u);
  EXPECT_FALSE(Storage.exists((generic + ".bak").c_str()));
}

TEST_F(StatsStorageTest, ResetCannotRevivePreservedLegacyStatsOrEraseAnotherReader) {
  BookReadingStats book;
  book.totalReadingSeconds = 565;
  ASSERT_TRUE(book.save(CACHE));
  const auto generic = std::string(CACHE) + "/stats_v8.bin";
  auto recreated = std::vector<uint8_t>(107);
  recreated[0] = 8;
  recreated[3] = 95;
  writeBytes(generic, recreated);
  switchReader(6);
  book.totalReadingSeconds = 60;
  ASSERT_TRUE(book.save(CACHE));
  switchReader(5);
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 0u);
  EXPECT_TRUE(Storage.exists(generic.c_str()));
  switchReader(6);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 60u);
}

TEST_F(StatsStorageTest, FailedResetWriteKeepsNewestHistoryWithLegacyCollision) {
  BookReadingStats book;
  book.totalReadingSeconds = 565;
  ASSERT_TRUE(book.save(CACHE));
  legacyBook(95);
  faults.bytesUntilFailure = 0;
  EXPECT_FALSE(BookReadingStats::remove(CACHE));
  faults.bytesUntilFailure = -1;
  switchReader(5);
  EXPECT_EQ(BookReadingStats::load(CACHE).totalReadingSeconds, 565u);
}

TEST_F(StatsStorageTest, FreshCountersKeepTheirIdsAcrossCheckpointsAndRestoredCards) {
  auto global = GlobalReadingStats::load();
  auto book = BookReadingStats::load(CACHE);
  EXPECT_NE(global.counterId.bytes, StatsCounterId{}.bytes);
  EXPECT_NE(book.counterId.bytes, global.counterId.bytes);
  global.totalReadingSeconds = book.totalReadingSeconds = 3600;
  ASSERT_TRUE(global.save());
  ASSERT_TRUE(book.save(CACHE));
  std::map<std::string, HostFileData> backup;
  for (const auto& [path, data] : Storage.files) backup.emplace(path, *data);
  const auto globalId = global.counterId.bytes;
  const auto bookId = book.counterId.bytes;
  switchReader(5);  // reboot
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, globalId);
  EXPECT_EQ(BookReadingStats::load(CACHE).counterId.bytes, bookId);
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  EXPECT_NE(GlobalReadingStats::load().counterId.bytes, globalId);
  EXPECT_NE(BookReadingStats::load(CACHE).counterId.bytes, bookId);
  Storage.reset();
  for (const auto& [path, data] : backup) Storage.files.emplace(path, std::make_shared<HostFileData>(data));
  switchReader(5);
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, globalId);
  EXPECT_EQ(BookReadingStats::load(CACHE).counterId.bytes, bookId);
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 3600u);
}

TEST_F(StatsStorageTest, ResetFailureRetainsThePreviousCounterIdentity) {
  auto global = GlobalReadingStats::load();
  global.totalReadingSeconds = 3600;
  ASSERT_TRUE(global.save());
  faults.bytesUntilFailure = 0;
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  faults.bytesUntilFailure = -1;
  const auto recovered = GlobalReadingStats::load();
  EXPECT_EQ(recovered.totalReadingSeconds, 3600u);
  EXPECT_EQ(recovered.counterId.bytes, global.counterId.bytes);
}

TEST_F(StatsStorageTest, UpgradingVersionThreeSlotsRetainsNewestCountersAndLegacyIdentity) {
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  for (uint32_t seq = 1; seq <= 2; ++seq) {
    std::vector<uint8_t> bytes(167);
    bytes[0] = 3;
    progress_record::writeU32(bytes.data() + 5, seq * 3600);
    progress_record::writeU32(bytes.data() + 159, seq);
    progress_record::writeU32(bytes.data() + 163, progress_record::crc32(bytes.data(), 163));
    writeBytes(path + (seq == 1 ? "" : ".bak"), bytes);
  }
  auto stats = GlobalReadingStats::load();
  ASSERT_EQ(stats.totalReadingSeconds, 7200u);
  EXPECT_EQ(stats.counterId.bytes, StatsCounterId{}.bytes);
  stats.totalReadingSeconds += 60;
  ASSERT_TRUE(stats.save());
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 7260u);
  EXPECT_EQ(GlobalReadingStats::load().counterId.bytes, StatsCounterId{}.bytes);
  // A failed new-format save must still recover the previous old-format slot.
  faults.bytesUntilFailure = 20;
  stats.totalReadingSeconds += 60;
  EXPECT_FALSE(stats.save());
  faults.bytesUntilFailure = -1;
  EXPECT_EQ(GlobalReadingStats::load().totalReadingSeconds, 7260u);
}

TEST_F(StatsStorageTest, DailyIdentitySurvivesGlobalResetAndChangesForAFreshCard) {
  StatsCounterId before, after;
  ASSERT_TRUE(DailyReadingStats::counterId(before));
  EXPECT_NE(before.bytes, StatsCounterId{}.bytes);
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 600));
  ASSERT_TRUE(DailyReadingStats::flush());
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(after));
  EXPECT_EQ(after.bytes, before.bytes);
  Storage.reset();  // fresh SD on the same hardware
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(after));
  EXPECT_NE(after.bytes, before.bytes);
}

TEST_F(StatsStorageTest, ExistingDailyHistoryAdoptsLegacyIdentityAndNeverChangesIt) {
  const uint32_t day = readingStatsDayIndex({2026, 11, 1});
  ASSERT_TRUE(DailyReadingStats::record(day, 600));
  ASSERT_TRUE(DailyReadingStats::flush());
  const auto path = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true) + "/counter_id.bin";
  ASSERT_TRUE(Storage.remove(path.c_str()));  // simulate a pre-ID daily directory
  switchReader(5);
  StatsCounterId id;
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  EXPECT_EQ(id.bytes, StatsCounterId{}.bytes);
  uint32_t seconds = 0;
  ASSERT_TRUE(DailyReadingStats::secondsFor(day, seconds));
  EXPECT_EQ(seconds, 600u);
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  EXPECT_EQ(id.bytes, StatsCounterId{}.bytes);
}

TEST_F(StatsStorageTest, UncertainDailyIdentityNeverStartsAnotherLifetime) {
  StatsCounterId id;
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  const auto path = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true) + "/counter_id.bin";
  const auto bytes = Storage.files.at(path)->bytes;
  for (const auto& bad : {std::vector<uint8_t>{}, std::vector<uint8_t>{2}, std::vector<uint8_t>{1, 2, 3}}) {
    writeBytes(path, bad);
    switchReader(5);
    EXPECT_FALSE(DailyReadingStats::counterId(id));
  }
  writeBytes(path, bytes);
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(id));
  faults.failReadPath = path;
  switchReader(5);
  EXPECT_FALSE(DailyReadingStats::counterId(id));
}

TEST_F(StatsStorageTest, DailyIdentityPublicationFailureKeepsReadingPending) {
  ASSERT_FALSE(StatsStorage::localPath(DailyReadingStats::DIRECTORY, true).empty());
  faults.failOpenWrite = true;
  EXPECT_FALSE(DailyReadingStats::record(readingStatsDayIndex({2026, 11, 1}), 60));
  faults.failOpenWrite = false;
  StatsCounterId id;
  EXPECT_TRUE(DailyReadingStats::counterId(id));
  EXPECT_TRUE(DailyReadingStats::record(readingStatsDayIndex({2026, 11, 1}), 60));
}

TEST_F(StatsStorageTest, DailyIdentityRecoversACompleteUnpublishedMarker) {
  StatsCounterId before, after;
  ASSERT_TRUE(DailyReadingStats::counterId(before));
  const auto path = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true) + "/counter_id.bin";
  ASSERT_TRUE(Storage.rename(path.c_str(), (path + ".tmp").c_str()));
  switchReader(5);
  ASSERT_TRUE(DailyReadingStats::counterId(after));
  EXPECT_EQ(after.bytes, before.bytes);
  EXPECT_TRUE(Storage.exists(path.c_str()));
}

TEST_F(StatsStorageTest, FailedDailyDirectoryScanNeverInventsAnIdentity) {
  const auto directory = StatsStorage::localPath(DailyReadingStats::DIRECTORY, true);
  ASSERT_TRUE(Storage.mkdir(directory.c_str()));
  faults.failScan = true;
  StatsCounterId id;
  EXPECT_FALSE(DailyReadingStats::counterId(id));
  EXPECT_FALSE(Storage.exists((directory + "/counter_id.bin").c_str()));
  faults.failScan = false;
  EXPECT_TRUE(DailyReadingStats::counterId(id));
}

TEST_F(StatsStorageTest, ReclassifyingLegacyBookAndGlobalSlotsMovesOnlyTheCompletion) {
  std::vector<uint8_t> global(175 + two_slot::TRAILER_BYTES);
  global[0] = 4;
  global[13] = 5;
  global[159] = 0x3a;
  progress_record::writeU32(global.data() + 175, 7);
  progress_record::writeU32(global.data() + 179, progress_record::crc32(global.data(), 179));
  writeBytes("/.crosspoint/global_stats.bin", global);

  std::vector<uint8_t> book(91);
  book[0] = 7;
  book[3] = 120;
  book[11] = 1;
  book[75] = 0x77;
  writeBytes(std::string(CACHE) + "/stats_v7.bin", book);

  bool article = false;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  ASSERT_TRUE(article);
  auto stats = GlobalReadingStats::load();
  EXPECT_EQ(stats.completedBooks, 4u);
  EXPECT_EQ(stats.completedArticles, 1u);
  EXPECT_EQ(stats.counterId.bytes[0], 0x3a);
  auto title = BookReadingStats::load(CACHE);
  EXPECT_TRUE(title.isArticle);
  EXPECT_TRUE(title.isCompleted);
  EXPECT_EQ(title.totalReadingSeconds, 120u);
  EXPECT_EQ(title.counterId.bytes[0], 0x77);
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  EXPECT_FALSE(article);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 5u);
  EXPECT_EQ(GlobalReadingStats::load().completedArticles, 0u);
}

TEST_F(StatsStorageTest, LegacyFinishesOnlyUseCompletionsThatPredateTheUpgrade) {
  std::vector<uint8_t> legacyGlobal(17);
  legacyGlobal[0] = 2;
  legacyGlobal[13] = 1;
  writeBytes("/.crosspoint/global_stats.bin", legacyGlobal);

  std::vector<uint8_t> finished(91);
  finished[0] = 7;
  finished[11] = 1;
  writeBytes(std::string(CACHE) + "/stats_v7.bin", finished);
  const std::string otherCache = "/.crosspoint/epub_other";
  ASSERT_TRUE(Storage.mkdir(otherCache.c_str()));
  writeBytes(otherCache + "/stats_v7.bin", finished);

  auto global = GlobalReadingStats::load();
  BookReadingStats newBook;
  ASSERT_TRUE(global.setCompleted(newBook, true, true));
  ASSERT_TRUE(global.save());
  ASSERT_EQ(global.completedBooks, 2u);

  bool article = false;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  ASSERT_TRUE(article);
  global = GlobalReadingStats::load();
  EXPECT_EQ(global.unassignedLegacyBooks, 0u);
  EXPECT_EQ(global.completedBooks, 1u);
  EXPECT_EQ(global.completedArticles, 1u);

  ASSERT_TRUE(GlobalReadingStats::toggleArticle(otherCache, article));
  EXPECT_TRUE(article);
  const auto other = BookReadingStats::load(otherCache);
  EXPECT_TRUE(other.isCompleted);
  EXPECT_FALSE(other.completionCredited);
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  global = GlobalReadingStats::load();
  EXPECT_EQ(global.completedBooks, 2u);
  EXPECT_EQ(global.completedArticles, 0u);
  EXPECT_EQ(global.unassignedLegacyBooks, 0u);
}

TEST_F(StatsStorageTest, UntrackedAndResetCompletionsCannotMoveAnotherTitlesCredit) {
  auto global = GlobalReadingStats::load();
  BookReadingStats first;
  global.setCompleted(first, true, true);
  ASSERT_TRUE(first.save(CACHE));
  ASSERT_TRUE(global.save());
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  global = GlobalReadingStats::load();
  BookReadingStats another, untracked;
  global.setCompleted(another, true, true);
  global.setCompleted(untracked, true, false);
  ASSERT_TRUE(global.save());
  const std::string untrackedCache = "/.crosspoint/epub_untracked";
  ASSERT_TRUE(Storage.mkdir(untrackedCache.c_str()));
  ASSERT_TRUE(untracked.save(untrackedCache));
  bool untrackedArticle = false;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(untrackedCache, untrackedArticle));
  ASSERT_TRUE(untrackedArticle);
  bool article = false;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  EXPECT_TRUE(article);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 1u);
  EXPECT_EQ(GlobalReadingStats::load().completedArticles, 0u);
}

TEST_F(StatsStorageTest, ArticleCreditSurvivesRebootAndPerBookResetKeepsTheType) {
  BookReadingStats live;
  live.counterId = StatsCounterId::create();
  bool article = false;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article, &live));
  auto book = BookReadingStats::load(CACHE);
  ASSERT_EQ(book.counterId.bytes, live.counterId.bytes);
  ASSERT_TRUE(live.isArticle);
  auto global = GlobalReadingStats::load();
  global.setCompleted(book, true, true);
  ASSERT_TRUE(book.save(CACHE));
  ASSERT_TRUE(global.save());
  switchReader(5);
  ASSERT_TRUE(BookReadingStats::peekCompleted(CACHE));
  EXPECT_TRUE(BookReadingStats::load(CACHE).isArticle);
  EXPECT_EQ(GlobalReadingStats::load().completedArticles, 1u);
  ASSERT_TRUE(BookReadingStats::remove(CACHE));
  auto cleared = BookReadingStats::load(CACHE);
  EXPECT_TRUE(cleared.isArticle);
  EXPECT_FALSE(cleared.isCompleted);
  EXPECT_FALSE(cleared.completionCredited);
}

TEST_F(StatsStorageTest, UnreadableTitleCannotBeOverwrittenByClassification) {
  auto book = BookReadingStats::load(CACHE);
  book.totalReadingSeconds = 1234;
  ASSERT_TRUE(book.save(CACHE));
  const auto path = StatsStorage::localPath(std::string(CACHE) + "/stats_v8.bin");
  Storage.files.at(path)->bytes.back() ^= 1;
  const auto damaged = Storage.files.at(path)->bytes;
  bool article = false;
  EXPECT_FALSE(GlobalReadingStats::toggleArticle(CACHE, article));
  EXPECT_EQ(Storage.files.at(path)->bytes, damaged);
}

TEST_F(StatsStorageTest, FailedCounterTransferRollsBackClassificationAndCanRetry) {
  auto global = GlobalReadingStats::load();
  auto book = BookReadingStats::load(CACHE);
  global.setCompleted(book, true, true);
  ASSERT_TRUE(book.save(CACHE));
  ASSERT_TRUE(global.save());
  faults.failNextWritePath = StatsStorage::localPath("/.crosspoint/global_stats.bin") + ".bak";
  bool article = false;
  EXPECT_FALSE(GlobalReadingStats::toggleArticle(CACHE, article));
  EXPECT_FALSE(BookReadingStats::load(CACHE).isArticle);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 1u);
  EXPECT_EQ(GlobalReadingStats::load().completedArticles, 0u);
  // A sync error after a valid retry write must still move exactly one count.
  faults.failSync = true;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  EXPECT_TRUE(article);
  EXPECT_EQ(GlobalReadingStats::load().completedBooks, 0u);
  EXPECT_EQ(GlobalReadingStats::load().completedArticles, 1u);
}

TEST_F(StatsStorageTest, FirstUnfinishedMarkSurvivesRestartBackupAndMarkingBack) {
  auto liveGlobal = GlobalReadingStats::load();
  auto liveBook = BookReadingStats::load(CACHE);
  EXPECT_FALSE(liveGlobal.hasArticles());
  bool article = false;
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article, &liveBook, &liveGlobal.counterId));
  EXPECT_TRUE(liveBook.isArticle);
  switchReader(5);
  auto marked = GlobalReadingStats::load();
  EXPECT_TRUE(marked.hasArticles());
  EXPECT_EQ(marked.completedArticles, 0u);
  EXPECT_EQ(marked.counterId.bytes, liveGlobal.counterId.bytes);
  EXPECT_EQ(BookReadingStats::load(CACHE).counterId.bytes, liveBook.counterId.bytes);
  ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
  EXPECT_FALSE(article);
  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> exported;
  ASSERT_TRUE(GlobalReadingStats::exportLocal(exported.data(), exported.size()));
  EXPECT_EQ(exported[183], 1);
  char name[64];
  ASSERT_TRUE(backupGlobalStats(false, name, sizeof(name)));
  ASSERT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_FALSE(GlobalReadingStats::load().hasArticles());
  ASSERT_TRUE(GlobalReadingStats::restoreBackup((std::string("/.crossink-stats-backup/") + name).c_str()));
  EXPECT_TRUE(GlobalReadingStats::load().hasArticles());
}

TEST_F(StatsStorageTest, NearbyArticleHistoryIncludesUnfinishedMarksAndEarlierV5Counts) {
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/synced_stats"));
  std::vector<uint8_t> peer(GlobalReadingStats::V4_FILE_SIZE, 0);
  peer[0] = 4;
  peer[13] = 6;
  writeBytes("/.crosspoint/synced_stats/old.bin", peer);
  EXPECT_FALSE(GlobalReadingStats::loadAggregated().hasArticles());
  peer.resize(GlobalReadingStats::CURRENT_FILE_SIZE);
  peer[0] = 5;
  peer[13] = 0;
  peer[183] = 1;
  writeBytes("/.crosspoint/synced_stats/peer.bin", peer);
  EXPECT_TRUE(GlobalReadingStats::loadAggregated().hasArticles());
  EXPECT_FALSE(GlobalReadingStats::load().hasArticles());
  EXPECT_EQ(GlobalReadingStats::loadAggregated().completedArticles, 0u);
  peer[183] = 0;
  peer[175] = 1;
  writeBytes("/.crosspoint/synced_stats/peer.bin", peer);
  EXPECT_TRUE(GlobalReadingStats::loadAggregated().hasArticles());
  EXPECT_EQ(GlobalReadingStats::loadAggregated().completedBooks, 6u);
  writeBytes("/.crosspoint/global_stats.bin", peer);
  auto old = GlobalReadingStats::load();
  EXPECT_TRUE(old.articleMarked);
  old.completedArticles = 0;
  ASSERT_TRUE(old.saveSnapshot());
  EXPECT_TRUE(GlobalReadingStats::load().hasArticles());
}

TEST_F(StatsStorageTest, FirstUnfinishedMarkCanRecoverAtEveryPublicationStep) {
  for (int cut = 0; cut < 6; ++cut) {
    SCOPED_TRACE(cut);
    Storage.reset();
    switchReader(5);
    ASSERT_TRUE(Storage.mkdir(CACHE));
    const auto globalPath = StatsStorage::localPath("/.crosspoint/global_stats.bin");
    const auto bookPath = StatsStorage::localPath(std::string(CACHE) + "/stats_v8.bin");
    const auto marker = globalPath + ".article";
    if (cut == 0) faults.powerCutOnWrite = globalPath + ".new";
    if (cut == 1) faults.powerCutOnWrite = bookPath + ".new";
    if (cut == 2) faults.powerCutOnWrite = marker + ".tmp";
    if (cut == 3) faults.powerCutOnWrite = globalPath + ".bak";
    if (cut == 4) faults.powerCutOnWrite = bookPath + ".bak";
    if (cut == 5) faults.powerCutOnRemove = marker;
    bool article = false;
    EXPECT_THROW(GlobalReadingStats::toggleArticle(CACHE, article), SimulatedPowerCut);
    switchReader(5);
    ASSERT_TRUE(GlobalReadingStats::recoverArticleTransfer());
    auto global = GlobalReadingStats::load();
    EXPECT_FALSE(GlobalReadingStats::recoveryRequired());
    EXPECT_EQ(global.hasArticles(), cut == 5);
    EXPECT_EQ(BookReadingStats::load(CACHE).isArticle, cut == 5);
    EXPECT_EQ(global.completedArticles, 0u);
    EXPECT_FALSE(Storage.exists(marker.c_str()));
    ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
    EXPECT_TRUE(GlobalReadingStats::load().hasArticles());
  }
}

TEST_F(StatsStorageTest, NearbyAggregationKeepsBookAndArticleCountsSeparate) {
  auto local = GlobalReadingStats::load();
  local.completedArticles = 2;
  ASSERT_TRUE(local.save());
  std::vector<uint8_t> exported(GlobalReadingStats::CURRENT_FILE_SIZE);
  ASSERT_TRUE(GlobalReadingStats::exportLocal(exported.data(), exported.size()));
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/synced_stats"));
  writeBytes("/.crosspoint/synced_stats/other.bin", exported);
  std::vector<uint8_t> legacy(175);
  legacy[0] = 4;
  legacy[13] = 3;
  writeBytes("/.crosspoint/synced_stats/legacy.bin", legacy);
  const auto aggregate = GlobalReadingStats::loadAggregated(local);
  EXPECT_EQ(aggregate.completedBooks, 3u);
  EXPECT_EQ(aggregate.completedArticles, 4u);
}

TEST_F(StatsStorageTest, InterruptedCountedTransfersFollowTheDurableTitleExactlyOnce) {
  for (int cut = 0; cut < 3; ++cut) {
    SCOPED_TRACE(cut);
    Storage.reset();
    switchReader(5);
    ASSERT_TRUE(Storage.mkdir(CACHE));
    auto global = GlobalReadingStats::load();
    global.totalReadingSeconds = 1500;
    global.completedBooks = global.unassignedLegacyBooks = 3;
    auto book = BookReadingStats::load(CACHE);
    book.isCompleted = book.completionCredited = book.legacyCompletion = true;
    book.totalReadingSeconds = 75;
    ASSERT_TRUE(global.save());
    if (cut == 1) {
      // The very first v8 write must not shadow the existing v7 snapshot when it tears.
      std::vector<uint8_t> v7(91);
      v7[0] = 7;
      v7[3] = 75;
      v7[11] = 1;
      std::copy(book.counterId.bytes.begin(), book.counterId.bytes.end(), v7.begin() + 75);
      writeBytes(std::string(CACHE) + "/stats_v7.bin", v7);
    } else {
      ASSERT_TRUE(book.save(CACHE));
    }

    const std::string globalPath = StatsStorage::localPath("/.crosspoint/global_stats.bin");
    const std::string bookPath = StatsStorage::localPath(std::string(CACHE) + "/stats_v8.bin");
    const std::string marker = globalPath + ".article";
    if (cut == 0) faults.powerCutOnWrite = globalPath + ".bak";
    if (cut == 1) faults.powerCutOnWrite = bookPath + ".new";
    if (cut == 2) faults.powerCutOnRemove = marker;
    bool article = false;
    EXPECT_THROW(GlobalReadingStats::toggleArticle(CACHE, article), SimulatedPowerCut);
    ASSERT_TRUE(Storage.exists(marker.c_str()));

    switchReader(6);
    ASSERT_TRUE(GlobalReadingStats::recoverArticleTransfer());
    ASSERT_TRUE(Storage.exists(marker.c_str()));  // A copied card cannot apply the source reader's intent.
    switchReader(5);
    const bool committed = cut == 2;
    for (int reboot = 0; reboot < 2; ++reboot) {
      ASSERT_TRUE(GlobalReadingStats::recoverArticleTransfer());
      EXPECT_FALSE(Storage.exists(marker.c_str()));
      global = GlobalReadingStats::load();
      EXPECT_EQ(global.completedBooks, committed ? 2u : 3u);
      EXPECT_EQ(global.unassignedLegacyBooks, committed ? 2u : 3u);
      EXPECT_EQ(global.completedArticles, committed ? 1u : 0u);
      EXPECT_EQ(global.hasArticles(), committed);
      EXPECT_EQ(global.totalReadingSeconds, 1500u);
      book = BookReadingStats::load(CACHE);
      EXPECT_EQ(book.isArticle, committed);
      EXPECT_EQ(book.legacyCompletion, !committed);
      EXPECT_EQ(book.totalReadingSeconds, 75u);
      switchReader(5);
    }
    ASSERT_TRUE(GlobalReadingStats::toggleArticle(CACHE, article));
    EXPECT_EQ(article, !committed);
  }
}

TEST_F(StatsStorageTest, InterruptedArticleTransferPreservesUnverifiableHistoryAndCanRetryRecovery) {
  auto global = GlobalReadingStats::load();
  auto book = BookReadingStats::load(CACHE);
  global.setCompleted(book, true, true);
  ASSERT_TRUE(book.save(CACHE));
  ASSERT_TRUE(global.save());
  const auto path = StatsStorage::localPath("/.crosspoint/global_stats.bin");
  const auto marker = path + ".article";
  faults.powerCutOnRemove = marker;
  bool article = false;
  EXPECT_THROW(GlobalReadingStats::toggleArticle(CACHE, article), SimulatedPowerCut);
  switchReader(5);

  faults.failReadPath = marker;
  EXPECT_FALSE(GlobalReadingStats::recoverArticleTransfer());
  EXPECT_TRUE(GlobalReadingStats::recoveryRequired());
  EXPECT_FALSE(global.saveSnapshot());
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_TRUE(Storage.exists(marker.c_str()));
  faults.failReadPath.clear();
  EXPECT_TRUE(GlobalReadingStats::recoverArticleTransfer());
  EXPECT_FALSE(GlobalReadingStats::recoveryRequired());
  EXPECT_EQ(GlobalReadingStats::load().completedArticles, 1u);
}

TEST_F(StatsStorageTest, NearbyExportsSeparateOrCombinedCountsForTheReceivingReader) {
  auto stats = GlobalReadingStats::load();
  stats.totalSessions = 12;
  stats.completedBooks = 3;
  stats.completedArticles = 2;
  ASSERT_TRUE(stats.saveSnapshot());
  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> local{}, payload{};
  ASSERT_TRUE(GlobalReadingStats::exportLocal(local.data(), local.size()));

  for (uint8_t version : {3, 4, 5}) {
    const auto size = GlobalReadingStats::copyForNearby(local, version, payload.data(), payload.size());
    ASSERT_EQ(size, version == 3 ? 159u : version == 4 ? GlobalReadingStats::V4_FILE_SIZE : local.size());
    ASSERT_EQ(payload[0], version);
    EXPECT_EQ(progress_record::readU32(payload.data() + 1), 12u);
    EXPECT_EQ(progress_record::readU32(payload.data() + 13), version < 5 ? 5u : 3u);
    if (version == 5) EXPECT_EQ(payload, local);
    if (version >= 4)
      EXPECT_TRUE(std::equal(stats.counterId.bytes.begin(), stats.counterId.bytes.end(), payload.begin() + 159));
  }
  EXPECT_EQ(GlobalReadingStats::copyForNearby(local, 4, payload.data(), 159), 0u);
  stats.completedBooks = UINT32_MAX;
  ASSERT_TRUE(stats.saveSnapshot());
  ASSERT_TRUE(GlobalReadingStats::exportLocal(local.data(), local.size()));
  ASSERT_EQ(GlobalReadingStats::copyForNearby(local, 4, payload.data(), payload.size()),
            GlobalReadingStats::V4_FILE_SIZE);
  EXPECT_EQ(progress_record::readU32(payload.data() + 13), UINT32_MAX);
}
