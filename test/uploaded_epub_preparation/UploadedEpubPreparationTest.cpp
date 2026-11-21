#include "src/network/UploadedEpubPreparation.h"

#include <gtest/gtest.h>

#include <string>

TEST(UploadedEpubPreparation, AcceptsCanonicalFatPaths) {
  EXPECT_TRUE(uploadedepub::isCanonicalPath("/book.epub"));
  EXPECT_TRUE(uploadedepub::isCanonicalPath("/Books/New arrivals/Café.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath(""));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("//book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/Books/../book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/Books/./book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/ Books/book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/Books /book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/Books/book.epub/"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/Books\\book.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/Books/book\t.epub"));
  EXPECT_FALSE(uploadedepub::isCanonicalPath(std::string_view("/book.epub\0extra", 16)));
  EXPECT_FALSE(uploadedepub::isCanonicalPath("/" + std::string(uploadedepub::MAX_PATH_BYTES, 'x')));
}

TEST(UploadedEpubPreparation, MatchesOnlyTheLastAvailableNonemptyUpload) {
  EXPECT_TRUE(uploadedepub::matchesCandidate("/Books/book.epub", 42, "/Books/book.epub"));
  EXPECT_FALSE(uploadedepub::matchesCandidate("/Books/book.epub", 0, "/Books/book.epub"));
  EXPECT_FALSE(uploadedepub::matchesCandidate("/Books/book.epub", 42, "/book.epub"));
  EXPECT_FALSE(uploadedepub::matchesCandidate("/Books/book.epub", 42, "/Books/Book.epub"));
  EXPECT_FALSE(uploadedepub::matchesCandidate("/Books/book (1).epub", 42, "/Books/book.epub"));
}

TEST(UploadedEpubPreparation, RecognizesOptimizerMarkerAndProtectionEntries) {
  EXPECT_TRUE(uploadedepub::isOptimizedEntry("META-INF/crossink/optimizer-v1.json"));
  EXPECT_FALSE(uploadedepub::isOptimizedEntry("META-INF/crossink/optimizer-v1.json.txt"));
  EXPECT_FALSE(uploadedepub::isOptimizedEntry("OPS/optimizer-v1.json"));
  EXPECT_TRUE(uploadedepub::isProtectedEntry("META-INF/encryption.xml"));
  EXPECT_TRUE(uploadedepub::isProtectedEntry("META-INF/License.LCPL"));
  EXPECT_TRUE(uploadedepub::isProtectedEntry("meta-inf/Rights.xml"));
  EXPECT_FALSE(uploadedepub::isProtectedEntry("OPS/encryption.xml"));
}
