#include <FileIndex.h>
#include <NaturalSort.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {
bool accept(const char* name, bool) { return name[0] != '.'; }

class FileIndexTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    Storage.mkdir("/");
    Storage.mkdir("/books");
  }
};

TEST_F(FileIndexTest, MergesShortFinalRunsAndAppendsUniqueNamesInBatches) {
  for (const unsigned i : {0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 257, 1025}) {
    SCOPED_TRACE(i);
    std::vector<std::string> expected;
    for (unsigned j = 0; j < i; ++j) {
      const auto name = "Vol " + std::to_string(j) + ".epub";
      fake::add("/books/" + name);
      expected.push_back(name);
    }
    fake::resetIoCounters();
    FileIndex index;
    ASSERT_TRUE(index.open("/books", accept));
    EXPECT_EQ(index.totalCount(), expected.size());
    if (i >= 257) {
      // Distinct sort keys only need chunk reads and merge-pass flushes.
      EXPECT_LT(fake::reads, i);
      EXPECT_LT(fake::flushes, 20u);
    }
    FileIndex::Entry entry{};
    for (size_t row = 0; row < expected.size(); ++row) {
      ASSERT_TRUE(index.entryAt(row, entry));
      EXPECT_EQ(expected[row], entry.name);
    }
    ASSERT_FALSE(index.entryAt(i, entry));
  }
}

TEST_F(FileIndexTest, LongSharedPrefixesAndNaturalEqualsRetainExactOrderOnReopen) {
  const std::string prefix = "A collection with a deliberately long shared prefix - ";
  std::vector<std::pair<bool, std::string>> expected;
  // Force a >64-entry tie to exercise spilled merges, plus ties next to
  // unique prefixes to alternate shared use of the offset scratch area.
  for (unsigned j = 0; j < 259; ++j) {
    std::string name = (j % 5 == 0 ? "early-" : prefix) + std::to_string((j * 43) % 259) + ".epub";
    const bool dir = j % 11 == 0;
    if (dir)
      Storage.mkdir(("/books/" + name).c_str());
    else
      fake::add("/books/" + name);
    expected.emplace_back(dir, name);
  }
  for (const auto* suffix : {"tie-002.epub", "tie-02.EPUB", "tie-2.epub"}) {
    const auto name = prefix + suffix;
    fake::add("/books/" + name);
    expected.emplace_back(false, name);
  }
  fake::add("/books/.hidden.epub");
  // The fake directory enumerates lexical filenames. Equal natural keys
  // keep that enumeration order, including case and leading zeros.
  std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first : FsHelpers::naturalCompare(a.second.c_str(), b.second.c_str()) < 0;
  });
  FileIndex index;
  for (int attempt = 0; attempt < 2; ++attempt) {
    ASSERT_TRUE(index.open("/books", accept));
    ASSERT_EQ(index.totalCount(), expected.size());
    for (size_t row = 0; row < expected.size(); ++row) {
      FileIndex::Entry item{};
      ASSERT_TRUE(index.entryAt(row, item));
      EXPECT_EQ(expected[row].first, item.isDir);
      EXPECT_EQ(expected[row].second, item.name);
    }
    index.close();
  }
}

TEST_F(FileIndexTest, ReadOrWriteFailureDuringMergeKeepsPreviousIndexAndCanRetry) {
  FileIndex index;
  for (int i = 0; i < 255; ++i) fake::add("/books/Test " + std::to_string(i));
  ASSERT_TRUE(index.open("/books", accept));
  index.close();
  std::string path;
  for (const auto& [name, file] : fake::files) {
    if (name.ends_with(".idx")) path = name;
  }
  ASSERT_FALSE(path.empty());
  const auto original = fake::files[path]->bytes;
  fake::add("/books/new");
  fake::failWritePath = "/.crosspoint/fileindex/runs.b";
  EXPECT_FALSE(index.open("/books", accept));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_EQ(original, fake::files[path]->bytes);
  fake::failureTriggered = false;
  fake::failRead = 8;
  EXPECT_FALSE(index.open("/books", accept));
  EXPECT_TRUE(fake::failureTriggered);
  EXPECT_EQ(original, fake::files[path]->bytes);
  const auto backup = path + ".bak";
  for (const auto& target :
       {path + ".tmp", std::string("/.crosspoint/fileindex/runs.a"), std::string("/.crosspoint/fileindex/runs.b")}) {
    SCOPED_TRACE(target);
    for (bool close : {false, true}) {
      fake::failureTriggered = false;
      (close ? fake::failClosePath : fake::failSyncPath) = target;
      EXPECT_FALSE(index.open("/books", accept));
      EXPECT_TRUE(fake::failureTriggered);
      EXPECT_EQ(original, fake::files[path]->bytes);
    }
  }
  for (const auto& target : {path, backup}) {
    fake::failRenameToPath = target;
    EXPECT_FALSE(index.open("/books", accept));
    EXPECT_TRUE(fake::failRenameToPath.empty());
    ASSERT_TRUE(Storage.exists(path.c_str()));
    EXPECT_EQ(original, fake::files[path]->bytes);
    EXPECT_FALSE(Storage.exists(backup.c_str()));
  }

  // A reboot can see the backup on its own, or both the uncommitted new
  // file and the backup. Either state must restore the committed index.
  ASSERT_TRUE(Storage.remove("/books/new"));
  for (bool partial : {false, true}) {
    ASSERT_TRUE(Storage.rename(path.c_str(), backup.c_str()));
    if (partial) fake::add(path, "incomplete replacement");
    ASSERT_TRUE(index.open("/books", accept));
    index.close();
    ASSERT_TRUE(Storage.exists(path.c_str()));
    EXPECT_EQ(original, fake::files[path]->bytes);
    EXPECT_FALSE(Storage.exists(backup.c_str()));
  }
  fake::add("/books/new");
  ASSERT_TRUE(index.open("/books", accept));
  EXPECT_EQ(256u, index.totalCount());
}

TEST_F(FileIndexTest, FailedTieSpillSyncOrCloseCannotPublishTheReplacement) {
  FileIndex index;
  for (int i = 0; i < 130; ++i) {
    fake::add("/books/A shared prefix longer than a sort key " + std::to_string(i));
  }
  ASSERT_TRUE(index.open("/books", accept));
  index.close();
  std::string path;
  for (const auto& [name, file] : fake::files) {
    if (name.ends_with(".idx")) path = name;
  }
  ASSERT_FALSE(path.empty());
  const auto original = fake::files[path]->bytes;
  fake::add("/books/Another title");
  for (const char* target : {"/.crosspoint/fileindex/tie.a", "/.crosspoint/fileindex/tie.b"}) {
    SCOPED_TRACE(target);
    for (bool close : {false, true}) {
      fake::failureTriggered = false;
      (close ? fake::failClosePath : fake::failSyncPath) = target;
      EXPECT_FALSE(index.open("/books", accept));
      EXPECT_TRUE(fake::failureTriggered);
      EXPECT_EQ(original, fake::files[path]->bytes);
    }
  }
  ASSERT_TRUE(index.open("/books", accept));
  EXPECT_EQ(131u, index.totalCount());
}
}  // namespace
