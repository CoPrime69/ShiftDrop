# Build, run and manual test checklist

Covers M0 to M3c. Sections M (video and audio), N (Compress and Fit to size) and S (Float shelf, carry and clipboard) are the newest; sections A to E are the M0/M1 checks.

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

./scripts/get-ffmpeg.ps1            # once: the pinned LGPL ffmpeg.exe into build\engines (170 MB download, SHA-256 checked)
cmake --preset x64-release          # first run builds wil, sqlite3, nlohmann-json, libwebp, gtest (a few minutes)
cmake --build --preset x64-release
ctest --preset x64-release          # unit tests (the MediaEngine ones run the real ffmpeg.exe)
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

For section M, make a test video and song with the bundled ffmpeg (or use your own):

```powershell
$ff = "$env:LOCALAPPDATA\Programs\ShiftDrop\engines\ffmpeg.exe"   # or .\build\engines\ffmpeg.exe
& $ff -f lavfi -i testsrc=s=1281x721:r=30:d=20 -f lavfi -i sine=f=440:d=20 -c:v mpeg4 -c:a pcm_s16le -shortest C:\temp\sd\clip.avi
& $ff -f lavfi -i sine=f=440:d=20 C:\temp\sd\song.wav
```

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

### L. Wheel redesign and top panel
- [ ] Drag a file and hold **Shift**: the wheel springs open around the cursor with one rounded tile per slice; Explorer's thumbnail disappears while you are over the wheel and comes back when you leave it
- [ ] Move over a tile: it lifts outward in your Windows accent colour and the centre shows what it does (e.g. "JPEG, quality 92")
- [ ] A top panel drops down at the top centre of that monitor with Convert / Tools / File actions and their keys, Convert highlighted
- [ ] Still dragging, press **Ctrl**, then **Alt**: the wheel turns to Tools, then File actions, and the panel's highlight slides along
- [ ] Turn the mouse wheel instead: each notch steps to the next or previous wheel (wraps round); the window under the cursor does not scroll; after the drop, scrolling works normally again
- [ ] Drop on a Convert tile with a few files: the panel turns into "Converting N files to …" with a progress bar, then "N files converted to …", and tucks away after about 2 s
- [ ] Make a batch fail partly (include a `.txt` renamed to `.png`): the result stays, with the reason, until you click it; clicking shows the outputs in Explorer
- [ ] Release over the centre: nothing runs, the wheel fades and the panel goes up
- [ ] Over the wheel Explorer's drag picture stays with its full name, with no "+ Copy" badge and no mouse pointer
- [ ] Tray menu > **Hide cursor over the wheel** unticked: the pointer stays visible over the wheel
- [ ] Drag off the wheel onto the desktop or a folder: the pointer comes back and Explorer shows its usual badge there
- [ ] Drag slowly out of the wheel: it fades as you pass the tiles and shrinks away smoothly when you leave it
- [ ] Drag a PDF (or `notes.txt`) and hold **Shift**: the Tools wheel opens with Inspect (nothing on Convert applies; before M4 this was the File-actions wheel); scrolling does not stop on the all-dimmed wheels
- [ ] Tray menu > **Size**: Small, Medium and Large change the wheel and panel size on the next drag; Auto is larger on a 2K/1440p screen than on 1080p
- [ ] Tray menu > **Style** > Glass: the wheel shows a frosted copy of what is behind it; Solid is opaque
- [ ] Windows Settings > Accessibility > Visual effects > Animation effects off: the wheel and panel appear and switch without animation

