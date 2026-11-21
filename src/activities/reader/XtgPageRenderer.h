#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

namespace xtc {
// XTG rows are byte-padded, with zero bits for ink. Solid black runs can
// share a single rotated span. Only mixed bytes need individual pixel writes.
inline void drawXtgPage(const GfxRenderer& renderer, const uint8_t* data, const int width, const int height) {
  const int stride = (width + 7) / 8;
  const int visibleWidth = std::min(width, renderer.getScreenWidth());
  const int visibleHeight = std::min(height, renderer.getScreenHeight());
  for (int y = 0; y < visibleHeight; ++y) {
    const uint8_t* row = data + y * stride;
    for (int x = 0; x < visibleWidth; x += 8) {
      const uint8_t pixels = row[x / 8];
      if (pixels == 0xff) continue;
      if (pixels == 0) {
        const int start = x;
        while (x + 8 < visibleWidth && row[x / 8 + 1] == 0) x += 8;
        renderer.fillRect(start, y, std::min(x + 8, visibleWidth) - start, 1);
      } else {
        for (int bit = 0; bit < 8 && x + bit < visibleWidth; ++bit) {
          if ((pixels & (0x80 >> bit)) == 0) renderer.drawPixel(x + bit, y, true);
        }
      }
    }
  }
}
}  // namespace xtc
