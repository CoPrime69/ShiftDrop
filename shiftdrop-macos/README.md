# ShiftDrop for macOS (early prototype)

A native Swift menu-bar app for Apple Silicon (arm64, macOS 13.4 or later) with the same idea as the
Windows app: drag a file in Finder, press **Shift**, drop it on a slice of the wheel. It is a prototype,
not a release: it is signed ad hoc, not notarized, and has no installer or updater yet.

## What works

- The wheel: drag files, press Shift, scroll or press Tab/arrows/1-9 to change ring, drop on a slice.
- Images: convert (PNG, JPG, WebP, AVIF, HEIC, TIFF, BMP), rotate, flip, resize, compress, under 1 MB.
- Video and audio (ffmpeg): convert, compress by percent or to a size target.
- PDF (pages to images, text, merge, compress), documents, subtitles (SRT, VTT, TXT), CSV/TSV, archives.
- Copy text (OCR), Float shelf, copy/move/duplicate/reveal/terminal, a history of results.
- Recipes: the same JSON format as Windows, user recipes in `~/Library/Application Support/ShiftDrop/recipes`.

## Build

You need Xcode (or its command-line tools) and [Homebrew](https://brew.sh), but only to build. The
finished app needs no Homebrew: ffmpeg and libwebp are inside it.

```sh
brew install cmake ninja meson pkgconf   # once: tools that compile the engines, never part of the app
shiftdrop-macos/scripts/build-engines.sh  # once (about 10 minutes): static libwebp and an LGPL ffmpeg
shiftdrop-macos/app/build_app.sh          # builds app/build/ShiftDrop.app
open shiftdrop-macos/app/build/ShiftDrop.app
```

The engines go to `app/vendor/`, which git ignores, so a fresh clone must run `build-engines.sh` once.
After that, only `build_app.sh` is needed for rebuilds.

`build-engines.sh` builds both engines from pinned sources (a git tag and its commit, or a tarball and
its SHA-256) into `app/vendor/`, which git ignores:

- **libwebp** (BSD), linked statically into the app. ImageIO can't write WebP.
- **ffmpeg** (LGPL only, no x264 or x265), copied into the app at `Contents/Resources/engines/ffmpeg`.
  H.264 and HEVC use the Mac's VideoToolbox hardware encoders. It links only macOS system libraries,
  so the app needs no Homebrew on the Mac it runs on. The build uses the same component list as the
  Windows build (`scripts/ffmpeg-components.txt`) minus the Windows-only GPU encoders.

The app bundle also carries `Contents/Resources/licenses/`: the licences of ffmpeg and the libraries
inside it, the exact configure line, and `ffmpeg-sources.tar.xz` (the complete corresponding source),
as the LGPL asks.

## Test

```sh
shiftdrop-macos/scripts/build-libwebp-macos.sh   # once: the package links libwebp (ffmpeg is not needed)
cd shiftdrop-macos/app && swift test             # needs Xcode (XCTest)
```

The tests cover the pure parts: ffmpeg log parsing (`Media.parseInfo`, `parseProgress`), `planSizeFit`,
subtitles, CSV/TSV and output file names. `.github/workflows/macos.yml` runs `swift build` and `swift test`
on macOS for changes under `shiftdrop-macos/`.

## Share it (no Apple Developer account)

The app is signed ad hoc, not with a Developer ID, and it is not notarized. That is fine for you and a
few people you trust; macOS will warn the first time it opens. It is not suitable for public download.

1. Zip it: `ditto -c -k --keepParent shiftdrop-macos/app/build/ShiftDrop.app ShiftDrop.zip`, and send the zip.
2. The other person needs an **Apple Silicon Mac on macOS 13.4 or later**. Nothing else to install.
3. They unzip it and open `ShiftDrop.app`. macOS blocks it the first time. They open
   **System Settings > Privacy & Security**, scroll down and click **Open Anyway**. (On recent macOS
   versions, right-click > Open no longer skips the warning.)
   Or, in Terminal: `xattr -dr com.apple.quarantine ShiftDrop.app`, then open it.
4. They allow **Input Monitoring** for ShiftDrop when asked (see First run).

A notarized `.dmg` for the public needs the Apple Developer Program. That is a later step.

## First run

macOS asks for **Input Monitoring** (System Settings > Privacy & Security) so the app can see the
Shift key during a drag. By default `build_app.sh` signs ad hoc, so you grant it again after each build.
To keep the permission across rebuilds, sign with your own local identity: set `SHIFTDROP_SIGN_KEYCHAIN`
(path to a keychain holding a "ShiftDrop Dev" code-signing certificate) and
`SHIFTDROP_SIGN_KEYCHAIN_PASSWORD` before running it.

## Prototype limits

- **Move and Duplicate can't be undone yet.** (The Windows app can.)
- **Wheel keys** (Tab, arrows, `[` `]`, `` ` ``, 1-9, Esc) are swallowed while the wheel is open only if you also
  allow ShiftDrop under **Accessibility**; without it they work in the wheel but also reach the app in front.
- **Accent colour** follows the macOS accent colour. To use a preset instead (Tide is the first):
  `defaults write dev.shiftdrop.mac accent tide` (or blue, purple, pink, red, orange, green, graphite; `system` to go back).
  There is no settings window yet.

## Layout

- `recipes/builtin.json` the built-in wheel slices (one data file; `build_app.sh` copies it into the app)
- `app/` the Swift package (`Sources/ShiftDrop`), `build_app.sh`, and `vendor/` (built engines, ignored)
- `scripts/` `build-engines.sh`, `build-libwebp-macos.sh`, `build-ffmpeg-macos.sh`
