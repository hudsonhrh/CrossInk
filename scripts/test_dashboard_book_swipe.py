#!/usr/bin/env python3
"""Exercise Dashboard book switching through the X4 Pro simulator's touch input."""

import argparse
import json
import os
from pathlib import Path
import shutil
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


def write_finished_epub(sd, path, short_read):
    shutil.copyfile(ROOT / "test/epubs/test_reader_rendering_matrix.epub", sd / path[1:])
    # EPUB cache names use a 64-bit FNV-1a hash of the absolute SD path.
    path_hash = 14695981039346656037
    for byte in path.encode("utf-8"):
        path_hash = ((path_hash ^ byte) * 1099511628211) & 0xffffffffffffffff
    cache = sd / ".crosspoint" / f"epub_{path_hash}"
    cache.mkdir()
    stats = bytearray(107)  # BookReadingStats v8 payload
    stats[0] = 8
    stats[11] = 1  # isCompleted
    stats[16] = (1 << 2) if short_read else 0  # FLAG_ARTICLE
    (cache / "stats_v8_crossink-simulator.bin").write_bytes(stats)


def check(program, theme, titles, sequence, *, browse=False, initial="", reference=None,
          finished_books=(), finished_short_reads=()):
    with tempfile.TemporaryDirectory(prefix="crossink-dashboard-") as directory:
        work = Path(directory)
        state = work / "fs_/.crosspoint"
        state.mkdir(parents=True)
        recent = []
        paths = {}
        for title in titles:
            finished = title in finished_books or title in finished_short_reads
            path = f"/{title}.epub" if finished else f"/{title}.txt"
            paths[title] = path
            recent.append({"path": path, "title": title, "coverState": 1})
            if finished:
                write_finished_epub(work / "fs_", path, title in finished_short_reads)
            elif title != "Deleted":
                (work / "fs_" / path[1:]).write_text(f"{title}\n" * 100)
        (state / "recent.json").write_text(json.dumps({"books": recent}))
        (state / "crossink-settings.json").write_text(json.dumps({"uiTheme": theme}))
        (state / "state.json").write_text(json.dumps({
            "showBootScreen": False, "lastSleepFromReader": False,
            "openEpubPath": paths.get(initial, ""), "releaseNotesSeen": 65535,
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
        elif sequence[-1]:
            assert "Entering activity: TxtReader" in log, log
            opened = json.loads((state / "state.json").read_text())["openEpubPath"]
            assert opened == paths[sequence[-1]], f"Tapped {sequence[-1]} but opened {opened}"
        else:
            assert log.count("Entering activity:") == 1, log
            assert json.loads((state / "recent.json").read_text())["books"] == recent
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
    empty = check(program, DASHBOARD, [], ["", ""])
    check(program, MINIMAL, history, ["Aster", "Birch", "Aster"], finished_books=["Birch"])
    finished = {"finished_books": ["Novel"], "finished_short_reads": ["Essay"]}
    check(program, DASHBOARD, ["Essay", "Aster", "Novel", "Birch", "Cedar", "Dune"],
          ["Aster", "Birch", "Cedar", "Aster"], initial="Essay", reference=frames, browse=True, **finished)
    check(program, DASHBOARD, ["Novel", "Essay"], ["", ""], initial="Novel", reference=empty, **finished)


if __name__ == "__main__":
    main()
