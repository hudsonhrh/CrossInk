// Adapted from CrossPoint PR #3806 by Daniil Pankov (T-Damer).
// Synthetic regression cases originated in lpla's PR review.
// https://github.com/crosspoint-reader/crosspoint-reader/pull/3806

#include <HalStorage.h>
#include <XtcParser.h>
#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using namespace xtc;

namespace {

constexpr const char* kPath = "/book.xtc";
constexpr uint16_t kSide = 8;
constexpr uint8_t kFill = 0x55;

// 1-bit pages are row-major ((w+7)/8 * h); 2-bit pages are two column-major planes (w * ceil(h/8) * 2).
uint32_t payloadSize(uint32_t pageMagic) { return pageMagic == XTH_MAGIC ? kSide * ((kSide + 7) / 8) * 2 : 8; }

// One-page book: header, one page table entry, one page.
std::vector<uint8_t> buildBook(uint32_t containerMagic, uint32_t pageMagic) {
  XtcHeader header{};
  header.magic = containerMagic;
  header.versionMajor = 1;
  header.pageCount = 1;
  header.pageTableOffset = sizeof(XtcHeader);
  header.dataOffset = sizeof(XtcHeader) + sizeof(PageTableEntry);

  XtgPageHeader page{};
  page.magic = pageMagic;
  page.width = kSide;
  page.height = kSide;
  page.dataSize = payloadSize(pageMagic);

  PageTableEntry entry{header.dataOffset, static_cast<uint32_t>(sizeof(XtgPageHeader) + page.dataSize), kSide, kSide};

  std::vector<uint8_t> bytes(header.dataOffset + sizeof(XtgPageHeader) + page.dataSize, kFill);
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + header.pageTableOffset, &entry, sizeof(entry));
  std::memcpy(bytes.data() + header.dataOffset, &page, sizeof(page));
  return bytes;
}

void setFirstPageOffset(std::vector<uint8_t>& bytes, uint64_t offset) {
  PageTableEntry entry;
  std::memcpy(&entry, bytes.data() + sizeof(XtcHeader), sizeof(entry));
  entry.dataOffset = offset;
  std::memcpy(bytes.data() + sizeof(XtcHeader), &entry, sizeof(entry));
}

struct DepthCase {
  const char* name;
  uint32_t containerMagic;
  uint32_t pageMagic;
  uint8_t expectedDepth;
};

class XtcParserDepth : public ::testing::TestWithParam<DepthCase> {};

// Matching files keep working, and mismatched ones (xtctool writes "XTC\0" around
// "XTH\0" pages) follow the bit depth of their pages.
TEST_P(XtcParserDepth, BitDepthFollowsPagesAndPageLoads) {
  const DepthCase& c = GetParam();
  Storage.setFile(kPath, buildBook(c.containerMagic, c.pageMagic));

  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  EXPECT_EQ(parser.getBitDepth(), c.expectedDepth);

  PageInfo info{};
  ASSERT_TRUE(parser.getPageInfo(0, info));
  EXPECT_EQ(info.bitDepth, c.expectedDepth);

  const size_t expected = payloadSize(c.pageMagic);
  uint8_t buffer[64]{};
  ASSERT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), expected);
  for (size_t i = 0; i < expected; i++) EXPECT_EQ(buffer[i], kFill) << "byte " << i;

  EXPECT_EQ(parser.loadPage(0, buffer, expected - 1), 0u);
  EXPECT_EQ(parser.getLastError(), XtcError::MEMORY_ERROR);

  size_t streamed = 0;
  const XtcError status = parser.loadPageStreaming(
      0,
      [&](const uint8_t* data, size_t size, size_t offset) {
        EXPECT_EQ(offset, streamed);
        for (size_t i = 0; i < size; i++) EXPECT_EQ(data[i], kFill);
        streamed += size;
      },
      3);
  EXPECT_EQ(status, XtcError::OK);
  EXPECT_EQ(streamed, expected);
  EXPECT_EQ(parser.loadPageStreaming(0, [](const uint8_t*, size_t, size_t) {}, 0), XtcError::MEMORY_ERROR);
}

INSTANTIATE_TEST_SUITE_P(Containers, XtcParserDepth,
                         ::testing::Values(DepthCase{"XtcWithXtgPages", XTC_MAGIC, XTG_MAGIC, 1},
                                           DepthCase{"XtchWithXthPages", XTCH_MAGIC, XTH_MAGIC, 2},
                                           DepthCase{"XtcWithXthPages", XTC_MAGIC, XTH_MAGIC, 2},
                                           DepthCase{"XtchWithXtgPages", XTCH_MAGIC, XTG_MAGIC, 1}),
                         [](const ::testing::TestParamInfo<DepthCase>& info) { return info.param.name; });