### M. Video and audio (M3a)
- [ ] The log at startup says `media: ffmpeg at …\engines\ffmpeg.exe`
- [ ] Drag `clip.avi` (or a phone video) and hold **Shift**: the Convert wheel shows MP4, MOV, MKV, WebM, AVI, WMV, GIF and MP3; AVI is dimmed (already AVI); no image formats
- [ ] Drag `song.wav` and hold **Shift**: MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA; WAV is dimmed
- [ ] Drag a video and a song together: only MP3 is offered
- [ ] Drag a photo and a video together: the wheel shows image and video formats, all dimmed, centre "Mixed files"
- [ ] Drop the video on **MP4**: the top panel shows "Converting 1 file to MP4…" with a percentage that climbs and a bar that moves smoothly; then "1 file converted to MP4"; `clip (converted).mp4` plays in the Films & TV / Media Player app, picture not stretched
- [ ] The log names the H.264 encoder once (`ffmpeg: H.264 encoder h264_nvenc` / `h264_qsv` / `h264_amf` / `h264_mf` / `libopenh264`). Task Manager shows `ffmpeg.exe` under ShiftDrop while it runs, at below-normal priority, and no console window ever appears
- [ ] Drop on **GIF**: an animated GIF, at most 480 px wide, that loops
- [ ] Drop on **MKV**: done almost instantly (streams copied), and the result plays
- [ ] Drop the song on each audio format: every result plays; **MP3** from the video gives just its sound
- [ ] Drop a long video (a few minutes) on **WebM**, then click the **×** in the top panel: it says "Cancelling…", then "Cancelled" / "Nothing was saved"; no `.sdpart` or `.webm` file is left in the folder and `ffmpeg.exe` is gone from Task Manager
- [ ] Drop three videos on MP4 and cancel during the second: "Cancelled after 1 of 3", the first output stays
- [ ] Quit ShiftDrop from the tray during a long conversion: `ffmpeg.exe` ends at once and no `.sdpart` is left
- [ ] Tray > History: the conversion is listed; Undo moves the output to the Recycle Bin
- [ ] Rename `engines\ffmpeg.exe` in the install folder and restart ShiftDrop: dragging a video and holding Shift shows the video formats dimmed with "Media engine missing" in the centre; images still work. Rename it back
- [ ] With Wi-Fi off, everything above works the same

### N. Compress and Fit to size (M3b)
- [ ] Drag a phone photo (JPG or PNG, a few MB) and hold **Ctrl**: the Tools wheel has **Compress** and **Under 1 MB**
- [ ] Drop on **Under 1 MB**: `photo (compressed).jpg` is at most 1 MB (Properties > Size), same width and height if it fit, and looks right
- [ ] Drop that result on **Under 1 MB** again: refused with "already under 1 MB", nothing written
- [ ] Drop a PNG screenshot with transparency on **Compress**: a JPG on white; a WebP stays WebP
- [ ] Drag a video and hold **Ctrl**: **Compress**, **50% size**, **Under 10 MB**, **Under 25 MB**; a song shows the same (results are .m4a)
- [ ] Drop a 1–3 minute 1080p video on **Under 10 MB**: progress in the top panel; `clip (compressed).mp4` is at most 10 MB and plays; if it needed another try the log says `media: fit attempt 2`
- [ ] Drop a 4K phone video on **Compress**: at most 1080p, smaller than the source
- [ ] Drop a video already under 10 MB on **Under 10 MB**: "already under 10 MB"
- [ ] Convert speed: drop an iPhone/Android H.264 or HEVC `.mov`/`.mp4` on **MP4** (or **MOV**): done in a second or two (streams copied; the log says `plan: copy video, copy audio`)
- [ ] Startup log: the H.264 encoder line appears a moment after start, before any video is dropped
- [ ] Cancel (×) during **Under 25 MB** on a long video: nothing left in the folder

