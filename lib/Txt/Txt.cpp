#include "Txt.h"

#include <AtomicFile.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>

namespace {
const atomic_file::Operations progressFileOps{
    nullptr,
    [](void*, const char* path) { return Storage.exists(path); },
    [](void*, const char* path) { return Storage.remove(path); },
    [](void*, const char* from, const char* to) { return Storage.rename(from, to); },
};
}  // namespace

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("TXT", "File does not exist: %s", filepath.c_str());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    LOG_ERR("TXT", "Failed to open file: %s", filepath.c_str());
    return false;
  }

  const uint64_t bytes = file.fileSize64();
  file.close();
  if (bytes > UINT32_MAX) {
    LOG_ERR("TXT", "Book is too large for 32-bit TXT offsets: %s", filepath.c_str());
    return false;
  }
  fileSize = static_cast<size_t>(bytes);

  loaded = true;
  return true;
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .txt extension
  if (FsHelpers::hasTxtExtension(filename)) {
    filename.resize(filename.length() - 4);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath(ImageToneProfile toneProfile, bool* sourceBitmap) const {
  if (sourceBitmap) *sourceBitmap = false;
  if (toneProfile == ImageToneProfile::PiccoCover) {
    // A copied sidecar still needs tone conversion. A JPEG-derived cache already has it.
    auto sourcePath = piccoSourceCoverPath();
    if (Storage.exists(sourcePath.c_str())) {
      if (sourceBitmap) *sourceBitmap = true;
      return sourcePath;
    }
  }
  return cachePath + "/cover" + coverToneSuffix(toneProfile) + ".bmp";
}

bool Txt::generateCoverBmp(ImageToneProfile toneProfile) const {
  // Already generated, return true
  std::string destination = getCoverBmpPath(toneProfile);
  if (Storage.exists(destination.c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    if (toneProfile == ImageToneProfile::PiccoCover) destination = piccoSourceCoverPath();
    // Copy BMP file to cache
    HalFile src, dst;
    if (!Storage.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", destination, dst)) {
      src.close();
      return false;
    }
    uint8_t buffer[256];
    bool success = true;
    while (src.available()) {
      const int bytesRead = src.read(buffer, sizeof(buffer));
      if (bytesRead <= 0 || dst.write(buffer, bytesRead) != static_cast<size_t>(bytesRead)) {
        LOG_ERR("TXT", "Failed to copy BMP cover");
        success = false;
        break;
      }
    }
    src.close();
    dst.close();
    if (!success) Storage.remove(destination.c_str());
    return success;
  } else if (FsHelpers::hasJpgExtension(coverImagePath)) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    HalFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", destination, coverBmp)) {
      coverJpg.close();
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, true, toneProfile);

    coverJpg.close();
    coverBmp.close();
    if (!success) {
      LOG_ERR("TXT", "Failed to generate BMP from JPG cover image");
      Storage.remove(destination.c_str());
    }
    return success;
  }

  // PNG files are not supported (would need a PNG decoder)
  LOG_ERR("TXT", "Cover image format not supported (only BMP/JPG/JPEG)");
  return false;
}

bool Txt::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("TXT", "Failed to clear cache");
    return false;
  }

  return true;
}

int Txt::readProgress(const char* tag, uint8_t* data, const size_t capacity) const {
  const std::string path = cachePath + "/progress.bin";
  if (!atomic_file::recover(progressFileOps, path.c_str(), (path + ".bak").c_str())) {
    LOG_ERR(tag, "Failed to recover TXT progress");
    return -1;
  }
  if (!Storage.exists(path.c_str())) return 0;
  HalFile file;
  if (!Storage.openFileForRead(tag, path, file)) return -1;
  const size_t expected = std::min(file.size(), capacity);
  const int bytes = file.read(data, expected);
  const bool closed = file.close();
  if (!closed || bytes != static_cast<int>(expected)) {
    LOG_ERR(tag, "Incomplete TXT progress read");
    return -1;
  }
  return bytes;
}

bool Txt::writeProgress(const uint8_t* data, const size_t size) const {
  const std::string path = cachePath + "/progress.bin";
  const std::string backup = path + ".bak";
  const std::string temporary = path + ".tmp";
  if (!atomic_file::recover(progressFileOps, path.c_str(), backup.c_str())) {
    LOG_ERR("TRS", "Failed to recover TXT progress before saving");
    return false;
  }
  HalFile file;
  if (!Storage.openFileForWrite("TRS", temporary, file)) return false;
  const bool synced = file.write(data, size) == size && file.sync();
  const bool closed = file.close();
  if (!synced || !closed || !atomic_file::publish(progressFileOps, path.c_str(), temporary.c_str(), backup.c_str())) {
    LOG_ERR("TRS", "Failed to publish TXT progress");
    return false;
  }
  return true;
}

bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    return false;
  }

  const int bytesRead = file.seek(offset) ? file.read(buffer, length) : -1;
  const bool ok = bytesRead >= 0 && static_cast<size_t>(bytesRead) == length;
  file.close();
  if (!ok) LOG_ERR("TXT", "Incomplete content read at %zu (%zu bytes)", offset, length);
  return ok;
}
