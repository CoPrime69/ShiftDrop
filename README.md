# ShiftDrop

Drag a file, hold **Shift**, and a wheel appears under the cursor. Drop on a slice to convert the
file. Everything runs offline. Windows 11, native C++20 / Win32 / COM.

Status: **M0 + M1** (gesture, overlay, PNG/JPG/BMP/TIFF conversion through WIC).

- Plan, REA stages, architecture mapping, decisions and open questions: [docs/PLAN.md](docs/PLAN.md)
- Build, run and the manual test checklist: [docs/TESTING.md](docs/TESTING.md)

## Layout

```
shiftdrop/
├─ CMakeLists.txt          MSVC, C++20, static CRT, Unicode; option SHIFTDROP_M0
├─ CMakePresets.json       x64-release (M1) and x64-release-m0
├─ vcpkg.json              manifest mode: wil
├─ docs/
│  ├─ PLAN.md
│  ├─ TESTING.md
│  └─ research/product-research-and-design.md   approved feature plan (S1–S6, M2–M8)
└─ src/
   ├─ app.manifest         PerMonitorV2, asInvoker, Common Controls v6, long paths
   ├─ ShiftDrop.rc         icon + VERSIONINFO
   ├─ ShiftDrop.ico        original artwork
   ├─ resource.h
   ├─ Brand.h              product name in one place
   ├─ Messages.h           WM_APP messages between hook, workers and the main window
   ├─ main.cpp             wWinMain, single instance, App (hidden window, gesture state, tray, batches)
   ├─ MouseHook.h/.cpp     WH_MOUSE_LL: records state, posts messages, nothing else
   ├─ Overlay.h/.cpp       layered topmost no-activate wheel, Direct2D + UpdateLayeredWindow, hit-testing
   ├─ DropTarget.h/.cpp    IDropTarget (WRL), CF_HDROP reading, COPY-never-MOVE
   ├─ Tray.h/.cpp          Shell_NotifyIcon, menu, balloon notifications
   ├─ JobQueue.h/.cpp      std::jthread worker pool (MTA), stop_token cancellation
   ├─ IConverter.h         converter interface: supports / actions / run
   ├─ WicConverter.h/.cpp  WIC decode → orientation → BGRA → encode; runtime codec discovery
   ├─ OutputPath.h/.cpp    "name (converted).ext", never overwrites
   ├─ Log.h/.cpp           %LOCALAPPDATA%\ShiftDrop\logs
   └─ Settings.h/.cpp      %LOCALAPPDATA%\ShiftDrop\settings.ini
```

## Build

```powershell
git clone https://github.com/CoPrime69/ShiftDrop C:\dev\shiftdrop
cd C:\dev\shiftdrop
# Developer PowerShell for VS 2022, with $env:VCPKG_ROOT set (see docs/TESTING.md)
cmake --preset x64-release
cmake --build --preset x64-release
.\build\x64-release\Release\ShiftDrop.exe
```

Clean-room note: ShiftDrop reimplements the behaviour of Tangerine (macOS, by @th2049) from public
descriptions only. No code, names, icons, artwork or text from Tangerine are used.
