# ShiftDrop

Drag a file, hold a key, and a wheel appears under the cursor. Drop on a slice to act on the file.
Everything runs offline. Windows 10/11, native C++20 / Win32 / COM.

Drag a file and press **Shift**: the wheel opens on the section that suits the file best. Turn the
mouse wheel to step through the sections (Convert, Tools, Reduce size, File actions) in order, round and round. No mouse wheel? Keep Shift held and tap 1, 2, 3 or 4 to jump to a section.

| Section | Slices so far |
|---|---|
| Convert | Images: PNG, JPG, WebP, TIFF, BMP, Web (a recipe: resize + WebP). Videos: MP4, MOV, MKV, WebM, AVI, WMV, GIF, MP3. Audio: MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA. PDFs: JPG, PNG (every page), TXT, Markdown, DOCX, EPUB, HTML. Images: PDF (one PDF from all of them). Documents (DOCX, Markdown, HTML, TXT, RTF, RTFD, Pages): DOCX, PDF, Markdown, HTML, EPUB, TXT. Spreadsheets (XLSX, XLS, ODS, CSV, TSV): XLSX, ODS, CSV, TSV, PDF. Subtitles (SRT, VTT): SRT, VTT, TXT. With Office or LibreOffice installed: Exact PDF of Word, Excel and PowerPoint files, and (PowerPoint) PNG or JPG slides. Archives (ZIP, TAR, TGZ, GZ, 7z, RAR): Extract, ZIP, TAR.GZ, 7z. Folders: ZIP, TAR.GZ, 7z |
| Tools | Rotate, Flip, 1920 px, 1080 px, 50%, Compress, Under 1 MB, Inspect (every metadata field, searchable); for video and audio Compress, 50% size, Under 10 MB, Under 25 MB; for video Trim…, Crop…, Speed…, Join…, Frames…, Split…, Redact…; for audio Normalize, Trim…, Channels…, Bleep…, Visualizer…; PDFs: Merge, Organize, Compress, Smallest. Images and PDFs: Copy text (OCR), Searchable PDF. Images: Crop, Redact (cover text, faces and personal details), Share-safe (verified metadata scrub with a review step; also video and audio), Edit (light, colour, detail, dehaze, clarity, noise, grain), Annotate (arrows, text, shapes, callouts, numbers, magnifiers, pictures, pen, highlighter), Background (solid, gradient, blur or picture, with shape, spacing, corners and shadow); for two or more images Collage (grid, row, column, featured) |
| Reduce size | 90%, 80% … 20% smaller, for photos, video, audio and PDFs. The centre shows the size now; hover a slice to see the size it comes out at |
| File actions | Copy, Move to…, Copy path, Terminal, Duplicate, Float (a shelf that holds files until you drag them into any app), Share, Remove metadata (images, video, audio), Zip (one ZIP of everything dropped) |

Each wheel shows only what fits the files you drag: a video gets video formats, a song audio formats.
A panel at the top of the screen shows which section you are on, then the job's progress and result. The tray menu sets the
size (Auto adapts to your screen) and the style (Solid or Glass).

Arrow keys pick a slice and Enter runs it, mid-drag. Without dragging, press Ctrl+Shift+Space on
Explorer's selection, or right-click > ShiftDrop…, and the wheel opens on those files. While a slice
is highlighted, a card under the wheel predicts the result: format, size, name and what changes.

The video and audio tools open one timeline editor (filmstrip or waveform, playback, typed timecodes) whose
tabs hold every tool of its kind: Trim, Crop, Speed, Join, Frames, Split and Redact for video; Trim,
Channels, Bleep and Visualizer for audio.

The photo tools (Edit, Annotate, Background, Collage) open an image editor that previews the real
result live on the GPU (Direct2D, with our own shaders); the saved copy keeps the photo's format and
its camera, date and location details.

Status: **M3a** (video and audio). Every slice is a recipe (your own go in `%LOCALAPPDATA%\ShiftDrop\recipes`), every
run is in History, and any run can be undone. Long jobs show their progress in the top panel and
can be cancelled there. It starts with Windows and lives in the tray.

Video and audio go through a bundled, unmodified LGPL build of [FFmpeg](https://ffmpeg.org) that
ShiftDrop runs as a separate process, restricted to local files (no network access).

Documents, spreadsheets and subtitles are converted by ShiftDrop's own code (with miniz, pugixml and
md4c); documents become PDFs through Windows' built-in "Microsoft Print to PDF". Nothing from
Microsoft Office or LibreOffice is bundled: when one is installed, ShiftDrop drives it for the exact
PDF and slide-picture slices, invisibly, and closes it again.

Archives are read by [libarchive](https://libarchive.org) inside a separate worker,
`engines\ShiftDropWorker.exe`, that runs in a Windows AppContainer with no capabilities: it can't
open your files or reach the network, it only gets the archive it reads, and ShiftDrop checks every
name before writing anything. Archives with `..` or absolute paths are refused, links are skipped,
and zip bombs stop at a size limit.

- Plan, REA stages, parity map, decisions and open questions: [docs/PLAN.md](docs/PLAN.md)
- Build, run and the manual test checklist: [docs/TESTING.md](docs/TESTING.md)
- Writing recipes: [docs/recipes.md](docs/recipes.md)
- Plugin design (not built yet): [docs/plugins.md](docs/plugins.md)
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
├─ vcpkg.json              wil, sqlite3, nlohmann-json, libwebp, qpdf, zlib, libarchive, gtest
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
   ├─ archive/             Zip, Extract and repack (libarchive), entry checks, the worker's read job
   ├─ sandbox/             AppContainer + job launcher, the worker exe, the ffmpeg jail
   ├─ Settings, Startup    settings.ini, start with Windows
   └─ Log, Brand, Utf, Messages, resource, ShiftDrop.rc, app.manifest
```

Clean-room note: ShiftDrop reimplements the behaviour of Tangerine (macOS, by @th2049) from public
descriptions only. No code, names, icons, artwork or text from Tangerine are used.
