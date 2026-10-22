# Manual test checklist

ShiftDrop is a gesture app, so most of it can only be checked by hand on a real Windows PC. The unit
tests (`ctest`, see [BUILDING.md](BUILDING.md#run-the-tests)) cover the converters, recipes, naming,
history and the sandbox. This list covers the rest: the wheel, the windows, the shell integration and
how the results look.

Use it before a release, or after a change, run the sections it touches. Tick each line. When a line
fails, open an issue with that line, what you saw, and the matching part of the log.

## Before you start

Build and run ShiftDrop as described in [BUILDING.md](BUILDING.md), or install a release. Follow the
log live in a second window:

```powershell
Get-Content "$env:LOCALAPPDATA\ShiftDrop\logs\shiftdrop-$(Get-Date -Format yyyyMMdd).log" -Wait -Tail 30
```

Quit from the tray menu, or `Stop-Process -Name ShiftDrop`. Only one copy runs at a time.

Make some test files:

```powershell
mkdir C:\temp\sd -Force; cd C:\temp\sd
Copy-Item "$env:WINDIR\Web\Wallpaper\Windows\img0.jpg" .\photo.jpg
Set-Content .\notes.txt "hello"
mkdir .\folder -Force; Set-Content .\folder\inside.txt "x"
mkdir C:\temp\sd-target -Force
```

Also put a PNG with transparency and a phone photo taken in portrait orientation in that folder.
For the video and audio sections, make a test video and song with the bundled ffmpeg (or use your own):

```powershell
$ff = "$env:LOCALAPPDATA\Programs\ShiftDrop\engines\ffmpeg.exe"   # or .\build\engines\ffmpeg.exe
& $ff -f lavfi -i testsrc=s=1281x721:r=30:d=20 -f lavfi -i sine=f=440:d=20 -c:v mpeg4 -c:a pcm_s16le -shortest C:\temp\sd\clip.avi
& $ff -f lavfi -i sine=f=440:d=20 C:\temp\sd\song.wav
```

In this list, "Convert › PNG" means: drag the file, press **Shift**, and drop on the PNG slice of the
Convert section. "Open **Tools**" means: press **Shift** mid-drag, then scroll (or press Shift+2) to
the Tools section. Section Z needs a PC without Visual Studio.

## Sections

- A. Startup
- B. Placeholder gesture build
- C. Convert wheel basics
- D. Monitors, DPI and edge cases
- E. Wheel sections
- F. Convert and tools
- G. File actions
- H. History and undo
- I. Start with Windows, recipes, recovery
- J. Wheel look and top panel
- K. Video and audio
- L. Compress and Fit to size
- M. Reduce size
- N. PDF, text from anything and Share
- O. Float shelf, carry and clipboard
- P. Keyboard, Explorer selection, predicted output, inspector
- Q. Image canvas, Crop, Redact Photos, Share-safe, Remove metadata
- R. Photo tools: Edit, Annotate, Background, Collage
- S. AVIF, HEIC, SVG and multi-frame files
- T. Video and audio tools
- U. Documents, spreadsheets, subtitles, Office bridge
- V. Archives and the sandbox
- W. Files dragged out of browsers and mail apps
- X. Settings and tray entries
- Y. Toasts, more settings, Rename and screen readers
- Z. Install, upgrade, uninstall

### A. Startup
- [ ] A tray icon appears (the page with a folded corner, in one colour) and a notification says ShiftDrop is running, naming the version (the placeholder build says "M0 placeholder build")
- [ ] Right-click the tray icon: the menu includes History…, Open recipes folder, Open logs folder, Start with Windows and Quit; each works
- [ ] Launching the exe a second time shows "ShiftDrop is already running" and no second icon
- [ ] `settings.ini` exists in `%LOCALAPPDATA%\ShiftDrop` with `Convert=Shift`, `Tools=None`, `Files=None` (an older file with `Tools=Ctrl` / `Files=Alt` is switched to None once, and the log says so)
- [ ] The log lists the WIC decoders and encoders on your PC (note whether WebP/HEIC/AVIF appear)
- [ ] Task Manager: memory at idle is a few MB; CPU is 0% while you move the mouse normally
- [ ] Restart Explorer (`Stop-Process -Name explorer`): the tray icon comes back
- [ ] Tray glyph: white on a dark taskbar, near-black on a light one. Settings > Personalisation > Colours > "Choose your default Windows mode" Light, then Dark: the glyph switches within a second, with no restart; "Custom" with Windows dark and apps light keeps the white one
- [ ] Tray glyph at 100%, 125%, 150% and 200% scaling (Settings > Display > Scale, changed while it runs): sharp at each, no blur or soft edges; 175% is sharp too (the 32 px drawing scaled down)
- [ ] The app icon (the tilted page with the teal corner) on ShiftDrop.exe in Explorer at every view size, in Task Manager, on the Settings, History and editor title bars and in Alt+Tab, sharp at 100% to 200% scaling
- [ ] Tilted at every size: the tray glyph (white page or near-black page, with the teal corner, on a dark and a light taskbar), the 16 px title-bar icon of Settings, History, Rename, Inspect, a question window and every editor, the taskbar button, Alt+Tab, the toast's app icon and Explorer's Small icons and Details views all show the page tilted with the teal corner, not the upright page; the 16 px tray glyph and title-bar icon look crisp, not smeared, at 100%

### B. Placeholder gesture build
- [ ] In the placeholder build (preset `x64-release-m0`), drag `photo.jpg` in Explorer, then press and hold Shift while dragging: a circle labelled "ShiftDrop / 1 file" appears under the cursor (move the mouse a pixel if it doesn't appear at once)
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

### C. Convert wheel basics
- [ ] Shift during a drag of `photo.jpg`: the Convert wheel, with PNG at the top and the other image formats clockwise
- [ ] The JPG slice is dimmed (already JPG); hovering it, the centre says why
- [ ] Hovering PNG, BMP or TIFF highlights the slice in your accent colour
- [ ] Drop on PNG: `photo (converted).png` appears next to the original; the top panel says the file was converted
- [ ] Click the result in the top panel: Explorer opens with the new file selected
- [ ] Repeat the same drop: `photo (converted 2).png` is created; the first output is untouched
- [ ] Release on the centre: nothing happens (cancel), the wheel closes
- [ ] Drag 5 images, drop on BMP: 5 outputs, one result in the top panel for the batch; Explorer stays responsive during conversion
- [ ] Transparent PNG → JPG: transparent areas are white (not black)
- [ ] Portrait phone photo → PNG: output is upright (EXIF orientation applied)
- [ ] Drop into a read-only folder (or `C:\Program Files`): the top panel says it failed, with the reason; no partial file is left
- [ ] Light and dark mode (Settings > Personalisation > Colours): the wheel follows the app theme

### D. Monitors, DPI and edge cases
- [ ] With two monitors at different scaling (e.g. 100% and 150%), the wheel is the same physical size relative to the text on each, centred exactly on the cursor
- [ ] Hover highlighting matches the slice under the cursor on both monitors
- [ ] Near a screen edge the wheel stays centred on the cursor (it may be partly off-screen)
- [ ] Run ShiftDrop **as administrator**: a warning notification appears; dragging from normal Explorer onto the wheel does not work (expected, UIPI)
- [ ] Run it normally and drag a file **from an elevated app** (e.g. an elevated Notepad's File > Open dialog): does the wheel appear? Does the drop arrive? Note what the log says
- [ ] Move the mouse over an elevated window: does the log show "hook: no events ... reinstalled"?
- [ ] Drag a file from 7-Zip and from VS Code's explorer with Shift: note which show the wheel (browser and mail drags are section W)
- [ ] In `settings.ini`, set `Convert=Ctrl` and `Tools=Shift`, quit and start it again: the wheels swap keys. Set a key to `None`: that wheel never opens
- [ ] Left-handed mouse (swap buttons in Settings): does the gesture still work?

### E. Wheel sections
- [ ] Drag `photo.jpg`, hold **Shift**: the Convert section shows the image formats (PNG, JPG dimmed, WebP, AVIF, HEIC, TIFF, BMP, Web and PDF)
- [ ] Scroll one notch: Tools, with Rotate right, Rotate left, Flip, 1920 px, 1080 px, 50% and the other image tools
- [ ] Scroll again: Reduce size, then File actions (Copy, Move to folder, Copy path, Terminal, Duplicate and the rest), then back to Convert
- [ ] Drag `notes.txt` with Shift and scroll to File actions: every action that takes any file is available
- [ ] Drag `folder` with Shift and scroll to File actions: Duplicate is dimmed, the others work

### F. Convert and tools
- [ ] Convert › **Web** on a large photo: one file `photo (converted).webp`, at most 2048 px on the long edge; no `~photo...sdpart` file is left behind and no intermediate PNG appears
- [ ] Convert › **WebP** on a transparent PNG: transparency is kept (check in a browser)
- [ ] Tools › **Rotate right** on `photo.jpg`: `photo (edited).jpg`, rotated clockwise, still a JPG
- [ ] Tools › **1080 px** on a small image (under 1080 px): output has the same size (never upscales)
- [ ] Portrait phone photo on Tools › Rotate right: the result is rotated relative to how the photo *looks* (EXIF applied first)
- [ ] Drop 5 images on Convert › BMP: one result in the top panel for the batch

### G. File actions
- [ ] File actions › **Copy** on 2 files, then Ctrl+V in another Explorer folder: both are copied
- [ ] File actions › **Copy path**, paste in Notepad: one quoted path per line
- [ ] With a clipboard manager running (Win+V history on), **Copy**, **Copy path**, **Copy text** and the inspector's Ctrl+C still work every time
- [ ] File actions › **Terminal**: Windows Terminal (or PowerShell) opens in that folder
- [ ] File actions › **Duplicate** on `notes.txt`: `notes (copy).txt` with the same content
- [ ] File actions › **Move to folder** on `photo.jpg` + `folder`: a folder picker titled "Move 2 items to" opens **in front** of Explorer; pick `C:\temp\sd-target`; both move there; the top panel says they moved
- [ ] Move a file into a folder that already has a file with that name: the existing file is untouched and the moved one gets a new name
- [ ] Cancel the folder picker: nothing moves

### H. History and undo
- [ ] Tray menu > **History…**: a window lists the runs above, newest first, with When / Action / Files / Result
- [ ] Select the Web run, **Undo**: the WebP goes to the Recycle Bin, the original stays, the row says "Undone"
- [ ] Convert again, open the output in Paint, change a pixel and save, then **Undo** that run: refused, "Not undone: ... was changed after ShiftDrop created it", nothing deleted
- [ ] Undo the Move run: the items go back to `C:\temp\sd`
- [ ] Undo a Move after creating a new `photo.jpg` in the original place: refused, nothing moved
- [ ] **Show files** (or double-click a row): Explorer opens with the outputs selected
- [ ] Esc closes the history window; Tab moves between the list and the buttons
- [ ] Drag the history window to a monitor with another scaling: text and buttons stay crisp and correctly sized
- [ ] Windows in dark mode: the history window is dark like Settings (title bar, list, its header and buttons); switch Windows to light while it is open and it turns light at once, with its grey face as before
- [ ] Type part of a file's name in the search box: the list narrows with every letter to the runs with that file; an action's name (`png`) finds its runs; two words (`png holiday`) need both; clearing the box shows everything again
- [ ] **Action** shows "All actions" and each action used, once; picking one keeps only its runs. **When**: Any time, Today, Last 7 days. All three work together, and the line by the buttons says "N of M runs" (or that nothing matches)
- [ ] An undone run's row is grey and says "Undone"; a failed run's row (drop a `.txt` renamed to `.png` on PNG) is red and says "Failed"; both in dark mode too
- [ ] **Clear history** asks first, saying only the list is cleared and cleared runs can no longer be undone. Cancel keeps everything. **Clear history** empties the list ("Cleared N runs. No file was changed."); every output file is still where it was, and the next conversion shows up as before. A run still converting when you clear stays in the list

### I. Start with Windows, recipes, recovery
- [ ] First start: tray menu shows **Start with Windows** ticked; `Get-ItemProperty HKCU:\Software\Microsoft\Windows\CurrentVersion\Run` lists `ShiftDrop` pointing at this exe with `--autostart`
- [ ] Sign out and back in (or restart): ShiftDrop is in the tray, with no "is running" notification
- [ ] Untick Start with Windows: the Run value is gone; restarting ShiftDrop does not turn it back on
- [ ] Tray menu > **Open recipes folder**: `web-ready.json` is there. Copy it to `mine.json`, change `"id"` to `"mine"`, `"label"` to `"Mine"` and the quality to `140`; quit from the tray and start it again: a warning names `mine.json` and the quality field; fix it to `60` and start it again: a "Mine" slice appears on the Convert wheel
- [ ] Start a big batch, kill ShiftDrop from Task Manager mid-way, start it again: no `.sdpart` files remain; the log says "recovery: removed leftover staging file"; History shows the run as Interrupted

### J. Wheel look and top panel
- [ ] Drag a file and hold **Shift**: the wheel springs open around the cursor with one rounded tile per slice
- [ ] Move over a tile: it lifts outward in the accent colour (ShiftDrop's teal unless Settings › Accent colour says otherwise) and the centre shows what it does (e.g. "JPEG, quality 92")
- [ ] A top panel drops down at the top centre of that monitor with Convert / Tools / File actions (names only, no Shift / Ctrl / Alt keycaps), Convert highlighted
- [ ] Still dragging, press **Ctrl** or **Alt**: nothing changes (only Shift opens the wheel; Ctrl and Alt keep their usual drag meaning)
- [ ] Turn the mouse wheel instead: each notch steps to the next or previous wheel in order, Convert, Tools, File actions, Convert... (wraps round both ways, never skips one); the window under the cursor does not scroll; after the drop, scrolling works normally again
- [ ] No mouse wheel: still dragging with Shift held, tap **1**, **2**, **3** or **4**: the wheel jumps straight to that section (Convert, Tools, Reduce size, File actions); with a section turned off in Settings the numbers follow the sections that are left. The top panel hint reads "Scroll or Shift+1–4"
- [ ] Drop on a Convert tile with a few files: the panel turns into "Converting N files to …" with a progress bar, then "N files converted to …", and tucks away after about 2 s
- [ ] Make a batch fail partly (include a `.txt` renamed to `.png`): the result stays, with a warning icon and the reason, until you click it; clicking shows the outputs in Explorer
- [ ] Release over the centre: nothing runs, the wheel fades and the panel goes up
- [ ] When the wheel opens, Explorer's drag picture (the file's icon or thumbnail with its full name) stays visible over the wheel and follows the cursor across the tiles, with the mouse pointer; there is no "+ Copy" badge under it. Several files show Explorer's stack and count the same way. The file's own icon staying where it was is Windows: a drag only copies until you drop
- [ ] With the panel at the top: drag the file up over the top panel, and to the bottom tiles while the predicted-output card shows: the drag picture stays on top of both
- [ ] Drag a picture off a web page in Edge or Chrome (or an attachment out of Outlook) and press Shift: the browser's (or Outlook's) own drag picture stays over the wheel too, no "+ Copy" badge
- [ ] Drag off the wheel onto the desktop or a folder: Explorer shows its usual badge there ("+ Copy to Desktop")
- [ ] The tray menu has no **Hide cursor over the wheel** item, and the pointer is never hidden over the wheel. A `settings.ini` from an older version with `HideCursor=1` under `[Appearance]` changes nothing, and the key is gone after the next Size or Style change
- [ ] Drag slowly out of the wheel: it fades as you pass the tiles and shrinks away smoothly when you leave it
- [ ] Labels are never cut off: drag `song.mp3`, a video, a BMP and a PDF and open **Tools** (and page with **More**) at Size Small, Medium, Large and Auto, at 100%, 125%, 150% and 200% scaling: no slice label ends in "…" anywhere. **Under 10 MB** and **Under 25 MB** read in full (on two lines, "Under" over "10 MB", when one is too wide), **Compress** and **50% size** are whole, and every label keeps clear of its tile's edges and of the centre disc with a little room to spare
- [ ] One label style on every tile: every slice label is the same size and weight (13 px Semibold at Size Medium, larger only with Small / Medium / Large and the screen's scaling), on the inner and the outer ring, on icon tiles and on text-only tiles (**Reduce size** 90%…20%, **PNG**, **Under 10 MB**) alike; a wheel never mixes two label sizes. A label too long for one line goes onto two at a space and is never made smaller; a number and its unit ("10 MB", "1920 px") always stay on one line. The log line `overlay: label '…' does not fit its tile` never appears for ShiftDrop's own slices
- [ ] One icon size on both rings: on **File actions** with a PDF (two rings) the inner and the outer icons are the same size (20 px at Medium), each above its label
- [ ] Drag a file from Explorer and move slowly across the tiles with Shift: Explorer's drag picture never sits on the hovered tile's label; the label slides along its tile, out from under the picture, and back when the picture moves on (the wheel itself, its tiles and gaps do not move)
- [ ] The centre: the main line ("1 file", "1.0 MB", "Not for videos") is larger and bold (20 px), the line under it ("Save a WebP copy next to it", the section name) small and grey (12 px); a long main line goes onto two lines, never "…"
- [ ] Style **Solid**: the tiles sit on a round plate (white in light mode, `#1A1C22` in dark) with a fine edge; Glass keeps the frosted plate
- [ ] The image Tools wheel shows **Rotate right** and **Rotate left** (not two "Rotate" slices told apart by arrows); the video and audio editor slices read **Trim**, **Crop**, **Speed**, **Join**, **Frames**, **Split**, **Redact**, **Channel**, **Bleep**, **Visuals** with no "…"; File actions shows **Move to folder** and **Rename**
- [ ] Drag a file from Explorer with Shift: the drag picture sits over the centre, and the count ("1 file", "3 files", or the size on Reduce size) is in a small rounded badge just below the picture (above it when the picture hangs low), never hidden under it; arrow keys while the cursor is still in the centre show the picked slice's detail in that badge. Move onto a slice and the count is back in the centre with the wheel's name under it. In click mode (Explorer selection) there is no picture, and the count sits in the centre as before
- [ ] Drag `notes.txt` and hold **Shift**: the Tools wheel opens with Inspect (nothing on Convert applies); scrolling still visits the all-dimmed wheels, which show why nothing applies
- [ ] A new install (no settings.ini) starts at Size **Auto** with the top panel at 100%; an existing settings.ini keeps the Size it has (Medium stays Medium)
- [ ] Tray menu > **Size**: Small, Medium and Large change the wheel and panel size on the next drag; Auto is larger on a 2K/1440p screen than on 1080p
- [ ] 1080p at 100%, Size Auto: the top panel's wheel names are easy to read at arm's length (about 18 px text, a panel about 62 px tall), clearly larger than the wheel's own labels; the hint "Scroll or Shift+1–4" is readable too; edges and text are sharp, not blurred
- [ ] 2560x1440 at 100% and at 125%, Size Auto: the panel is larger again (about 24 px text, about 80 px tall) and still sharp; on a second monitor at a different scaling it is the same size relative to that monitor's text
- [ ] A 1366x768 screen at 100% (or a window-sized VM), every Size: the whole panel fits on screen with all four names and the hint; at 150% with Size Large the hint is dropped before any name is cut
- [ ] At the new size, the progress panel (spinner, "Converting N files to …", "· 40%", ×, the thin bar along its bottom edge) and the result panel (handle, two lines, Float and Copy buttons) only grow: nothing is cut off and the × and buttons are easy to hit
- [ ] Progress, as the brand kit draws it: a small accent spinner, the job's name, then "· N%" in grey (one file of a video moves within itself), a round × at the end that turns red under the mouse, and a thin accent bar along the bottom edge of the pill
- [ ] A finished convert: on the left a small chip with grip dots and a page with an accent corner, then "N files converted to …" over a small grey line, then the round Float and Copy buttons; hovering the pill says "Click to show in folder · drag the file out" ("the files" for several); dragging the chip (or the pill) carries the outputs into another app
- [ ] A failed job (rename a `.txt` to `.png` and convert it alone): the pill is outlined in red, with a red ! icon, "Couldn’t convert name.png" and "Click to dismiss" beside it, the reason under it; it stays until clicked, and clicking only dismisses it (no Explorer window)
- [ ] The predicted-output card under the wheel: white (light) or `#1F2127` (dark) with a soft shadow and small corners; the picture's thumbnail (or a page mark for a file without one), "WebP" in bold with "1.2 MB · was 3.4 MB" in grey on the right, and the output's name under it; a third grey line only when something changes (pixel size, metadata removed)
- [ ] Settings › **Top panel size**: a slider from 50% to 300% in 10% steps (100% by default), with its value beside it ("160%"); arrow keys move it one step. At 100% the panel is as above; drag a file with Shift after each change (no restart): the switcher, the progress and the result panels grow by that share on top of the tray's Size (Small, Medium, Large and Auto still differ), text, icons, gaps and corners together, sharp at 100%, 125% and 150% scaling. At 50% the panel is half that size and still sharp, all four names whole
- [ ] Top panel size at 300% on a 1366x768 screen (or at 150% scaling): the panel grows only as far as all four names stay whole and it stays in the top half of the screen (the log says `island: top panel size 300% does not fit this screen; drawn at N%`); the value survives a restart as `[Appearance] PanelSize=` in settings.ini
- [ ] Tray menu > **Style** > Glass: the wheel shows a frosted copy of what is behind it; Solid is opaque
- [ ] Windows Settings > Accessibility > Visual effects > Animation effects off: the wheel and panel appear and switch without animation

### K. Video and audio
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

### L. Compress and Fit to size
- [ ] Drag a phone photo (JPG or PNG, a few MB), press **Shift** and scroll to **Tools**: it has **Compress** and **Under 1 MB**
- [ ] Drop on **Under 1 MB**: `photo (compressed).jpg` is at most 1 MB (Properties > Size), same width and height if it fit, and looks right
- [ ] Drop that result on **Under 1 MB** again: refused with "already under 1 MB", nothing written
- [ ] Drop a PNG screenshot with transparency on **Compress**: a JPG on white; a WebP stays WebP
- [ ] Drag a video and open **Tools**: **Compress**, **50% size**, **Under 10 MB**, **Under 25 MB**; a song shows the same (results are .m4a)
- [ ] Drop a 1–3 minute 1080p video on **Under 10 MB**: progress in the top panel; `clip (compressed).mp4` is at most 10 MB and plays; if it needed another try the log says `media: fit attempt 2`
- [ ] Drop a 4K phone video on **Compress**: at most 1080p, smaller than the source
- [ ] Drop a video already under 10 MB on **Under 10 MB**: "already under 10 MB"
- [ ] Convert speed: drop an iPhone/Android H.264 or HEVC `.mov`/`.mp4` on **MP4** (or **MOV**): done in a second or two (streams copied; the log says `plan: copy video, copy audio`)
- [ ] Startup log: the H.264 encoder line appears a moment after start, before any video is dropped
- [ ] Cancel (×) during **Under 25 MB** on a long video: nothing left in the folder

### M. Reduce size
- [ ] Drag a 3 MB photo, press Shift and scroll to **Reduce size** (Convert, Tools, Reduce size, File actions): eight slices, −90% to −20%; the centre shows "3 MB" (the photo's size)
- [ ] Hover **−90%**: the centre shows "≈ 307 KB" with "was 3 MB" under it; moving to **−50%** changes it to "≈ 1.5 MB"
- [ ] Drop on **−90%**: `photo (reduced).jpg` appears, at most 10% of the original size (WebP stays WebP; PNG becomes JPEG); if quality alone cannot get there the picture is made smaller too
- [ ] The same with a video (MP4 out, at most the shown size) and a song (M4A out)
- [ ] A PDF of scans on **−70%**: `(reduced).pdf` at most 30% of the size; a PDF of mostly text gets as small as it can and the panel says it could not get smaller; one that cannot shrink at all leaves no file
- [ ] Three files: the centre shows their total size, and hovering shows the total they come out at

### N. PDF, text from anything and Share
Have ready: a text PDF with a few pages (`report.pdf`, e.g. saved from Word), a two-column PDF
(`paper.pdf`, e.g. a journal article), a scanned PDF (a phone scan with no text layer), two more PDFs, a phone photo of a printed page (`page.jpg`, taken upright and
one taken sideways), a PNG with transparency, and a PDF with a password.

- [ ] Drag `report.pdf` and hold **Shift**: Convert shows JPG, PNG, TXT and Markdown
- [ ] Drop on **PNG**: a folder `report (pages)` holds `report page 01.png`, `report page 02.png`, … at 300 DPI (an A4 page is 2480 × 3508); a one-page PDF gives `report (converted).png` instead
- [ ] Drop on **JPG**: the same as JPEGs; they open in Photos
- [ ] Drop on **TXT**: `report (converted).txt` has the text in reading order with blank lines between paragraphs; on **MD**, headings get `#`, paragraphs are one line each, bullets become `- `
- [ ] Drop `paper.pdf` on **TXT** and on **MD**: the title and any heading that runs across both columns come first, then the left column top to bottom, then the right; lines of the two columns are never mixed. A one-column PDF reads as before
- [ ] Drop the scanned PDF on **TXT**: the text comes from OCR (the log says `ocr`); with no OCR language installed, the file says nothing for those pages and the log explains why
- [ ] Drag three photos (one PNG with transparency) with **Shift** and drop on **PDF**: one `… (converted).pdf` with the pages in Explorer's order, each the photo's size; the sideways phone photo stands upright; the transparent PNG shows on white
- [ ] Drag two PDFs, open **Tools**, drop on **Merge**: a window lists them in Explorer's order with page counts. Drag one row above the other (a blue line shows where it lands), or select it and press **Alt+↑**; **Merge** (or Enter) writes `first (merged).pdf` with the pages in that order; the panel says "Merged 2 PDFs into N pages". Esc or Cancel writes nothing
- [ ] Drop one PDF on **Merge**: the panel says to drop two or more
- [ ] In Merge, **More…** (left of Cancel) shows every page of both PDFs, numbered in the final order, each with its file's colour tag (the list's rows carry the same tags). Drag a page of the second PDF in among the first's; Ctrl/Shift+click several, **Alt+arrows** move them, **R** turns, **Delete** leaves out. The preview on the right shows the selected page with Previous / Next and "k of N"; double-click a page to view it large. **Merge and export** writes `first (merged).pdf` exactly as shown; Undo in the panel takes it back
- [ ] In that view, right-click a page (or **Edit page** above the preview) > **Redact…**: the page opens in the Redact editor; box a line and apply: the page gets a pencil mark and the box. Annotate, Crop and Light and colour work the same way; **Revert** brings back the original. In the exported PDF, Ctrl+F in Edge finds none of the redacted text; the source PDFs are unchanged
- [ ] Edit PDF on a 4-page PDF: step through every page with **Next** and by clicking each thumbnail, at 100% and again at 150% or 200% display scale (or after moving the window to a monitor with another scale): the large preview turns sharp within a moment on every page, not only the first, and small text in it is as crisp as in Edge at that size
- [ ] Redact one line of a table on page 3 and apply: the thumbnail and the large preview show page 3 with the box (never a blank page with a grey "3"). Redact it again: both update. **Revert** (or Undo): the original page is back in both. In the saved PDF the box covers that one line, exactly where it was in the editor, and the rest of the page is unchanged
- [ ] Edit PDF > right-click a page > **Fill and sign…**: type into the form's blanks, add a tick, the date and a signature, apply. In the saved PDF at 400 % in Edge the page's own text and the typed text are both sharp; Ctrl+F finds the typed words and copying them pastes the same text; the signature has no white box around it. Edit the page again: the marks come back editable
- [ ] Fill and sign a sideways (rotated) page and a page with a cropped margin: the marks land exactly where they were placed
- [ ] New text boxes start at 32 pt: Fill and sign (Text and Date), Annotate on a PDF page (Text and Callout) and Annotate Photos (32 px) all show 32 in Size for the first box. Change Size with nothing selected: the next box in that editor uses it; a newly opened editor starts at 32 again
- [ ] Merge two PDFs > **More…**, edit a page, **Back**, **More…** again, edit another page, **Merge and export**: both edited pages are in the output, each with its own edit
- [ ] Merge two PDFs > **More…** > **Add files…** (bottom left): pick a third PDF (and one already in the merge): the new one's pages appear after the others with their own colour tag and sharp thumbnails, the line above the pages says "Added name.pdf (N pages)" and that the other is already there; Ctrl+Z takes the added pages out again. **Back**: the list shows the new PDF last; **Merge and export** writes all three. A `.txt` renamed to `.pdf` is not added, and the line says why
- [ ] Drop one PDF on **Edit**, **Add files…** another: the title becomes "Edit 2 PDFs", Save becomes **Merge and export**, and the output has both PDFs' pages
- [ ] **Back** returns to the list with the page changes kept ("3 of 5 pages"); swap the files: each file's pages follow their file, and pages moved in among the other file's go back to their own (the list says so). Cancel after page changes asks first
- [ ] Drop `report.pdf` on **Edit** (Tools, next to Organize): the page view opens directly and **Save** writes `report (edited).pdf`. Drop two PDFs on Edit: both in one view, **Back** shows the list, **Merge and export** writes `first (merged).pdf`. A 300-page PDF fills in the pages on screen first and scrolls smoothly. Settings > Tools > PDF lists Edit
- [ ] Drop `report.pdf` on **Pages**: a grid of page thumbnails. Click a page, Shift+click another (range), Ctrl+click toggles; drag the selection elsewhere; **R** / **Shift+R** rotate; **Ctrl+D** duplicates; **Delete** removes; **Ctrl+Z** undoes; arrows move the focus, Ctrl/Alt+arrows move the pages. **Save** writes `report (organized).pdf` exactly as shown; bookmarks to pages that are kept still work. Cancel after changes asks first
- [ ] Hover a dimmed slice (for example HEIC on a PC without the HEIF extension): the wheel's centre says why it can't be used, and fades back when you move off
- [ ] Drop two PDFs on **Pages**: two windows open, one per PDF; saving one writes only that PDF's `(organized)` copy, and the other window stays open
- [ ] Organize, Merge (and its pages) and Edit follow the Windows light/dark setting, and look right at 150% scaling
- [ ] Drop a large scanned PDF on **Compress**: the panel says "X to Y (N% smaller)" and the result looks the same at normal zoom; **Smallest** gives a smaller file. Drop the compressed file on Compress again: it is skipped ("already compact"), no bigger file is ever kept
- [ ] Drop `page.jpg` on **Copy text** (Tools): the panel says "Copied N words"; Ctrl+V in Notepad pastes the text. Drop `report.pdf` and the photo together: "Copied N words from 2 files". No file is written
- [ ] Drop the scanned PDF (or `page.jpg`) on **OCR PDF**: `… (searchable).pdf` looks exactly like the scan; in Edge, Ctrl+F finds a word and selecting text highlights it over the picture; copied text pastes correctly. `report.pdf` on OCR PDF is skipped ("already searchable")
- [ ] Open the `… (searchable).pdf` (from a PDF and from `page.jpg`) in Adobe Acrobat Reader: no "Cannot find or create the font" message; File > Properties > Fonts lists SDGlyphless (Embedded); Ctrl+F finds a word, selecting highlights over the scan, and copied text pastes correctly. The same for a Fill and sign and an Annotate (text box) output: its fonts are listed as Embedded Subset and Acrobat opens it silently
- [ ] Settings > Time & language > Language: with no OCR-capable language, Copy text and Searchable are dimmed for photos and say why in the centre
- [ ] Drag two files, open **File actions**, drop on **Share**: the Windows share sheet opens near the cursor with both files (Nearby sharing, Phone Link, Mail, other apps). Share to Mail or OneNote: the files arrive. Close the sheet: nothing else happens
- [ ] The password PDF on any PDF slice: the panel says "the PDF is locked with a password"; a `.txt` renamed to `.pdf` says "not a PDF file"
- [ ] Tray > History lists each PDF job; Undo moves its output (or the pages folder) to the Recycle Bin
- [ ] With Wi-Fi off, everything above works the same

### O. Float shelf, carry and clipboard
Float shelf:
- [ ] The shelf's look, light and dark: small (8 px) corners; a header with the pin icon, "Float shelf" and "N files" in grey, then Pin, **Clear all** and Close as the Settings window's buttons (square-cornered hover, a push button for Clear all, the same focus ring under Tab); a selected row has a light accent wash with a short accent bar on its left; a hovered row shows the folder and × buttons in the same style
- [ ] An empty shelf: a dashed grey outline with a blank page and "Drag files here to keep them for later." on two lines; dragging files over it turns the outline and the page's corner accent
- [ ] The folded tab is wider (30 px) and shows the newest thumbnail over an accent badge with the count in white
- [ ] Drag `photo.jpg` and open **File actions**: it has **Float** (pin icon); dropping on it slides a shelf out of the right edge of that screen with the photo's thumbnail, name and "size · folder"; the top panel says "photo.jpg on the Float shelf"
- [ ] About two seconds later (mouse elsewhere) the pinned shelf folds into a slim tab on the edge showing the thumbnail and "1"; hovering the tab unfolds it smoothly, leaving folds it again
- [ ] Drag three more files straight onto the tab (no key held): it unfolds as the drag arrives, the badge under the drag picture says "Add to the Float shelf", and the files are added at the top; dropping one that is already there moves it up instead of adding it twice
- [ ] Add more than five items: the list scrolls with the mouse wheel and shows a thin scroll bar
- [ ] Drag one item out into an Explorer folder: it is **copied** there (the original stays on the shelf and on disk); into a browser upload box, a mail message and a chat app (Teams, Discord, Slack, WhatsApp): each gets the file
- [ ] Ctrl+click two items (or click one and Shift+click another), drag one of them out: both go; the handle at the bottom says "Drag 2 selected into an app"; with nothing selected it says "Drag all N into an app" and dragging it carries everything
- [ ] Hover an item: a folder button (Explorer opens with it selected) and a × (removes it from the shelf only); right-click: Open, Show in folder, Copy, Remove, Pin/Unpin, Clear all, Close the shelf
- [ ] Click an item, then use the keyboard: Up/Down move, Shift+Up/Down extend, Ctrl+A, Enter opens, Delete removes, Ctrl+C copies (the footer says "Copied…"), Esc folds a pinned shelf
- [ ] Drag the shelf by its header to the middle of the screen: it floats there (no folding); drag it near the left, top or bottom edge and let go: it glides flush against that edge with square outer corners, and folds when you leave it
- [ ] The pin button in the header pins a floating shelf to the nearest edge and unpins a pinned one
- [ ] With two monitors at different scaling (100 % and 150 %), drag the shelf from one to the other: it changes size to match, pins to the edges of the monitor it is on and never spills onto the other one
- [ ] Delete or rename one of the shelf's files in Explorer: within a few seconds it disappears from the shelf
- [ ] The header reads "Float" with the count ("3 files", "1 folder", "4 items" when both); **Clear all** (next to the ×, or right-click › "Clear all") empties it and it **stays**: the count goes, a dashed box under the header says "Drop files here" (no footer, no Clear all); the files are still where they were in Explorer; removing the last item with its × or Delete, or deleting its file in Explorer, leaves the same empty shelf
- [ ] Empty shelf: drag a file from Explorer, one from the desktop, and a picture out of a web page (Edge, Chrome, Firefox) onto the dashed box: it fills with the accent colour while the drag is over it, the badge says "Add to the Float shelf", and on drop each file is added (the browser's picture is saved where tray › Save browser drops to says, then put on the shelf); the originals are not moved or changed and no wheel opens
- [ ] Empty and pinned: it folds into a tab with the pin icon; dragging a file onto the tab unfolds it and the drop adds the file
- [ ] The × at the right of the header (tooltip "Close the shelf (the tray icon brings it back)") hides it; the tray's **Show Float shelf** is now unticked; ticking it brings the shelf back where it was with its files; Alt+F4 on the shelf and right-click › "Close the shelf" do the same
- [ ] Close the shelf, quit and restart ShiftDrop: it stays hidden (`"shown": false` in shelf.json); tick **Show Float shelf**: back
- [ ] With the shelf closed, use **Float** on a file (or a result's Float button): the shelf comes back with the file on it and **Show Float shelf** is ticked again, as before
- [ ] Click the shelf, then Tab: the focus moves from the list to Pin, Clear all and the × (a ring around the button), Shift+Tab goes back; Enter or Space presses the button; hover each header button: a tooltip names it
- [ ] Empty shelf at every tray Size (Small, Medium, Large, Auto) and at 100 %, 150 % and 200 % scaling, light and dark, Solid and Glass: the dashed box, its icon and "Drop files here" are centred and crisp, nothing is cut off, and the × and pin buttons are the same size as with files, and "Clear all" fits its button
- [ ] Quit and restart ShiftDrop with files on the shelf: it comes back (folded if pinned) in the same place with the same files; `%LOCALAPPDATA%\ShiftDrop\shelf.json` lists them; with an empty shelf that was on, it comes back empty
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

### P. Keyboard, Explorer selection, predicted output, inspector
Have ready: a large phone photo (with GPS), a small PNG, a video, a song, and a folder with 20 or more image recipes (copy `web-ready.json` in the recipes folder under new ids, then restart) for paging.

Keyboard during a drag
- [ ] Drag a photo, hold **Shift**, let go of the mouse-free hand: **Right** highlights the slice at 12 o'clock, then each one clockwise; **Left** goes back; dimmed slices are skipped; holding an arrow repeats
- [ ] **Up** / **Down** on a one-ring wheel switch to the previous / next wheel (like scrolling); the first slice of the new wheel is lit
- [ ] With the wheel open from Explorer, press **1**–**4** (with or without Shift): the wheel jumps to that section
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
- [ ] The mouse wheel switches sections, in order and wrapping round
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
- [ ] File actions (Copy, Move to folder) show no card

Inspector
- [ ] Drag the phone photo, open **Tools**, drop on **Inspect**: a window opens with groups File, Image, Camera, GPS, …, EXIF; nothing is written next to the photo
- [ ] Type "gps" in the search box: only GPS fields remain; "canon 2023" matches fields containing both words
- [ ] Double-click a row, or select it and press Ctrl+C: the value is on the clipboard; right-click > Copy all shown fields gives every field as text
- [ ] Drop three files (photo, video, song) on Inspect: one window with three tabs; the video tab lists Media, Video track 1, Audio track 1, chapters and tags
- [ ] Move the window to a monitor with another scale: text and layout stay crisp and fit
- [ ] Esc closes the window; Ctrl+F jumps to the search box
- [ ] Windows in dark mode: the inspector is dark (title bar, search box, list and its group headings readable); switching Windows to light while it is open turns it light

### Q. Image canvas, Crop, Redact Photos, Share-safe, Remove metadata
Have ready: a phone photo with GPS (portrait, taken with the phone upright), a PNG with transparency, a screenshot that shows an email address, a phone number and a card number (use a test number such as 4111 1111 1111 1111), a group photo with faces, a short phone video, and an MP3 with tags.

Canvas (Crop or Redact)
- [ ] Drag the phone photo, open **Tools**, drop on **Crop**: an editor opens in the app's colours (follows Windows light/dark), the photo fits the window and is upright
- [ ] Mouse wheel zooms around the pointer; **Ctrl+0** fits, **Ctrl+1** shows 100 %; the zoom chip at the bottom toggles between them; at high zoom pixels stay sharp (not blurred)
- [ ] Hold **Space** and drag, or drag with the middle button: the picture pans
- [ ] Move the window to a monitor with another scale: text, handles and lines stay crisp and the same size
- [ ] **Ctrl+Z** / **Ctrl+Y** (and the toolbar arrows) undo and redo every change, including side-panel changes
- [ ] **Esc** closes without writing anything; **Enter** applies (when a number box is not being edited)
- [ ] The PNG with transparency shows a checkerboard behind it

Crop
- [ ] Drag a box: the outside is darkened, a "W × H" tag shows the size in real pixels, the thirds grid shows (toggle in the panel)
- [ ] Drag the edges and corners; hold **Shift** to keep the shape; arrows nudge by one pixel, Shift+arrows by ten
- [ ] Aspect ratio Free, Original, 1:1, 4:5, 3:2, 16:9, 9:16 and Custom (type W and H): the box changes to the shape and stays inside the picture
- [ ] Type an exact Width and Height in pixels: the output is exactly that size
- [ ] Rotate left / right turns the picture; Straighten (slider or typed degrees) tilts it and the box stays inside the picture
- [ ] Apply: `photo (cropped).jpg` appears next to the photo, the original is untouched; cropping again makes `photo (cropped 2).jpg`
- [ ] Open the cropped JPG's Properties > Details: camera, date and location are kept (Crop is not a privacy tool; use Share-safe for that), the size is the new size, it opens upright everywhere
- [ ] PNG stays PNG, WebP stays WebP
- [ ] Drop five photos of different sizes on Crop: the editor shows the first one and the panel says "The other 4 images" with Centred / Same place; Apply makes five cropped copies, each with the first one's shape (centred, or in the same place); with an exact size set, each is that size where it fits

Redact Photos
- [ ] Drop the screenshot on **Redact**: drag over the email: a black cover appears; drag several more; click one to select, Ctrl+click to select more, Del removes them
- [ ] Switch a selected cover to Blur and to Pixelate; switch Solid's colour to white and to a custom colour (colour picker)
- [ ] Find > Personal details: the email, phone and card number get suggested covers labelled with what they are; Find > All text covers every line; "Keep all" / "Remove all" work
- [ ] Drop the group photo, Find > Faces: each face gets a cover (or a line says face detection is not available)
- [ ] Apply: `screenshot (redacted).png`; zoom in on it: the covered parts are gone (solid is exactly the chosen colour); Properties > Details shows no camera, date or GPS
- [ ] On a PC with no OCR language installed, the Find section says how to add one

Share-safe
- [ ] Drag the phone photo, open **Tools**, drop on **Private**: a small "Checking the photo for personal details" window shows progress; with nothing found in the picture it saves straight away
- [ ] The top panel says something like "Removed GPS, camera, 14 fields"; `photo (share-safe).jpg` has no GPS, camera or date in Properties > Details, and still opens upright
- [ ] Drop the screenshot on Share-safe: a "Review before sharing" editor opens with covers on the email, phone and card number; remove one, add one, Apply: the result says "Covered N details · …"
- [ ] Esc in the review window saves nothing
- [ ] Drop the phone video on Share-safe: `video (share-safe).mp4` plays the same, is about the same size (no re-encode), and its location and creation date are gone (Inspect on it shows no GPS / location tag)
- [ ] Drop an already clean file (a Share-safe output): it is skipped with "already clean"

Remove metadata (File actions)
- [ ] Drop the phone photo on **No EXIF**: `photo (no metadata).jpg`, no editor, the result line lists what went
- [ ] Drop the MP3: `song (no metadata).mp3` has no title, artist or cover art in Explorer, and still plays
- [ ] Drop a WebP and a PNG with text chunks: both come out clean (check with Inspect)

### R. Photo tools: Edit, Annotate, Background, Collage
Have ready: a 12 MP phone photo (JPEG with camera, date and GPS), a hazy or foggy landscape, a noisy night shot, a screenshot (PNG), a PNG with transparency, and four photos of different shapes (portrait, landscape, square).

Tools wheel
- [ ] Drag the phone photo, press **Shift** and scroll to the Tools wheel: Edit, Annotate, Background are there; Collage is not
- [ ] Drag two or more photos: Collage is there too (with 17 image tools the wheel pages: 15 slices and More)
- [ ] Drag a video or a PDF: none of the four photo tools are there

Edit Photos
- [ ] Drop the phone photo on **Edit**: the editor opens with Presets and four tabs (Light, Colour, Detail, Effects); every slider moves the picture live, with no lag at fit and at 100 %
- [ ] Press and hold on the picture: the original shows with an "Original" tag; let go: the edit is back
- [ ] Auto brightens a dark photo sensibly; each preset (Vivid, Soft, Warm, Cool, Mono, Film) looks like its name; Original and Reset clear everything
- [ ] Dehaze +60 on the hazy landscape: the fog lifts without dark halos round the horizon; −40 adds haze
- [ ] Clarity +60: local contrast up, no halos round edges; −60 softens
- [ ] Noise reduction 70 on the night shot: smooth skies, edges still sharp; Grain 40 adds film grain that stays the same at every zoom
- [ ] Ctrl+Z / Ctrl+Y undo and redo slider moves (one step per drag)
- [ ] Save: `photo (edited).jpg` next to the photo; Properties > Details keeps camera, date and GPS; it opens upright
- [ ] The day's log (tray > Open logs folder) has "photo: edit 4000x3000 -> 4000x3000 on GPU in N ms" (WARP on a PC without a graphics card); record N for a 12 MP photo here
- [ ] Drop five photos: one editor (the first photo), Save all makes five edited copies with the same settings
- [ ] PNG stays PNG with its transparency; WebP stays WebP

Annotate Photos
- [ ] Drop the screenshot on **Mark up**: Arrow is chosen; drag: an arrow from the press to the release; Shift snaps it to 45°
- [ ] Text: click, type, Enter for a new line, Esc (or Ctrl+Enter) to finish; click it again to edit; Esc on an empty text removes it
- [ ] In a text: the caret follows the arrows, Home/End, Ctrl+arrows; Shift selects, double-click a word; Backspace/Delete, Ctrl+A/C/V/X and Ctrl+Z work as in Notepad; typed Hindi and emoji show and delete whole
- [ ] **Font** (or Ctrl+Shift+F) lists the installed fonts, each in its own face; type to find one. Size, B/I/U, colour and alignment change the selected box; Ctrl+[ and Ctrl+] step the size
- [ ] **Auto-wrap** on: drag a side handle narrower, the lines rewrap. Off: the box grows with the text and dragging a corner scales the type; a side handle sets a width (Auto-wrap turns on)
- [ ] **Date** puts today's date where you click; **Tick** and **Cross** mark a form's boxes (drag for bigger); Delete removes the selected mark; Undo and Redo step through every change
- [ ] Signature > **Draw…**: sign with the mouse or a pen, **Add**: click to place it; its corners keep its shape (Shift stretches). **From a photo…**: a phone photo of a signature on white loses its paper ("Make the white see-through"). Saved signatures come back after a restart (in `%LOCALAPPDATA%\ShiftDrop`); × removes one. Nothing goes online
- [ ] The signature pad and the font list follow light/dark, look right at 150 % and 200 %, and work by keyboard (Tab, Enter, Esc)
- [ ] Box and Circle (Shift keeps them even; Filled toggle), Callout (press on the subject, drag to where the bubble goes, type), Number (1, 2, 3 in the order placed; deleting 1 renumbers), Magnifier (press on a detail, drag the loupe; its grips change the spot and the magnification)
- [ ] Pen and Highlighter draw smooth strokes; the highlighter is see-through and doesn't darken where it overlaps itself
- [ ] Insert picture: pick a PNG logo; it lands in the middle and can be moved and resized by its corners
- [ ] Select a mark: colour, width and size change it; To front and Delete work; arrows nudge it (Shift by ten); the letter keys V A T B O C N M P H switch tools
- [ ] Drop three screenshots: the switcher in the panel moves between them, each keeps its own marks; Save writes only those with marks (the rest are skipped)
- [ ] Save: `screenshot (annotated).png`; the marks look as in the editor at 100 %

Add a Background
- [ ] Drop the PNG with transparency on **Frame**: gradient by default with a soft shadow and rounded corners
- [ ] Solid (pick a custom colour), Gradient (From, To, Direction), Blur (the photo's own blurred copy behind it), Picture (choose any picture; its blur slider)
- [ ] Shape Auto, 1:1, 4:5, 4:3, 3:2, 16:9, 9:16: the canvas takes the shape with the picture centred; Spacing, Rounded corners and Shadow change live; the panel shows the output size
- [ ] Shape **Fit to picture** (after 9:16): the canvas is exactly the picture's size, no margin; Spacing and Shadow leave the panel; raise Rounded corners and the background shows only in the corners. Save: the output has the photo's own pixel size
- [ ] Save: `logo (background).png`; a JPEG source gives a JPEG with its EXIF

Create a Collage
- [ ] Drop the four photos on **Collage**: "Opening 4 photos…", then a grid; Row and Column keep every photo whole; Featured makes the first one large
- [ ] Drag a photo onto another: they swap; Shuffle and Reverse change the order; Undo puts it back
- [ ] Shape (grid and featured), Longer side (type 2000), Spacing, Rounded corners, background colour and "Show whole photos" all change the preview live
- [ ] Save: one `first-photo (collage).jpg` (named after the first photo in the collage), the longer side as typed, no camera or GPS in its details

GPU and speed
- [ ] On a laptop with a graphics card and on a VM without one (WARP): every preview works; the log names the device
- [ ] Unplug or update the graphics driver while an editor is open (or change the display): the preview comes back

Editors' frame, as the App Editors page draws it (picture editor, PDF page view and Merge list, video and audio editor; light and dark, two accents)
- [ ] Crop a photo: the title bar says `photo.jpg · Picture editor`; along the top a tool bar with a **Crop** tab (icon over the name, tinted, a short accent bar under it), the file's name beside it, and on the right undo, redo, a thin line and a **Fit** drop-down; the picture in the grey well; the options panel on the right titled **Crop**; along the bottom a footer with `4032 × 3024 · 3.4 MB · Enter applies…` on the left and Cancel and **Crop** (semibold, accent) on the right
- [ ] The zoom drop-down: Fit, 25%, 50%, 100%, 200%, 400%; picking one zooms about the middle and the field shows it ("100%"); Ctrl+wheel and the zoom keys still work and the field follows them
- [ ] Crop's ratios are round chips that wrap: the chosen one tinted with an accent outline and a check mark; Straighten's slider has a 16 DIP knob with an accent dot; Thirds grid shows "On" beside its switch (12 DIP knob, accent), "Off" with an outlined switch
- [ ] Redact: Solid / Blur / Pixelate is a segmented control (grey track, the chosen option raised with an accent line under it); a Redact of several pictures shows ‹ › and `2 of 3 · name` on the tool bar
- [ ] Edit Photos: the tab and panel say **Edit**; presets are chips; Light / Colour / Detail / Effects is segmented. Add a Background says **Background**, Create a Collage **Collage**, Annotate Photos **Annotate**, Fill and sign **Fill and sign**
- [ ] Annotate: the marks (Select, Text, Date, Tick, Cross, Arrow, Box, Circle, Callout, Number, Magnifier, Pen, Highlighter) are tiles of an icon over the name, five to a row; the chosen one tinted and outlined. Signature buttons still read **Draw…** and **From a photo…**, the second with an up arrow; the signature pad's Draw / From a photo is segmented and its Choose a picture… has an up arrow
- [ ] Colour swatches: 26 DIP circles; the chosen one has an accent ring with a gap; the custom one still has its small + badge and opens the colour dialog
- [ ] Every option and button of every picture tool is still there and works (Crop, Redact, Share-safe review, Edit, Annotate with Fill and sign, Background with Fit to picture, Collage); the panel scrolls when it is taller than the window
- [ ] PDF page view (Edit PDF with three PDFs): the tool bar's Duplicate, Delete and Undo have icons; pages sit on the grey well; the file letters are square tags, A teal, B purple, C orange (also in the Merge list); the footer has Add files… on the left, then the page count, then Back / Cancel / Merge and export
- [ ] Merge list: each PDF is a card with its number, its letter tag and its name over its pages and size; the chosen one tinted with an accent outline; the footer shows "N pages in all"
- [ ] Trim a video: the title says `clip.mp4 · Video editor`; the tool bar's tabs (Trim, Crop, Speed, Join, Frames, Split, Redact) have the wheel's icons; the preview sits in the grey well and the timeline on the panel colour under it; the play button is a round accent button; Keep selection / Remove selection is segmented; Fade in and out reads "Off" beside its switch
- [ ] Change Video Speed: the speeds are chips that wrap; Crop Video's shapes are chips; section names read in sentence case ("Output size", not "OUTPUT SIZE")
- [ ] Bleep a song: `song.flac · Audio editor`, tabs Trim, Channels, Bleep, Visualizer with icons, every option as before (#61's Duck, keep pitch, styles, Add clips)
- [ ] Video frames, the waveform, the visualizer's preview and annotation colours are the content's own colours in both themes; only the frame around them follows the theme and the accent

### S. AVIF, HEIC, SVG and multi-frame files
Have ready: a large phone photo (JPG), a transparent PNG logo, an `.avif` from a phone or a website
(Chrome: right-click an image on a site that serves AVIF > Save image as), an iPhone `.heic`, and three
SVGs: one with `width`/`height` (most exported logos), one with only a `viewBox` (most icon sets), and
one with text in it. For the last part: an animated GIF (any from a chat app or a website) and a
multi-page TIFF (from a scanner or a fax app). Check Settings > Apps > Installed apps for **HEIF Image Extensions** and **HEVC
Video Extensions** first, and note which are installed.

AVIF
- [ ] Drag the photo and open the Convert wheel: **AVIF** sits next to WebP; hovering it shows "AVIF, quality 60" and a predicted size on the card
- [ ] Drop on AVIF: `photo (converted).avif` appears within a few seconds (a 12 MP photo); it is clearly smaller than the JPG and looks the same at 100% zoom
- [ ] Open the result in Edge or Chrome (drag it into a tab) and in the Photos app: both show it the right way up and with the right colours
- [ ] The transparent PNG on AVIF: the result keeps the transparency (check in a browser on a dark page)
- [ ] Drag the downloaded `.avif`: the Convert wheel offers PNG, JPG, WebP, TIFF, BMP, PDF (AVIF is dimmed: it already is one); PNG works, also on a PC without the AV1 Video Extension
- [ ] The `.avif` on the Tools wheel's Rotate: the result is an `.avif`, turned; on Compress: an `.avif` again (or "already compressed" when it can't get smaller)
- [ ] The `.avif` on Reduce size, **−50%**: an `.avif` at most half the size
- [ ] Ten photos on AVIF at once: the top panel shows progress, the PC stays usable, Cancel stops the rest

HEIC
- [ ] With both extensions installed: the iPhone `.heic` on PNG and JPG works; the photo is upright
- [ ] With both installed: the photo (JPG) on **HEIC** gives `photo (converted).heic`, which Photos opens
- [ ] With both installed: the `.heic` on the Tools wheel's Rotate, 1920 px and 50%: each result is a `.heic` (turned, smaller), which Photos opens
- [ ] On a PC that opens the `.heic` but can't save HEIC (the HEIC slice for a JPG is dimmed): the same slices give a `.jpg`, and the card on hover already says JPG
- [ ] Without HEVC Video Extensions (uninstall it, or use a PC that lacks it): the HEIC slice is dimmed and the log says `wheel: 'HEIC' dimmed: install "HEVC Video Extensions"...`; the other slices still work
- [ ] Without HEIF Image Extensions: dragging the `.heic` dims every Convert slice and the centre says "Needs HEIF Image Extensions"; the HEIC slice for a JPG is dimmed, and the log line names both extensions to install
- [ ] Install the missing extension and restart ShiftDrop: the slices come back

SVG
- [ ] The SVG with a size on PNG: the PNG has exactly the SVG's width and height, transparent around the drawing
- [ ] The viewBox-only SVG on PNG: 2048 px on its long side, in the viewBox's shape, sharp edges, transparent background
- [ ] The SVG with text on PNG: the text is drawn (Arial / Segoe UI / Times look-alikes), not missing
- [ ] SVG on JPG: white background where the SVG is transparent; on WebP and AVIF: transparency kept
- [ ] SVG on **PDF** (Convert wheel): a one-page PDF with the drawing; several SVGs together give one page each
- [ ] Hover a slice with an SVG dragged: the card shows the output size (e.g. 2048×1024 for the viewBox-only one)
- [ ] An `.svgz` (gzipped SVG) works the same as `.svg`
- [ ] A broken SVG (rename a `.txt` to `.svg`): the job fails with "not a readable SVG" and leaves nothing behind

Crop, Redact and Share-safe
- [ ] The `.avif` on Crop: the cropped result is an `.avif`; the `.heic` on Crop: a `.heic` with both HEIC extensions installed, else a `.jpg`; the SVG on Crop: a `.png` with the transparency kept
- [ ] The `.avif` on Share-safe: a clean `.avif` comes out

Animated GIFs and multi-page TIFFs
- [ ] The animated GIF on WebP: `name (converted).webp` animates in Edge or Chrome at the same speed as the GIF; the top panel says "Animated WebP, N frames"
- [ ] The animated GIF on AVIF: `name (converted).avif` animates in Chrome
- [ ] The animated GIF on PNG: a folder `name (frames)` with `name frame 01.png`, `02`... Each frame looks complete (not just the changed bits); drop again and a second folder `name (frames 2)` appears, the first untouched
- [ ] The animated GIF on JPG: the same folder, frames on white
- [ ] A GIF with more than 100 frames (a 10-second clip from a chat app) on PNG: it asks first. **Save every Nth frame** (about 100 files): the folder holds frames 1, N+1, 2N+1… under their own numbers; **Save all N frames**: every frame; **Cancel**: no folder. The same GIF on WebP or TIFF doesn't ask
- [ ] The animated GIF on TIFF: one `.tiff`; the Photos app (or Windows Photo Viewer) pages through every frame
- [ ] The multi-page TIFF on PDF (Convert wheel): one PDF with every page, each its own size; with a JPG dropped alongside, the JPG is a page too
- [ ] The multi-page TIFF on PNG: a folder `name (frames)` with `name page 01.png` and so on
- [ ] A one-frame GIF on PNG: an ordinary `name (converted).png`, no folder
- [ ] Undo (History) on a frames folder moves the whole folder to the Recycle Bin

### T. Video and audio tools
Have ffmpeg in place (`scripts/get-ffmpeg.ps1`, see [BUILDING.md](BUILDING.md)), a phone video with sound (1080p, 20–60 s), a second clip of another size, a video with no sound, a stereo song with cover art, and a mono voice memo.

The wheel:
- [ ] Drag a video and open **Tools**: Compress, 50% size, Under 10 MB, Under 25 MB, Inspect, and **Trim**, **Crop**, **Speed**, **Join**, **Frames**, **Split**, **Redact** on two rings
- [ ] Drag a song: the four Compress slices, Inspect, **Normalize**, **Trim**, **Channel**, **Bleep**, **Visuals**; a photo gets none of these

The editor window (try with each tool):
- [ ] Drop the video on **Trim**: a window opens on the monitor you dropped on, titled "Trim Video – ShiftDrop", in the Windows light or dark theme with your accent colour (the title bar matches); the picture, then the filmstrip under it, fill in within a second or two
- [ ] The tabs at the top read Trim, Crop, Speed, Join, Frames, Split, Redact (for a song: Trim, Channels, Bleep, Visualizer). Clicking one, or Ctrl+1 to Ctrl+7, switches tool without reloading the file, and the window title follows. Confirming runs the tool chosen last: History and the output name say so ("clip (speed…)")
- [ ] Turn the mouse wheel over the timeline: it zooms around the cursor with sharper thumbnails; **0** shows the whole clip; drag the thin bar under the timeline to scroll
- [ ] Type `0:05.5` in the time field and press Enter: the playhead jumps there; text that is not a time stays red
- [ ] ← and → step one frame (Shift: one second); Space plays with sound and the playhead follows; Space again stops
- [ ] Tab walks through the tabs, the transport buttons, the options and the footer with a visible focus ring; arrow keys change a focused choice; Enter is the main button; Esc closes and nothing is written
- [ ] With two monitors at different scaling, drag the window from one to the other: text and lines stay sharp and the layout scales

Video tools:
- [ ] **Trim**: drag the handles, or press I and O at the playhead, or type in From and To; "Play the kept part" (Shift+Space) plays only that. Exact gives `clip (trimmed).mp4` that starts and ends on those frames; Fast is instant, and its start can move back to a keyframe
- [ ] **Crop**: drag on the picture, then drag the edges and corners; 16:9, 9:16, 1:1, 4:5, Original and Free keep the shape; type a Width (the height follows). The result has exactly that size and keeps the sound
- [ ] **Speed**: pick 0.5× or 2×, or type 1.8 or 180%; Space previews at that speed with the pitch kept. 2× halves the length; the silent video works too
- [ ] **Join**: drop three clips on **Join**. Drag rows to reorder, or select one and press Alt+↑ / Alt+↓; Delete leaves one out. Same-size phone clips say "joined without re-encoding" and are quick; with the other-size clip it re-encodes to the first clip's size with black bars. One MP4 comes out
- [ ] **Join** a landscape and a portrait clip: under OTHER SHAPES, **Fit** (the default) keeps the whole picture with black bars; **Fill** crops the portrait clip to fill the frame, no bars, and the note under RESULT says so
- [ ] **Frames**: press M (or double-click the timeline) at three places and drag a mark to move it; "Save 3 frames" makes a folder next to the video with full-size PNGs named by their time
- [ ] **Split**: add two cuts with M, or choose Equal parts and 3; the parts land in one folder, numbered in order, and play end to end without a gap
- [ ] **Redact**: drag two areas on the picture, one Blur and one Solid; select one and press I and O to show it for part of the video only. The preview shows the blur; in the result each area is covered only for its own time
- [ ] **Trim** with **Remove selection**: the caption reads PART TO REMOVE, the part is shaded red on the timeline, Removed and Result show the lengths, Exact/Fast is off ("always re-encodes") and the button reads "Remove part"; `clip (trimmed).mp4` jumps straight from before the part to after it. **Fade in and out** (keep or remove) gives half a second of fade to black and silence at each cut
- [ ] **Speed** 2× with **Keep pitch** off: Space and the result sound higher, like a fast tape; with it on (the default) the voice keeps its pitch; on the silent video the switch is greyed out
- [ ] **Frames**: **Every N** (type `30` or `30 frames`) and **Per second** (`2`, `0.5 a second`) draw ticks on the timeline; RESULT counts the frames and estimates Each and In all for PNG, JPG and WebP (WebP smallest). The folder holds exactly that many pictures in that format, named by their time; pressing M goes back to **Marked**
- [ ] **Split** → **Every**, type `0:30` (or `90`): Parts and Last part follow, dashed lines show the cuts, and each part but the last is that long; `0:00.5` or a length giving over 1000 parts stays red
- [ ] **Join** → **Add clips…**: pick two more videos (Ctrl+click); they join the end of the list, get thumbnails, and can be dragged, moved and removed like the others
- [ ] Drop three videos on **Crop**: the header says "File 1 of 3"; Next file and Skip this file walk through them; at the end one batch runs (one History entry) and the skipped file is left alone

Audio tools:
- [ ] **Normalize** a song: a small window asks "How loud should it be?" with **Speech** (−16 LUFS), **Music** (−14) and **Broadcast** (−23); Esc or Cancel does nothing. With several songs it says they all get the same loudness and asks once
- [ ] **Normalize** a quiet song and pick Speech: the top panel says "Loudness −3x.x → −16.0 LUFS"; normalizing the result again says "already at −16.0 LUFS" and writes nothing
- [ ] **Trim** on the waveform with the handles or I and O; "Also trim silence at both ends" cuts the quiet lead-in of the voice memo
- [ ] **Channels**: Make mono, Swap left and right, and the Left and Right volume sliders; while playing, A switches between the original and the result. On a mono file "Make mono" changes nothing and the main button is off
- [ ] **Bleep**: drag across the waveform to mark two parts (M adds one at the playhead); a 1 kHz tone or silence; Space plays with the bleeps, A compares with the original
- [ ] **Visualizer** (tab in the song's editor): Landscape, Portrait or Square; the moving waveform, or a still picture (the cover art, or one you choose); the MP4 has the song's sound and length
- [ ] **Bleep** → **Duck**: a "Lower by" slider (3–60 dB, 20 by default); Space plays the marked parts quieter, not cut out, and so does the result
- [ ] **Visualizer** styles: **Bars**, **Wave**, **Circle**, **Square**, **Vertical** each show their look in the preview and the MP4 moves with the sound; **Choose a picture…** puts a darkened picture behind any of them (**No picture** takes it away); **Picture** is the still picture as before

Always:
- [ ] Cancel (×) in the top panel during a long render: nothing is left in the folder (no `.sdpart`)
- [ ] An `.mp4` that holds only sound, dropped with a real video on **Crop**: its page says "This file has no video to edit." and Skip moves on; the other video is still cropped
- [ ] Undo from History removes the outputs (a frames or split folder goes with all its files)

Recipe examples (save in `%LOCALAPPDATA%\ShiftDrop\recipes` and restart):
```json
{ "id": "normalize-14", "label": "−14 LUFS", "description": "Normalize Volume for streaming", "ring": "tools", "suffix": "normalized",
  "icon": "E767", "steps": [ { "action": "audio.normalize", "params": { "target": -14, "ask": false } } ] }
```
```json
{ "id": "speed-2x", "label": "2× speed", "description": "Twice as fast, same pitch", "ring": "tools", "icon": "EC4A",
  "steps": [ { "action": "video.speed", "params": { "factor": 2, "ask": false } } ] }
```
- [ ] After a restart a song's Tools wheel also has **−14 LUFS** and a video's **2× speed**; both run at once with no window (the speed one names its output `clip (2x).mp4`)

### U. Documents, spreadsheets, subtitles, Office bridge
Have ready: a Word document with headings, a bulleted and a numbered list, a table, a link and a
picture (`report.docx`); a Markdown file (`notes.md`); an RTF saved from WordPad with a picture; an
RTFD package from a Mac (a folder ending in `.rtfd`); an old `.pages` file; a text PDF; an Excel
workbook with two sheets, a date column and numbers with leading zeros (`book.xlsx`); the same saved
as Excel 97-2003 (`book.xls`); a CSV saved by European Excel (semicolons); an SRT and a VTT from
YouTube; a PowerPoint deck. For the Office bridge you need Microsoft Office or LibreOffice installed.

Documents
- [ ] Drag `report.docx` and hold **Shift**: Convert shows PDF, Markdown, HTML, EPUB and TXT (no DOCX), plus **Exact PDF** when Word or LibreOffice is installed
- [ ] Drop on **MD**: a folder `report (converted)` holds `report.md` and `images/`; the Markdown shows headings as `#`, lists, the table as a `|` table, the link, and the picture (open it in VS Code's preview)
- [ ] Drop on **HTML**: the same folder layout; the page opens in Edge with the picture, the table and working links. With no pictures in the document you get a single `report (converted).html`
- [ ] Drop on **EPUB**: `report (converted).epub` opens in an e-book app (Thorium, Calibre's viewer, or Edge's extension) with a table of contents; the picture shows
- [ ] Drop on **TXT**: plain text, bullets as •, the table as tab-separated lines, links as "text (address)"
- [ ] Drop on **PDF**: no print dialog appears; `report (converted).pdf` has A4 pages (Letter if Windows' region is the US), 2 cm margins, page numbers at the bottom, selectable text, the table's header row repeated when it runs onto a new page
- [ ] Turn off "Microsoft Print to PDF" (Windows Features), restart ShiftDrop: the PDF slice is dimmed and the centre says the printer is missing. Turn it back on
- [ ] Drop `notes.md` on **DOCX**: Word opens it with real headings (the navigation pane lists them), real lists and the table. On **HTML**: it looks like the Markdown preview
- [ ] Drop the WordPad RTF on **DOCX**: bold, italic, the picture and the larger headings come through
- [ ] Drop the `.rtfd` folder with **Shift**: the documents slices appear; **DOCX** includes its pictures
- [ ] Drop the `.pages` file on **DOCX**: the text arrives (from its preview PDF); a newer `.pages` with no preview PDF says so in the panel
- [ ] Drop the text PDF with **Shift**: besides the PDF slices of section N there are **DOCX**, **EPUB** and **HTML**; the DOCX has the text reflowed with headings and bullets
- [ ] Drop a PDF with photos or charts (a Word report saved as PDF) on **DOCX**: each picture sits between the paragraphs it sat between in the PDF, about the size it had on the page and never wider than the text, as a normal inline picture in Word. A logo on every page appears once; bullets and thin rules don't appear as pictures
- [ ] The same PDF on **EPUB**: the pictures show in the e-book app. On **HTML**: a folder `… (converted)` holds the page and `images/`, and Edge shows the pictures
- [ ] A scanned PDF on **DOCX**: the text comes from OCR, and the page scans are not added as pictures

Spreadsheets
- [ ] Drag `book.xlsx` with **Shift**: ODS, CSV, TSV and PDF (no XLSX)
- [ ] Drop on **CSV**: a folder `book (converted)` with one CSV per sheet; double-click one: Excel opens it with accents right, dates as 2024-01-31, leading zeros kept as text
- [ ] Drop on **ODS**: LibreOffice Calc (or Excel) opens it with both sheets, the dates still dates, the numbers still numbers
- [ ] Drop on **PDF**: each sheet is a table under its name, wide sheets come out landscape, the header row repeats on each page
- [ ] A CSV with more than 20,000 rows on **PDF**: it asks first. **Print the first 20,000 rows**: the last page ends with a note saying how many rows were left out; **Print all rows**: every row, no note; **Cancel**: nothing is made and the panel says so
- [ ] Drop `book.xls` on **XLSX**: the values, dates and sheet names come through; a password-protected .xls says it is protected
- [ ] Drop the semicolon CSV on **XLSX**: the columns are split right (not everything in column A)
- [ ] A one-sheet workbook to CSV gives a single `… (converted).csv`, not a folder

Subtitles
- [ ] Drag an `.srt` with **Shift**: VTT and TXT. Drop on **VTT**: the file plays in a browser `<track>` or VLC with the right timing; italics survive
- [ ] Drop the YouTube `.vtt` on **SRT**: VLC shows it; the styling and voice tags are gone, the text is right
- [ ] Drop it on **TXT**: one line per subtitle, without the repeated lines of roll-up captions

Office bridge (with Microsoft Office)
- [ ] Drag the deck with **Shift**: **Exact PDF**, **PNG slides**, **JPG slides**. Drop on **Exact PDF**: the PDF looks exactly like the deck; PowerPoint never shows a window, and if PowerPoint was already open with your own deck, it stays open with your deck untouched
- [ ] Drop on **PNG slides**: a folder `deck (converted)` with `Slide 01.png`, `Slide 02.png`, … at 1920 pixels wide
- [ ] Drop `report.docx` and `book.xlsx` on **Exact PDF**: the PDFs match Word's and Excel's own PDF export; Task Manager shows no WINWORD.EXE or EXCEL.EXE left afterwards
- [ ] Drop a password-protected DOCX on **Exact PDF**: it fails with a message instead of a hidden password prompt
- [ ] Start an Exact PDF of a large deck and press × in the top panel: it stops, nothing is left next to the deck, and PowerPoint (if we started it) is gone from Task Manager
- [ ] Start an Exact PDF of a large deck, then drop another file on **Move to folder** (or **Rename**): the move is done at once, while the export keeps running

Office bridge (LibreOffice only, no Microsoft Office)
- [ ] The deck shows **Exact PDF** but not the slide pictures; the PDF comes out right, no LibreOffice window appears, and a LibreOffice window you already had open is not disturbed
- [ ] With neither installed, no Office bridge slices appear at all

- [ ] With Wi-Fi off, everything above works the same; Tray > History lists each job and Undo moves its output (or folder) to the Recycle Bin

### V. Archives and the sandbox
Prepare a folder `Photos` with a few pictures, a subfolder and a file with a non-English name
(for example `Müller 日本.txt`); a ZIP, a TAR.GZ, a 7z and a RAR made elsewhere (7-Zip or WinRAR);
a ZIP protected with a password the classic way (7-Zip: ZIP, encryption ZipCrypto); a 7z, a RAR and a ZIP
(AES-256) with a password; and an old ZIP made on a non-UTF-8 system if you have one.

Compress
- [ ] Drag `Photos` onto **Zip** on the File-actions wheel: `Photos (zipped).zip` appears next to the folder; open it in Explorer: the subfolder, the non-English name and the dates are all there
- [ ] Select three files in one folder and drop them on **Zip** on the File-actions wheel: `Archive.zip` next to them, holding the three files; do it again: `Archive (2).zip`, the first untouched
- [ ] Drop two folders that sit in different places (put them on the Float shelf first) on **Zip**: `Archive.zip` next to the first one; one file on its own still gives `name (zipped).zip`
- [ ] Drag `Photos` to the Convert wheel: it shows ZIP, TAR.GZ and 7z; **TAR.GZ** makes `Photos (archived).tar.gz`, **7z** makes `Photos (archived).7z`; both open in 7-Zip with the same files
- [ ] Zip a folder of a few GB: the top panel's progress moves smoothly; press × halfway: no `.zip` and no `.sdpart` file is left

Extract and convert
- [ ] Drag each archive (ZIP, TAR.GZ, 7z, RAR) to the Convert wheel: it shows Extract, ZIP, TAR.GZ, 7z (on a ZIP, TAR.GZ or 7z its own format is dimmed)
- [ ] **Extract** on each: a folder `name (extracted)` next to it with the same files, names and dates; an archive holding one top-level folder doesn't add an extra folder level
- [ ] The old non-UTF-8 ZIP: accented names come out right (not `M├╝ller`)
- [ ] **ZIP** on the RAR and the 7z: `name (converted).zip` with the same contents
- [ ] The password-protected ZIP on **Extract**: a small password box names the file and shows what you type as dots; a wrong password says "That password didn't work" and asks again; the right one extracts everything, without asking again for the other files inside
- [ ] The same ZIP, **Cancel** in the password box: the top panel says "no password given, so nothing was extracted"; no folder and no `.sdpart` are left
- [ ] The same ZIP on **7z**: the box asks the same way; the 7z opens in 7-Zip with no password
- [ ] Open the password box and press × in the top panel: the box closes and the job is cancelled
- [ ] A password-protected 7z, a password-protected RAR and a ZIP made with AES-256 in 7-Zip: no box; the panel says that kind can't be opened yet
- [ ] Search `%LOCALAPPDATA%\ShiftDrop` (logs and `history.db`) for the password you typed: it is nowhere
- [ ] A big archive on Extract, × halfway: no folder and no `.sdpart` is left next to it
- [ ] Undo the last extract from History: the folder goes to the Recycle Bin

Sandbox
- [ ] Extract an archive, then open `%LOCALAPPDATA%\ShiftDrop\logs`: no "sandbox:" warning line
- [ ] While a large extract runs, Process Explorer (or Task Manager > Details, column "Integrity level") shows `ShiftDropWorker.exe` at **AppContainer**, under ShiftDrop.exe, and it disappears when the job ends
- [ ] Kill ShiftDrop.exe in Task Manager during a large extract: ShiftDropWorker.exe ends at once
- [ ] Set `[Sandbox] Archives=0` in `settings.ini`, restart, extract: it still works, and the worker shows integrity level Medium
- [ ] Optional, on a PC with a GPU (GPU encoders inside the AppContainer are not tested in CI): set `[Sandbox] Media=1`, restart, convert a video to MP4 and compress one (GPU encoder in use): both work, ffmpeg.exe shows **AppContainer** in Process Explorer; open an issue with any failure and the log lines
- [ ] Uninstall: the AppContainer profile is removed (`%LOCALAPPDATA%\Packages\shiftdrop.sandbox` is gone)

### W. Files dragged out of browsers and mail apps
These drags carry no real file (no path), only a name and the contents. ShiftDrop saves them in
`%LOCALAPPDATA%\ShiftDrop\dropped` when they are dropped, runs the slice on that copy and writes the
result to the Desktop (or the folder chosen below); the copy is deleted when the job is done.
- [ ] Edge and Chrome: drag a picture off a web page (a PNG or JPG; WebP if the page serves it), press Shift: the wheel shows image formats for it (named after the picture), with the predicted name on the card; drop on **PNG**: `name (converted).png` appears on the Desktop and the panel says "Saved to Desktop"
- [ ] The same in Firefox, and with **Tools › 50%**: `name (edited).png` on the Desktop
- [ ] Do it twice: the second result is `name (converted 2).png`, the first one is untouched
- [ ] **File actions › Copy**: the picture is saved on the Desktop (`name.png`, or `name (2).png` if that is taken) and pasting in Explorer pastes it; **Float** puts that saved copy on the shelf; **Duplicate** writes `name (copy).png` on the Desktop; **Move to folder** saves it into the folder you pick
- [ ] Outlook (classic): drag an attachment (a PDF or a photo) out of a mail with Shift: its slices show; convert it: the result is on the Desktop. Drag a whole mail onto **File actions › Copy**: a `.msg` is saved on the Desktop and opens in Outlook
- [ ] Thunderbird: drag an attachment with Shift: the same
- [ ] A large attachment (50 MB or more): the top panel shows "Saving 1 dropped file…" with progress before the job starts
- [ ] Tray › **Save browser drops to** › **Downloads**: the next result is in Downloads; **Ask every time**: a folder picker ("Save here") comes first, Cancel saves nothing; **Desktop** again: back to the Desktop. The choice survives a restart (`[Output] VirtualFiles` in settings.ini)
- [ ] After a few drops, `%LOCALAPPDATA%\ShiftDrop\dropped` is empty (quit with a job running, start again: it is emptied at startup)
- [ ] Real files from Explorer still convert next to themselves

### X. Settings and tray entries
- [ ] Right-click the tray icon: **Settings…**, **History…** and **Show Float shelf** (ticked while the shelf is on) are near the top
- [ ] **Settings…** opens a window (dark when Windows is dark) with the app's icon, name and version at the top left, **Find a setting**, and General, Appearance, Wheel, Output, Advanced and About; the page in use has a short accent bar. Each page is a column of rounded cards, one setting per card, that scrolls with the mouse wheel and the scroll bar
- [ ] Keyboard only: Tab goes from Find a setting to the pages to each card's control, Shift+Tab back; Up and Down move between the pages and open them; Space flips a switch; Left and Right move a segmented choice (Theme, Style, Size, Save browser drops to) and pick as they go; Tab scrolls the focused control into view; every focused control has a visible focus ring; Esc closes the window
- [ ] Find a setting: type `accent`: Appearance opens with an accent ring around **Accent colour**, and Enter moves the focus to it; `powershell` finds Terminal app on General; `zebra` says no setting matches
- [ ] Find a setting knows the Wheel page and other words: `reduce size` opens Wheel on the **Reduce size** tab with a ring around the tab and says "Wheel › Reduce size, and N more"; `photos reduce` opens the Photos card of Reduce size with a ring around it; `webp` rings the **WebP** chip under Convert › Photos (Enter puts the focus on it); `color`, `highlight` find Accent colour; `dark` Theme; `startup` Start with Windows; `ocr` Text recognition language; `rename` Output naming; `context menu` the right-click entry; any case and any word order work
- [ ] Scrolling Settings never leaves streaks (Prakhar's 2K laptop, 150% and 175%, dark): on Appearance scroll up and down fast with the wheel, the scroll bar's thumb and its arrows, then drag the **Top panel size** thumb back and forth: the slider's track, thumb and the selected navigation item repaint cleanly every time, with no repeated lines; the same on Wheel with all its cards open, and in History, Rename and the inspector (scroll their lists, resize the windows)
- [ ] Borders all the way round, at 100%, 125%, 150% and 175%: every card's 1 px border and rounded corners are complete and crisp (Accent colour and the Wheel page's cards included, under their header too), and the selected navigation item's rounded fill has whole corners
- [ ] No stray rectangles: the **Preview** label above the mini wheel sits on the preview's own darker panel; Output's live example is one rounded panel from end to end with "Live example" at its right on the same colour; About's "Version 0.5.0" sits on the card colour and the pill reads **Free and open source** in full, at every scaling; History's, Rename's, the inspector's and a question's labels sit on exactly their card or bar colour
- [ ] Focus rings only for the keyboard: click a tab on Wheel (Tools), a navigation item, a chip, a switch, a button: no ring, only the selected look (accent underline and semibold for a tab); press Tab: a thin rounded ring shows on the focused control, whole even on a control that fills its own space (a navigation item); click again: the rings go. The same in History, Rename, the inspector and a question
- [ ] General: **Start with Windows**, **Text recognition language**, **Show Float shelf**, **Save browser drops to** and **Terminal app** (with **Command line**, greyed out unless Custom command is picked) do what the tray menu's entries do, and the tray menu shows the change at once (and the other way round: change it in the tray, open Settings again)
- [ ] Appearance: a preview of the wheel at the top in the Style and accent in use; **Theme**, **Style** and **Size** (Auto on a new install) as segmented choices; **Top panel size** as below; **Accent colour** folds open on the swatches with **Use my Windows accent colour** under them, and folded it names the accent in use
- [ ] Wheel: tabs Convert, Tools, Reduce size, File actions; a card per kind of file (Photos, Video, Audio, PDF, Any file…) with a chip per action and a small wheel that shows exactly the tiles those chips leave, laid out as the real wheel lays them out. Turn **TIFF** off under Convert › Photos: its chip loses the tick and the accent, the small wheel drops a tile; drag a photo, press Shift: TIFF is gone and the other formats share the circle evenly; turn it on again: it is back
- [ ] Wheel › **Turn off this section** on Tools: scrolling goes Convert, Reduce size, File actions (no Tools tab in the top panel), Tools' chips are dimmed but keep their choices, and **Turn on this section** brings them back as they were; on the last section left the button is dimmed and says why to a screen reader. **Reset to default** turns the section and every action in it on; quitting and starting again keeps your choices (`[Features]` in settings.ini)
- [ ] Wheel › **Find an action**: `webp` leaves only the cards with WebP, opened, showing only the matching chips; a word nothing matches says so
- [ ] Output: four choices with a live example (`photo.jpg  →  photo (converted).jpg`) that follows the choice; Advanced: the right-click entry switch and the two folder buttons; About: the version, Free and open source, and the source code link (opens in the browser)
- [ ] Narrator on Settings: every switch, drop-down, slider and chip says its name, its state and its description; the swatches say their names
- [ ] At 100%, 150% and 200% scaling, and after moving the window between monitors with different scaling: the cards, text and icons stay sharp and in proportion; narrower than 820 px the navigation column narrows
- [ ] Untick **Show Float shelf**: the shelf hides (with its files kept); tick it with an empty shelf: it opens saying "Drop files here"; drop a file on it: it is added; Clear all empties it and it stays on screen
- [ ] With two or more OCR languages installed: tray › **Text recognition language** lists Automatic and each language; pick one and **Copy text** on a picture in that language reads it; with one language installed the submenu is not there

### Y. Toasts, more settings, Rename and screen readers
- [ ] Installed copy: convert a long video and drop a photo on **PNG** while it runs; when the video finishes a Windows toast (not a balloon) says "Your video is ready" over "name (converted).mp4 · 31 MB · took 4 min 12 s", with **Open** and **Open folder**. **Open folder** selects the output in Explorer, **Open** plays it
- [ ] Three photos converted while another job owns the panel: the toast says "3 files converted to …" over "photo (converted).webp and 2 more, next to the originals." (or "to Downloads." when saved there)
- [ ] One file that fails to convert while another job owns the panel (a `.txt` renamed to `.png`): the toast says "Couldn’t convert name.png" over the reason, with only **Dismiss**, which closes it
- [ ] Two outputs in that toast: only **Open folder**, and it selects both. Quit ShiftDrop and click a toast still in the notification centre: Explorer opens all the same, and no tray icon appears
- [ ] Installed copy: one job that takes over 10 seconds (a long video on MP4, or 50 photos on AVIF), with nothing else dropped: the top panel shows the result and a toast with **Open folder** (and **Open** for one output) also appears; a batch of many files gives one toast, not one per file
- [ ] A job under 10 seconds (one photo on PNG): the panel only, no toast. Cancel a long job with the panel's ×: no toast
- [ ] Settings > System > Notifications, ShiftDrop off: a long job shows no toast
- [ ] Portable copy (no Start menu shortcut): the same case shows the old balloon; Focus assist on (Do not disturb): no toast, the result is in the notification centre
- [ ] Settings › **Terminal**: Windows Terminal, PowerShell and Command Prompt each open at the dropped folder from **File actions › Terminal**; **Custom command** with `code {folder}` opens VS Code there; the command box is greyed out unless Custom is picked
- [ ] Settings › **Theme** › **Light** while Windows is dark: the wheel, the top panel, the card and Settings itself turn light at once; **Dark** the other way; **Follow Windows** follows it again
- [ ] Settings › **Accent colour**: a row of round swatches, Tide (teal) first, then Blue, Purple, Pink, Red, Orange, Green, Graphite, your own colours, and a **+** swatch named **Custom…** (never cut short, not bold) to add one, with a **Use my Windows accent colour** switch under them, which is **on** on a new install (the wheel and editors use your Windows accent colour); turning it off picks Tide; hovering a swatch shows its name. Arrow keys move between the swatches and pick as they go, Tab lands on the ticked one, and the focus ring shows on the focused one
- [ ] With Tide, in light and dark mode: the hovered wheel slice is the teal `#0B7A69` with white text on it, and so is the top panel's active section; the current wheel's dot in the centre is `#0B7A69` in light mode and the light teal `#2DD4B4` in dark mode; the Float shelf's selected row and drop highlight, the top panel's progress bar and spinner, and the editors' primary buttons, switches, sliders and selection are teal too (in the editors, dark mode uses `#2DD4B4` with dark text, light mode `#0B7A69` with white text)
- [ ] The brand's greys, light and dark (Settings › Theme): Settings, History, Rename and the inspector have Ink (`#15171E`) text on white or `#F3F4F6` in light mode and `#F2F3F5` text on `#1B1D22` in dark mode; the editors' window is `#F3F4F6` / `#1B1D22` with white / `#24272D` side panels; the top panel, the result card and the Float shelf are white in light mode and `#1F2127` in dark mode (Solid)
- [ ] History: a failed run's text is red (`#C02B2B` light, `#FF7A7A` dark) and an undone one grey, both easy to read; Rename's refused names are the same red; Settings' and Rename's hint texts are the same grey
- [ ] Fill and sign › Add a signature: the drawing card and the From a photo card are white with a grey line and grey hint in both themes; the picture editor's signature tiles are white too, and the remove mark on a hovered tile is red with readable text on it
- [ ] Pick each preset in turn, in light and in dark, and drag a file with Shift after each one (no restart): the hovered slice, the top panel, the shelf and a newly opened editor (picture editor, Trim, PDF page view) all take the new colour, and every label on the colour and every accent-coloured label next to it is easy to read
- [ ] Click **+ Custom…**: the Windows colour dialog opens; pick a dark navy and **OK**: a new swatch for it appears after the presets, named by its code (`#1A2B5C`) and ticked, the wheel uses a lighter navy in dark mode so its dark label reads and the navy itself in light mode; pick a pale yellow: dark mode keeps it, light-mode buttons get a darker olive with white text. **Cancel** changes nothing
- [ ] Add three colours of your own: each gets its own swatch, in the order added, chosen at once; adding one already there just chooses it. Add enough to fill the line: the swatches wrap onto a new line, every name whole. Pick a preset, then click one of your own: it is chosen again (no dialog)
- [ ] Right-click a colour of your own (or Tab to it and press the menu key): **Delete colour** removes it; deleting the one in use goes back to the Windows accent if that was in use before you chose it, else to Tide. Right-clicking a preset offers nothing. Your colours survive a restart (`AccentCustoms=#…,#…` in settings.ini); a settings.ini from 0.5.0 with one `AccentCustom` shows it as one swatch
- [ ] Turn **Use my Windows accent colour** on: no swatch is ticked and everything follows the Windows accent colour (Windows Settings › Personalization › Colors) on the next wheel, as in 0.5.0, in a lighter tone in dark mode when the Windows colour is dark; change the Windows colour: the next wheel follows it. Turn the switch off: the swatch you had comes back; clicking a swatch also turns the switch off
- [ ] The accent survives a restart (`[Appearance] Accent=` and `AccentCustom=#RRGGBB` in settings.ini); with `Accent=teal`, or `Accent=custom` and no `AccentCustom`, ShiftDrop starts with the Windows accent and the log says why
- [ ] Narrator on Settings: the swatches read as radio buttons ("Tide, radio button, checked"), your own colours as "Custom colour #…, radio button", the + swatch as "Add a colour of your own…, button", the switch as a check box with its state
- [ ] Settings › **Output names** › each style, converting `photo.jpg` twice: `photo (converted).png` / `photo (converted 2).png`, `photo-converted.png` / `photo-converted-2.png`, `photo_converted.png` / `photo_converted_2.png`, and the folder style: `ShiftDrop\photo.png` then `ShiftDrop\photo (2).png`. The card's predicted name matches each. No existing file is ever replaced
- [ ] Folder style with a file called `ShiftDrop` (no extension) next to the photo: the convert fails with a message and the file is untouched
- [ ] Folder style with a file called `ShiftDrop` next to a photo, a text file and a video: **PNG**, **Duplicate** and **MP4** each fail with "cannot write next to the source", and tray › **History** lists each as failed
- [ ] Folder style, a broken `bad.png` (a text file renamed) on **JPG** in a folder with no `ShiftDrop` folder yet: the convert fails and no empty `ShiftDrop` folder is left behind
- [ ] All four settings survive a restart (`[Appearance] Theme`, `[FileActions] Terminal` / `TerminalCommand`, `[Output] Naming` in settings.ini)
- [ ] Select 12 photos, **File actions › Rename**: a window lists them in Explorer's order; `Holiday {n:000}` previews `Holiday 001.jpg` … `Holiday 012.jpg`; `{date} {name}` puts each file's modified date first; Find `IMG_` Replace `trip-` changes only names that have it
- [ ] A pattern without `{n}` (two files would get the same name): those rows turn red, the status says why and **Rename** is greyed out; a name with `?` or ending in a dot is refused the same way; a name already taken in the folder is refused
- [ ] Rename shows **Change: Name / Name and extension**, Name picked. Pick **Name and extension**: the pattern becomes `{name}.{ext}` and the list still shows today's names; type `{name}.png` for a `.jpg`: the note says "the extension changes from .jpg to .png", the status starts with "Careful:", and **Rename** still works. Back to **Name**: `.{ext}` leaves the pattern and the extensions are kept whatever you type
- [ ] **Name and extension** with Find `.jpeg` Replace `.jpg`: `photo.jpeg` becomes `photo.jpg`; a folder in the selection keeps its name without a stray dot
- [ ] **Rename**: the top panel says "Renamed 12 items"; tray › **History** › the run › **Undo**: every file has its old name back. Renaming `a.txt` to `A.txt` works
- [ ] Narrator on: Shift+drag a photo: Narrator says "Convert wheel, 8 actions"; moving over slices reads each one ("PNG, lossless", "TIFF, not available: …"); scrolling to another section says its name; Shift+1–4 the same
- [ ] Narrator: dropping says what the top panel shows ("Converting 1 file to PNG…"), then its result line and the line under it; a failure is read too. With Accessibility Insights, the wheel shows as a list of list items with the highlighted one selected
- [ ] NVDA: the same announcements (2023.1 or later)
- [ ] History, Rename, the inspector and the questions in the new look, light and dark, at 100%, 150% and 200% (and moved between monitors): History lists runs under **Today**, **Yesterday** and older days, each row the file ("photo.heic and 2 more"), the action, "12 min ago", and a pill on Undone, Failed and Partly done runs; search, Action, When, Clear history, Undo, Show files and Enter work as before; Tab reaches each, with a focus ring
- [ ] Rename: the {name} {n} {date} {ext} {folder} buttons type into the pattern where the caret is; Name / Name and extension and Match case; the preview list shows "old → new" with "already exists" and extension warnings as pills, the count over it, and **Rename N files** stays off until every name works
- [ ] Inspect three files: a tab per file (Left and Right on a tab, or Ctrl+PageDown, move between them), Search, the read-only note, fields under their groups; Ctrl+C, the context menu and double-click copy as before
- [ ] An archive with a password: the question window asks for it ("Password needed", masked field, **Open** greyed out until something is typed); a wrong one asks again with "That password didn't work. Try again." under the field; Esc or Cancel stops; the × in the top panel closes the window
- [ ] A very long spreadsheet to PDF and a long GIF to PNG: the question with its two choices as buttons (the first in the accent colour) and Cancel; Tab to a button shows what it does under the text. Normalize volume: Speech, Music and Broadcast as a list (arrows pick), **Normalize** and Cancel. Clear history and the PDF editor's close question use the same window, over their window

### Z. Install, upgrade, uninstall
1. Run the setup. SmartScreen may warn (unsigned): **More info**, **Run anyway**. No UAC prompt appears.
   The first page says "Setup will install ShiftDrop <version> on this PC. It runs offline and needs no
   account. Works on Windows 10 and 11." with this build's version.
   Setup.exe has the new app icon; the welcome and finish pages show the dark side picture with the
   tilted page, and the inner pages the small mark at the top right, sharp at 100%, 150% and 200%.
2. The wizard shows the folder (`%LOCALAPPDATA%\Programs\ShiftDrop`) and a ticked "Start ShiftDrop when I
   sign in to Windows". The last page says "Drag any file and press Shift to open the wheel. ShiftDrop
   lives in the tray." with "Start ShiftDrop" ticked; Finish: the tray icon appears. Untick it on another
   install: Finish starts nothing.
3. Start menu has ShiftDrop. Settings > Apps lists ShiftDrop with its version.
4. Run the same setup again while ShiftDrop is running: no "close the app" error; the old copy exits
   and the new one starts. The startup checkbox is not shown (your tray-menu choice is kept).
5. Untick "Start with Windows" in the tray menu, run the setup again, sign out and in: ShiftDrop does
   not start.
6. Uninstall from Settings > Apps while it runs: the tray icon goes away, the folder and Start menu
   entry are removed, `HKCU\…\Run` has no ShiftDrop value, and `%LOCALAPPDATA%\ShiftDrop` is still there.
