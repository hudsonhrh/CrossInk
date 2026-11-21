#!/usr/bin/env python3
"""Exercise the production preparer on a disposable macOS FAT32 disk image."""

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools/card-preparer"
sys.path.insert(0, str(TOOLS))
from prepare import RECEIPT, cache_keys, esp_hash  # noqa: E402

THUMBS = {"thumb_200x390.bmp", "thumb_296x468.bmp", "thumb_340x540.bmp"}


def epub(path, title="Original", css="p { margin-top: 1em; text-align: center; }", protection=None):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mimetype", "application/epub+zip")
        z.writestr("META-INF/container.xml", """<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
          <rootfiles><rootfile full-path="OPS/volume.opf" media-type="application/oebps-package+xml"/></rootfiles>
          </container>""")
        css_item = '<item id="css" href="style.dat" media-type="text/css"/>' if css is not None else ""
        z.writestr("OPS/volume.opf", f"""<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
          <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{title}</dc:title>
          <dc:creator>CrossInk Fixture</dc:creator><dc:language>en</dc:language></metadata>
          <manifest><item id="text" href="chapter.xhtml" media-type="application/xhtml+xml"/>
          {css_item}<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
          <spine><itemref idref="text"/></spine></package>""")
        z.writestr("OPS/nav.xhtml", """<html xmlns="http://www.w3.org/1999/xhtml"
          xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol>
          <li><a href="chapter.xhtml">Chapter</a></li></ol></nav></body></html>""")
        link = '<link rel="stylesheet" href="style.dat"/>' if css is not None else ""
        z.writestr("OPS/chapter.xhtml", f"""<html xmlns="http://www.w3.org/1999/xhtml">
          <head>{link}</head><body><p>The same page on cold and prepared cards.</p></body></html>""")
        if css is not None:
            z.writestr("OPS/style.dat", css)
        if protection:
            z.writestr(protection, "<EncryptedData/>")


