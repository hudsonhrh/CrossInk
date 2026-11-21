"""Trace the actual stats renderer, goHome route and Home's one-shot flush on both board profiles."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
activities = root / "src/activities"


def function(path, signature):
    source = (activities / path).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


stats = function("reader/BookStatsActivity.cpp", "void BookStatsActivity::render(")
stats_header = (activities / "reader/BookStatsActivity.h").read_text()
stats_state = stats_header[stats_header.index("  Page page ="):stats_header.index("  int selectedEditField")]
history_entry = stats_header[stats_header.index("  void openHistoryPage("):stats_header.index("  void onFrontlightPanelClosed()")]
go_home = function("ActivityManager.cpp", "void ActivityManager::goHome(")
pending = function("ActivityManager.cpp", "void ActivityManager::processPendingActivities(")
# Compile the actual multi-pop loop, excluding the unrelated push/replace lifecycle and boot alerts.
pop = pending[:pending.index("    } else if (pendingActivity)")] + "    }\n  }\n}"
home = function("home/HomeActivity.cpp", "void HomeActivity::render(")
start = home.index("  const auto displayHomeBuffer =")
flush = home[start:home.index("\n  };", start) + len("\n  };")]
# Compile the normal layout's submission after the final menu drawing and before thumbnail work.
after_hints = home.index("\n", home.rindex("  GUI.drawButtonHints("))
flush_call = home[after_hints:home.index("  if (!recentsLoaded", after_hints)]
home_header = (activities / "home/HomeActivity.h").read_text()
ctor = home_header[home_header.index("  explicit HomeActivity("):home_header.index("  void onEnter() override;")]
def guarded_hook(header):
    member = header.index("setRestoreRefreshMode(")
    return header[header.rindex("#if ", 0, member):header.index("#endif", member) + len("#endif")]


restore = guarded_hook(home_header)
base_restore = guarded_hook((activities / "Activity.h").read_text())

harness = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "HalDisplay.h"

using Mode = HalDisplay::RefreshMode;
struct GfxRenderer {
  std::vector<Mode> frames;
  void displayBuffer(Mode mode = HalDisplay::FAST_REFRESH) { frames.push_back(mode); }
};
struct RenderLock { void unlock() {} };
struct MappedInputManager { void setMenuMode(bool) {} };
struct {
  bool showHeatmap = true;
  bool shouldShowReadingHeatmap() const { return showHeatmap; }
} SETTINGS;
// Drawing itself is outside this test. The two real render exits must each submit exactly one frame.
#define renderPerBookStatsPage(...) ((void)0)
#define renderGlobalStatsPage(...) ((void)0)
#define renderNoRtcCombinedStatsPage(...) ((void)0)
#define renderHeatmapStatsPage(...) ((void)0)
#define renderEditBookDatesPage(...) ((void)0)
#define drawPageIndicators(...) ((void)0)

struct BookStatsActivity {
  enum class Page { PerBook, ThisDevice, AllDevices, History, EditDates };
  GfxRenderer renderer;
  STATS_STATE
  bool showAllDevicesStats = true, noRtc = false;
  bool usesNoRtcSingleScreenLayout() const { return noRtc; }
#if CROSSINK_APP_READING_HEATMAP
  HISTORY_ENTRY
#endif
  void render(RenderLock&&);
};
enum class HomeMenuItem { NONE, FILE_BROWSER, PLUGINS, LIBRARY, FILE_TRANSFER, SETTINGS_MENU };
struct Activity {
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Activity(std::string n, GfxRenderer& r, MappedInputManager& m) : name(std::move(n)), renderer(r), mappedInput(m) {}
  virtual ~Activity() = default;
  virtual bool isHomeActivity() const { return false; }
  BASE_RESTORE
  bool usesMenuButtonOrientation() const { return true; }
  bool requiresFreshBackdrop() const { return false; }
  void onFrontlightPanelClosed() {}
  int result = 0;
  std::function<void(int)> resultHandler;
};
struct HomeActivity : Activity {
  Mode initialRefreshMode;
  HomeMenuItem initialMenuItem;
  std::string initialBookPath;
  HOME_CONSTRUCTOR
  HOME_RESTORE
  bool isHomeActivity() const override { return true; }
  void flushNormalLayout() {
    HOME_FLUSH
    HOME_FLUSH_CALL
  }
};
using ActivityResult = int;
enum class PendingAction { None, Pop, Replace };
struct TouchRegistry {
  static TouchRegistry& getInstance() { static TouchRegistry registry; return registry; }
  void clear() {}
};
struct { void ensureLoaded(GfxRenderer&) {} } filenameFontSystem;
#define LOG_ERR(...) ((void)0)
struct ActivityManager {
  GfxRenderer renderer;
  MappedInputManager mappedInput;
  std::unique_ptr<Activity> currentActivity, afterReturnActivity, pendingActivity;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  Activity* afterReturnOwner = nullptr;
  bool afterReturnOwnerExited = false, restoredActivityNeedsRender = false;
  PendingAction pendingAction = PendingAction::None;
  std::string preferredHomeBookPath;
  bool returningHomeThroughSettings = false;
  std::unique_ptr<HomeActivity> home;
  void replaceActivity(std::unique_ptr<HomeActivity> next) { home = std::move(next); }
  void goHome(HomeMenuItem, Mode);
  void processPendingActivities();
  void exitActivity(RenderLock&) { currentActivity.reset(); }
  void continueHomeReaderUnwind() {}
  void restoreBackdropBehindCurrentOverlay() { assert(false); }
  int updates = 0;
  void requestUpdate() { ++updates; }
};
'''.replace("STATS_STATE", stats_state).replace("HISTORY_ENTRY", history_entry).replace(
    "HOME_CONSTRUCTOR", ctor).replace("HOME_RESTORE", restore).replace("BASE_RESTORE", base_restore).replace(
    "HOME_FLUSH_CALL", flush_call).replace("HOME_FLUSH", flush)

cases = r'''
int main() {
  constexpr auto fast = HalDisplay::FAST_REFRESH;
  constexpr auto full = HalDisplay::FULL_REFRESH;
  constexpr auto homeAfterStats = FREEINK_DEVICE_X4PRO ? HalDisplay::HALF_REFRESH : fast;
  using Page = BookStatsActivity::Page;
  auto paint = [](BookStatsActivity& stats, Mode expected) {
    stats.render({});
    assert((stats.renderer.frames == std::vector<Mode>{expected}));
    stats.renderer.frames.clear();
  };
  // Ordinary entries and all later tab switches use the fast waveform.
  for (auto first : {Page::PerBook, Page::ThisDevice}) {
    BookStatsActivity stats;
    stats.page = first;
    paint(stats, fast);
    for (auto tab : {Page::ThisDevice, Page::AllDevices, Page::History, Page::AllDevices, Page::ThisDevice,
                     Page::PerBook, Page::EditDates, Page::EditDates, Page::PerBook}) {
      stats.page = tab;
      paint(stats, fast);
    }
  }
#if CROSSINK_APP_READING_HEATMAP
  // The Dashboard shortcut requests a clean transition exactly once. Returning to History within Stats stays fast.
  BookStatsActivity direct;
  direct.openHistoryPage();
  assert(direct.page == Page::History);
  paint(direct, HalDisplay::HALF_REFRESH);
  paint(direct, fast);
  direct.page = Page::ThisDevice;
  paint(direct, fast);
  direct.page = Page::History;
  paint(direct, fast);

  SETTINGS.showHeatmap = false;
  BookStatsActivity hidden;
  hidden.openHistoryPage();
  assert(hidden.page == Page::PerBook);
  paint(hidden, fast);
#endif
  BookStatsActivity combined;
  combined.noRtc = true;
  paint(combined, fast);
  paint(combined, fast);

  ActivityManager manager;
  auto source = [&](const char* name) { return std::make_unique<Activity>(name, manager.renderer, manager.mappedInput); };
  manager.currentActivity = source("BookStats");
  auto returnHome = [&](Mode requested, Mode expected) {
    manager.goHome(HomeMenuItem::NONE, requested);
    assert(manager.home);
    manager.home->flushNormalLayout();
    manager.home->flushNormalLayout();
    assert((manager.renderer.frames == std::vector<Mode>{expected, fast}));
    manager.renderer.frames.clear();
  };
  returnHome(fast, homeAfterStats);
  returnHome(full, full);
  returnHome(HalDisplay::HALF_REFRESH, HalDisplay::HALF_REFRESH);
  manager.currentActivity->name = "Library";
  returnHome(fast, fast);
  manager.currentActivity = nullptr;
  returnHome(fast, fast);

  // Capacitive Back can pop to an existing Home. Closing Stats via the frontlight
  // sheet queues its second pop from the sheet's result handler in the same pass.
  auto popHome = [&](const char* child, bool viaSheet, Mode queued, Mode expected) {
    auto home = std::make_unique<HomeActivity>(manager.renderer, manager.mappedInput, HomeMenuItem::NONE, queued);
    auto* restored = home.get();
    manager.stackActivities.push_back(std::move(home));
    if (viaSheet) {
      auto sheet = source("FrontlightPanel");
      sheet->resultHandler = [&](int) { manager.pendingAction = PendingAction::Pop; };
      manager.stackActivities.push_back(std::move(sheet));
    }
    manager.currentActivity = source(child);
    manager.pendingAction = PendingAction::Pop;
    manager.updates = 0;
    manager.processPendingActivities();
    assert(manager.currentActivity.get() == restored && manager.stackActivities.empty() && manager.updates == 1);
    restored->flushNormalLayout();
    restored->flushNormalLayout();
    assert((manager.renderer.frames == std::vector<Mode>{expected, fast}));
    manager.renderer.frames.clear();
  };
  popHome("BookStats", false, fast, homeAfterStats);
  popHome("BookStats", true, fast, homeAfterStats);
  popHome("BookStats", true, full, full);
  popHome("Settings", true, fast, fast);
  // The simulator wraps an actual Home in a different Activity subclass with this predicate.
  // Running with the undefined behavior sanitizer also detects an invalid concrete downcast here.
  struct WrappedHome : Activity {
    using Activity::Activity;
    bool isHomeActivity() const override { return true; }
  };
  auto wrapped = std::make_unique<WrappedHome>("Home", manager.renderer, manager.mappedInput);
  auto* original = wrapped.get();
  manager.stackActivities.push_back(std::move(wrapped));
  manager.currentActivity = source("BookStats");
  manager.pendingAction = PendingAction::Pop;
  manager.processPendingActivities();
  assert(manager.currentActivity.get() == original);
  // Without a parent the manager creates a new Home after destroying the source activity.
  manager.currentActivity = source("BookStats");
  manager.pendingAction = PendingAction::Pop;
  manager.processPendingActivities();
  manager.home->flushNormalLayout();
  assert((manager.renderer.frames == std::vector<Mode>{homeAfterStats}));
}
'''

with tempfile.TemporaryDirectory(prefix="crossink-stats-refresh-") as temp:
    path = Path(temp)
    (path / "HalDisplay.h").write_text("struct HalDisplay { enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH }; };")
    source = path / "navigation.cpp"
    source.write_text("\n".join((harness, stats, go_home, pop, cases)))
    for pro in (0, 1):
        binary = path / "test"
        subprocess.run([os.environ.get("CXX", "c++"), *shlex.split(os.environ.get("CXXFLAGS", "")),
                        "-std=c++17", "-UNDEBUG", f"-DFREEINK_DEVICE_X4PRO={pro}",
                        f"-DCROSSINK_APP_READING_HEATMAP={pro}", "-I", temp, "-I", str(activities),
                        str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
