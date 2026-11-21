# Prepare a CrossInk card on a Mac

CrossInk Card Preparer builds EPUB metadata and stylesheet caches and the three Lyra Carousel thumbnails for two-bit XTCH books. Run it after copying books to a card to move this work ahead of the reader’s first open or first Home visit.

Preparation is optional. CrossInk builds what it needs when you open a book. Use the Mac tool for a batch of new books already on the card. For a new EPUB sent over Wi-Fi, you can also prepare it from your phone or Mac in the reader’s [File Manager upload dialog](webserver.md#epub-optimization).

## Build and install

Use the source revision that matches the reader’s X4 Pro firmware. The supported build uses an Apple Silicon Mac, Xcode Command Line Tools, CMake 3.21 or newer, GCC 15, and Python 3.9 or newer. Initialize the repository’s submodules as described in [Getting Started](development/getting-started.md). CMake fetches the simulator storage adapter and ArduinoJson at the versions used by the firmware.

```sh
CC=gcc-15 CXX=g++-15 cmake -S tools/card-preparer -B build/card-preparer \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build build/card-preparer --parallel
cmake --install build/card-preparer --prefix "$PWD/CrossInk-Card-Preparer"
```

To use a prebuilt Apple Silicon CLI bundle from the matching revision, unzip it and use the same `bin/crossink-prepare` command. The bundle targets macOS 15 or newer, needs Python 3.9 or newer on `PATH`, and includes the GCC runtime in the engine.

## Prepare a mounted card

In Finder, unzip the bundle. On the X4 Pro, choose **File Transfer → USB Drive** and connect it to the Mac. The microSD can stay in the reader. Finish copying your books. In Terminal, drag the extracted `bin/crossink-prepare` executable into the window, type a space and `--volume` followed by another space, then drag the reader’s mounted volume from Finder and press Return. The resulting command looks like this:

```sh
./CrossInk-Card-Preparer/bin/crossink-prepare --volume "/Volumes/CROSSINK"
```

Wait for the summary and eject the volume in Finder before returning to CrossInk. The command prints one result per book and exits with status 1 if any book fails. Use `--verbose` to see cache-engine diagnostics. A full offline card copy can also be prepared with `--staging-directory /path/to/card-copy`. Preserve its directory names and copy its generated cache directories only to a card with the same books at the same paths and where those cache directories are absent.

## Coverage and existing caches

- Tested on macOS with a disposable FAT32 mount and the X4 Pro simulator. Match the X4 Pro firmware revision and use Lyra Carousel for the three prepared thumbnail sizes. Other board and filesystem combinations have no compatibility claim.
- Paths are case-sensitive for cache identity. The tool supports printable ASCII, including spaces and mixed case, and the curly quotes `‘’“”`, with up to 255 UTF-8 bytes per component and 1023 in the full card path. It skips hidden files and folders, symbolic links, FAT-reserved characters and names ending with a space or period. Other Unicode names such as `Café.epub` are left to CrossInk because macOS can report a spelling that differs from the card’s FAT directory entry.
- Protected EPUBs are skipped, including books with an encryption manifest, license or rights record, and ZIP encryption. EPUB CSS that the host cannot finish is left for the reader. Page layout, EPUB covers, standard one-bit XTC and other thumbnail dimensions are built on demand.
- A new cache is staged and then published under the book’s card-relative key. A receipt records the book and generated-file SHA-256 hashes. Reruns verify the hashes and can repair missing or truncated generated files when the engine can reproduce the original bytes. Existing caches without a receipt are left to the reader.
- If a prepared book changes in place, the tool retires the previous generated cache. Where reader files or a legacy cache are present, it removes only generated files matching the receipt, preserves settings and progress, and reports an error. Give the replacement book a new filename in that case so remaining reader cache cannot be reused for it. A failed or interrupted run can be rerun while the volume is still mounted. Fresh bundles are published whole, and an interrupted mixed-cache retirement can be completed by a rerun.
