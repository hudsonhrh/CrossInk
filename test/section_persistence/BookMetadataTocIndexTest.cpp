#include <Epub/BookMetadataCache.h>
#include <gtest/gtest.h>
#include <ZipFile.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

void writeSpine(BookMetadataCache& cache, const std::vector<std::string>& hrefs) {
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  for (const auto& href : hrefs) cache.createSpineEntry(href);
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
}

void checkToc(BookMetadataCache& cache, const std::vector<int>& expected) {
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());
  ASSERT_TRUE(cache.buildBookBin("/book.epub", {}));
  ASSERT_TRUE(cache.load());
  ASSERT_EQ(cache.getTocCount(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(cache.getTocEntry(i).spineIndex, expected[i]);
}

TEST(BookMetadataTocIndexTest, OutOfOrderDuplicateAndCollidingHrefsKeepFirstExactMatch) {
  Storage.reset();
  // These filenames have the same low 32 bits of their 64-bit FNV-1a hashes.
  const std::string first = "Text/c7d999420678ce622.xhtml";
  const std::string second = "Text/c9cde06b0dbbba27b.xhtml";
  std::vector<std::string> spine = {"0", first, "2", "3", "4", second, "6", first, "8"};
  BookMetadataCache cache("/cache");
  writeSpine(cache, spine);
  for (const auto& href : std::vector<std::string>{"6", second, first, first, "missing", "8"}) {
    cache.createTocEntry(href, href, "", 1);
  }
  checkToc(cache, {6, 5, 1, 1, -1, 8});

  // A new pass on the same object takes the unindexed tiny-book path.
  writeSpine(cache, {"other", "again"});
  cache.createTocEntry("Again", "again", "", 1);
  cache.createTocEntry("Missing", first, "", 1);
  checkToc(cache, {1, -1});
}

TEST(BookMetadataTocIndexTest, OrderedTocReadsSpineLinearlyAndSparseTocReadsOnlyItsPrefix) {
  Storage.reset();
  std::vector<std::string> spine;
  for (int i = 0; i < 299; ++i) spine.push_back("Text/" + std::to_string(i) + ".xhtml");

  BookMetadataCache cache("/cache");
  writeSpine(cache, spine);
  cache.createTocEntry("First", spine[0], "", 1);
  EXPECT_LE(Storage.reads("/cache/spine.bin.tmp"), 4u);
  for (size_t i = 1; i < spine.size(); ++i) cache.createTocEntry("Next", spine[i], "", 1);
  EXPECT_LE(Storage.reads("/cache/spine.bin.tmp"), 4 * spine.size());
  ASSERT_TRUE(cache.endTocPass());
}

TEST(BookMetadataTocIndexTest, DuplicateHrefAlwaysMapsToFirstSpineAtThreshold) {
  for (const unsigned count : {299, 300, 301}) {
    SCOPED_TRACE(count);
    Storage.reset();
    BookMetadataCache cache("/cache");
    writeSpine(cache, std::vector<std::string>(count, "shared.xhtml"));
    cache.createTocEntry("Late anchor", "shared.xhtml", "last", 1);
    cache.createTocEntry("Early anchor", "shared.xhtml", "first", 1);
    checkToc(cache, {0, 0});
  }
}

TEST(BookMetadataTocIndexTest, TruncatedTemporarySpineAbortsAndRebuildsOnRetry) {
  for (const unsigned count : {2, 8, 299, 300}) {
    SCOPED_TRACE(count);
    Storage.reset();
    BookMetadataCache cache("/cache");
    ASSERT_TRUE(cache.beginWrite());
    ASSERT_TRUE(cache.beginContentOpfPass());
    for (unsigned i = 0; i < count; ++i) cache.createSpineEntry(std::to_string(i));
    ASSERT_TRUE(cache.endContentOpfPass());
    const std::string path = "/cache/spine.bin.tmp";
    auto bytes = Storage.bytes(path);
    bytes.pop_back();  // Every href is intact. Only the last row's final field is short.
    Storage.put(path, bytes);
    if (count < 300) {
      ASSERT_TRUE(cache.beginTocPass());
      cache.createTocEntry("Last", std::to_string(count - 1), "", 1);
      EXPECT_FALSE(cache.endTocPass());
    } else {
      EXPECT_FALSE(cache.beginTocPass());
    }
    EXPECT_FALSE(cache.failedForLowMemory());
    writeSpine(cache, {"one", "two"});
    cache.createTocEntry("Two", "two", "", 1);
    checkToc(cache, {1});
  }
}

TEST(BookMetadataTocIndexTest, AssemblyRejectsTruncatedRowsAndKeepsPreviousBook) {
  Storage.reset();
  BookMetadataCache cache("/cache");
  writeSpine(cache, {"first"});
  cache.createTocEntry("Original", "first", "", 1);
  checkToc(cache, {0});
  const auto original = Storage.bytes("/cache/book.bin");

  writeSpine(cache, {"changed", "second"});
  cache.createTocEntry("Chapter", "second", "here", 1);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());
  for (const std::string path : {"/cache/toc.bin.tmp", "/cache/spine.bin.tmp"}) {
    SCOPED_TRACE(path);
    auto intact = Storage.bytes(path);
    auto truncated = intact;
    truncated.pop_back();
    Storage.put(path, truncated);
    EXPECT_FALSE(cache.buildBookBin("/book.epub", {}));
    ASSERT_TRUE(Storage.exists("/cache/book.bin"));
    EXPECT_EQ(Storage.bytes("/cache/book.bin"), original);
    Storage.put(path, intact);
  }
  Storage.failRenameFrom = "/cache/book.bin.tmp";
  EXPECT_FALSE(cache.buildBookBin("/book.epub", {}));
  ASSERT_EQ(Storage.bytes("/cache/book.bin"), original);
  ASSERT_TRUE(Storage.rename("/cache/book.bin", "/cache/book.bin.bak"));
  EXPECT_TRUE(BookMetadataCache::exists("/cache"));
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.getTocEntry(0).title, "Original");
  writeSpine(cache, {"changed", "second"});
  cache.createTocEntry("Chapter", "second", "here", 1);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());
  ASSERT_TRUE(cache.buildBookBin("/book.epub", {}));
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.getTocEntry(0).spineIndex, 1);
}

