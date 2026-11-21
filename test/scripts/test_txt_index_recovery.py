#!/usr/bin/env python3
"""Run the TXT index and progress storage paths against a fault-injecting SD card."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def definition(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


reader = (ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text()
txt = (ROOT / "lib/Txt/Txt.cpp").read_text()
ops = txt[txt.index("const atomic_file::Operations"):txt.index("}  // namespace")]
functions = [ops, definition(reader, "size_t unfinishedWindowSuffix")]
for method in ["int Txt::readProgress", "bool Txt::writeProgress", "bool Txt::readContent"]:
    functions.append(definition(txt, method))
for method in ["bool TxtReaderActivity::appendPageOffset", "int TxtReaderActivity::pageAtOffset",
               "bool TxtReaderActivity::buildPageIndex"]:
    functions.append(definition(reader, method))

with tempfile.TemporaryDirectory(prefix="crossink-txt-") as temp:
    temp = Path(temp)
    (temp / "production.inc").write_text("\n\n".join(functions))
    executable = temp / "test"
    subprocess.run(["c++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-fno-sanitize-recover=undefined",
                    "-I" + str(ROOT / "lib/Utf8"), "-I" + str(ROOT / "lib/Memory"), "-I" + str(ROOT / "lib/Serialization"), "-I" + str(temp),
                    str(ROOT / "test/txt_index/TxtIndexRecovery.cpp"), str(ROOT / "lib/Serialization/AtomicFile.cpp"),
                    str(ROOT / "lib/Utf8/Utf8.cpp"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Passed TXT rolling reads, offset lookup, allocation and page limits, and interrupted bookmark saves")
