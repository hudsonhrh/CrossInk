"""Exercise the two cached Section lookups with a counted in-memory file."""

import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
source = (HERE.parents[1] / "lib/Epub/Epub/Section.cpp").read_text()


def definition(signature):
    start = source.index(signature)
    end = source.index("\n}", start)
    return source[start:end + 2]


with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    # Section.cpp also builds the renderer. Compile the real lookup bodies alone,
    # as the clipping-selection host test does for its reader methods.
    header = source[source.index("constexpr uint32_t HEADER_SIZE ="):].split(";", 1)[0] + ";"
    methods = [definition("std::optional<uint16_t> " + name) for name in
               ("findPageInPositionIndex(", "Section::getPageForParagraphIndex(", "Section::getPageForListItemIndex(")]
    (work / "position_methods.inc").write_text("\n".join([header, *methods]))
    exe = work / "section_position_index"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", directory, str(HERE / "SectionPositionIndexTest.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