### S. Float shelf, carry and clipboard (M3c)
Float shelf:
- [ ] Drag `photo.jpg` and hold **Alt**: the File-actions wheel has **Float** (pin icon); dropping on it slides a shelf out of the right edge of that screen with the photo's thumbnail, name and "size · folder"; the top panel says "photo.jpg on the Float shelf"
- [ ] About two seconds later (mouse elsewhere) the pinned shelf folds into a slim tab on the edge showing the thumbnail and "1"; hovering the tab unfolds it smoothly, leaving folds it again
- [ ] Drag three more files straight onto the tab (no key held): it unfolds as the drag arrives, the badge under the drag picture says "Add to the Float shelf", and the files are added at the top; dropping one that is already there moves it up instead of adding it twice
- [ ] Add more than five items: the list scrolls with the mouse wheel and shows a thin scroll bar
- [ ] Drag one item out into an Explorer folder: it is **copied** there (the original stays on the shelf and on disk); into a browser upload box, a mail message and a chat app (Teams, Discord, Slack, WhatsApp): each gets the file
- [ ] Ctrl+click two items (or click one and Shift+click another), drag one of them out: both go; the handle at the bottom says "Drag 2 selected into an app"; with nothing selected it says "Drag all N into an app" and dragging it carries everything
- [ ] Hover an item: a folder button (Explorer opens with it selected) and a × (removes it from the shelf only); right-click: Open, Show in folder, Copy, Remove, Pin/Unpin, Clear the shelf
- [ ] Click an item, then use the keyboard: Up/Down move, Shift+Up/Down extend, Ctrl+A, Enter opens, Delete removes, Ctrl+C copies (the footer says "Copied…"), Esc folds a pinned shelf
- [ ] Drag the shelf by its header to the middle of the screen: it floats there (no folding); drag it near the left, top or bottom edge and let go: it glides flush against that edge with square outer corners, and folds when you leave it
- [ ] The pin button in the header pins a floating shelf to the nearest edge and unpins a pinned one
- [ ] With two monitors at different scaling (100 % and 150 %), drag the shelf from one to the other: it changes size to match, pins to the edges of the monitor it is on and never spills onto the other one
- [ ] Delete or rename one of the shelf's files in Explorer: within a few seconds it disappears from the shelf
- [ ] The trash button (or "Clear the shelf") empties it and it fades away; Float on a file brings it back
- [ ] Quit and restart ShiftDrop with files on the shelf: it comes back (folded if pinned) in the same place with the same files; `%LOCALAPPDATA%\ShiftDrop\shelf.json` lists them
- [ ] Windows dark and light app theme: the shelf follows it (Settings > Personalisation > Colours), also while it is open; tray Style > Glass makes it translucent
- [ ] Animation effects off: the shelf appears, folds and moves without animation

Convert-and-carry and drop to clipboard (top panel):
- [ ] Convert `photo.jpg` to PNG: the result in the top panel stays while the mouse is over it, and its second line says "Drag into any app, or click to show in Explorer"; it has a pin button and a copy button
- [ ] Drag the result itself into a chat app or a browser upload box: the output file (`photo (converted).png`) arrives there, and the panel tucks away; drag a batch result of 3 files into an Explorer folder: all three are copied
- [ ] Drag the result onto the Float shelf: the outputs are added
- [ ] The **pin** button puts the outputs on the Float shelf (the button turns into a tick, the line says so)
- [ ] The **copy** button: paste in Explorer copies the output file; for a single image, paste in Paint, Word or a chat message box pastes the picture itself (with transparency where the app supports PNG)
- [ ] Copy something else right after clicking copy on a big photo: your newer copy is not replaced when the picture is ready
- [ ] A failed or cancelled job with no outputs shows no buttons and cannot be dragged

### O. Keyboard, Explorer selection, predicted output, inspector (M4)
Have ready: a large phone photo (with GPS), a small PNG, a video, a song, and a folder with 20 or more image recipes (copy `web-ready.json` in the recipes folder under new ids, then restart) for paging.

Keyboard during a drag
- [ ] Drag a photo, hold **Shift**, let go of the mouse-free hand: **Right** highlights the slice at 12 o'clock, then each one clockwise; **Left** goes back; dimmed slices are skipped; holding an arrow repeats
- [ ] **Up** / **Down** on a one-ring wheel switch to the previous / next wheel (like scrolling); the first slice of the new wheel is lit
- [ ] Press **Enter** on a highlighted slice: the job runs on the dragged photo (top panel), the drag ends as if Esc was pressed, and nothing is moved or copied into the folder under the cursor. Release the mouse afterwards: nothing happens
- [ ] A slice picked by key stays lit while the mouse is still; moving the mouse a few pixels hands the highlight back to the pointer
- [ ] **Esc** still cancels the drag; the source window never loses focus (its title bar stays active the whole time)

