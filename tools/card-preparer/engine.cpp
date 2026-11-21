// Native driver for the firmware cache builders. All storage is disposable.
#include <Arduino.h>
#include <ContentProtection.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Xtc.h>
#include <ZipFile.h>

#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>

#include "components/themes/lyra/LyraCarouselTheme.h"

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

#undef Serial
ESPMock ESP;
HWCDC Serial;

namespace {
bool oversizedStylesheet = false;
}

void logPrintf(const char* level, const char* origin, const char* format, ...) {
  // Production marks intentional oversize skips complete. The desktop target
  // has a lower stylesheet cap than the X4 Pro, so let the device parse these.
  if (std::strcmp(origin, "EBP") == 0 && std::string_view(format).starts_with("CSS file too large (")) {
    oversizedStylesheet = true;
  }
  std::fprintf(stderr, "[%s] [%s] ", level, origin);
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
}

// This engine has no credentials or decryption path. The CLI also checks ZIP
// encryption flags before running it, including encrypted entries without XML.
std::unique_ptr<freeink::content::ContentDecryptor> freeink::content::openProtectedBook(
    const std::string& epubPath, std::string& error) {
  ZipFile zip(epubPath);
  if (!zip.open()) {
    error = "unreadable EPUB";
    return nullptr;
  }
  size_t bytes = 0;
  if (zip.getInflatedFileSize("META-INF/encryption.xml", &bytes) || zip.getInflatedFileSize("META-INF/license.lcpl", &bytes) ||
      zip.getInflatedFileSize("META-INF/rights.xml", &bytes)) {
    error = "protected EPUB";
  }
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "epub-key") == 0) {
    std::puts(Epub::cachePathForFilePath(argv[2], "").substr(1).c_str());
    return 0;
  }
  if (argc != 4 || argv[3][0] != '/') {
    std::fprintf(stderr, "Usage: crossink-prepare-engine {epub|xtch} SCRATCH /BOOK\n");
    return 2;
  }
  // Never point the HAL at the mounted card. Both the input snapshot and
  // firmware's cache output live under the caller's private workspace.
  if (setenv("CROSSPOINT_SIM_SD", argv[2], 1) != 0) return 2;
  const std::string path = argv[3];
  constexpr auto cacheRoot = "/.crosspoint";
  if (std::strcmp(argv[1], "epub") == 0) {
    Epub book(path, cacheRoot);
    if (!book.load(true, false, Epub::XLocationLoadMode::Skip) || book.getSpineItemsCount() <= 0) return 1;
    const auto& cache = book.getCachePath();
    if (oversizedStylesheet || book.getCssParser()->inspectCache() != CssParser::CacheStatus::Complete) {
      const auto rules = cache + "/css_rules.cache";
      if (Storage.exists(rules.c_str()) && !Storage.remove(rules.c_str())) return 1;
      std::fprintf(stderr, "CSS cache incomplete or unavailable, firmware will retry\n");
    }
    std::puts(cache.c_str());
    return 0;
  }
  if (std::strcmp(argv[1], "xtch") != 0) return 2;
  Xtc book(path, cacheRoot);
  if (!book.load() || book.getBitDepth() != 2) return 1;
  xtc::PageInfo firstPage{};
  if (!book.getPageInfo(0, firstPage)) return 1;
  using Theme = LyraCarouselTheme;
  if (!book.generateThumbBmp(Theme::kCenterCoverW, Theme::kCenterCoverH) ||
      !book.generateThumbBmp(Theme::kCenterThumbW, Theme::kCenterThumbH) ||
      !book.generateThumbBmp(Theme::kSideCoverW, Theme::kSideCoverH)) {
    return 1;
  }
  if (firstPage.height % 8 != 0 &&
      !Storage.exists((book.getCachePath() + "/cover_src_xtch_padded_v1.bin").c_str())) {
    return 1;
  }
  std::puts(book.getCachePath().c_str());
  return 0;
}
