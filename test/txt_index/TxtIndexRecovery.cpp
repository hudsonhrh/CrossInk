#include <AtomicFile.h>
#include <ChunkedPageIndex.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define LOG_ERR(...) ((void)0)
void vTaskDelay(int) {}
constexpr size_t CHUNK_SIZE = 8192;
struct {
  char sdFontFamilyName[2]{};
} SETTINGS;

enum class Fault { None, Read, Write, Sync, Close, Rename };
struct {
  std::map<std::string, std::vector<uint8_t>> files;
  unsigned handles = 0, bookOpens = 0;
  size_t bookBytes = 0;
  Fault fault = Fault::None;
  bool consume(Fault candidate) {
    if (fault != candidate) return false;
    fault = Fault::None;
    return true;
  }
} card;

struct HalFile {
  std::string path;
  size_t pos = 0;
  size_t size() const { return card.files.at(path).size(); }
  int read(uint8_t* out, size_t bytes) {
    bytes = std::min(bytes, size() - pos);
    if (bytes && card.consume(Fault::Read)) --bytes;
    if (bytes) std::memcpy(out, card.files.at(path).data() + pos, bytes);
    pos += bytes;
    if (path == "book") card.bookBytes += bytes;
    return static_cast<int>(bytes);
  }
  size_t write(const uint8_t* in, size_t bytes) {
    if (bytes && card.consume(Fault::Write)) --bytes;
    card.files.at(path).assign(in, in + bytes);
    return bytes;
  }
  bool sync() { return !card.consume(Fault::Sync); }
  bool close() {
    if (path.empty()) return true;
    path.clear();
    assert(card.handles == 1);
    --card.handles;
    return !card.consume(Fault::Close);
  }
  bool seek(size_t n) {
    if (n > size()) return false;
    pos = n;
    return true;
  }
};

struct {
  bool exists(const char* path) const { return card.files.contains(path); }
  bool remove(const char* path) const { return card.files.erase(path) > 0; }
  bool rename(const char* from, const char* to) const {
    if (!exists(from) || exists(to)) return false;
    if (std::string(from).ends_with(".tmp") && card.consume(Fault::Rename)) return false;
    auto file = card.files.extract(from);
    file.key() = to;
    card.files.insert(std::move(file));
    return true;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) const {
    assert(card.handles == 0);
    if (!exists(path.c_str())) return false;
    ++card.handles;
    if (path == "book") ++card.bookOpens;
    file.path = path;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) const {
    assert(card.handles == 0);
    card.files[path].clear();
    ++card.handles;
    file.path = path;
    return true;
  }
} Storage;

struct Txt {
  bool loaded = true;
  std::string cachePath = "/cache", filepath = "book";
  size_t getFileSize() const { return card.files.at(filepath).size(); }
  const std::string& getPath() const { return filepath; }
  bool readContent(uint8_t* out, size_t offset, size_t length) const;
  int readProgress(const char* tag, uint8_t* out, size_t bytes) const;
  bool writeProgress(const uint8_t* data, size_t bytes) const;
};

struct Allocator : PageIndexAllocator {
  static inline unsigned chunks = 0;
  static inline unsigned allowed = UINT32_MAX;
  template <typename T>
  std::unique_ptr<T> allocateObject() const {
    ++chunks;
    return chunks > allowed ? nullptr : PageIndexAllocator::allocateObject<T>();
  }
};

struct TxtReaderActivity {
  Txt* txt;
  int renderer = 0;
  size_t totalPages = 0, pageBytes = 37;
  ChunkedPageIndex<uint32_t, Allocator> pageOffsets{};
  struct {
    void showIndexing(int) {}
  } openingFeedback;
  explicit TxtReaderActivity(Txt& book) : txt(&book) {}
  bool appendPageOffset(uint32_t);
  int pageAtOffset(uint32_t) const;
  bool buildPageIndex();
  size_t wrapPage(uint8_t* window, size_t size, size_t offset, void*) {
    // A real bitmap SD font loads missing metrics from a second path during wrapping.
    if (SETTINGS.sdFontFamilyName[0]) {
      HalFile font;
      assert(Storage.openFileForRead("FONT", "font", font));
      font.close();
    }
    const auto& source = card.files.at(txt->filepath);
    assert(std::memcmp(window, source.data() + offset, size) == 0);
    return std::min(pageBytes, size);
  }
};