Wheel capacity
- [ ] With 9 to 16 recipes on a wheel, the slices sit on two rings; labels stay readable; hovering the outer ring lights the outer slice, the gap between the rings never "loses" the highlight; Up / Down move between the rings
- [ ] With more than 16, the last slice is **More** ("Page 1 of 2" in the centre): hovering it for a moment during a drag turns the page; Enter on it turns it too; the last page wraps to the first

Wheel on Explorer's selection (hotkey)
- [ ] Select three photos in File Explorer and press **Ctrl+Shift+Space**: the wheel opens at the pointer in click mode ("3 files"), Explorer keeps the focus
- [ ] Hover a slice and click: it runs on the three photos; arrows + Enter do the same; arrows do not move Explorer's selection while the wheel is open, and work again in Explorer once it closes
- [ ] **Esc**, a right-click, a click outside the wheel, or Alt+Tab close it without doing anything; clicking the centre closes it too
- [ ] Shift, Ctrl and Alt (pressed again) and the mouse wheel switch wheels
- [ ] On the desktop: select icons, press the hotkey: the same
- [ ] With nothing selected: a notification says "Nothing selected"
- [ ] In Excel (or VS Code), Ctrl+Shift+Space still does what it does there
- [ ] Tray menu > "Wheel on Explorer's selection (Ctrl+Shift+Space)" unticked: the hotkey does nothing; ticked again: it works. `[Keyboard] ExplorerHotkey=Ctrl+Alt+W` in settings.ini and a restart change the keys; a hotkey taken by another app gives a notification at startup

Right-click entry
- [ ] Right-click a photo in Explorer (Windows 11: Show more options): **ShiftDrop…** is there; it opens the wheel on the photo in click mode
- [ ] Select 20 files, right-click > ShiftDrop…: one wheel opens for all 20 (not 20 wheels), within about half a second
- [ ] Right-click a folder > ShiftDrop…: the File-actions wheel offers what applies to folders
- [ ] With ShiftDrop not running, right-click > ShiftDrop… starts it quietly (no "is running" notification) and opens the wheel
- [ ] Tray menu > the right-click entry toggle removes and adds it; uninstalling removes it

Predicted output
- [ ] Drag the large photo, hold Shift, hover **WebP**: a card under the wheel shows the thumbnail, "WebP", "~1.2 MB · was 3.4 MB" (numbers vary), the output name "photo (converted).webp" and "removes GPS, camera"
- [ ] Drop it: the real file's size is within about a third of the prediction
- [ ] Hover a rotate slice: the thumbnail on the card is turned the same way, and the detail says "4032×3024 → 3024×4032"
- [ ] Hover a Fit to… / resize slice: "→ 1920×1440"; hover Compress with a size target: "≤ 2 MB"
- [ ] The small PNG on **JPG**: the size has no "~" (exact)
- [ ] The video on **MKV**: "same streams, ~…"; on **MP4**: a size from duration × bitrate; the song on **MP3**: "audio only" style sizes
- [ ] Moving quickly across slices never stutters the wheel; the card follows within a moment; near the bottom of the screen it shows above the wheel
- [ ] File actions (Copy, Move to…) show no card

Inspector
- [ ] Drag the phone photo, hold **Ctrl**, drop on **Inspect**: a window opens with groups File, Image, Camera, GPS, …, EXIF; nothing is written next to the photo
- [ ] Type "gps" in the search box: only GPS fields remain; "canon 2023" matches fields containing both words
- [ ] Double-click a row, or select it and press Ctrl+C: the value is on the clipboard; right-click > Copy all shown fields gives every field as text
- [ ] Drop three files (photo, video, song) on Inspect: one window with three tabs; the video tab lists Media, Video track 1, Audio track 1, chapters and tags
- [ ] Move the window to a monitor with another scale: text and layout stay crisp and fit
- [ ] Esc closes the window; Ctrl+F jumps to the search box

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
./scripts/get-ffmpeg.ps1     # if section 2 hasn't already
./scripts/make-notices.ps1
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.3.1 installer\ShiftDrop.iss
.\build\installer\ShiftDrop-Setup-0.3.1.exe
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