TEST(XtcParser, ShortXtchStreamReportsErrorAfterPartialSecondPlane) {
  auto book = buildBook(XTCH_MAGIC, XTH_MAGIC);
  book.pop_back();
  Storage.setFile(kPath, book);
  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  size_t received = 0;
  const auto error = parser.loadPageStreaming(
      0,
      [&](const uint8_t*, size_t size, size_t offset) {
        EXPECT_EQ(received, offset);
        received += size;
      },
      3);
  EXPECT_EQ(error, XtcError::READ_ERROR);
  EXPECT_EQ(received, payloadSize(XTH_MAGIC) - 1);
}

TEST(XtcParser, MixedSizeXtgUsesEachPagesBytePaddedRows) {
  // A 16x8 cover, a 3x2 interior page and a 17x10 page exercise both directions.
  const std::pair<uint16_t, uint16_t> shapes[] = {{16, 8}, {3, 2}, {17, 10}};
  XtcHeader header{};
  header.magic = XTC_MAGIC;
  header.versionMajor = 1;
  header.pageCount = 3;
  header.pageTableOffset = sizeof(XtcHeader);
  header.dataOffset = sizeof(XtcHeader) + 3 * sizeof(PageTableEntry);
  std::vector<uint8_t> bytes(header.dataOffset);
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (int i = 0; i < 3; ++i) {
    const auto [w, h] = shapes[i];
    const uint32_t payload = ((w + 7u) / 8) * h;
    const XtgPageHeader page{XTG_MAGIC, w, h, 0, 0, payload, 0};
    const PageTableEntry entry{bytes.size(), static_cast<uint32_t>(sizeof(page)) + payload, w, h};
    std::memcpy(bytes.data() + header.pageTableOffset + i * sizeof(entry), &entry, sizeof(entry));
    bytes.resize(bytes.size() + entry.dataSize, 10 + i);
    std::memcpy(bytes.data() + entry.dataOffset, &page, sizeof(page));
  }
  Storage.setFile(kPath, std::move(bytes));
  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  for (int i : {0, 2, 1, 0}) {
    PageInfo info{};
    ASSERT_TRUE(parser.getPageInfo(i, info));
    EXPECT_EQ(info.width, shapes[i].first);
    EXPECT_EQ(info.height, shapes[i].second);
    std::vector<uint8_t> bitmap(xtgPageBytes(info.width, info.height));
    ASSERT_EQ(parser.loadPage(i, bitmap.data(), bitmap.size()), bitmap.size());
    for (auto px : bitmap) EXPECT_EQ(px, 10 + i);
  }
}

TEST(XtcParser, RejectsDisagreementBeforePublishingAnyPixels) {
  for (int damage = 0; damage < 4; ++damage) {
    auto bytes = buildBook(XTC_MAGIC, XTG_MAGIC);
    PageTableEntry entry{};
    std::memcpy(&entry, bytes.data() + sizeof(XtcHeader), sizeof(entry));
    XtgPageHeader page{};
    std::memcpy(&page, bytes.data() + entry.dataOffset, sizeof(page));
    if (damage == 0) page.width--;  // Same byte length; dimensions still matter.
    if (damage == 1) page.dataSize--;
    if (damage == 2) entry.dataSize--;  // Payload would spill into the next entry.
    if (damage == 3) page.compression = 1;
    std::memcpy(bytes.data() + sizeof(XtcHeader), &entry, sizeof(entry));
    std::memcpy(bytes.data() + entry.dataOffset, &page, sizeof(page));
    Storage.setFile(kPath, std::move(bytes));
    XtcParser parser;
    ASSERT_EQ(parser.open(kPath), XtcError::OK);
    uint8_t buffer[64]{};
    EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
    EXPECT_EQ(parser.getLastError(), XtcError::CORRUPTED_HEADER);
    EXPECT_EQ(parser.loadPageStreaming(0, [](const uint8_t*, size_t, size_t) { ADD_FAILURE(); }),
              XtcError::CORRUPTED_HEADER);
  }
}