#include "production.inc"

void testIndex() {
  Txt book;
  TxtReaderActivity reader(book);
  card.files["book"].resize(32000);
  for (size_t i = 0; i < book.getFileSize(); ++i) card.files["book"][i] = i % 251;
  card.files["font"] = {1};
  for (bool sdFont : {false, true}) {
    SETTINGS.sdFontFamilyName[0] = sdFont ? 'X' : 0;
    card.bookBytes = card.bookOpens = 0;
    assert(reader.buildPageIndex());
    assert(card.handles == 0 && card.bookBytes == book.getFileSize());
    assert(sdFont ? card.bookOpens > 1 : card.bookOpens == 1);
    for (uint32_t i = 0; i < book.getFileSize(); ++i)
      assert(reader.pageAtOffset(i) == static_cast<int>(i / reader.pageBytes));
    card.fault = Fault::Read;
    assert(!reader.buildPageIndex() && card.handles == 0);
  }
  SETTINGS.sdFontFamilyName[0] = 0;
  Allocator::chunks = 0;
  Allocator::allowed = 1;
  assert(!reader.buildPageIndex() && card.handles == 0 && reader.pageOffsets.size() == 64);
  Allocator::allowed = UINT32_MAX;
  card.files["book"].resize(UINT16_MAX + 1);
  reader.pageBytes = 1;
  assert(!reader.buildPageIndex() && reader.pageOffsets.size() == UINT16_MAX && card.handles == 0);
  card.files["book"].pop_back();
  assert(reader.buildPageIndex() && reader.pageOffsets.size() == UINT16_MAX && card.handles == 0);
  card.files["book"].clear();
  assert(reader.buildPageIndex() && reader.pageOffsets.size() == 1 && reader.pageOffsets[0] == 0);
}

void testProgress() {
  Txt book;
  const std::array<uint8_t, 6> old{2, 0, 6, 7, 0, 0}, next{3, 0, 7, 8, 0, 0};
  std::array<uint8_t, 6> saved{};
  assert(book.readProgress("TEST", saved.data(), saved.size()) == 0);
  assert(book.writeProgress(old.data(), old.size()));
  for (Fault fault : {Fault::Write, Fault::Sync, Fault::Close, Fault::Rename}) {
    card.fault = fault;
    assert(!book.writeProgress(next.data(), next.size()));
    assert(book.readProgress("TEST", saved.data(), saved.size()) == 6 && old == saved);
    assert(card.handles == 0);
  }
  assert(book.writeProgress(next.data(), next.size()));
  card.fault = Fault::Read;
  assert(book.readProgress("TEST", saved.data(), saved.size()) == -1);
  assert(book.readProgress("TEST", saved.data(), saved.size()) == 6 && next == saved);
  // Power loss after the previous primary was renamed, before publishing its replacement.
  assert(Storage.rename("/cache/progress.bin", "/cache/progress.bin.bak"));
  assert(book.readProgress("TEST", saved.data(), saved.size()) == 6 && next == saved);
  assert(card.handles == 0);
}

int main() {
  for (const std::string glyph : {"é", "漢", "𐍈"}) {
    const std::string text = "a" + glyph;
    for (size_t cut = 1; cut <= text.size(); ++cut) {
      const size_t expected = cut == text.size() ? 0 : cut - 1;
      assert(unfinishedWindowSuffix(reinterpret_cast<const uint8_t*>(text.data()), cut) == expected);
    }
  }
  const uint8_t crlf[] = {'x', '\r', '\n'};
  assert(unfinishedWindowSuffix(crlf, 2) == 1 && unfinishedWindowSuffix(crlf, 3) == 0);
  testIndex();
  testProgress();
}
