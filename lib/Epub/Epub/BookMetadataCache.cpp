#include "BookMetadataCache.h"

#include <AtomicFile.h>
#include <BufferedFile.h>
#include <Logging.h>
#include <Memory.h>
#include <PoolBudget.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <limits>

#include "FsHelpers.h"

namespace {
constexpr uint32_t BOOK_CACHE_MAGIC = 0x425843FF;  // bytes: 0xFF, "CXB"
constexpr uint8_t BOOK_CACHE_VERSION = 10;         // v10: namespace-aware OPF parsing
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpBookBinFile[] = "/book.bin.tmp";
constexpr char backupBookBinFile[] = "/book.bin.bak";
const atomic_file::Operations metadataFileOps{
    nullptr,
    [](void*, const char* path) { return Storage.exists(path); },
    [](void*, const char* path) { return Storage.remove(path); },
    [](void*, const char* from, const char* to) { return Storage.rename(from, to); },
};
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
constexpr size_t METADATA_ARENA_SLAB_BYTES = 4096;
// Reuse at most 32 KiB of arena-backed targets instead of one per chapter.
// This transient buffer is too large for the stack and is released after indexing.
constexpr size_t MAX_SIZE_LOOKUP_TARGETS = 2048;
// Buffer size for the buildBookBin streams. 3 buffers x 4KB, transient (freed on
// return); 4KB = 8 SD sectors per transfer, enough to stop the sector-cache thrash.
constexpr size_t BUILD_IO_BUFFER_SIZE = 4096;
// Cap the reader-session speedup at 4KB on constrained C3 devices. EPUBs with
// larger spines keep using the existing on-disk lookup path.
constexpr uint16_t MAX_CACHED_CUMULATIVE_SIZES = 1024;

// Entry (de)serializers, templated so they run over HalFile and the Buffered*
// wrappers alike (two instantiations each -- a few hundred bytes of flash, in
// exchange for the build path streaming at SD speed instead of per-pod).
template <typename F>
uint32_t writeSpineEntryTo(F& file, const BookMetadataCache::SpineEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

template <typename F>
uint32_t writeTocEntryTo(F& file, const BookMetadataCache::TocEntry& entry) {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

template <typename F>
bool readMetadataString(F& file, size_t fileSize, std::string& value) {
  uint32_t len = 0;
  if (file.read(&len, sizeof(len)) != sizeof(len)) return false;
  // Most hrefs fit in one sector. A short read detects truncated small fields
  // directly; check long lengths against the file before resizing the string.
  if (len > 256) {
    const size_t pos = file.position();
    if (pos > fileSize || len > fileSize - pos || len > std::numeric_limits<int>::max()) return false;
  }
  value.resize(len);
  return len == 0 || file.read(value.data(), len) == static_cast<int>(len);
}

template <typename F>
bool readSpineEntryFrom(F& file, size_t fileSize, BookMetadataCache::SpineEntry& entry) {
  uint8_t fields[sizeof(entry.cumulativeSize) + sizeof(entry.tocIndex)];
  if (!readMetadataString(file, fileSize, entry.href) || file.read(fields, sizeof(fields)) != sizeof(fields))
    return false;
  memcpy(&entry.cumulativeSize, fields, sizeof(entry.cumulativeSize));
  memcpy(&entry.tocIndex, fields + sizeof(entry.cumulativeSize), sizeof(entry.tocIndex));
  return true;
}

template <typename F>
bool readTocEntryFrom(F& file, size_t fileSize, BookMetadataCache::TocEntry& entry) {
  return readMetadataString(file, fileSize, entry.title) && readMetadataString(file, fileSize, entry.href) &&
         readMetadataString(file, fileSize, entry.anchor) && file.read(&entry.level, sizeof(entry.level)) == sizeof(entry.level) &&
         file.read(&entry.spineIndex, sizeof(entry.spineIndex)) == sizeof(entry.spineIndex);
}

constexpr int SMALL_SPINE_BATCH_LIMIT = 48;

// A small spine can still live in a ZIP with thousands of images. Match its
// normalized chapter names in bounded batches so ZIP entry order does not
// force a complete directory scan for every out-of-order chapter.
std::unique_ptr<uint32_t[]> lookupSmallSpineSizes(serialization::BufferedFileReader& spine, const size_t spineBytes,
                                                  const uint16_t count, ZipFile& zip) {
  struct Batch {
    ZipFile::EntryTarget targets[SMALL_SPINE_BATCH_LIMIT];
    ZipFile::EntryIdentity identities[SMALL_SPINE_BATCH_LIMIT];
    char names[2048];
  };
  // One transient work buffer plus four bytes per spine entry. Keep the
  // existing cursor-based lookup available when the internal heap is tight.
  if (!MemoryBudget::canAllocateInternal(sizeof(Batch) + count * sizeof(uint32_t),
                                         MemoryBudget::EPUB_INFLATE_INTERNAL_RESERVE))
    return nullptr;
  auto batch = makeUniqueNoThrow<Batch>();
  auto sizes = makeUniqueNoThrow<uint32_t[]>(count);
  if (!batch || !sizes) {
    LOG_ERR("BMC", "Small spine size lookup scratch unavailable; using individual ZIP lookups");
    return nullptr;
  }
  uint16_t first = 0;
  uint16_t pending = 0;
  size_t used = 0;
  auto resolve = [&]() {
    std::sort(batch->targets, batch->targets + pending,
              [](const auto& a, const auto& b) { return a.hash < b.hash || (a.hash == b.hash && a.len < b.len); });
    std::fill_n(batch->identities, pending, ZipFile::EntryIdentity{});
    if (zip.fillEntryIdentities(batch->targets, pending, batch->identities, pending, /*requireUniqueNames=*/true) < 0)
      return false;
    for (uint16_t i = 0; i < pending; ++i) sizes[first + i] = batch->identities[i].uncompressedSize;
    first += pending;
    pending = 0;
    used = 0;
    return true;
  };
  for (uint16_t i = 0; i < count; ++i) {
    BookMetadataCache::SpineEntry row;
    if (!readSpineEntryFrom(spine, spineBytes, row)) return nullptr;
    const std::string path = FsHelpers::normalisePath(row.href);
    if (path.size() >= sizeof(batch->names)) return nullptr;
    if ((pending == SMALL_SPINE_BATCH_LIMIT || path.size() >= sizeof(batch->names) - used) && !resolve()) return nullptr;
    char* name = batch->names + used;
    memcpy(name, path.c_str(), path.size() + 1);
    used += path.size() + 1;
    batch->targets[pending] = {ZipFile::fnvHash64(name, path.size()), static_cast<uint16_t>(path.size()), pending,
                               name};
    ++pending;
  }
  if (!resolve()) return nullptr;
  return sizes;
}

}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  loaded = false;
  bookFile.close();
  lowMemoryFailure = false;
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  // Open spine file for writing
  if (!Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  // Wrapper OOM is fine: createSpineEntry falls back to unbuffered writes.
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(spineFile, BUILD_IO_BUFFER_SIZE);
  return true;
}

bool BookMetadataCache::endContentOpfPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  if (!flushed) {
    LOG_ERR("BMC", "Failed writing spine tmp file");
  }
  return flushed;
}

bool BookMetadataCache::beginTocPass() {
  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  tocReadFailed = false;
  const size_t spineBytes = spineFile.size();
  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.resetStorage();
    spineHrefIndexArena.release();
    if (!spineHrefIndexArena.init(METADATA_ARENA_SLAB_BYTES) || !spineHrefIndex.resize(spineCount)) {
      LOG_ERR("BMC", "Failed to allocate spine href index arena for %u spine items", spineCount);
      lowMemoryFailure = true;
      spineHrefIndex.resetStorage();
      spineHrefIndexArena.release();
      tocFile.close();
      spineFile.close();
      return false;
    }
    for (int i = 0; i < spineCount; i++) {
      SpineEntry entry;
      if (!readSpineEntryFrom(spineFile, spineBytes, entry)) {
        LOG_ERR("BMC", "Failed reading temporary spine row %d", i);
        endTocPass();
        return false;
      }
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                if (a.hrefHash != b.hrefHash) return a.hrefHash < b.hrefHash;
                if (a.hrefLen != b.hrefLen) return a.hrefLen < b.hrefLen;
                return a.spineIndex < b.spineIndex;
              });
    useSpineHrefIndex = true;
  } else {
    useSpineHrefIndex = false;
  }

  // Share the existing 4 KiB TOC writer budget. Even at 299 rows, the
  // writer retains over three SD sectors. Keep the linear scan on tight heaps.
  indexedRows = 0;
  nextRowOffset = 0;
  spineRows.reset();
  if (!useSpineHrefIndex && spineCount >= 8 &&
      MemoryBudget::canAllocateInternal(BUILD_IO_BUFFER_SIZE, MemoryBudget::EPUB_INFLATE_INTERNAL_RESERVE)) {
    spineRows = makeUniqueNoThrow<SpineRow[]>(spineCount);
  }
  const size_t writerCapacity = BUILD_IO_BUFFER_SIZE - (spineRows ? spineCount * sizeof(SpineRow) : 0);
  passOut = makeUniqueNoThrow<serialization::BufferedFileWriter>(tocFile, writerCapacity);
  return true;
}

