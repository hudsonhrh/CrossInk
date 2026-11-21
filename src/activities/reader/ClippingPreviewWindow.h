#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "ClippingStore.h"
#include "clippings/ClippingPreview.h"

class ClippingPreviewWindow {
 public:
  static constexpr size_t CAPACITY = 20;

  void reserve(const size_t clippingCount) {
    for (size_t slot = 0; slot < std::min(clippingCount, CAPACITY); ++slot) {
      labels[slot].reserve(clippingPreview::MAX_BYTES + clippingPreview::ELLIPSIS_BYTES);
    }
  }

  void invalidate() { cachedCount = 0; }

  // Only cache a complete window, so a redraw can retry any failed reads.
  void load(const ClippingStore& store, const size_t first, const size_t count) {
    const size_t rows = std::min(count, CAPACITY);
    if (first == cachedFirst && rows == cachedCount) return;
    cachedFirst = first;
    cachedCount = store.readClippingPreviews(first, labels.data(), rows) ? rows : 0;
  }

  const std::string& operator[](const size_t slot) const { return labels[slot]; }

 private:
  std::array<std::string, CAPACITY> labels;
  size_t cachedFirst = 0;
  size_t cachedCount = 0;
};