def xtch(path, height=417):
    width = 319
    plane_bytes = width * ((height + 7) // 8)
    pixels = bytes((n * 29 + n // 29) % 256 for n in range(plane_bytes * 2))
    page = struct.pack("<4sHHBBIQ", b"XTH\0", width, height, 0, 0, len(pixels), 0) + pixels
    header = struct.pack("<4sBBHBBBBIQQQQII", b"XTCH", 1, 0, 1, 0, 0, 0, 0, 0, 0, 56, 72, 0, 0, 0)
    path.write_bytes(header + struct.pack("<QIHH", 72, len(page), width, height) + page)


@contextmanager
def fat_volume(work, name):
    mount = work / name
    mount.mkdir()
    image = work / (name + ".dmg")
    subprocess.run(["hdiutil", "create", "-size", "96m", "-fs", "MS-DOS FAT32", "-layout", "NONE",
                    "-volname", "CI-PREP-TEST", image], check=True, timeout=30)
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", mount, image], check=True, timeout=30)
    try:
        assert mount.is_mount()
        yield mount
    finally:
        subprocess.run(["hdiutil", "detach", mount], check=True, timeout=30)


def cli(engine, volume, *extra, staging=False):
    return [sys.executable, str(TOOLS / "prepare.py"), "--engine", str(engine),
            "--staging-directory" if staging else "--volume", str(volume), *extra]


def run(args, expected=0):
    process = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=30)
    print(process.stdout, end="", flush=True)
    assert process.returncode == expected, (args, process.returncode, process.stdout)
    return process.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def publish_tests(engine, work, card):
    books = card / "Shelf A" / "MixedCase"
    books.mkdir(parents=True)
    book = books / "My Book.EPUB"
    panels = books / "Panels.xtch"
    wide = books / "Aligned.xtch"
    epub(book)
    xtch(panels)
    xtch(wide, 416)
    dot = books / ".Hidden.epub"
    epub(dot)
    cache = card / ".crosspoint"
    cache.mkdir()
    settings = cache / "crossink-settings.json"
    settings.write_bytes(b'{"uiTheme":4}')
    progress = cache / "recent.json"
    progress.write_bytes(b'["keep"]')
    args = cli(engine, card, "--verbose")
    output = run(args)
    assert output.count("  prepared ") == 3
    for path, names in ((book, {"book.bin", "css_rules.cache"}),
                        (panels, THUMBS | {"cover_src_xtch_padded_v1.bin"}),
                        (wide, THUMBS)):
        logical = "/" + path.relative_to(card).as_posix()
        target = cache / cache_keys(logical)[0]
        files = {entry.name for entry in target.iterdir()}
        assert files == names | {RECEIPT}, files
        receipt = json.loads((target / RECEIPT).read_text())
        assert receipt["path"] == logical and receipt["source_sha256"] == sha(path)
        assert receipt["artifacts"] == {name: sha(target / name) for name in names}
        for name in names & THUMBS:
            bmp = (target / name).read_bytes()
            width, height = map(int, name[6:-4].split("x"))
            assert struct.unpack_from("<2sI", bmp) == (b"BM", len(bmp))
            assert struct.unpack_from("<iiHH", bmp, 18) == (width, -height, 1, 1)
            assert len(bmp) == 62 + ((width + 31) // 32 * 4) * height
        if path == panels:
            source = (target / "cover_src_xtch_padded_v1.bin").read_bytes()
            assert struct.unpack_from("<4sBBHHHI", source) == (b"XCS1", 1, 2, 319, 417, 80, 33360)
            assert len(source) == 16 + 33360
    assert run(args).count("verified prepared cache for the same book") == 3

    logical = "/" + book.relative_to(card).as_posix()
    target = cache / cache_keys(logical)[0]
    first = (target / "book.bin").read_bytes()
    assert first[:5] == b"\xffCXB\x0a" and b"Original" in first
    old = book.stat()
    epub(book, title="Replaced")
    assert book.stat().st_size == old.st_size
    os.utime(book, ns=(old.st_atime_ns, old.st_mtime_ns))
    assert run(args).count("  prepared ") == 1
    replacement = (target / "book.bin").read_bytes()
    assert replacement != first and b"Replaced" in replacement

    css = (target / "css_rules.cache").read_bytes()
    assert css[4:6] == b"\x18\0" and int.from_bytes(css[6:8], "little") == 1
    current_progress = target / "progress.bin"
    current_progress.write_bytes(b"reader progress")
    (target / "book.bin").write_bytes(replacement[:11])
    (target / "css_rules.cache").unlink()
    assert "repaired book.bin, css_rules.cache" in run(args)
    assert (target / "book.bin").read_bytes() == replacement
    assert (target / "css_rules.cache").read_bytes() == css
    assert current_progress.read_bytes() == b"reader progress"
    (target / "css_rules.cache").write_bytes(b"reader replacement")
    assert "modified cache file css_rules.cache" in run(args, expected=1)
    assert (target / "css_rules.cache").read_bytes() == b"reader replacement"
    (target / "css_rules.cache").write_bytes(css)

    padded_cache = cache / cache_keys("/Shelf A/MixedCase/Panels.xtch")[0]
    removed = padded_cache / "cover_src_xtch_padded_v1.bin"
    original = removed.read_bytes()
    removed.unlink()
    assert "repaired cover_src_xtch_padded_v1.bin" in run(args)
    assert removed.read_bytes() == original
    legacy_source = padded_cache / "cover_src_xtch_v1.bin"
    legacy_source.write_bytes(b"older firmware cache")
    assert "skipped legacy XTCH source" in run(args)
    assert removed.read_bytes() == original
    assert legacy_source.read_bytes() == b"older firmware cache"
    legacy_source.unlink()

    extra_file = target / "thumb_200x390.bmp"
    extra_file.write_bytes(b"device-created thumbnail")
    epub(book, title="Must preserve")
    assert "retired matching prepared files" in run(args, expected=1)
    assert current_progress.read_bytes() == b"reader progress"
    assert extra_file.read_bytes() == b"device-created thumbnail"
    assert not (target / "book.bin").exists() and not (target / "css_rules.cache").exists()
    book.unlink()

    # A previously prepared book which changed to protected content loses the
    # host-owned cache and never receives derived output from the new EPUB.
    own = books / "Previously prepared.epub"
    epub(own)
    assert "  prepared " in run(args)
    own_target = cache / cache_keys("/" + own.relative_to(card).as_posix())[0]
    epub(own, protection="META-INF/encryption.xml")
    plain, drm, bad = books / "No css.epub", books / "Loan.epub", books / "Broken.xtch"
    epub(plain, css=None)
    epub(drm, protection="META-INF/license.lcpl")
    bad.write_bytes(b"invalid XTCH")
    encrypted = books / "Zip encrypted.epub"
    epub(encrypted)
    masked = bytearray(encrypted.read_bytes())
    offset = masked.index(b"PK\x01\x02")
    masked[offset + 8] |= 1
    encrypted.write_bytes(masked)
    local_encrypted = books / "Local ZIP encrypted.epub"
    epub(local_encrypted)
    masked = bytearray(local_encrypted.read_bytes())
    masked[6] |= 1
    local_encrypted.write_bytes(masked)
    accented = books / "Accent Café.epub"
    curly = books / "Curly’s.epub"
    epub(accented)
    epub(curly)
    huge = books / "Large CSS.epub"
    epub(huge, css="/*" + "x" * (128 * 1024) + "*/p{margin-top:1em}")
    too_many = books / "Too many rules.epub"
    epub(too_many, css="".join(f".c{i}{{margin-top:1em}}" for i in range(1501)))
    invalid_epub = books / "Broken.epub"
    invalid_epub.write_bytes(b"invalid EPUB")
    old_book = books / "Legacy.epub"
    epub(old_book)
    old_keys = cache_keys("/" + old_book.relative_to(card).as_posix())
    legacy = cache / old_keys[1]
    legacy.mkdir()
    (legacy / "progress.bin").write_bytes(b"legacy progress")
    output = run(args, expected=1)
    assert output.count("skipped protected EPUB") == 4
    assert output.count("skipped unsupported path") == 1
    assert "/Shelf A/MixedCase/Curly’s.epub" in output
    assert (cache / cache_keys("/Shelf A/MixedCase/Curly’s.epub")[0] / "book.bin").is_file()
    assert output.count("(firmware will build CSS)") == 2
    assert "skipped legacy cache" in output
    assert "File is not a zip file" in output and "cache engine failed" in output
    assert not own_target.exists()
    for path in (bad, invalid_epub, drm, encrypted, local_encrypted, dot, accented):
        assert not (cache / cache_keys("/" + path.relative_to(card).as_posix())[0]).exists()
    for path in (huge, too_many):
        target = cache / cache_keys("/" + path.relative_to(card).as_posix())[0]
        assert {entry.name for entry in target.iterdir()} == {"book.bin", RECEIPT}
    assert not (cache / old_keys[0]).exists() and (legacy / "progress.bin").read_bytes() == b"legacy progress"
    assert settings.read_bytes() == b'{"uiTheme":4}' and progress.read_bytes() == b'["keep"]'
    assert not list(cache.glob(".crossink-prepare-*"))
    assert not any(path.name.startswith("._") for path in cache.iterdir()
                   if path.name[2:].startswith(("epub_", "xtc_")) and path.name[2:] not in old_keys)

    fd = os.open(card, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert "holds the volume lock" in run(args, expected=1)
    finally:
        os.close(fd)
    print("PASS mounted FAT32 publish, binary formats, stale/current, protection, invalid, legacy, lock")


def interruption_tests(engine, work):
    volume = work / "faults"
    volume.mkdir()
    book = volume / "Replace.epub"
    epub(book)
    target = volume / ".crosspoint" / cache_keys("/Replace.epub")[0]
    args = cli(engine, volume, staging=True)
    run(args)
    epub(book, title="Replaced")
    # Stop after the real retire-rename. There should be a firmware cache miss,
    # and a later CLI run should publish without reading the abandoned staging.
    wrapper = """import os, sys
from pathlib import Path
sys.path.insert(0, sys.argv.pop(1))
import prepare
real_rename = prepare.rename_new
def interrupt(source, destination):
    real_rename(source, destination)
    if destination.name == 'stale':
        os._exit(99)
prepare.rename_new = interrupt
sys.exit(prepare.main())
"""
    patched = [sys.executable, "-B", "-c", wrapper, str(TOOLS), *args[2:]]
    run(patched, expected=99)
    assert not target.exists()
    run(args)
    assert b"Replaced" in (target / "book.bin").read_bytes()

    replaced = volume / "Previously read.epub"
    epub(replaced)
    read_target = volume / ".crosspoint" / cache_keys("/Previously read.epub")[0]
    run(args)
    progress = read_target / "progress.bin"
    progress.write_bytes(b"keep progress")
    epub(replaced, title="Another book")
    wrapper = """import os, sys
sys.path.insert(0, sys.argv.pop(1))
import prepare
real_sync = prepare.sync_directory
def interrupt(path):
    real_sync(path)
    if (path / 'progress.bin').exists() and not (path / 'css_rules.cache').exists():
        os._exit(99)
prepare.sync_directory = interrupt
sys.exit(prepare.main())
"""
    run([sys.executable, "-B", "-c", wrapper, str(TOOLS), *args[2:]], expected=99)
    assert (read_target / "book.bin").is_file() and not (read_target / "css_rules.cache").exists()
    assert "retired matching prepared files" in run(args, expected=1)
    assert not (read_target / "book.bin").exists() and progress.read_bytes() == b"keep progress"
    replaced.unlink()

    # Fail after real cache generation while copying to card staging.
    fault_book = volume / "Space.epub"
    epub(fault_book)
    wrapper = """import errno, os, stat, sys
sys.path.insert(0, sys.argv.pop(1))
import prepare
real_sync = prepare.os.fsync
def full(fd):
    if stat.S_ISREG(os.fstat(fd).st_mode):
        raise OSError(errno.ENOSPC, 'injected no space during staged write')
    real_sync(fd)
prepare.os.fsync = full
sys.exit(prepare.main())
"""
    run([sys.executable, "-B", "-c", wrapper, str(TOOLS), *args[2:]], expected=1)
    fault_target = volume / ".crosspoint" / cache_keys("/Space.epub")[0]
    assert not fault_target.exists()
    run(args)
    assert fault_target.exists()

    # Run the real engine on a snapshot, then replace the mounted source before
    # the publication identity check. The output must remain unpublished.
    active = volume / "Concurrent.epub"
    epub(active)
    fake = work / "engine-also-edits-book"
    fake.write_text(f'#!/bin/sh\n"{engine}" "$@" || exit $?\nprintf changed >> "{active}"\n')
    fake.chmod(0o700)
    assert "book changed during preparation" in run(cli(fake, volume, staging=True), expected=1)
    assert not (volume / ".crosspoint" / cache_keys("/Concurrent.epub")[0]).exists()
    active.unlink()

    active = volume / "Cache appeared.epub"
    epub(active)
    appeared = volume / ".crosspoint" / cache_keys("/Cache appeared.epub")[0]
    fake.write_text(f'#!/bin/sh\n"{engine}" "$@" || exit $?\n'
                    f'mkdir "{appeared}"\nprintf ours > "{appeared / "progress.bin"}"\n')
    assert "File exists" in run(cli(fake, volume, staging=True), expected=1)
    assert set(appeared.iterdir()) == {appeared / "progress.bin"}
    assert (appeared / "progress.bin").read_bytes() == b"ours"
    assert "skipped existing cache" in run(args)

    receipt = target / RECEIPT
    record = json.loads(receipt.read_text())
    record["artifacts"].pop("book.bin")
    receipt.write_text(json.dumps(record))
    assert "invalid preparation receipt" in run(args, expected=1)
    assert b"Replaced" in (target / "book.bin").read_bytes()

    escaped = work / "outside"
    escaped.mkdir()
    other = work / "other"
    other.mkdir()
    (other / ".crosspoint").symlink_to(escaped)
    epub(other / "Book.epub")
    assert ".crosspoint must be a regular directory" in run(cli(engine, other, staging=True), expected=1)
    assert not list(escaped.iterdir())

    collision_volume = work / "collision"
    collision_volume.mkdir()
    seen = {}
    for index in range(300000):
        name = f"/{hashlib.sha256(str(index).encode()).hexdigest()[:12]}.xtch"
        key = esp_hash(name)
        if key in seen:
            for path in (name, seen[key]):
                xtch(collision_volume / path[1:])
            break
        seen[key] = name
    else:
        raise AssertionError("No deterministic hash collision")
    assert run(cli(engine, collision_volume, staging=True), expected=1).count("cache-key collision") == 2
    assert not (collision_volume / ".crosspoint").exists()
    print("PASS interrupted retire, staging ENOSPC, concurrent source/cache changes, invalid receipt, symlink, collision")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    args = parser.parse_args()
    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="crossink-card-integration-") as base:
        work = Path(base)
        with fat_volume(work, "mounted") as card:
            publish_tests(args.engine.resolve(), work, card)
        interruption_tests(args.engine.resolve(), work)
    print(f"PASS host integration ({time.monotonic() - start:.2f} s)")


if __name__ == "__main__":
    main()
