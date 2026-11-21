#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ZipFileStreamReader {};

class ZipFile {
 public:
  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };

  struct EntryTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
    const char* path;
  };
  struct EntryIdentity {
    uint32_t crc32 = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    bool found = false;
  };

  // Optional fixture directory, in physical archive order.
  static inline std::vector<std::string> entries;
  static inline std::vector<size_t> batches;
  static inline size_t failBatch = 0;
  static inline size_t individualLookups = 0;
  uint32_t directoryCursorPosition() const { return cursor; }
  static void clearFixture() {
    entries.clear();
    batches.clear();
    failBatch = individualLookups = 0;
  }
  explicit ZipFile(const std::string&) {}
  bool open() { return true; }
  void close() {}
  bool getInflatedFileSize(const char* path, size_t* size) {
    if (!entries.empty()) {
      ++individualLookups;
      const auto it = std::find(entries.begin(), entries.end(), path);
      if (it == entries.end()) return false;
      cursor = static_cast<uint32_t>(it - entries.begin()) + 1;
      *size = cursor;  // Each physical entry has a distinct size.
      return true;
    }
    *size = 1;
    return true;
  }
  static uint64_t fnvHash64(const char*, size_t) { return 0; }
  int fillEntryIdentities(const EntryTarget* targets, size_t count, EntryIdentity* out, size_t length, bool requireUniqueNames) {
    batches.push_back(count);
    if (failBatch == batches.size()) return -1;
    int matches = 0;
    for (size_t i = 0; i < count; ++i) {
      const auto& t = targets[i];
      const auto it = std::find(entries.begin(), entries.end(), t.path);
      if (it != entries.end() && t.index < length) {
        if (requireUniqueNames && std::find(it + 1, entries.end(), t.path) != entries.end()) return -1;
        out[t.index].found = true;
        out[t.index].uncompressedSize = static_cast<uint32_t>(it - entries.begin()) + 1;
        ++matches;
      }
    }
    return matches;
  }
  int fillUncompressedSizes(const SizeTarget*, size_t, uint32_t*, size_t) { return 0; }

 private:
  uint32_t cursor = 0;
};
