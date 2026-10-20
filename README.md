# ShiftDrop

Drag a file, hold a key, and a wheel appears under the cursor. Drop on a slice to act on the file.
Everything runs offline. Windows 10/11, native C++20 / Win32 / COM.

| Hold | Wheel | M2 slices |
|---|---|---|
| Shift | Convert | PNG, JPG, WebP, TIFF, BMP, Web (a recipe: resize + WebP) |
| Ctrl | Tools | Rotate, Flip, 1920 px, 1080 px, 50% |
| Alt | File actions | Copy, Move to…, Copy path, Terminal, Duplicate |

Still dragging, press another wheel's key or turn the mouse wheel to switch wheels; a panel at the top
of the screen shows which wheel you are on, then the job's progress and result. The tray menu sets the
size (Auto adapts to your screen) and the style (Solid or Glass).

Status: **M2**. Every slice is a recipe (your own go in `%LOCALAPPDATA%\ShiftDrop\recipes`), every
run is in History, and any run can be undone. It starts with Windows and lives in the tray.

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
├─ scripts/                check-imports, make-notices, test-installer
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
   ├─ PipelineRunner       staging → pipeline → atomic commit → hash → history
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
