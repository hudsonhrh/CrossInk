#!/usr/bin/env python3
"""Prepare generated CrossInk caches on an exclusively mounted X4 Pro card."""

import argparse
from collections import defaultdict
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from unicodedata import normalize
import zipfile

RECEIPT = ".crossink-prepared.json"
CHUNK_BYTES = 1024 * 1024
KEY = re.compile(r"(epub|xtc)_[0-9]+")
THUMBS = {"thumb_200x390.bmp", "thumb_296x468.bmp", "thumb_340x540.bmp"}
PADDED_SOURCE = "cover_src_xtch_padded_v1.bin"
LEGACY_XTCH_SOURCE = "cover_src_xtch_v1.bin"
PROTECTED_ENTRIES = {"meta-inf/encryption.xml", "meta-inf/license.lcpl", "meta-inf/rights.xml"}


def esp_hash(path):
    """Xtensa's 32-bit libstdc++ _Hash_bytes, with _Hash_impl's default seed."""
    data = path.encode("utf-8")
    mask = (1 << 32) - 1
    multiplier = 0x5BD1E995
    value = 0xC70F6907 ^ len(data)
    tail = len(data) // 4 * 4
    for offset in range(0, tail, 4):
        word = int.from_bytes(data[offset:offset + 4], "little") * multiplier & mask
        word = (word ^ (word >> 24)) * multiplier & mask
        value = ((value * multiplier) & mask) ^ word
    if tail != len(data):
        value = ((value ^ int.from_bytes(data[tail:], "little")) * multiplier) & mask
    value = ((value ^ (value >> 13)) * multiplier) & mask
    return value ^ (value >> 15)


def cache_keys(path):
    if Path(path).suffix.lower() == ".epub":
        # Epub::cachePathForFilePath delegates to ZipFile::fnvHash64.
        value = 14695981039346656037
        for byte in path.encode("utf-8"):
            value = (value ^ byte) * 1099511628211 & ((1 << 64) - 1)
        return tuple(dict.fromkeys((f"epub_{value}", f"epub_{esp_hash(path)}")))
    return (f"xtc_{esp_hash(path)}",)


def safe_card_name(path):
    # SdFat uses the FAT long file name spelling. Avoid host Unicode
    # normalization. The supported curly quotes are normalization-invariant
    # and retain their spelling in the mounted and raw FAT directory entries.
    return len(path.encode("utf-8")) <= 1023 and all(
        (0x20 <= ord(c) < 0x7F and c not in '<>:"\\|?*') or c in "‘’“”" for c in path
    ) and all(
        0 < len(part.encode("utf-8")) <= 255 and not part.endswith((" ", ".")) for part in path[1:].split("/")
    )


def inventory(volume, device):
    def fail(error):
        raise error

    paths = {}
    claimed_keys = defaultdict(set)
    case_names = defaultdict(set)
    for directory, subdirs, files in os.walk(volume, onerror=fail):
        parent = Path(directory)
        local_dirs = []
        for name in sorted(subdirs):
            if name.startswith("."):
                continue
            entry = (parent / name).lstat()
            if entry.st_dev == device and stat.S_ISDIR(entry.st_mode):
                local_dirs.append(name)
        subdirs[:] = local_dirs
        for name in sorted(files):
            book = parent / name
            if name.startswith(".") or book.suffix.lower() not in {".epub", ".xtc", ".xtch"}:
                continue
            relative = "/" + book.relative_to(volume).as_posix()
            case_names[normalize("NFC", relative).casefold()].add(relative)
            paths[relative] = book
            for spelling in {relative, normalize("NFC", relative), normalize("NFD", relative)}:
                for key in cache_keys(spelling):
                    claimed_keys[key].add(relative)
    return paths, claimed_keys, case_names


