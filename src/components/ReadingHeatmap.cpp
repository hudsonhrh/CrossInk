#include "ReadingHeatmap.h"

#include "AppCapabilities.h"

#if CROSSINK_APP_READING_HEATMAP
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "activities/reader/ReadingStatsUtils.h"

namespace ReadingHeatmap {
void formatDuration(const uint32_t seconds, char* buffer, const size_t len) {
  if (seconds) {
    formatCompactReadingDuration(seconds, buffer, len);
  } else {
    snprintf(buffer, len, "%s", tr(STR_STATS_ZERO_MINUTES));
  }
}

void drawCell(const GfxRenderer& renderer, const int x, const int y, const int size, const uint8_t level,
              const bool black) {
  // Quarter-tone dots, a checkerboard, and reversed dots make five densities with one bit of ink.
  if (level >= 3) {
    renderer.fillRect(x, y, size, size, black);
  } else {
    renderer.drawRect(x, y, size, size, black);
  }
  if (level == 0 || level == 4) return;
  const bool checker = level == 2;
  const bool dotInk = level == 3 ? !black : black;
  for (int dy = 1; dy < size - 1; dy += checker ? 1 : 2) {
    for (int dx = checker ? 1 + (dy % 2) : 2; dx < size - 1; dx += 2) {
      renderer.drawPixel(x + dx, y + dy, dotInk);
    }
  }
}

void drawMonth(const GfxRenderer& renderer, const int x, const int y, const int size, const int gap,
               const uint32_t* seconds, const Scale scale, const bool black) {
  for (int i = 0; i < MonthDays; ++i) {
    drawCell(renderer, x + (i % MonthColumns) * (size + gap), y + (i / MonthColumns) * (size + gap), size,
             levelForSeconds(seconds[i], scale), black);
  }
}
}  // namespace ReadingHeatmap
#endif
