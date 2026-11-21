#pragma once

#include <GfxRenderer.h>
#include <Xtc/XtcTypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xtc {
enum class XtchRenderPass { Base, Lsb, Msb };

// XTH columns start on the right and run top to bottom. In normal portrait,
// a panel-sized page is already in physical framebuffer order, including the bits.
inline bool hasNativeXtchLayout(const GfxRenderer& renderer, int width, int height) {
  return renderer.getOrientation() == GfxRenderer::Portrait && width == renderer.getDisplayHeight() &&
         height == renderer.getDisplayWidth() && height % 8 == 0 && renderer.getDisplayWidthBytes() == height / 8 &&
         renderer.getWriteOriginY() == 0 && renderer.getWriteRows() == width;
}

// An upright XTH column runs along a physical framebuffer row. Smaller pages
// leave margins and larger ones are clipped to the panel.
inline bool canWriteXtchColumns(const GfxRenderer& renderer) {
  return renderer.getOrientation() == GfxRenderer::Portrait &&
         renderer.getDisplayWidthBytes() >= (renderer.getDisplayWidth() + 7) / 8 && renderer.getWriteOriginY() == 0 &&
         renderer.getWriteRows() == renderer.getDisplayHeight();
}

// One contiguous piece of one source plane. With both PSRAM masks available,
// stage the first plane in msb, then derive all three outputs as the second
// plane arrives. Framebuffer ones mean white; mask ones select the gray drive.
inline void drawNativeXtchPlane(uint8_t* base, const uint8_t* data, size_t size, bool secondPlane, XtchRenderPass pass,
                                uint8_t* lsb, uint8_t* msb, bool* hasGray) {
  // Only unequal source bits need a gray pulse. The base can hold the first
  // plane until the second arrives, even on a single-framebuffer device.
  if (secondPlane && pass == XtchRenderPass::Base && hasGray && !*hasGray) {
    *hasGray = memcmp(msb ? msb : base, data, size) != 0;
  }
  if (msb != nullptr) {
    if (!secondPlane) {
      memcpy(msb, data, size);
      return;
    }
    for (size_t i = 0; i < size; ++i) {
      const uint8_t first = msb[i];
      const uint8_t second = data[i];
      base[i] = ~(first | second);
      lsb[i] = ~first & second;
      msb[i] = first ^ second;
    }
    return;
  }

  if (!secondPlane) {
    if (pass != XtchRenderPass::Lsb) {
      memcpy(base, data, size);
    } else {
      for (size_t i = 0; i < size; ++i) base[i] = ~data[i];
    }
  } else if (pass == XtchRenderPass::Base) {
    for (size_t i = 0; i < size; ++i) base[i] = ~(base[i] | data[i]);
  } else if (pass == XtchRenderPass::Lsb) {
    for (size_t i = 0; i < size; ++i) base[i] &= data[i];
  } else {
    for (size_t i = 0; i < size; ++i) base[i] ^= data[i];
  }
}

inline void drawCroppedXtchColumns(const GfxRenderer& renderer, const uint8_t* data, size_t size, size_t offset,
                                   int width, int height, bool secondPlane, XtchRenderPass pass, uint8_t* lsb,
                                   uint8_t* msb, bool* hasGray) {
  const size_t columnBytes = (height + 7) / 8;
  const int visibleHeight = std::min<int>(height, renderer.getDisplayWidth());
  const size_t wholeBytes = visibleHeight / 8;
  const int tailBits = visibleHeight % 8;
  while (size > 0) {
    const size_t yByte = offset % columnBytes;
    const size_t count = std::min(size, columnBytes - yByte);
    // XTH starts with the rightmost logical column. An oversize page starts off-panel.
    const int panelRow = renderer.getDisplayHeight() - width + static_cast<int>(offset / columnBytes);
    if (panelRow >= 0 && yByte < wholeBytes + (tailBits != 0)) {
      const size_t target = static_cast<size_t>(panelRow) * renderer.getDisplayWidthBytes() + yByte;
      uint8_t* base = renderer.getWriteTarget() + target;
      uint8_t* lsbTarget = lsb ? lsb + target : nullptr;
      uint8_t* msbTarget = msb ? msb + target : nullptr;
      const size_t wholeCount = std::min(count, wholeBytes - yByte);
      drawNativeXtchPlane(base, data, wholeCount, secondPlane, pass, lsbTarget, msbTarget, hasGray);
      // XTH packs each column independently. Its padding may contain arbitrary bits.
      if (tailBits && yByte + wholeCount == wholeBytes && wholeCount < count) {
        const uint8_t visible = data[wholeCount] & (0xff << (8 - tailBits));
        drawNativeXtchPlane(base + wholeCount, &visible, 1, secondPlane, pass,
                            lsbTarget ? lsbTarget + wholeCount : nullptr, msbTarget ? msbTarget + wholeCount : nullptr,
                            hasGray);
      }
    }
    offset += count;
    data += count;
    size -= count;
  }
}

inline void drawXtchPlane(const GfxRenderer& renderer, const uint8_t* data, size_t size, size_t offset, int width,
                          int height, bool secondPlane, XtchRenderPass pass) {
  const size_t columnBytes = (height + 7) / 8;
  for (size_t i = 0; i < size; ++i) {
    const size_t column = (offset + i) / columnBytes;
    if (column >= static_cast<size_t>(width)) break;
    const int x = width - 1 - static_cast<int>(column);
    const int yBase = static_cast<int>((offset + i) % columnBytes) * 8;
    for (int bit = 0; bit < 8 && yBase + bit < height; ++bit) {
      const bool set = data[i] & (0x80 >> bit);
      if (pass == XtchRenderPass::Base && set) {
        renderer.drawPixel(x, yBase + bit, true);
      } else if (pass == XtchRenderPass::Lsb && !set) {
        renderer.drawPixel(x, yBase + bit, secondPlane);
      } else if (pass == XtchRenderPass::Msb && set) {
        renderer.drawPixel(x, yBase + bit, secondPlane && !renderer.isPixelBlack(x, yBase + bit));
      }
    }
  }
}

// Chunks may straddle the boundary between the two XTH planes. Only panel-sized
// portrait pages are contiguous. Other upright pages are clipped column by
// column; rotated pages and partial strip targets use logical pixels.
inline bool drawXtchChunk(const GfxRenderer& renderer, const uint8_t* data, size_t size, size_t offset, int width,
                          int height, XtchRenderPass pass, bool native, uint8_t* lsb = nullptr, uint8_t* msb = nullptr,
                          bool* hasGray = nullptr) {
  const size_t planeSize = xtchPlaneBytes(width, height);
  if (offset > 2 * planeSize || size > 2 * planeSize - offset) return false;
  while (size > 0) {
    const bool secondPlane = offset >= planeSize;
    const size_t planeOffset = secondPlane ? offset - planeSize : offset;
    const size_t count = std::min(size, planeSize - planeOffset);
    if (native) {
      drawNativeXtchPlane(renderer.getWriteTarget() + planeOffset, data, count, secondPlane, pass,
                          lsb ? lsb + planeOffset : nullptr, msb ? msb + planeOffset : nullptr, hasGray);
    } else if (canWriteXtchColumns(renderer)) {
      drawCroppedXtchColumns(renderer, data, count, planeOffset, width, height, secondPlane, pass, lsb, msb, hasGray);
    } else {
      drawXtchPlane(renderer, data, count, planeOffset, width, height, secondPlane, pass);
    }
    offset += count;
    data += count;
    size -= count;
  }
  return true;
}
}  // namespace xtc