def signature(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def digest_file(path, output=None, device=None):
    # The preparer runs with exclusive card access. Check path/file identity
    # around each read as well, to catch edits from another host process.
    checksum = hashlib.sha256()
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or (device is not None and before.st_dev != device):
            raise ValueError("expected a regular file on this volume")
        while block := source.read(CHUNK_BYTES):
            checksum.update(block)
            if output is not None:
                output.write(block)
        after = os.fstat(source.fileno())
    if signature(before) != signature(after) or signature(before) != signature(path.lstat()):
        raise ValueError("file changed while reading")
    return checksum.hexdigest()


def valid_artifacts(kind, names):
    if kind == "epub":
        return {"book.bin"} <= names <= {"book.bin", "css_rules.cache"}
    # Padded XTCH needs its production source cache to distinguish valid
    # current thumbnails from ones produced before the padded-column fix.
    return THUMBS <= names <= THUMBS | {PADDED_SOURCE}


def read_receipt(target, logical, kind):
    receipt = target / RECEIPT
    try:
        with os.fdopen(os.open(receipt, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as file:
            info = os.fstat(file.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > 4096:
                raise ValueError("invalid preparation receipt")
            record = json.loads(file.read(4097))
        files = record["artifacts"]
        if (record["schema"] == 1 and record["path"] == logical and isinstance(files, dict)
                and valid_artifacts(kind, files.keys()) and re.fullmatch(r"[0-9a-f]{64}", record["source_sha256"])
                and all(re.fullmatch(r"[0-9a-f]{64}", digest) for digest in files.values())):
            return record
    except FileNotFoundError:
        return None
    except (KeyError, TypeError, ValueError):
        pass
    raise ValueError("invalid preparation receipt or cache-key collision")


def owned_artifacts(target, recorded, device):
    owned = set()
    present = {path.name for path in target.iterdir()} - {RECEIPT}
    for name in present & recorded.keys():
        path = target / name
        if stat.S_ISREG(path.lstat().st_mode) and digest_file(path, device=device) == recorded[name]:
            owned.add(name)
    return owned, present


def retire_stale(target, owned, present, legacy, cache_root):
    if owned == present and not legacy:
        # Move the whole bundle out of firmware's namespace before rebuilding.
        with tempfile.TemporaryDirectory(prefix=".crossink-prepare-", dir=cache_root) as trash:
            rename_new(target, Path(trash) / "stale")
            sync_directory(cache_root)
        return

    # Preserve files outside the receipt and files the reader may have
    # replaced. Keep the receipt so an interrupted retirement can be retried.
    # With CSS gone first, removing book.bin gives the EPUB reader a cold miss.
    for name in sorted(owned, key=lambda name: (name == "book.bin", name)):
        (target / name).unlink()
        sync_directory(target)
    raise ValueError("book changed; retired matching prepared files but a reader or legacy cache remains. "
                     "Give the replacement book a new path")


def shortened_copy(path, original, device):
    # A repair may replace a strict byte prefix of a known generated file.
    # Preserve other contents, including reader replacements and aliases.
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as current:
        info = os.fstat(current.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_dev != device or info.st_size >= original.stat().st_size:
            return False
        with original.open("rb") as expected:
            while block := current.read(CHUNK_BYTES):
                if block != expected.read(len(block)):
                    return False
        return signature(info) == signature(os.fstat(current.fileno())) == signature(path.lstat())


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def rename_new(source, destination):
    # Darwin can refuse existing targets atomically on filesystems such as APFS.
    # macOS FAT32 lacks RENAME_EXCL. With the volume locked and offline from
    # firmware, recheck absence before renaming there. Publishing a directory
    # also cannot replace a nonempty directory or a regular file.
    rename = ctypes.CDLL(None, use_errno=True).renamex_np
    rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
    rename.restype = ctypes.c_int
    if rename(os.fsencode(source), os.fsencode(destination), 0x00000004) == 0:
        return
    error = ctypes.get_errno()
    if error in (errno.ENOTSUP, errno.EOPNOTSUPP):
        if os.path.lexists(destination):
            raise FileExistsError(errno.EEXIST, os.strerror(errno.EEXIST), str(destination))
        os.rename(source, destination)
        return
    raise OSError(error, os.strerror(error), str(destination))


def protected_epub(path):
    with path.open("rb") as source, zipfile.ZipFile(source) as archive:
        for entry in archive.infolist():
            name = entry.filename.replace("\\", "/").strip("/").casefold()
            if entry.flag_bits & 0x41 or name in PROTECTED_ENTRIES:
                return True
            source.seek(entry.header_offset)
            local = source.read(8)
            if len(local) != 8 or local[:4] != b"PK\x03\x04":
                raise zipfile.BadZipFile("invalid ZIP local header")
            if struct.unpack_from("<H", local, 6)[0] & 0x41:
                return True
    return False


def prepare_one(engine, root, source, logical, keys, device, verbose):
    kind = source.suffix.lower()[1:]
    cache_root = root / ".crosspoint"
    target = cache_root / keys[0]
    legacy_paths = [cache_root / key for key in keys[1:]]
    legacy = any(os.path.lexists(path) for path in legacy_paths)
    existing = os.path.lexists(target)
    if legacy and not existing:
        return "skipped legacy cache"
    if existing:
        info = target.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_dev != device:
            raise ValueError("cache key is occupied by a non-directory or another volume")
    receipt = read_receipt(target, logical, kind) if existing else None
    if existing and not receipt:
        return "skipped existing cache"

    repair = set()
    verified_source = None
    if receipt:
        source_before = signature(source.lstat())
        verified_source = digest_file(source, device=device)
        recorded = receipt["artifacts"]
        owned, present = owned_artifacts(target, recorded, device)
        if signature(source.lstat()) != source_before:
            raise ValueError("book changed during verification")
        if receipt["source_sha256"] == verified_source:
            if PADDED_SOURCE in recorded and os.path.lexists(target / LEGACY_XTCH_SOURCE):
                return "skipped legacy XTCH source; firmware will refresh the cover"
            repair = recorded.keys() - owned
            if not repair:
                return "verified prepared cache for the same book"
        else:
            retire_stale(target, owned, present, legacy, cache_root)

    with tempfile.TemporaryDirectory(prefix="crossink-prep-") as work:
        scratch = Path(work)
        snapshot = scratch / logical[1:]
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        with snapshot.open("xb") as output:
            checksum = digest_file(source, output, device)
        if verified_source is not None and checksum != verified_source:
            raise ValueError("book changed during preparation")

        if kind == "epub" and protected_epub(snapshot):
            return "skipped protected EPUB"
        result = subprocess.run(
            [engine, kind, scratch, logical], text=True, capture_output=True, timeout=180,
        )
        if verbose or result.returncode != 0:
            sys.stderr.write(result.stderr)
        if result.returncode != 0:
            raise ValueError(f"cache engine failed (exit {result.returncode})")
        engine_key = Path(result.stdout.strip()).name
        if (not KEY.fullmatch(engine_key) or not engine_key.startswith("epub_" if kind == "epub" else "xtc_")
                or (kind == "epub" and engine_key != keys[0])):
            raise ValueError("engine and card cache paths disagree")
        built = scratch / ".crosspoint" / engine_key
        supported = {"book.bin", "css_rules.cache"} if kind == "epub" else THUMBS | {PADDED_SOURCE}
        artifacts = {path.name: path for path in built.iterdir() if path.name in supported}
        if not valid_artifacts(kind, artifacts.keys()):
            raise ValueError("engine did not generate the required cache files")

        root_sidecar = root / "._.crosspoint"
        had_sidecar = os.path.lexists(root_sidecar)
        try:
            cache_root.mkdir()
        except FileExistsError:
            pass
        else:
            if not had_sidecar:
                root_sidecar.unlink(missing_ok=True)
        info = cache_root.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_dev != device:
            raise ValueError("cache must be a directory on this volume")
        sync_directory(root)
        with tempfile.TemporaryDirectory(prefix=".crossink-prepare-", dir=cache_root) as stage:
            staged = Path(stage) / "candidate"
            staged.mkdir()
            digests = {}
            for name in sorted(repair or artifacts.keys()):
                artifact = artifacts.get(name)
                if artifact is None:
                    raise ValueError(f"engine cannot reproduce {name} from the preparation receipt")
                with (staged / name).open("xb") as output:
                    digests[name] = digest_file(artifact, output)
                    output.flush()
                    os.fsync(output.fileno())
            if not repair:
                with (staged / RECEIPT).open("x", encoding="utf-8") as output:
                    json.dump({"schema": 1, "path": logical, "source_sha256": checksum, "artifacts": digests}, output)
                    output.flush()
                    os.fsync(output.fileno())
            # macOS may attach AppleDouble provenance to new FAT entries.
            # Remove it only from the private staging files we just created.
            for name in (*digests, RECEIPT):
                (staged / ("._" + name)).unlink(missing_ok=True)
            (staged.parent / ("._" + staged.name)).unlink(missing_ok=True)
            sync_directory(staged)
            sync_directory(staged.parent)
            if digest_file(source, device=device) != checksum:
                raise ValueError("book changed during preparation")
            if repair:
                info = target.lstat()
                if (not stat.S_ISDIR(info.st_mode) or info.st_dev != device
                        or read_receipt(target, logical, kind) != receipt):
                    raise ValueError("preparation receipt changed during repair")
                present = {path.name for path in target.iterdir()}
                replace = set()
                for name, digest in digests.items():
                    destination = target / name
                    if digest != recorded[name]:
                        raise ValueError(f"engine cannot reproduce {name} from the preparation receipt")
                    if os.path.lexists(destination):
                        if name not in present or not shortened_copy(destination, staged / name, device):
                            raise ValueError(f"modified cache file {name}; keeping reader data")
                        replace.add(name)
                for name in digests:
                    if name in replace:
                        os.replace(staged / name, target / name)
                    else:
                        rename_new(staged / name, target / name)
                sync_directory(target)
                return "repaired " + ", ".join(digests)
            if any(os.path.lexists(path) for path in legacy_paths):
                raise ValueError("legacy cache appeared during preparation")
            rename_new(staged, target)
            sync_directory(cache_root)
        status = "prepared " + ", ".join(digests)
        if kind == "epub" and "css_rules.cache" not in digests:
            status += " (firmware will build CSS)"
        return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    location = parser.add_mutually_exclusive_group(required=True)
    location.add_argument("--volume", type=Path, help="mounted SD or USB Drive root")
    location.add_argument("--staging-directory", type=Path, help="offline directory representing the whole card")
    parser.add_argument("--engine", type=Path, default=Path(__file__).resolve().with_name("crossink-prepare-engine"),
                        help="override the installed cache engine next to this command")
    parser.add_argument("--verbose", action="store_true", help="show firmware cache-builder diagnostics")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("this tool uses macOS exclusive rename")
    root = (args.volume or args.staging_directory).resolve()
    if root in (Path("/"), Path("/System/Volumes/Data")) or (args.volume and not root.is_mount()):
        parser.error("--volume requires the mount point of the card")
    engine = args.engine.resolve()
    if not engine.is_file() or not os.access(engine, os.X_OK):
        parser.error("cache engine missing or not executable; install the CLI or pass --engine")
    try:
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError as error:
        print(error, file=sys.stderr)
        return 1
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Anchor relative access to the open mount instead of following a
        # subsequent mount or symlink at the original mount-point path.
        os.fchdir(fd)
        root = Path(".")
        device = os.fstat(fd).st_dev
        cache_info = root / ".crosspoint"
        if os.path.lexists(cache_info):
            info = cache_info.lstat()
            if not stat.S_ISDIR(info.st_mode) or info.st_dev != device:
                raise ValueError(".crosspoint must be a regular directory on this volume")
        books, claimed_keys, case_names = inventory(root, device)
        failures = 0
        for logical, source in books.items():
            if source.suffix.lower() == ".xtc":
                continue
            try:
                if not safe_card_name(logical) or not stat.S_ISREG(source.lstat().st_mode):
                    print(f"{logical!r}  skipped unsupported path", flush=True)
                    continue
                keys = cache_keys(logical)
                if len(case_names[normalize("NFC", logical).casefold()]) != 1 or any(
                    len(claimed_keys[key]) != 1 for key in keys
                ):
                    raise ValueError("path or cache-key collision")
                status = prepare_one(engine, root, source, logical, keys, device, args.verbose)
                print(f"{logical!r}  {status}", flush=True)
            except (OSError, ValueError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
                print(f"{logical!r}  {error}", file=sys.stderr, flush=True)
                failures += 1
        return 1 if failures else 0
    except BlockingIOError:
        print("Another preparer holds the volume lock", file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
