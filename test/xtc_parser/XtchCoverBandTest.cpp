#include <XtchCoverBand.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

TEST(XtchCoverBandTest, PartialColumnsAndSplitPlanesMatchIndependentPixelDecoder) {
  constexpr std::array<std::pair<size_t, size_t>, 8> sizes{{
      {1, 1},
      {2, 7},
      {5, 9},
      {31, 15},
      {79, 65},
      {319, 417},
      {480, 800},
      {800, 480},
  }};
  for (const auto& [width, height] : sizes) {
    const size_t rowBytes = (width + 3) / 4;
    const size_t columnBytes = (height + 7) / 8;
    const size_t planeBytes = width * columnBytes;
    std::vector<uint8_t> file(2 * planeBytes);
    uint32_t generator = 17;
    for (auto& byte : file) {
      generator = 1664525u * generator + 1013904223u;
      byte = static_cast<uint8_t>(generator >> 24);
    }
    std::vector<uint8_t> expected(rowBytes * height, 0);
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x) {
        const size_t address = (width - 1 - x) * columnBytes + y / 8;
        const unsigned bit = 7 - y % 8;
        const unsigned color = ((file[address] >> bit) & 1) * 2 + ((file[planeBytes + address] >> bit) & 1);
        expected[y * rowBytes + x / 4] |= static_cast<uint8_t>(color << (6 - (x % 4) * 2));
      }
    }
    for (const size_t bandRows : {8, 32, 64, 256}) {
      for (const size_t chunk : {17, 1024, 4093}) {
        SCOPED_TRACE(::testing::Message() << width << 'x' << height << " band=" << bandRows << " chunk=" << chunk);
        std::vector<uint8_t> output(rowBytes * height, 0);
        for (size_t row = 0; row < height; row += bandRows) {
          const size_t rows = std::min(height - row, bandRows);
          for (size_t offset = 0; offset < file.size(); offset += chunk) {
            xtc::unpackXtchCoverBand(output.data() + row * rowBytes, rowBytes, row, rows, width, columnBytes,
                                     file.data() + offset, std::min(chunk, file.size() - offset), offset);
          }
        }
        EXPECT_EQ(expected, output);
      }
    }
  }
}
