#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

namespace ReadingHeatmap {
constexpr int MonthRows = 3;
constexpr int MonthColumns = 10;
constexpr int MonthDays = MonthRows * MonthColumns;
enum class Scale : uint8_t { Standard, Extended };

constexpr int thresholdMinutes(const Scale scale, const int boundaryIndex) {
  constexpr uint8_t standard[] = {15, 45, 90};
  return standard[boundaryIndex] * (scale == Scale::Extended ? 2 : 1);
}

// The final element is today. Wait for completed days so the scale stays steady during a session.
constexpr Scale scaleForMonth(const uint32_t* seconds, const size_t count = MonthDays) {
  constexpr int longDaysNeeded = 6;
  const uint32_t longDaySeconds = thresholdMinutes(Scale::Standard, 2) * 60;
  int longDays = 0;
  const size_t first = count > MonthDays ? count - MonthDays : 0;
  for (size_t day = first; day + 1 < count; ++day) {
    if (seconds[day] >= longDaySeconds) ++longDays;
  }
  return longDays >= longDaysNeeded ? Scale::Extended : Scale::Standard;
}

constexpr uint8_t levelForSeconds(const uint32_t seconds, const Scale scale) {
  if (seconds == 0) return 0;
  for (int boundary = 0; boundary < 3; ++boundary) {
    if (seconds < static_cast<uint32_t>(thresholdMinutes(scale, boundary)) * 60) return boundary + 1;
  }
  return 4;
}

void formatDuration(uint32_t seconds, char* buffer, size_t len);
void drawCell(const GfxRenderer& renderer, int x, int y, int size, uint8_t level, bool black = true);
void drawMonth(const GfxRenderer& renderer, int x, int y, int size, int gap, const uint32_t* seconds, Scale scale,
               bool black = true);
}  // namespace ReadingHeatmap