TEST(BookMetadataTocIndexTest, InvalidCachedOffsetsAndLengthsCannotBecomeEntries) {
  Storage.reset();
  BookMetadataCache cache("/cache");
  writeSpine(cache, {"first", "second"});
  cache.createTocEntry("Chapter", "second", "anchor", 1);
  checkToc(cache, {1});
  const auto good = Storage.bytes("/cache/book.bin");
  const std::string path = "/cache/book.bin";
  uint32_t lut = 0;
  memcpy(&lut, good.data() + sizeof(uint32_t) + sizeof(uint8_t), sizeof(lut));
  auto broken = good;
  uint32_t pos = 0;
  memcpy(broken.data() + lut, &pos, sizeof(pos));  // Points into the cache header.
  Storage.put(path, broken);
  ASSERT_TRUE(cache.load());
  EXPECT_TRUE(cache.getSpineEntry(0).href.empty());
  EXPECT_EQ(cache.getSpineCumulativeSize(0), 0u);

  broken = good;
  memcpy(&pos, good.data() + lut + 2 * sizeof(uint32_t), sizeof(pos));
  const uint32_t huge = UINT32_MAX; // Length at start of last TOC record.
  memcpy(broken.data() + pos, &huge, sizeof(huge));
  Storage.put(path, broken);
  ASSERT_TRUE(cache.load());
  EXPECT_TRUE(cache.getTocEntry(0).href.empty());

  broken = good;
  broken.resize(lut + 1);  // Header is intact but the lookup tables are short.
  Storage.put(path, broken);
  EXPECT_FALSE(cache.load());
  EXPECT_FALSE(cache.isLoaded());
  EXPECT_FALSE(Storage.exists(path.c_str()));
  Storage.put(path, good);
  ASSERT_TRUE(cache.load());
  EXPECT_EQ(cache.getTocEntry(0).spineIndex, 1);
}

