#!/usr/bin/env python3
"""Exercise Dashboard book switching through the X4 Pro simulator's touch input."""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DASHBOARD = 6
MINIMAL = 5


def book_pixels(path):
    # Compare the middle of the Home screen, excluding the status bar (clock).
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    assert data[:2] == b"BM" and bits in (24, 32), "Expected an SDL screenshot"
    stride = (width * bits + 31) // 32 * 4
    height = abs(signed_height)
    return data[offset + height // 8 * stride:offset + height * 7 // 8 * stride]


def check(program, theme, titles, sequence, *, browse=False, initial="", reference=None):
    with tempfile.TemporaryDirectory(prefix="crossink-dashboard-") as directory:
        work = Path(directory)
        state = work / "fs_/.crosspoint"
        state.mkdir(parents=True)
        recent = []
        for title in titles:
            path = f"/{title}.txt"
            recent.append({"path": path, "title": title, "coverState": 1})
            if title != "Deleted":
                (work / "fs_" / path[1:]).write_text(f"{title}\n" * 100)
        (state / "recent.json").write_text(json.dumps({"books": recent}))
        (state / "crossink-settings.json").write_text(json.dumps({"uiTheme": theme}))
        (state / "state.json").write_text(json.dumps({
            "showBootScreen": False, "lastSleepFromReader": False,
            "openEpubPath": f"/{initial}.txt" if initial else "", "releaseNotesSeen": 65535,
        }))

        actions = []
        screenshots = []
        for i in range(len(sequence)):
            when = 2000 + 1000 * i
            if i:
                actions.append(f"{when - 600}:SWIPE:0.75,0.35,0.25,0.35,180")
            screenshots.append(f"{when}:{work / f'book-{i}.bmp'}")
        final_gesture = "SWIPE:0.25,0.35,0.75,0.35,180" if browse else "TAP:0.30,0.35,100"
        actions.extend((f"{when + 400}:{final_gesture}", f"{when + 1600}:QUIT"))
        env = {k: v for k, v in os.environ.items() if not k.startswith(("CROSSPOINT_SIM_", "CROSSINK_SIMULATOR_SMOKE"))}
        env.update(SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", CROSSPOINT_SIM_WAKE_REASON="power",
                   CROSSPOINT_SIM_INPUT_SCRIPT=";".join(actions), CROSSPOINT_SIM_SCREENSHOTS=";".join(screenshots))
        result = subprocess.run([str(program)], cwd=work, env=env, capture_output=True, text=True, timeout=30)
        log = result.stdout + result.stderr
        assert result.returncode == 0 and "Entering activity: Home" in log, log

        frames = {}
        for i, title in enumerate(sequence):
            screenshot = work / f"book-{i}.bmp"
            assert screenshot.is_file(), log
            pixels = book_pixels(screenshot)
            if reference is not None:
                assert pixels == reference[title], f"Expected the cover for {title} at position {i}"
            if title in frames:
                assert pixels == frames[title], f"Swipe {i} did not return to {title}"
            else:
                assert pixels not in frames.values(), f"Swipe {i} did not display {title}"
                frames[title] = pixels

        if browse:
            assert "Entering activity: FileBrowser" in log, log
            assert json.loads((state / "recent.json").read_text())["books"] == recent
        elif titles:
            assert "Entering activity: TxtReader" in log, log
            opened = json.loads((state / "state.json").read_text())["openEpubPath"]
            assert opened == f"/{sequence[-1]}.txt", f"Tapped {sequence[-1]} but opened {opened}"
        else:
            assert log.count("Entering activity:") == 1, log
        print(f"PASS theme={theme} history={titles} displayed={sequence} browse={browse}", flush=True)
        return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, default=ROOT / ".pio/build/x4-pro-simulator/program")
    program = parser.parse_args().program.resolve()
    history = ["Aster", "Deleted", "Birch", "Cedar", "Dune"]
    frames = check(program, DASHBOARD, history, ["Aster", "Birch", "Cedar", "Aster", "Birch", "Cedar"])
    check(program, DASHBOARD, history, ["Cedar", "Aster"], browse=True, initial="Cedar", reference=frames)
    check(program, DASHBOARD, history[:3], ["Aster", "Birch", "Aster"])
    check(program, DASHBOARD, ["Aster"], ["Aster", "Aster"])
    check(program, DASHBOARD, [], ["", ""])
    check(program, MINIMAL, history, ["Aster", "Birch", "Aster"])


if __name__ == "__main__":
    main()