TEST(XtcParser, OddHeightXthPreservesTheLastColumnAndThePlaneBoundary) {
  auto bytes = buildBook(XTCH_MAGIC, XTH_MAGIC);
  constexpr uint16_t w = 3, h = 9;
  constexpr uint8_t pixels[] = {0x80, 0, 0x20, 0, 0, 0x80, 0, 0x80, 1, 0, 0x80, 0};
  XtgPageHeader page{XTH_MAGIC, w, h, 0, 0, sizeof(pixels), 0};
  PageTableEntry entry{sizeof(XtcHeader) + sizeof(PageTableEntry), sizeof(page) + sizeof(pixels), w, h};
  bytes.resize(entry.dataOffset + entry.dataSize);
  std::memcpy(bytes.data() + sizeof(XtcHeader), &entry, sizeof(entry));
  std::memcpy(bytes.data() + entry.dataOffset, &page, sizeof(page));
  std::memcpy(bytes.data() + entry.dataOffset + sizeof(page), pixels, sizeof(pixels));
  Storage.setFile(kPath, std::move(bytes));
  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  uint8_t buffer[sizeof(pixels)]{};
  EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), sizeof(pixels));
  EXPECT_EQ(std::memcmp(buffer, pixels, sizeof(pixels)), 0);
  std::vector<uint8_t> actual;
  ASSERT_EQ(parser.loadPageStreaming(0, [&](const uint8_t* data, size_t size, size_t) {
              actual.insert(actual.end(), data, data + size);
            }, 5), XtcError::OK);
  EXPECT_EQ(actual, std::vector<uint8_t>(std::begin(pixels), std::end(pixels)));
}

TEST(XtcParser, SignedSdReadFailureStopsStreamAndNextReadRecovers) {
  Storage.setFile(kPath, buildBook(XTCH_MAGIC, XTH_MAGIC));
  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  HalFile::failedReadOffset = sizeof(XtcHeader) + sizeof(PageTableEntry) + sizeof(XtgPageHeader) + 9;
  size_t calls = 0;
  const auto error = parser.loadPageStreaming(0, [&](const uint8_t*, size_t size, size_t offset) {
    EXPECT_EQ(size, 3u);
    EXPECT_EQ(offset, 3 * calls++);
  }, 3);
  HalFile::failedReadOffset = UINT64_MAX;
  EXPECT_EQ(error, XtcError::READ_ERROR);
  EXPECT_EQ(calls, 3u);
  EXPECT_EQ(parser.loadPageStreaming(0, [](const uint8_t*, size_t, size_t) {}, 5), XtcError::OK);
}

// An unknown first-page magic does not override the container; the page is still rejected.
TEST(XtcParser, UnknownPageMagicKeepsContainerDepth) {
  for (const uint32_t container : {XTC_MAGIC, XTCH_MAGIC}) {
    Storage.setFile(kPath, buildBook(container, 0x12345678));
    XtcParser parser;
    ASSERT_EQ(parser.open(kPath), XtcError::OK);
    EXPECT_EQ(parser.getBitDepth(), container == XTCH_MAGIC ? 2 : 1);
    uint8_t buffer[64]{};
    EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
    EXPECT_EQ(parser.getLastError(), XtcError::INVALID_MAGIC);
  }
}

// A first-page offset beyond the file (including one that would overflow offset + 4)
// is ignored by the probe: open still succeeds with the container depth, the page fails.
TEST(XtcParser, OutOfRangeFirstPageOffsetKeepsContainerDepth) {
  for (const uint64_t offset : {uint64_t{1} << 40, std::numeric_limits<uint64_t>::max() - 1}) {
    auto bytes = buildBook(XTC_MAGIC, XTH_MAGIC);
    setFirstPageOffset(bytes, offset);
    Storage.setFile(kPath, std::move(bytes));
    XtcParser parser;
    ASSERT_EQ(parser.open(kPath), XtcError::OK);
    EXPECT_EQ(parser.getBitDepth(), 1);
    uint8_t buffer[64]{};
    EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
  }
}

// A first page cut off inside its magic is ignored the same way.
TEST(XtcParser, TruncatedFirstPageKeepsContainerDepth) {
  auto bytes = buildBook(XTC_MAGIC, XTH_MAGIC);
  bytes.resize(sizeof(XtcHeader) + sizeof(PageTableEntry) + 2);
  Storage.setFile(kPath, std::move(bytes));
  XtcParser parser;
  ASSERT_EQ(parser.open(kPath), XtcError::OK);
  EXPECT_EQ(parser.getBitDepth(), 1);
  uint8_t buffer[64]{};
  EXPECT_EQ(parser.loadPage(0, buffer, sizeof(buffer)), 0u);
}

// Failed access must not change the container-derived depth or fail opening.
TEST(XtcParser, ProbeIoFailuresKeepContainerDepth) {
  for (bool failSeek : {false, true}) {
    Storage.setFile(kPath, buildBook(XTC_MAGIC, XTH_MAGIC));
    HalFile::failedSeekOffset = failSeek ? sizeof(XtcHeader) + sizeof(PageTableEntry) : UINT64_MAX;
    HalFile::shortReadOffset = failSeek ? UINT64_MAX : sizeof(XtcHeader) + sizeof(PageTableEntry);
    XtcParser parser;
    EXPECT_EQ(parser.open(kPath), XtcError::OK);
    EXPECT_EQ(parser.getBitDepth(), 1);
    HalFile::failedSeekOffset = HalFile::shortReadOffset = UINT64_MAX;
  }
}

}  // namespace
