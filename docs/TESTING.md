# Build, run and manual test checklist

Covers M0, M1 and M2. Section 3 (M2) is new; sections A to E are the M0/M1 checks.

## 1. One-time setup (PowerShell)

You need Visual Studio 2022 with the **Desktop development with C++** workload (it includes MSVC,
the Windows SDK and CMake), plus Git.

```powershell
# vcpkg (standalone clone; the copy bundled with VS also works if VCPKG_ROOT points at it)
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
$env:VCPKG_ROOT = 'C:\dev\vcpkg'

git clone https://github.com/CoPrime69/ShiftDrop C:\dev\shiftdrop
```

## 2. Configure, build, test, run

Open **Developer PowerShell for VS 2022** (so `cmake` and MSVC are on PATH):

```powershell
cd C:\dev\shiftdrop
git pull

cmake --preset x64-release          # first run builds wil, sqlite3, nlohmann-json, libwebp, gtest (a few minutes)
cmake --build --preset x64-release
ctest --preset x64-release          # unit tests
.\build\x64-release\Release\ShiftDrop.exe

# M0 build (placeholder circle; every drop only logs + notifies)
cmake --preset x64-release-m0
cmake --build --preset x64-release-m0
.\build\x64-release-m0\Release\ShiftDrop.exe
```

Follow the log live in a second window:

```powershell
Get-Content "$env:LOCALAPPDATA\ShiftDrop\logs\shiftdrop-$(Get-Date -Format yyyyMMdd).log" -Wait -Tail 30
```

Quit from the tray menu, or `Stop-Process -Name ShiftDrop`. Only one copy runs at a time.

Make test files:

```powershell
mkdir C:\temp\sd -Force; cd C:\temp\sd
Copy-Item "$env:WINDIR\Web\Wallpaper\Windows\img0.jpg" .\photo.jpg
Set-Content .\notes.txt "hello"
mkdir .\folder -Force; Set-Content .\folder\inside.txt "x"
mkdir C:\temp\sd-target -Force
```

Also put a PNG with transparency and a phone photo taken in portrait orientation in that folder.

## 3. M2 checklist

Tick each line. If a line fails, send me that line plus the log excerpt.