bool BookMetadataCache::endTocPass() {
  const bool flushed = !passOut || passOut->flush();
  passOut.reset();
  if (!flushed) {
    LOG_ERR("BMC", "Failed writing toc tmp file");
  }
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();
  spineRows.reset();

  spineHrefIndex.resetStorage();
  spineHrefIndexArena.release();
  useSpineHrefIndex = false;

  return flushed && !tocReadFailed;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) return false;
  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    spineFile.close();
    return false;
  }
  const std::string temporary = cachePath + tmpBookBinFile;
  if (!Storage.openFileForWrite("BMC", temporary, bookFile)) {
    spineFile.close();
    tocFile.close();
    return false;
  }
  // writeBookData owns the buffers: even on an error their destructors run
  // while their files are open. A failed build must never publish its header.
  const bool written = writeBookData(epubPath, metadata) && bookFile.sync();
  const bool closed = bookFile.close();
  spineFile.close();
  tocFile.close();
  if (!written || !closed) {
    LOG_ERR("BMC", "Failed building metadata cache");
    Storage.remove(temporary.c_str());
    return false;
  }

  const std::string destination = cachePath + bookBinFile;
  const std::string backup = cachePath + backupBookBinFile;
  if (!atomic_file::recover(metadataFileOps, destination.c_str(), backup.c_str()) ||
      !atomic_file::publish(metadataFileOps, destination.c_str(), temporary.c_str(), backup.c_str())) {
    LOG_ERR("BMC", "Failed publishing metadata cache");
    Storage.remove(temporary.c_str());
    return false;
  }
  return true;
}

