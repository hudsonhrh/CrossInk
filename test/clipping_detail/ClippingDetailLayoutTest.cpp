#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "clippings/ClippingDetailLayout.h"

namespace {
int countCodepoints(const char* text) {
  int width = 0;
  auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (utf8NextCodepoint(&cursor) != 0) ++width;
  return width;
}

std::vector<std::string> wrap(std::string text, const int width) {
  clippingDetail::normalizeInPlace(text);
  const std::string normalized = text;
  std::vector<clippingDetail::Line> lines;
  clippingDetail::wrap(text, width, countCodepoints, lines);
  std::vector<std::string> renderedLines;
  renderedLines.reserve(lines.size());
  for (const auto line : lines) {
    clippingDetail::withLineText(text, line, [&](const char* row) { renderedLines.emplace_back(row); });
  }
  EXPECT_EQ(text, normalized);
  return renderedLines;
}
}  // namespace

TEST(ClippingDetailLayout, HardBreaksPreserveLeadingAndInteriorBlankLines) {
  const auto lines = wrap("\r\nfirst\n \t\r\nsecond\r\n\r\nthird\n", 80);
  EXPECT_EQ(lines, (std::vector<std::string>{"", "first", "", "second", "", "third"}));
}

TEST(ClippingDetailLayout, InlineUnicodeSpacesCollapseWhileParagraphSeparatorsBreak) {
  const auto lines = wrap("  first\u00A0\u2003\tsecond\u2028third\u2029\t\u202Ffourth\u2009\u3000fifth  ", 80);
  EXPECT_EQ(lines, (std::vector<std::string>{"first second", "third", "fourth fifth"}));
}

TEST(ClippingDetailLayout, HardBreakAfterAnExactWidthLineDoesNotAddABlankRow) {
  const auto lines = wrap("one two\r\nthree four", 7);
  EXPECT_EQ(lines, (std::vector<std::string>{"one two", "three", "four"}));
}

TEST(ClippingDetailLayout, LongWordsSplitAtUtf8BoundariesBeforeHardBreaks) {
  const auto lines = wrap("hi é中😀é\r\n\r\nend\né中😀\nfin", 2);
  EXPECT_EQ(lines, (std::vector<std::string>{"hi", "é中", "😀é", "", "en", "d", "é中", "😀", "fi", "n"}));
}

TEST(ClippingDetailLayout, EmptyTextReplacesThePreviousLayout) {
  std::vector<clippingDetail::Line> lines;
  std::string text = "previous text";
  clippingDetail::wrap(text, 80, countCodepoints, lines);
  ASSERT_FALSE(lines.empty());
  text.clear();
  clippingDetail::wrap(text, 80, countCodepoints, lines);
  EXPECT_TRUE(lines.empty());
}

TEST(ClippingDetailLayout, MaximumLengthWithBlankLinesRetainsAllRows) {
  std::string text = "a" + std::string(4094, '\n') + "b";
  std::vector<clippingDetail::Line> lines;
  clippingDetail::wrap(text, 80, countCodepoints, lines);
  ASSERT_EQ(lines.size(), 4095u);
  EXPECT_EQ(lines.front().start, 0u);
  EXPECT_EQ(lines.front().end, 1u);
  EXPECT_TRUE(std::all_of(lines.begin() + 1, lines.end() - 1, [](const auto line) { return line.start == line.end; }));
  EXPECT_EQ(lines.back().start, 4095u);
  EXPECT_EQ(lines.back().end, 4096u);
}
