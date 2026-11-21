"""Exercise the production sleep bitmap pipeline with observable panel operations."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/activities/boot_sleep/SleepActivity.cpp").read_text()
helpers = source[source.index("bool needsSleepImageScrub("):
                 source.index("ImageToneProfile bookCoverToneProfile(")]
bitmap_path = source[source.index("bool SleepActivity::renderBitmapSleepScreen("):
                     source.index("void SleepActivity::renderCoverSleepScreen(")]
notice_path = source[source.index("void SleepActivity::showSleepPopup()"):
                     source.index("bool SleepActivity::rendersBeforeExit()")]

actions = (root / "src/GlobalActions.h").read_text()
manual = actions[actions.index("inline HalDisplay::RefreshMode manualScreenRefreshMode("):
                 actions.index("inline bool isPowerButtonActionAvailableOutsideReader(")]

harness = r'''
#include <cassert>
#include <string>
#include <string_view>

struct HalDisplay {
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode { Direct, Absolute };
  enum class GrayscaleBase { Combined, Separate };
  struct Caps { GrayscaleBase base; };
  const char* controller{};
  bool combined = true;
  bool inverted = false;
  bool isInverted() const { return inverted; }
#ifndef SIMULATOR
  const char* getControllerName() const { return controller; }
  Caps grayscaleCapabilities(GrayscaleMode) const {
    // The real X4 Pro masks its absolute and direct capabilities in Night Mode.
    return {combined && !inverted ? GrayscaleBase::Combined : GrayscaleBase::Separate};
  }
#endif
} display;
struct {
  bool x4 = false;
  bool deviceIsX4() const { return x4; }
} gpio;
#if FREEINK_DEVICE_X4PRO && !defined(SIMULATOR)
constexpr bool physicalPro = true;
#else
constexpr bool physicalPro = false;
#endif
#include "util/X4ProRefresh.h"

struct GfxRenderer {
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  bool absolute = true, direct = false, reject = false;
  mutable RenderMode mode = BW;
  mutable std::string events;
  mutable bool white = false;
  int orientation = 0;
  int getOrientation() const { return orientation; }
  void setOrientation(int value) { orientation = value; }
  bool supportsAbsoluteGrayscale() const { return absolute; }
  bool supportsDirectGrayscale() const { return direct; }
  void clearScreen(unsigned char color = 0xff) const { white = display.inverted ? color == 0 : color == 0xff; }
  void displayBuffer(HalDisplay::RefreshMode m, bool off = false) const {
    events += m == HalDisplay::FAST_REFRESH ? "fast" : m == HalDisplay::HALF_REFRESH ? "half" : "full";
    events += white ? "-white" : "-image";
    events += off ? "-off " : " ";
  }
  void displayGrayscaleBase(HalDisplay::RefreshMode) const { events += "overlay "; }
  bool displayDirectGrayscaleBase() const { events += "direct "; return !reject; }
  bool displayAbsoluteGrayscaleBase() const { events += "absolute "; return !reject; }
  void setRenderMode(RenderMode m) const { mode = m; }
  void copyGrayscaleLsbBuffers() const { events += "lsb "; }
  void copyGrayscaleMsbBuffers() const { events += "msb "; }
  void displayGrayBuffer(bool off) const { assert(off); events += "gray-off "; }
};
struct Bitmap {
  bool gray = true;
  bool hasGreyscale() const { return gray; }
};
struct CrossPointSettings {
  enum SLEEP_SCREEN_COVER_FILTER { NO_FILTER };
  enum Mode { CUSTOM, COVER, COVER_CUSTOM, OVERLAY, COVER_OVERLAY, QUICK_RESUME, BLANK };
};
struct {
  int sleepScreenCoverFilter = CrossPointSettings::NO_FILTER;
  CrossPointSettings::Mode sleepScreen = CrossPointSettings::CUSTOM;
  bool usesQuickResumeSleep(bool timeout) { return timeout || sleepScreen == CrossPointSettings::QUICK_RESUME; }
} SETTINGS;
struct { bool lastSleepFromReader = true; } APP_STATE;
enum { STR_ENTERING_SLEEP };
const char* tr(int) { return ""; }
struct {
  struct Rect { int width; };
  Rect drawPopup(const GfxRenderer& r, const char*, bool = false) { r.events += "popup "; return {1}; }
} GUI;
constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;
struct SleepActivity {
  mutable GfxRenderer renderer;
  mutable bool monochromeFrame = false;
  int failDraw = 0;
  mutable int draws = 0;
  int prepareSleepBitmap(Bitmap&) const { return 0; }
  bool fromTimeout = false, sleepPopupShown = false, overlayBackgroundAvailable = false;
  int sleepPopupOrientation = 1;
  void renderLastScreenSleepScreen() const { renderer.events += "moon "; }
  void showSleepPopup();
  bool drawSleepBitmap(Bitmap&, int) const {
    renderer.white = false;
    return ++draws != failDraw;
  }
  bool renderBitmapSleepScreen(Bitmap&) const;
};
'''

cases = r'''
int main() {
  for (const bool combined : {false, true}) {
    display.combined = combined;
    display.controller = combined ? "UC8179" : "separate";
    using S = CrossPointSettings;
    for (const auto mode : {S::CUSTOM, S::COVER, S::COVER_CUSTOM, S::OVERLAY, S::COVER_OVERLAY, S::QUICK_RESUME, S::BLANK}) {
      for (const bool timeout : {false, true}) {
        SleepActivity notice;
        SETTINGS.sleepScreen = mode;
        notice.fromTimeout = timeout;
        display.inverted = true;
        notice.showSleepPopup();
        const bool blankNotice = physicalPro && combined && !timeout &&
                                 (mode == S::CUSTOM || mode == S::COVER || mode == S::COVER_CUSTOM);
        assert(notice.renderer.events == (blankNotice ? "fast-white " : "popup "));
        assert(display.inverted);
        assert(notice.renderer.getOrientation() == 0 && notice.sleepPopupShown);
        assert(notice.overlayBackgroundAvailable == (mode == S::OVERLAY && !timeout));
      }
    }
  }
  display.inverted = false;
  for (const std::string_view part : {"UC8179", "UC8279", "SSD1677"}) {
    display.controller = part.data();
    display.combined = true;
    const bool direct = part != "SSD1677";
    assert(manualScreenRefreshMode() == (physicalPro && direct ? HalDisplay::HALF_REFRESH : HalDisplay::FULL_REFRESH));
    const std::string cleanup = physicalPro && !direct ? "full" : "half";
    const std::string base = direct ? "direct " : "absolute ";
    const std::string prepass = physicalPro ? cleanup + "-white " : "";
    Bitmap bitmap;
    SleepActivity gray;
    gray.renderer.direct = direct;
    assert(gray.renderBitmapSleepScreen(bitmap));
    assert(gray.renderer.events == prepass + base + "lsb msb gray-off ");
    assert(gray.draws == (physicalPro || direct ? 2 : 3));
    assert(!gray.monochromeFrame && gray.renderer.mode == GfxRenderer::BW);

    SleepActivity mono;
    bitmap.gray = false;
    assert(mono.renderBitmapSleepScreen(bitmap));
    assert(mono.renderer.events == cleanup + "-image-off ");
    assert(mono.monochromeFrame && mono.draws == 1);
    bitmap.gray = true;
    SleepActivity filtered;
    SETTINGS.sleepScreenCoverFilter = 1;
    assert(filtered.renderBitmapSleepScreen(bitmap));
    assert(filtered.renderer.events == mono.renderer.events);
    SETTINGS.sleepScreenCoverFilter = CrossPointSettings::NO_FILTER;

    SleepActivity failure;
    failure.renderer.direct = direct;
    failure.failDraw = physicalPro || direct ? 2 : 3;
    assert(!failure.renderBitmapSleepScreen(bitmap));
    assert(failure.renderer.events == prepass + base + "lsb ");
    assert(failure.renderer.mode == GfxRenderer::BW);
    failure.renderer.events.clear();
    failure.renderer.white = false;
    failure.renderer.reject = true;
    // A composed overlay passes its BW frame through the shared helper.
    assert(!beginSleepImageGrayscale(failure.renderer, direct));
    assert(!failure.renderer.white);
    assert(failure.renderer.events == (physicalPro ? cleanup + "-image " : "") + base);
  }
  for (const char* unrecognized : {static_cast<const char*>(nullptr), "unknown"}) {
    display.controller = unrecognized;
    assert(manualScreenRefreshMode() == HalDisplay::FULL_REFRESH);
    assert(x4ProCleanRefreshMode(display, HalDisplay::FULL_REFRESH) == HalDisplay::FULL_REFRESH);
    assert(x4ProCleanRefreshMode(display, HalDisplay::HALF_REFRESH) == HalDisplay::HALF_REFRESH);
  }
#if FREEINK_DEVICE_X4
  gpio.x4 = true;
  assert(manualScreenRefreshMode() == HalDisplay::HALF_REFRESH);
#endif
  display.combined = false;
  SleepActivity separate;
  Bitmap image;
  assert(separate.renderBitmapSleepScreen(image));
  assert(separate.draws == 3 && separate.renderer.events == "absolute lsb msb gray-off ");
  SleepActivity overlay;
  overlay.renderer.absolute = false;
  assert(overlay.renderBitmapSleepScreen(image));
  assert(overlay.draws == 3 && overlay.renderer.events == "overlay lsb msb gray-off ");
}
'''

with tempfile.TemporaryDirectory() as temp:
    path = Path(temp) / "sleep.cpp"
    for header in ("HalDisplay.h", "BoardConfig.h"):
        (Path(temp) / header).touch()
    path.write_text(harness + manual + helpers + notice_path + bitmap_path + cases)
    for flags in ([], ["-DFREEINK_DEVICE_X4=1"], ["-DFREEINK_DEVICE_X4PRO=1"],
                  ["-DFREEINK_DEVICE_X4PRO=1", "-DSIMULATOR"]):
        binary = path.with_suffix("")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-UNDEBUG", "-I", temp, "-I", str(root / "src"),
                        *flags, str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
