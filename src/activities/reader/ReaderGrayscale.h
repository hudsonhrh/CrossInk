#pragma once

#include <GfxRenderer.h>
#include <Logging.h>

namespace ReaderUtils {

enum class PageTone : uint8_t { Monochrome, GrayText, GrayImage };

// The saved 1-bit controller baseline represents intermediate shades as ink.
// On a transition to solid black, a differential refresh can therefore idle
// a pixel that is physically gray. An illustrated page also leaves more charge
// than the next ordinary text-AA waveform is intended to clear (#2190).
class PageToneHistory {
 public:
  bool advanceTo(PageTone next) {
    const bool clean = (last == PageTone::GrayImage && next != PageTone::GrayImage) ||
                       (last == PageTone::GrayText && next == PageTone::Monochrome);
    last = next;
    return clean;
  }

  HalDisplay::RefreshMode monochromeScreenMode() {
    return advanceTo(PageTone::Monochrome) ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  }

 private:
  PageTone last = PageTone::Monochrome;
};

// Separate-base panels scrub first on a cleanup turn. The X3 then needs its
// calibrated settle before mask uploads. Combined panels perform their own
// scrub when staging a non-fast base.
inline void displayReaderGrayscaleBase(const GfxRenderer& renderer, HalDisplay::RefreshMode mode) {
  if (mode != HalDisplay::FAST_REFRESH && !renderer.combinesGrayscaleBase()) {
    renderer.displayBuffer(mode);
    renderer.preconditionGrayscale();
  } else {
    renderer.displayGrayscaleBase(mode);
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    // Combined drivers still owe the page its initial B/W presentation.
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
