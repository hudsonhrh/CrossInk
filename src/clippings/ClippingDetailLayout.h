#pragma once

#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace clippingDetail {
struct Line {
  uint16_t start;
  uint16_t end;

  Line(const size_t start, const size_t end) : start(static_cast<uint16_t>(start)), end(static_cast<uint16_t>(end)) {}
};

// The renderer takes C strings. Expose a span without copying it, then restore
// its delimiter so a later measure or draw can use the same retained text.
template <typename UseText>
auto withLineText(std::string& text, const Line line, const UseText& use) {
  struct RestoreChar {
    char& slot;
    const char saved;
    ~RestoreChar() { slot = saved; }
  };
  char& delimiter = text[line.end];
  const RestoreChar restore{delimiter, delimiter};
  delimiter = '\0';
  return use(text.c_str() + line.start);
}

namespace detail {
inline bool isLineBreak(const uint32_t cp) { return cp == '\r' || cp == '\n' || cp == 0x2028 || cp == 0x2029; }

inline bool isInlineSpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
         cp == 0x205F || cp == 0x3000;
}

template <typename Measure>
void splitLongWord(std::string& text, size_t start, const size_t end, const int maxWidth, const Measure& measure,
                   std::vector<Line>& out) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* cursor = bytes + start;
  size_t committedEnd = start;
  while (cursor < bytes + end) {
    utf8NextCodepoint(&cursor);
    const size_t candidateEnd = cursor - bytes;
    if (committedEnd != start && withLineText(text, {start, candidateEnd}, measure) > maxWidth) {
      out.emplace_back(start, committedEnd);
      start = committedEnd;
    }
    committedEnd = candidateEnd;
  }
  out.emplace_back(start, end);
}
}  // namespace detail

inline void normalizeInPlace(std::string& text) {
  char* const begin = text.data();
  char* write = begin;
  auto* cursor = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* end = cursor + text.size();
  bool spacePending = false;
  while (cursor < end) {
    const auto* start = cursor;
    const uint32_t cp = utf8NextCodepoint(&cursor);
    if (detail::isLineBreak(cp)) {
      if (cp == '\r' && *cursor == '\n') ++cursor;
      *write++ = '\n';
      spacePending = false;
    } else if (cp == 0 || detail::isInlineSpace(cp)) {
      if (cp == 0) ++cursor;  // The decoder leaves embedded NULs unconsumed.
      spacePending = write != begin && write[-1] != '\n';
    } else {
      if (spacePending) *write++ = ' ';
      const size_t length = cursor - start;
      std::memmove(write, start, length);
      write += length;
      spacePending = false;
    }
  }
  text.resize(write - begin);
}

// Input is the normalized retained clipping, whose size fits in a Line offset.
template <typename Measure>
void wrap(std::string& text, const int maxWidth, const Measure& measure, std::vector<Line>& out) {
  out.clear();
  if (maxWidth <= 0) return;

  // A clipping can contain a hard break on every byte. Each row needs only
  // offsets into the retained text, including blank rows.
  if (!text.empty()) out.reserve(std::count(text.begin(), text.end(), '\n') + (text.back() != '\n'));
  for (size_t start = 0; start < text.size();) {
    const size_t end = std::min(text.find('\n', start), text.size());
    size_t lineStart = start;
    for (size_t word = start; word < end;) {
      const size_t wordEnd = std::min(text.find_first_of(" \n", word), end);
      if (withLineText(text, {lineStart, wordEnd}, measure) > maxWidth) {
        if (lineStart != word) {
          out.emplace_back(lineStart, word - 1);
          lineStart = word;
        }
        if (withLineText(text, {word, wordEnd}, measure) > maxWidth) {
          detail::splitLongWord(text, word, wordEnd, maxWidth, measure, out);
          lineStart = wordEnd + 1;
        }
      }
      word = wordEnd + 1;
    }
    if (start == end || lineStart < end) out.emplace_back(lineStart, end);
    start = end + 1;
  }
}
}  // namespace clippingDetail