TEST(BookMetadataTocIndexTest, OutOfOrderChapterSizesRemainExactAcrossBatchesAndScanFailure) {
  for (const size_t failAt : {0u, 2u}) {
    Storage.reset();
    ZipFile::clearFixture();
    ZipFile::failBatch = failAt;
    std::vector<std::string> spine;
    // Long names fill the 2 KiB path buffer before the target array fills.
    for (size_t i = 0; i < 143; ++i) {
      const std::string name = std::string(99, 'x') + std::to_string(i) + ".xhtml";
      ZipFile::entries.push_back(name);
      spine.push_back(name);
    }
    std::reverse(spine.begin(), spine.end());
    spine[70] = spine[4];      // Duplicate normalized chapter reference.
    spine[95] = "missing";     // A ZIP miss in a later batch must leave a zero size.
    BookMetadataCache cache("/cache");
    writeSpine(cache, spine);
    ASSERT_TRUE(cache.endTocPass());
    ASSERT_TRUE(cache.endWrite());
    ASSERT_TRUE(cache.buildBookBin("/book.epub", {}));
    ASSERT_TRUE(cache.load());
    uint32_t expected = 0;
    for (size_t i = 0; i < spine.size(); ++i) {
      const auto it = std::find(ZipFile::entries.begin(), ZipFile::entries.end(), spine[i]);
      if (it != ZipFile::entries.end()) expected += it - ZipFile::entries.begin() + 1;
      EXPECT_EQ(cache.getSpineCumulativeSize(i), expected);
    }
    ASSERT_GE(ZipFile::batches.size(), 2u);
    if (failAt) {
      EXPECT_EQ(ZipFile::batches.size(), failAt);
      EXPECT_EQ(ZipFile::individualLookups, spine.size());
    } else {
      EXPECT_LT(ZipFile::individualLookups, 10u);
      EXPECT_LT(*std::max_element(ZipFile::batches.begin(), ZipFile::batches.end()), 48u);
    }
  }
  ZipFile::clearFixture();
}

TEST(BookMetadataTocIndexTest, TwoReversedFrontmatterChaptersKeepTheForwardCursorForTheOrderedBody) {
  Storage.reset();
  ZipFile::clearFixture();
  for (unsigned i = 0; i < 299; ++i) ZipFile::entries.push_back(std::to_string(i) + ".xhtml");
  auto spine = ZipFile::entries;
  std::reverse(spine.begin(), spine.begin() + 3);
  BookMetadataCache cache("/cache");
  writeSpine(cache, spine);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());
  ASSERT_TRUE(cache.buildBookBin("/book.epub", {}));
  EXPECT_TRUE(ZipFile::batches.empty());
  EXPECT_EQ(ZipFile::individualLookups, spine.size());
  ZipFile::clearFixture();
}

TEST(BookMetadataTocIndexTest, OrderedChapterSizesContinueUsingForwardCursor) {
  Storage.reset();
  ZipFile::clearFixture();
  for (unsigned i = 0; i < 143; ++i) ZipFile::entries.push_back(std::to_string(i) + ".xhtml");
  BookMetadataCache cache("/cache");
  writeSpine(cache, ZipFile::entries);
  ASSERT_TRUE(cache.endTocPass());
  ASSERT_TRUE(cache.endWrite());
  ASSERT_TRUE(cache.buildBookBin("/book.epub", {}));
  EXPECT_TRUE(ZipFile::batches.empty());
  EXPECT_EQ(ZipFile::individualLookups, 143u);
  ZipFile::clearFixture();
}

}  // namespace