bool BookMetadataCache::writeBookData(const std::string& epubPath, const BookMetadata& metadata) {
  const size_t spineFileSize = spineFile.size();
  const size_t tocFileSize = tocFile.size();
  constexpr uint32_t headerASize = sizeof(BOOK_CACHE_MAGIC) + sizeof(BOOK_CACHE_VERSION) +
                                   /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  // Buffered streams for the whole build: every access below is sequential per
  // file, but interleaved ACROSS files, which thrashes SdFat's single shared
  // sector cache when unbuffered (one 512B SD transaction per 4-byte pod --
  // measured 31s for a 1,732-spine omnibus). Three 4KB buffers, freed on return.
  serialization::BufferedFileWriter bookOut(bookFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader spineIn(spineFile, BUILD_IO_BUFFER_SIZE);
  serialization::BufferedFileReader tocIn(tocFile, BUILD_IO_BUFFER_SIZE);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookOut, BOOK_CACHE_MAGIC);
  serialization::writePod(bookOut, BOOK_CACHE_VERSION);
  serialization::writePod(bookOut, lutOffset);
  serialization::writePod(bookOut, spineCount);
  serialization::writePod(bookOut, tocCount);
  // Metadata
  serialization::writeString(bookOut, metadata.title);
  serialization::writeString(bookOut, metadata.author);
  serialization::writeString(bookOut, metadata.language);
  serialization::writeString(bookOut, metadata.coverItemHref);
  serialization::writeString(bookOut, metadata.textReferenceHref);

  // Loop through spine entries, writing LUT positions
  for (int i = 0; i < spineCount; i++) {
    const uint32_t pos = spineIn.position();
    SpineEntry entry;
    if (!readSpineEntryFrom(spineIn, spineFileSize, entry)) return false;
    serialization::writePod(bookOut, pos + lutOffset + lutSize);
  }
  // Total size of the spine tmp file: entries land in book.bin after the toc LUT
  // and the full spine block, so toc LUT positions are offset by it.
  const auto spineBytes = static_cast<uint32_t>(spineIn.position());

  // Loop through toc entries, writing LUT positions
  for (int i = 0; i < tocCount; i++) {
    const uint32_t pos = tocIn.position();
    TocEntry entry;
    if (!readTocEntryFrom(tocIn, tocFileSize, entry)) return false;
    serialization::writePod(bookOut, pos + lutOffset + lutSize + spineBytes);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  Arena metadataArena;
  if (!metadataArena.init(METADATA_ARENA_SLAB_BYTES)) {
    LOG_ERR("BMC", "Failed to allocate metadata scratch arena (%u bytes)",
            static_cast<unsigned>(METADATA_ARENA_SLAB_BYTES));
    lowMemoryFailure = true;
    return false;
  }

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  ArenaVector<int16_t> spineToTocIndex(metadataArena);
  if (!spineToTocIndex.resize(spineCount)) {
    LOG_ERR("BMC", "Failed to allocate spine-to-TOC index for %u spine items", spineCount);
    lowMemoryFailure = true;
    return false;
  }
  for (size_t i = 0; i < spineToTocIndex.size(); ++i) {
    spineToTocIndex[i] = -1;
  }
  if (!tocIn.seek(0)) return false;
  for (int j = 0; j < tocCount; j++) {
    TocEntry tocEntry;
    if (!readTocEntryFrom(tocIn, tocFileSize, tocEntry)) return false;
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    return false;
  }
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we use a bounded batch lookup that scans the ZIP
  // central directory once per chunk and matches spine targets by hash.
  // Extra scans for huge books trade I/O for bounded target scratch memory.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  ArenaVector<uint32_t> spineSizes(metadataArena);
  bool useBatchSizes = false;

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    ArenaVector<ZipFile::SizeTarget> targets(metadataArena);
    const size_t batchCapacity = std::min<size_t>(spineCount, MAX_SIZE_LOOKUP_TARGETS);
    if (!targets.resize(batchCapacity) || !spineSizes.resize(spineCount)) {
      LOG_ERR("BMC", "Failed to allocate batch size lookup scratch for %u spine items", spineCount);
      lowMemoryFailure = true;
      zip.close();
      return false;
    }

    size_t targetCount = 0;
    int matched = 0;
    if (!spineIn.seek(0)) {
      zip.close();
      return false;
    }
    for (int i = 0; i < spineCount; i++) {
      SpineEntry entry;
      if (!readSpineEntryFrom(spineIn, spineFileSize, entry)) {
        zip.close();
        return false;
      }
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets[targetCount++] = t;
      if (targetCount < batchCapacity && i + 1 < spineCount) continue;

      std::sort(targets.begin(), targets.begin() + targetCount,
                [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
                  return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
                });
      matched += zip.fillUncompressedSizes(targets.data(), targetCount, spineSizes.data(), spineSizes.size());
      targetCount = 0;
    }
    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  if (!spineIn.seek(0)) {
    zip.close();
    return false;
  }
  int lastSpineTocIndex = -1;
  int directoryWraps = 0;
  int lastWrap = -1;
  int batchStart = -1;
  std::unique_ptr<uint32_t[]> smallSizes;
  for (int i = 0; i < spineCount; i++) {
    // Each batch can cost a complete directory scan. Require that many recent
    // wraps before trading away the forward cursor: a reversed cover/title
    // followed by correctly packaged chapters should stay on the cheap path.
    const int remaining = spineCount - i;
    constexpr int minRemainingChapters = 24;
    const int expectedScans = (remaining + SMALL_SPINE_BATCH_LIMIT - 1) / SMALL_SPINE_BATCH_LIMIT;
    if (lastWrap == i - 1 && batchStart < 0 && remaining >= minRemainingChapters &&
        directoryWraps >= std::max(2, expectedScans)) {
      batchStart = i;
      const size_t position = spineIn.position();
      smallSizes = lookupSmallSpineSizes(spineIn, spineFileSize, remaining, zip);
      if (!spineIn.seek(position)) {
        LOG_ERR("BMC", "Failed to restore spine after ZIP size lookup");
        zip.close();
        return false;
      }
    }
    SpineEntry spineEntry;
    if (!readSpineEntryFrom(spineIn, spineFileSize, spineEntry)) {
      zip.close();
      return false;
    }

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (smallSizes || useBatchSizes) {
      itemSize = smallSizes ? smallSizes[i - batchStart] : spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      const uint32_t previous = zip.directoryCursorPosition();
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      } else if (previous != 0 && zip.directoryCursorPosition() <= previous) {
        // A long run of forward matches makes earlier wraps a poor predictor.
        directoryWraps = i - lastWrap > minRemainingChapters ? 1 : directoryWraps + 1;
        lastWrap = i;
      }
    }

    constexpr size_t maxStoredCumulativeSize = std::numeric_limits<uint32_t>::max();
    if (itemSize > maxStoredCumulativeSize || cumSize > maxStoredCumulativeSize - itemSize) {
      LOG_ERR("BMC", "Spine cumulative size overflow for item %d (cumSize=%u, itemSize=%zu)", i, cumSize, itemSize);
      zip.close();
      return false;
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeSpineEntryTo(bookOut, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  if (!tocIn.seek(0)) return false;
  for (int i = 0; i < tocCount; i++) {
    TocEntry tocEntry;
    if (!readTocEntryFrom(tocIn, tocFileSize, tocEntry)) return false;
    writeTocEntryTo(bookOut, tocEntry);
  }

  return bookOut.flush();
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  return writeSpineEntryTo(file, entry);
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  return writeTocEntryTo(file, entry);
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  if (passOut) {
    writeSpineEntryTo(*passOut, entry);
  } else {
    writeSpineEntry(spineFile, entry);
  }
  spineCount++;
}

bool BookMetadataCache::findSpineRow(const std::string& href, int16_t& index) {
  const uint32_t hash = spineRows ? static_cast<uint32_t>(fnvHash64(href)) : 0;
  SpineEntry entry;
  const size_t spineBytes = spineFile.size();
  for (uint16_t i = 0; i < indexedRows; ++i) {
    if (spineRows[i].hash != hash) continue;
    if (!spineFile.seek(spineRows[i].offset) || !readSpineEntryFrom(spineFile, spineBytes, entry)) return false;
    if (entry.href == href) {
      index = static_cast<int16_t>(i);
      return true;
    }
  }

  // The unread suffix starts immediately after the last indexed row. Extending
  // lazily also avoids an extra spine pass for books with a sparse TOC.
  if (indexedRows >= spineCount) return true;
  if (!spineFile.seek(nextRowOffset)) return false;
  for (uint16_t i = indexedRows; i < spineCount; ++i) {
    if (!readSpineEntryFrom(spineFile, spineBytes, entry)) return false;
    if (spineRows) {
      spineRows[i] = {static_cast<uint32_t>(fnvHash64(entry.href)), nextRowOffset};
      nextRowOffset = static_cast<uint32_t>(spineFile.position());
      ++indexedRows;
    }
    if (entry.href == href) {
      index = static_cast<int16_t>(i);
      break;
    }
  }
  return true;  // A missing href is valid in an EPUB.
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  if (tocReadFailed) return;
  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    if (!findSpineRow(href, spineIndex)) {
      LOG_ERR("BMC", "Failed reading temporary spine during TOC lookup");
      tocReadFailed = true;
      return;
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  // Compose the title to NFC at index time so the cache stores precomposed glyphs;
  // device fonts have no combining-mark positioning, so NFD titles render broken.
  const TocEntry entry(utf8ComposeNfc(title), href, anchor, level, spineIndex);
  if (passOut) {
    writeTocEntryTo(*passOut, entry);
  } else {
    writeTocEntry(tocFile, entry);
  }
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::exists(const std::string& cachePath) {
  return Storage.exists((cachePath + bookBinFile).c_str()) || Storage.exists((cachePath + backupBookBinFile).c_str());
}

bool BookMetadataCache::load() {
  loaded = false;
  const auto bookBinPath = cachePath + bookBinFile;
  bookFile.close();
  if (!atomic_file::recover(metadataFileOps, bookBinPath.c_str(), (cachePath + backupBookBinFile).c_str())) {
    LOG_ERR("BMC", "Failed restoring previous metadata cache");
    return false;
  }
  if (!Storage.openFileForRead("BMC", bookBinPath, bookFile)) {
    return false;
  }

  uint32_t magic;
  if (!serialization::tryReadPod(bookFile, magic)) {
    LOG_DBG("BMC", "Cache header is truncated");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }
  if (magic != BOOK_CACHE_MAGIC) {
    LOG_DBG("BMC", "Cache magic mismatch");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }

  uint8_t version;
  if (!serialization::tryReadPod(bookFile, version)) {
    LOG_DBG("BMC", "Cache version is missing");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }

  const size_t fileBytes = bookFile.size();
  if (!serialization::tryReadPod(bookFile, lutOffset) || !serialization::tryReadPod(bookFile, spineCount) ||
      !serialization::tryReadPod(bookFile, tocCount) || !readMetadataString(bookFile, fileBytes, coreMetadata.title) ||
      !readMetadataString(bookFile, fileBytes, coreMetadata.author) ||
      !readMetadataString(bookFile, fileBytes, coreMetadata.language) ||
      !readMetadataString(bookFile, fileBytes, coreMetadata.coverItemHref) ||
      !readMetadataString(bookFile, fileBytes, coreMetadata.textReferenceHref) ||
      bookFile.position() != lutOffset || lutOffset > fileBytes ||
      (static_cast<size_t>(spineCount) + tocCount) > (fileBytes - lutOffset) / sizeof(uint32_t)) {
    LOG_DBG("BMC", "Cache metadata is truncated");
    bookFile.close();
    Storage.remove(bookBinPath.c_str());
    return false;
  }

  cacheSpineCumulativeSizes();
  loaded = true;
  return true;
}

void BookMetadataCache::cacheSpineCumulativeSizes() {
  cumulativeSizes.reset();
  cumulativeSizeCount = 0;
  if (!cacheCumulativeSizes || spineCount == 0 || spineCount > MAX_CACHED_CUMULATIVE_SIZES) {
    return;
  }

  auto sizes = makeUniqueNoThrow<uint32_t[]>(spineCount);
  if (!sizes) {
    LOG_DBG("BMC", "Could not allocate %u-byte cumulative size cache; using SD lookups",
            static_cast<unsigned>(spineCount * sizeof(uint32_t)));
    return;
  }

  const uint32_t lutSize = (static_cast<uint32_t>(spineCount) + tocCount) * sizeof(uint32_t);
  if (!bookFile.seek(lutOffset + lutSize)) {
    LOG_DBG("BMC", "Could not seek to spine data for cumulative size cache; using SD lookups");
    return;
  }

  for (uint16_t i = 0; i < spineCount; ++i) {
    uint32_t hrefLen = 0;
    int16_t tocIndex = -1;
    if (!serialization::tryReadPod(bookFile, hrefLen) || !bookFile.seekCur(hrefLen) ||
        !serialization::tryReadPod(bookFile, sizes[i]) || !serialization::tryReadPod(bookFile, tocIndex)) {
      LOG_DBG("BMC", "Could not populate cumulative size cache; using SD lookups");
      return;
    }
  }

  cumulativeSizes = std::move(sizes);
  cumulativeSizeCount = spineCount;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  SpineEntry entry;
  if (!seekToEntry(index) || !readSpineEntryFrom(bookFile, bookFile.size(), entry)) {
    LOG_ERR("BMC", "Failed reading spine entry %d", index);
    return {};
  }
  return entry;
}

size_t BookMetadataCache::getSpineCumulativeSize(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineCumulativeSize called but cache not loaded");
    return 0;
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineCumulativeSize index %d out of range", index);
    return 0;
  }

  if (index < static_cast<int>(cumulativeSizeCount)) {
    return cumulativeSizes[index];
  }

  // Seek to spine LUT item, then read only the cumulative size field from the entry.
  uint32_t hrefLen = 0;
  uint32_t cumulativeSize = 0;
  if (!seekToEntry(index) || !serialization::tryReadPod(bookFile, hrefLen) || !bookFile.seekCur(hrefLen) ||
      !serialization::tryReadPod(bookFile, cumulativeSize)) {
    LOG_ERR("BMC", "Failed reading cumulative size %d", index);
    return 0;
  }
  return static_cast<size_t>(cumulativeSize);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  TocEntry entry;
  if (!seekToEntry(spineCount + index) || !readTocEntryFrom(bookFile, bookFile.size(), entry)) {
    LOG_ERR("BMC", "Failed reading TOC entry %d", index);
    return {};
  }
  return entry;
}

bool BookMetadataCache::seekToEntry(size_t index) {
  uint32_t pos = 0;
  const size_t dataStart = lutOffset + (static_cast<size_t>(spineCount) + tocCount) * sizeof(uint32_t);
  return bookFile.seek(lutOffset + index * sizeof(uint32_t)) && serialization::tryReadPod(bookFile, pos) &&
         pos >= dataStart && bookFile.seek(pos);
}
