# ShiftDrop

Drag a file, hold a key, and a wheel appears under the cursor. Drop on a slice to act on the file.
Everything runs offline. Windows 10/11, native C++20 / Win32 / COM.

| Hold | Wheel | Slices so far |
|---|---|---|
| Shift | Convert | Images (also from AVIF, HEIC and SVG): PNG, JPG, WebP, AVIF, HEIC (with Windows' HEIF codec), TIFF, BMP, Web (a recipe: resize + WebP). Videos: MP4, MOV, MKV, WebM, AVI, WMV, GIF, MP3. Audio: MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA. PDFs: JPG, PNG (every page), TXT, Markdown. Images: PDF (one PDF from all of them) |
| Ctrl | Tools | Rotate, Flip, 1920 px, 1080 px, 50%, Compress, Under 1 MB, Inspect (every metadata field, searchable); for video and audio Compress, 50% size, Under 10 MB, Under 25 MB. PDFs: Merge, Organize, Compress, Smallest. Images and PDFs: Copy text (OCR), Searchable PDF |
| Alt | File actions | Copy, Move to…, Copy path, Terminal, Duplicate, Float (a shelf that holds files until you drag them into any app), Share |

Each wheel shows only what fits the files you drag: a video gets video formats, a song audio formats.
Still dragging, press another wheel's key or turn the mouse wheel to switch wheels; a panel at the top
of the screen shows which wheel you are on, then the job's progress and result. The tray menu sets the
size (Auto adapts to your screen) and the style (Solid or Glass).

Arrow keys pick a slice and Enter runs it, mid-drag. Without dragging, press Ctrl+Shift+Space on
Explorer's selection, or right-click > ShiftDrop…, and the wheel opens on those files. While a slice
is highlighted, a card under the wheel predicts the result: format, size, name and what changes.

Status: **M3a** (video and audio). Every slice is a recipe (your own go in `%LOCALAPPDATA%\ShiftDrop\recipes`), every
run is in History, and any run can be undone. Long jobs show their progress in the top panel and
can be cancelled there. It starts with Windows and lives in the tray.

Video and audio go through a bundled, unmodified LGPL build of [FFmpeg](https://ffmpeg.org) that
ShiftDrop runs as a separate process, restricted to local files (no network access).

- Plan, REA stages, parity map, decisions and open questions: [docs/PLAN.md](docs/PLAN.md)
- Build, run and the manual test checklist: [docs/TESTING.md](docs/TESTING.md)
- Writing recipes: [docs/recipes.md](docs/recipes.md)
- Design decisions: [docs/adr/](docs/adr/)

## Install

Download `ShiftDrop-Setup-<version>.exe` (from a GitHub Release, or the `ShiftDrop-Setup-…` artifact of
any CI run) and run it. It installs for your user only, needs no admin rights and nothing else
(no Visual Studio, no runtimes), adds a Start menu entry, and can start ShiftDrop when you sign in.
Uninstall from Settings > Apps; your settings, recipes and history in `%LOCALAPPDATA%\ShiftDrop` are kept.

The installer is not code-signed yet, so Windows SmartScreen may say "Windows protected your PC":
choose **More info**, then **Run anyway**.

## Build


```powershell
# Developer PowerShell for VS 2022, with $env:VCPKG_ROOT set (see docs/TESTING.md)
./scripts/get-ffmpeg.ps1      # once: pinned LGPL ffmpeg.exe into build\engines (SHA-256 checked)
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
.\build\x64-release\Release\ShiftDrop.exe
```

## Layout

```
shiftdrop/
├─ CMakeLists.txt          shiftdrop_core (static lib), ShiftDrop.exe, shiftdrop_tests
├─ CMakePresets.json       x64-release, x64-release-m0, test preset
├─ vcpkg.json              wil, sqlite3, nlohmann-json, libwebp, gtest
├─ .github/workflows/ci.yml  build, unit tests, no-network imports check, installer + smoke test
├─ installer/ShiftDrop.iss  per-user Inno Setup installer
├─ scripts/                get-ffmpeg, check-imports, make-notices, test-installer
├─ docs/                   PLAN, TESTING, recipes, adr/, research/
├─ tests/                  gtest unit tests
└─ src/
   ├─ main.cpp             wWinMain, single instance, App (gesture state, wheels, batches, tray)
   ├─ MouseHook            WH_MOUSE_LL: records state, posts messages, nothing else
   ├─ Overlay              layered topmost no-activate wheel, Direct2D, hit-testing
   ├─ DropTarget           IDropTarget (WRL), CF_HDROP, COPY-never-MOVE
   ├─ Tray                 Shell_NotifyIcon, menu, notifications
   ├─ HistoryWindow        runs list with Undo and Show files
   ├─ Action, Recipe       action registry; recipe JSON parsing and validation
   ├─ RecipeStore          built-in + user recipes
   ├─ ImagePipeline        fused WIC/libwebp pipeline: decode → orient → transform → encode
   ├─ PipelineRunner       staging → pipeline (images) or ffmpeg (media) → atomic commit → hash → history
   ├─ Media, Ffmpeg        video/audio formats and ffmpeg arguments; the ffmpeg child process (job object, progress, cancel)
   ├─ Island               the top panel: wheel tabs, job progress with cancel, results
   ├─ OutputPath           "name (converted).ext", staging and no-replace commit
   ├─ HistoryStore, Hash   SQLite history, SHA-256 (CNG)
   ├─ Undo, FileMove       undo to the Recycle Bin or back to the original place; Move to…
   ├─ FileActions          clipboard copy, copy path, terminal
   ├─ JobQueue             worker pool (MTA) and the shell thread (STA)
   ├─ Settings, Startup    settings.ini, start with Windows
   └─ Log, Brand, Utf, Messages, resource, ShiftDrop.rc, app.manifest
```

Clean-room note: ShiftDrop reimplements the behaviour of Tangerine (macOS, by @th2049) from public
descriptions only. No code, names, icons, artwork or text from Tangerine are used.