### F. Three wheels
- [ ] Drag `photo.jpg`, hold **Shift**: the Convert wheel shows PNG, JPG (dimmed), WebP, TIFF, BMP, Web; the centre says "Convert / 1 file"
- [ ] Hold **Ctrl** instead: the Tools wheel shows Rotate ↻, 1920 px, Flip, 50%, Rotate ↺, 1080 px
- [ ] Hold **Alt** instead: the File-actions wheel shows Copy, Move to…, Copy path, Terminal, Duplicate (note whether the source window's menu bar lights up when you let go of Alt)
- [ ] Drag `notes.txt` with Shift or Ctrl: every slice is dimmed, the centre says "Not an image"; with Alt every file action is available
- [ ] Drag `folder` with Alt: Duplicate is dimmed, the other four work

### G. Convert and tools
- [ ] Shift → **Web** on a large photo: one file `photo (converted).webp`, at most 2048 px on the long edge; no `~photo...sdpart` file is left behind and no intermediate PNG appears
- [ ] Shift → **WebP** on a transparent PNG: transparency is kept (check in a browser)
- [ ] Ctrl → **Rotate ↻** on `photo.jpg`: `photo (edited).jpg`, rotated clockwise, still a JPG
- [ ] Ctrl → **1080 px** on a small image (under 1080 px): output has the same size (never upscales)
- [ ] Portrait phone photo → Ctrl → Rotate ↻: the result is rotated relative to how the photo *looks* (EXIF applied first)
- [ ] Drop 5 images on Shift → BMP: one notification for the batch, ending with "Undo: tray menu > History"

### H. File actions
- [ ] Alt → **Copy** on 2 files, then Ctrl+V in another Explorer folder: both are copied
- [ ] Alt → **Copy path**, paste in Notepad: one quoted path per line
- [ ] Alt → **Terminal**: Windows Terminal (or PowerShell) opens in that folder
- [ ] Alt → **Duplicate** on `notes.txt`: `notes (copy).txt` with the same content
- [ ] Alt → **Move to…** on `photo.jpg` + `folder`: a folder picker titled "Move 2 items to" opens **in front** (tell me if it opens behind Explorer); pick `C:\temp\sd-target`; both move there; notification "Moved 2 item(s) to sd-target"
- [ ] Move a file into a folder that already has a file with that name: the existing file is untouched and the moved one gets a new name
- [ ] Cancel the folder picker: nothing moves

### I. History and undo
- [ ] Tray menu > **History…**: a window lists the runs above, newest first, with When / Action / Files / Result
- [ ] Select the Web run, **Undo**: the WebP goes to the Recycle Bin, the original stays, the row says "Undone"
- [ ] Convert again, open the output in Paint, change a pixel and save, then **Undo** that run: refused, "Not undone: ... was changed after ShiftDrop created it", nothing deleted
- [ ] Undo the Move run: the items go back to `C:\temp\sd`
- [ ] Undo a Move after creating a new `photo.jpg` in the original place: refused, nothing moved
- [ ] **Show files** (or double-click a row): Explorer opens with the outputs selected
- [ ] Esc closes the history window; Tab moves between the list and the buttons
- [ ] Drag the history window to a monitor with another scaling: text and buttons stay crisp and correctly sized

### J. Start with Windows, recipes, recovery
- [ ] First start: tray menu shows **Start with Windows** ticked; `Get-ItemProperty HKCU:\Software\Microsoft\Windows\CurrentVersion\Run` lists `ShiftDrop` pointing at this exe with `--autostart`
- [ ] Sign out and back in (or restart): ShiftDrop is in the tray, with no "is running" notification
- [ ] Untick Start with Windows: the Run value is gone; restarting ShiftDrop does not turn it back on
- [ ] Tray menu > **Open recipes folder**: `web-ready.json` is there. Copy it to `mine.json`, change `"id"` to `"mine"`, `"label"` to `"Mine"` and the quality to `140`; quit from the tray and start it again: a warning names `mine.json` and the quality field; fix it to `60` and start it again: a "Mine" slice appears on the Convert wheel
- [ ] Start a big batch, kill ShiftDrop from Task Manager mid-way, start it again: no `.sdpart` files remain; the log says "recovery: removed leftover staging file"; History shows the run as Interrupted

## 4. M0/M1 checklist

### A. Startup (both builds)
- [ ] A tray icon appears (blue disc with a white ring) and a notification says ShiftDrop is running, naming the build (M0 or M1)
- [ ] Right-click the tray icon: the menu shows History…, Open recipes folder, Open logs folder, Start with Windows and Quit; each works
- [ ] Launching the exe a second time shows "ShiftDrop is already running" and no second icon
- [ ] `settings.ini` exists in `%LOCALAPPDATA%\ShiftDrop` with `Convert=Shift`, `Tools=Ctrl`, `Files=Alt`
- [ ] The log lists the WIC decoders and encoders on your PC (note whether WebP/HEIC/AVIF appear)
- [ ] Task Manager: memory at idle is a few MB; CPU is 0% while you move the mouse normally
- [ ] Restart Explorer (`Stop-Process -Name explorer`): the tray icon comes back

### B. M0 gesture (M0 build)
- [ ] Drag `photo.jpg` in Explorer, then press and hold Shift while dragging: a circle labelled "ShiftDrop / 1 file" appears under the cursor (move the mouse a pixel if it doesn't appear at once)
- [ ] Press Shift first, then start the drag: the circle also appears
- [ ] Drop on the circle: the notification lists the file name; the log has the full path
- [ ] Clicking that notification opens Explorer with the dropped file selected
- [ ] The original file is still in place (Explorer did NOT move it even though Shift means "move")
- [ ] Drag 3 files at once: circle says "3 files"; all 3 paths are logged
- [ ] Drag from the Desktop: same behaviour
- [ ] Select text in Notepad or a browser and drag it with Shift held: no circle appears (log: "no CF_HDROP" or "released without an OLE drag")
- [ ] Drag a window by its title bar with Shift held: no circle appears
- [ ] With the circle visible, press Esc: it disappears and nothing is logged as dropped
- [ ] With the circle visible, drag out of it and drop on another folder: the circle disappears and Explorer performs its normal Shift = move

### C. M1 wheel (now the Convert wheel)
- [ ] Shift during a drag of `photo.jpg`: a wheel with PNG at the top, then clockwise JPG, WebP, TIFF, BMP, Web
- [ ] The JPG slice is dimmed (already JPG); hovering it shows the "not allowed" cursor
- [ ] Hovering PNG/BMP/TIFF highlights the slice in your accent colour; the cursor shows "+ Copy"
- [ ] Explorer's drag thumbnail stays visible over the wheel
- [ ] Drop on PNG: `photo (converted).png` appears next to the original; notification "Converted 1 file(s) to PNG"
- [ ] Click the notification: Explorer opens with the new file selected
- [ ] Repeat the same drop: `photo (converted 2).png` is created; the first output is untouched
- [ ] Release on the centre: nothing happens (cancel), the wheel closes
- [ ] Drag `notes.txt`: centre says "Not an image" and every slice is dimmed
- [ ] Drag `photo.jpg` + `notes.txt` together: "Not all images", every slice dimmed
- [ ] Drag 5 images, drop on BMP: 5 outputs, one notification for the batch; Explorer stays responsive during conversion
- [ ] Transparent PNG → JPG: transparent areas are white (not black)
- [ ] Portrait phone photo → PNG: output is upright (EXIF orientation applied). *Could not be verified in my test environment; please check this one carefully.*
- [ ] Drop into a read-only folder (or `C:\Program Files`): notification says it failed, with the reason; no partial file is left
- [ ] Light and dark mode (Settings > Personalisation > Colours): the wheel follows the app theme

### D. DPI and monitors
- [ ] With two monitors at different scaling (e.g. 100% and 150%), the wheel is the same physical size relative to the text on each, centred exactly on the cursor
- [ ] Hover highlighting matches the slice under the cursor on both monitors
- [ ] Near a screen edge the wheel stays centred on the cursor (partly off-screen is expected for now)

### E. Edge cases to observe (results go into Open questions)
- [ ] Run ShiftDrop **as administrator**: a warning notification appears; dragging from normal Explorer onto the wheel does not work (expected, UIPI)
- [ ] Run it normally and drag a file **from an elevated app** (e.g. an elevated Notepad's File > Open dialog): does the wheel appear? Does the drop arrive? Note what the log says
- [ ] Move the mouse over an elevated window: does the log show "hook: no events ... reinstalled"?
- [ ] Drag an image from Chrome/Edge with Shift: expected no wheel (virtual file, not CF_HDROP); note what the log says
- [ ] Drag an attachment from Outlook, a file from 7-Zip, a file from VS Code's explorer: note which show the wheel
- [ ] In `settings.ini`, set `Convert=Ctrl` and `Tools=Shift`, quit and start it again: the wheels swap keys. Set a key to `None`: that wheel never opens
- [ ] Left-handed mouse (swap buttons in Settings): does the gesture still work?

## 5. Installer

CI builds `ShiftDrop-Setup-<version>.exe` on every push and runs `scripts/test-installer.ps1` against
it. To build it yourself after section 2, with [Inno Setup 6.3+](https://jrsoftware.org/isinfo.php)
installed:

```powershell
./scripts/make-notices.ps1
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.2.0 installer\ShiftDrop.iss
.\build\installer\ShiftDrop-Setup-0.2.0.exe
```

### K. Install, upgrade, uninstall (on a PC without Visual Studio)

1. Run the setup. SmartScreen may warn (unsigned): **More info**, **Run anyway**. No UAC prompt appears.
2. The wizard shows the folder (`%LOCALAPPDATA%\Programs\ShiftDrop`) and a ticked "Start ShiftDrop when I
   sign in to Windows". Finish with "Launch ShiftDrop" ticked: the tray icon appears.
3. Start menu has ShiftDrop. Settings > Apps lists ShiftDrop with its version.
4. Run the same setup again while ShiftDrop is running: no "close the app" error; the old copy exits
   and the new one starts. The startup checkbox is not shown (your tray-menu choice is kept).
5. Untick "Start with Windows" in the tray menu, run the setup again, sign out and in: ShiftDrop does
   not start.
6. Uninstall from Settings > Apps while it runs: the tray icon goes away, the folder and Start menu
   entry are removed, `HKCU\…\Run` has no ShiftDrop value, and `%LOCALAPPDATA%\ShiftDrop` is still there.

