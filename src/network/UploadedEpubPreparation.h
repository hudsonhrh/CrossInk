#pragma once

#include <cstddef>
#include <string_view>

namespace uploadedepub {

constexpr size_t MAX_PATH_BYTES = 1023;

inline bool isCanonicalPath(std::string_view path) {
  if (path.size() < 2 || path.size() > MAX_PATH_BYTES || path.front() != '/' ||
      path.find_first_of("\\:") != std::string_view::npos) {
    return false;
  }
  for (const unsigned char c : path) {
    if (c < 32 || c == 127) return false;
  }
  path.remove_prefix(1);
  while (true) {
    const auto slash = path.find('/');
    const auto component = path.substr(0, slash);
    // SdFat resolves edge spaces and trailing dots as aliases.
    if (component.empty() || component.front() == ' ' || component.back() == '.' || component.back() == ' ') {
      return false;
    }
    if (slash == std::string_view::npos) return true;
    path.remove_prefix(slash + 1);
  }
}

inline bool matchesCandidate(const std::string_view trustedPath, const size_t trustedSize,
                             const std::string_view requestedPath) {
  return trustedSize != 0 && trustedPath == requestedPath;
}

inline bool isOptimizedEntry(const std::string_view entry) {
  return entry == "META-INF/crossink/optimizer-v1.json";
}

inline bool isProtectedEntry(const std::string_view entry) {
  constexpr std::string_view paths[]{"meta-inf/encryption.xml", "meta-inf/license.lcpl", "meta-inf/rights.xml"};
  for (auto path : paths) {
    if (path.size() != entry.size()) continue;
    size_t i = 0;
    while (i < path.size()) {
      const char c = entry[i];
      if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != path[i]) break;
      ++i;
    }
    if (i == path.size()) return true;
  }
  return false;
}

}  // namespace uploadedepub
