#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace xtc {

// The XTH input runs down each column, from right to left, in two planes.
// Only unpack the column bytes that intersect this group of output rows.
inline void unpackXtchCoverBand(uint8_t* band, size_t rowBytes, size_t firstRow, size_t rows, size_t width,
                                size_t columnBytes, const uint8_t* data, size_t size, size_t offset) {
  const size_t planeBytes = width * columnBytes;
  const size_t firstByte = firstRow / 8;
  const size_t endByte = (firstRow + rows + 7) / 8;
  while (size) {
    const bool secondPlane = offset >= planeBytes;
    const size_t position = secondPlane ? offset - planeBytes : offset;
    const size_t x = width - 1 - position / columnBytes;
    const size_t columnY = position % columnBytes;
    const size_t take = std::min(size, columnBytes - columnY);
    const auto ink = static_cast<uint8_t>((secondPlane ? 1 : 2) << (6 - 2 * (x % 4)));
    const size_t stop = std::min(columnY + take, endByte);
    for (size_t b = std::max(columnY, firstByte); b < stop; ++b) {
      const uint8_t bits = data[b - columnY];
      const size_t localY = b * 8 - firstRow;
      const size_t count = std::min<size_t>(8, rows - localY);
      for (size_t bit = 0; bit < count; ++bit) {
        if (bits & (0x80 >> bit)) band[(localY + bit) * rowBytes + x / 4] |= ink;
      }
    }
    offset += take;
    data += take;
    size -= take;
  }
}
}  // namespace xtc
