#pragma once

#include <BoardConfig.h>
#include <HalDisplay.h>

#include <optional>
#include <string_view>

// The X4 Pro is shipped with three controllers. On UltraChip, HALF loads the
// previous plane with the target's complement so every pixel is driven. The
// SSD1677 instead selects its multi-inversion OTP clean waveform with FULL.
inline std::optional<HalDisplay::RefreshMode> x4ProCleanRefreshMode(const HalDisplay& panel) {
#if FREEINK_DEVICE_X4PRO && !defined(SIMULATOR)
  if (const char* name = panel.getControllerName()) {
    const std::string_view controller{name};
    if (controller == "UC8179" || controller == "UC8279") return HalDisplay::HALF_REFRESH;
    if (controller == "SSD1677") return HalDisplay::FULL_REFRESH;
  }
#else
  (void)panel;
#endif
  return std::nullopt;
}

inline HalDisplay::RefreshMode x4ProCleanRefreshMode(const HalDisplay& panel, HalDisplay::RefreshMode fallback) {
  return x4ProCleanRefreshMode(panel).value_or(fallback);
}
