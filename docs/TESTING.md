# M0 + M1: build, run and manual test checklist

## 1. One-time setup (PowerShell)

You need Visual Studio 2022 with the **Desktop development with C++** workload (it includes MSVC,
the Windows SDK and CMake), plus Git.

```powershell
# vcpkg (standalone clone; the copy bundled with VS also works if VCPKG_ROOT points at it)
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
```

Clone the repository:

```powershell
git clone https://github.com/CoPrime69/ShiftDrop C:\dev\shiftdrop
```

## 2. Configure, build, run

Open **Developer PowerShell for VS 2022** (so `cmake` and MSVC are on PATH):

```powershell
cd C:\dev\shiftdrop

# M1 build (real wheel + WIC conversion)
cmake --preset x64-release
cmake --build --preset x64-release
.\build\x64-release\Release\ShiftDrop.exe

# M0 build (placeholder circle; every drop only logs + notifies)
cmake --preset x64-release-m0
cmake --build --preset x64-release-m0
.\build\x64-release-m0\Release\ShiftDrop.exe
```

The first configure downloads and installs `wil` into `build\<preset>\vcpkg_installed`.

Follow the log live in a second window:

```powershell
Get-Content "$env:LOCALAPPDATA\ShiftDrop\logs\shiftdrop-$(Get-Date -Format yyyyMMdd).log" -Wait -Tail 30
```

Quit from the tray menu, or `Stop-Process -Name ShiftDrop`. Only one copy runs at a time, so quit the
M0 build before starting the M1 build.

Make test files:

```powershell
mkdir C:\temp\sd -Force; cd C:\temp\sd
Copy-Item "$env:WINDIR\Web\Wallpaper\Windows\img0.jpg" .\photo.jpg
Set-Content .\notes.txt "hello"
```

Also put a PNG with transparency and a phone photo taken in portrait orientation in that folder.

## 3. Checklist

Tick each line. If a line fails, send me that line plus the log excerpt.

### A. Startup (both builds)
- [ ] A tray icon appears (blue disc with a white ring) and a notification says ShiftDrop is running, naming the build (M0 or M1)
- [ ] Right-click the tray icon: the menu shows "Open logs folder" and "Quit"; both work
- [ ] Launching the exe a second time shows "ShiftDrop is already running" and no second icon
- [ ] `settings.ini` exists in `%LOCALAPPDATA%\ShiftDrop` with `TriggerKey=Shift`
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

### C. M1 wheel (M1 build)
- [ ] Shift during a drag of `photo.jpg`: a wheel with PNG (top), JPG (right), BMP (bottom), TIFF (left) and "1 file" in the centre
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
- [ ] Set `TriggerKey=Ctrl` in `settings.ini`, restart, and repeat C.1 with Ctrl. Then `Alt`: does the source window's menu bar activate when you release Alt?
- [ ] Left-handed mouse (swap buttons in Settings): does the gesture still work?
