#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace {
struct MemoryFile {
  std::vector<uint8_t> bytes;
  unsigned reads = 0;
  unsigned seeks = 0;
  unsigned shortRead = 0;
  unsigned failedSeek = 0;

  template <typename T>
  void put(size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }
} file;

struct HalFile {
  size_t cursor = 0;
  size_t size() const { return file.bytes.size(); }
  bool seek(size_t offset) {
    if (++file.seeks == file.failedSeek || offset > size()) return false;
    cursor = offset;
    return true;
  }
  int read(void* out, size_t len) {
    len = std::min(len, size() - cursor);
    if (++file.reads == file.shortRead && len) --len;
    std::memcpy(out, file.bytes.data() + cursor, len);
    cursor += len;
    return static_cast<int>(len);
  }
};

struct {
  bool openFileForRead(const char*, const char*, HalFile&) { return true; }
} Storage;
namespace serialization {
template <typename T>
bool tryReadPod(HalFile& f, T& value) {
  return f.read(&value, sizeof(T)) == static_cast<int>(sizeof(T));
}
}  // namespace serialization
#define LOG_ERR(...) ((void)0)

struct Section {
  const char* filePath = "section.bin";
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t) const;
  std::optional<uint16_t> getPageForListItemIndex(uint16_t) const;
};
#include "position_methods.inc"

void makeSection(uint16_t count) {
  file = {};
  const uint32_t paragraphs = HEADER_SIZE + 5;
  const uint32_t list = paragraphs + sizeof(count) + count * sizeof(uint16_t);
  file.bytes.resize(list + count * sizeof(uint16_t));
  file.put(HEADER_SIZE - 3 * sizeof(uint32_t), paragraphs);
  file.put(HEADER_SIZE - 2 * sizeof(uint32_t), list);
  file.put(paragraphs, count);
  for (uint32_t i = 0; i < count; ++i) {
    // Deliberately different tables. A tie straddles two reads, followed by a decrease.
    const auto value = static_cast<uint16_t>(i == 31 || i == 32 ? 90 : i);
    file.put(paragraphs + sizeof(count) + i * sizeof(uint16_t), value);
    file.put(list + i * sizeof(uint16_t), static_cast<uint16_t>(value + 1));
  }
}
}  // namespace

int main() {
  const Section section;
  makeSection(67);
  assert(section.getPageForParagraphIndex(1) == 1);
  assert(section.getPageForListItemIndex(1) == 0);
  assert(section.getPageForParagraphIndex(90) == 31);
  assert(section.getPageForListItemIndex(91) == 31);

  for (auto lookup : {&Section::getPageForParagraphIndex, &Section::getPageForListItemIndex}) {
    for (uint16_t count : {0, 1, 31, 32, 33, 67, 65535}) {
      makeSection(count);
      const auto result = (section.*lookup)(UINT16_MAX);
      if (count == 0) {
        assert(!result);
      } else {
        assert(result == count - 1);
        assert(file.reads <= 3u + (count + 31u) / 32u);
      }
    }

    makeSection(67);
    assert((section.*lookup)(UINT16_MAX) == 66);
    const auto reads = file.reads, seeks = file.seeks;
    for (unsigned op = 1; op <= reads; ++op) {
      makeSection(67);
      file.shortRead = op;
      assert(!(section.*lookup)(UINT16_MAX));
    }
    for (unsigned op = 1; op <= seeks; ++op) {
      makeSection(67);
      file.failedSeek = op;
      assert(!(section.*lookup)(UINT16_MAX));
    }
  }
  makeSection(67);
  file.bytes.pop_back();
  assert(!section.getPageForListItemIndex(0));
  file.bytes.resize(HEADER_SIZE + 8);
  assert(!section.getPageForParagraphIndex(0));
  makeSection(67);
  file.put(HEADER_SIZE - 3 * sizeof(uint32_t), UINT32_MAX);
  assert(!section.getPageForParagraphIndex(0));
  assert(!section.getPageForListItemIndex(0));
}
