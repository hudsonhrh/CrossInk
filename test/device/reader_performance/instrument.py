"""Install the same optional measurement probes in a disposable CrossInk source tree."""
import argparse
import os
from pathlib import Path
import re


def install(root, label):
    header = root / "include/ReaderHardwareProbe.h"
    specs = {
        "src/activities/reader/ReaderActivity.cpp": [],
        "src/activities/util/BmpViewerActivity.cpp": [
            ("BmpViewerActivity::drawImage", "filePath.c_str()", "image-paint", "-1"),
        ],
        "src/activities/reader/TxtReaderActivity.cpp": [
            ("TxtReaderActivity::buildPageIndex", "txt ? txt->getPath().c_str() : nullptr", "txt-index", "-1"),
            ("TxtReaderActivity::renderPage", "txt ? txt->getPath().c_str() : nullptr", "txt-paint", "currentPage"),
        ],
        "src/activities/reader/EpubReaderActivity.cpp": [
            ("EpubReaderActivity::render", "epub ? epub->getPath().c_str() : nullptr", "epub-render", "currentSpineIndex"),
        ],
        "src/activities/reader/XtcReaderActivity.cpp": [
            ("XtcReaderActivity::renderPage", "xtc ? xtc->getPath().c_str() : nullptr", "xtc-paint", "pageToRender"),
        ],
    }
    pending = {}
    for name, operations in specs.items():
        target = root / name
        source = target.read_text()
        if "ReaderHardwareProbe.h" in source:
            raise ValueError(f"{target} has already been instrumented")
        if name.endswith("/ReaderActivity.cpp"):
            # Only the foreground open has this precise cache state. Library thumbnail
            # scans and recent-book metadata reads must not enter the measurements.
            anchor = "  const bool loaded = epub->load("
            end = "Epub::XLocationLoadMode::Immediate, true);"
            start = source.index(anchor)
            stop = source.index(end, start) + len(end)
            assert source.count(anchor) == 1
            call = source[start:stop].replace(anchor, "    return epub->load(", 1)
            source = (source[:start] + "  const bool loaded = [&]() {\n"
                      + '    ReaderHardwareProbe kitProbe(epub->getPath().c_str(), uncached ? "epub-cold" : "epub-warm", -1);\n'
                      + call + "\n  }();"+ source[stop:])
        for function, path, operation, pos in operations:
            pattern = rf"^(?:bool|void) {function}\([^;{{]+?\)\s*\{{|^(?:bool|void) {function}\(\)\s*\{{"
            matches = list(re.finditer(pattern, source, re.MULTILINE))
            if len(matches) != 1:
                raise ValueError(f"Expected one {function} definition; found {len(matches)}")
            end = matches[0].end()
            source = source[:end] + f'\n  ReaderHardwareProbe kitProbe({path}, "{operation}", {pos});' + source[end:]
        pending[target] = f'#include "{os.path.relpath(header, target.parent)}"\n' + source
    header.write_text(Path(__file__).with_name("ReaderHardwareProbe.h.in").read_text().replace("@BUILD@", label))
    for target, source in pending.items():
        target.write_text(source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disposable_tree", type=Path)
    parser.add_argument("label", choices=["A", "B"])
    args = parser.parse_args()
    install(args.disposable_tree.resolve(), args.label)
