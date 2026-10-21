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
- [ ] Scroll one notch: the Tools wheel shows Rotate ↻, 1920 px, Flip, 50%, Rotate ↺, 1080 px
- [ ] Scroll one more notch: the File-actions wheel shows Copy, Move to…, Copy path, Terminal, Duplicate
- [ ] Drag `notes.txt` with Shift: Convert and Tools slices are dimmed, the centre says "Not an image"; on File actions every action is available
- [ ] Drag `folder` with Shift and scroll to File actions: Duplicate is dimmed, the other four work

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
- [ ] Drag a file and hold **Shift**: the wheel springs open around the cursor with one rounded tile per slice
- [ ] Move over a tile: it lifts outward in your Windows accent colour and the centre shows what it does (e.g. "JPEG, quality 92")
- [ ] A top panel drops down at the top centre of that monitor with Convert / Tools / File actions (names only, no Shift / Ctrl / Alt keycaps), Convert highlighted
- [ ] Still dragging, press **Ctrl** or **Alt**: nothing changes (only Shift opens the wheel; Ctrl and Alt keep their usual drag meaning)
- [ ] Turn the mouse wheel instead: each notch steps to the next or previous wheel in order, Convert, Tools, File actions, Convert... (wraps round both ways, never skips one); the window under the cursor does not scroll; after the drop, scrolling works normally again
- [ ] No mouse wheel: still dragging with Shift held, tap **1**, **2**, **3** or **4**: the wheel jumps straight to that section (Convert, Tools, Reduce size, File actions); with a section turned off in Settings the numbers follow the sections that are left. The top panel hint reads "Scroll or Shift+1–4"
- [ ] Drop on a Convert tile with a few files: the panel turns into "Converting N files to …" with a progress bar, then "N files converted to …", and tucks away after about 2 s
- [ ] Make a batch fail partly (include a `.txt` renamed to `.png`): the result stays, with the reason, until you click it; clicking shows the outputs in Explorer
- [ ] Release over the centre: nothing runs, the wheel fades and the panel goes up
- [ ] When the wheel opens, Explorer's drag picture fades away and the wheel takes its place (no picture or name over the wheel, no mouse pointer); it comes back when you drag off the wheel. The file's own icon staying where it was is Windows: a drag only copies until you drop
- [ ] Tray menu > **Hide cursor over the wheel** unticked: the pointer stays visible over the wheel
- [ ] Drag off the wheel onto the desktop or a folder: the pointer comes back and Explorer shows its usual badge there
- [ ] Drag slowly out of the wheel: it fades as you pass the tiles and shrinks away smoothly when you leave it
- [ ] Drag `notes.txt` and hold **Shift**: the Tools wheel opens with Inspect (nothing on Convert applies; before M4 this was the File-actions wheel); scrolling still visits the all-dimmed wheels, which show why nothing applies
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

### O. PDF, text from anything and Share (M5)
Have ready: a text PDF with a few pages (`report.pdf`, e.g. saved from Word), a scanned PDF (a phone
scan with no text layer), two more PDFs, a phone photo of a printed page (`page.jpg`, taken upright and
one taken sideways), a PNG with transparency, and a PDF with a password.

- [ ] Drag `report.pdf` and hold **Shift**: Convert shows JPG, PNG, TXT and Markdown
- [ ] Drop on **PNG**: a folder `report (pages)` holds `report page 01.png`, `report page 02.png`, … at 300 DPI (an A4 page is 2480 × 3508); a one-page PDF gives `report (converted).png` instead
- [ ] Drop on **JPG**: the same as JPEGs; they open in Photos
- [ ] Drop on **TXT**: `report (converted).txt` has the text in reading order with blank lines between paragraphs; on **Markdown**, headings get `#`, paragraphs are one line each, bullets become `- `
- [ ] Drop the scanned PDF on **TXT**: the text comes from OCR (the log says `ocr`); with no OCR language installed, the file says nothing for those pages and the log explains why
- [ ] Drag three photos (one PNG with transparency) with **Shift** and drop on **PDF**: one `… (converted).pdf` with the pages in Explorer's order, each the photo's size; the sideways phone photo stands upright; the transparent PNG shows on white
- [ ] Drag two PDFs, hold **Ctrl**, drop on **Merge**: a window lists them in Explorer's order with page counts. Drag one row above the other (a blue line shows where it lands), or select it and press **Alt+↑**; **Merge** (or Enter) writes `first (merged).pdf` with the pages in that order; the panel says "Merged 2 PDFs into N pages". Esc or Cancel writes nothing
- [ ] Drop one PDF on **Merge**: the panel says to drop two or more
- [ ] Drop `report.pdf` on **Organize**: a grid of page thumbnails. Click a page, Shift+click another (range), Ctrl+click toggles; drag the selection elsewhere; **R** / **Shift+R** rotate; **Ctrl+D** duplicates; **Delete** removes; **Ctrl+Z** undoes; arrows move the focus, Ctrl/Alt+arrows move the pages. **Save** writes `report (organized).pdf` exactly as shown; bookmarks to pages that are kept still work. Cancel after changes asks first
- [ ] Hover a dimmed slice (for example HEIC on a PC without the HEIF extension): the wheel's centre says why it can't be used, and fades back when you move off
- [ ] Drop two PDFs on **Organize**: two windows open, one per PDF; saving one writes only that PDF's `(organized)` copy, and the other window stays open
- [ ] Organize and Merge follow the Windows light/dark setting, and look right at 150% scaling
- [ ] Drop a large scanned PDF on **Compress**: the panel says "X to Y (N% smaller)" and the result looks the same at normal zoom; **Smallest** gives a smaller file. Drop the compressed file on Compress again: it is skipped ("already compact"), no bigger file is ever kept
- [ ] Drop `page.jpg` on **Copy text** (Tools): the panel says "Copied N words"; Ctrl+V in Notepad pastes the text. Drop `report.pdf` and the photo together: "Copied N words from 2 files". No file is written
- [ ] Drop the scanned PDF (or `page.jpg`) on **Searchable**: `… (searchable).pdf` looks exactly like the scan; in Edge, Ctrl+F finds a word and selecting text highlights it over the picture; copied text pastes correctly. `report.pdf` on Searchable is skipped ("already searchable")
- [ ] Settings > Time & language > Language: with no OCR-capable language, Copy text and Searchable are dimmed for photos and say why in the centre
- [ ] Drag two files, hold **Alt**, drop on **Share**: the Windows share sheet opens near the cursor with both files (Nearby sharing, Phone Link, Mail, other apps). Share to Mail or OneNote: the files arrive. Close the sheet: nothing else happens
- [ ] The password PDF on any PDF slice: the panel says "the PDF is locked with a password"; a `.txt` renamed to `.pdf` says "not a PDF file"
- [ ] Tray > History lists each PDF job; Undo moves its output (or the pages folder) to the Recycle Bin
- [ ] With Wi-Fi off, everything above works the same

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
- [ ] File actions (Copy, Move to…) show no card

Inspector
- [ ] Drag the phone photo, hold **Ctrl**, drop on **Inspect**: a window opens with groups File, Image, Camera, GPS, …, EXIF; nothing is written next to the photo
- [ ] Type "gps" in the search box: only GPS fields remain; "canon 2023" matches fields containing both words
- [ ] Double-click a row, or select it and press Ctrl+C: the value is on the clipboard; right-click > Copy all shown fields gives every field as text
- [ ] Drop three files (photo, video, song) on Inspect: one window with three tabs; the video tab lists Media, Video track 1, Audio track 1, chapters and tags
- [ ] Move the window to a monitor with another scale: text and layout stay crisp and fit
- [ ] Esc closes the window; Ctrl+F jumps to the search box

### T. Video and audio tools (M9, M10)
Have ffmpeg in place (`scripts/get-ffmpeg.ps1`), a phone video with sound (1080p, 20–60 s), a second clip of another size, a video with no sound, a stereo song with cover art, and a mono voice memo.

The wheel:
- [ ] Drag a video and hold **Ctrl**: Compress, 50% size, Under 10 MB, Under 25 MB, Inspect, and **Trim…**, **Crop…**, **Speed…**, **Join…**, **Frames…**, **Split…**, **Redact…** on two rings
- [ ] Drag a song: the four Compress slices, Inspect, **Normalize**, **Trim…**, **Channels…**, **Bleep…**, **Visualizer…**; a photo gets none of these

The editor window (try with each tool):
- [ ] Drop the video on **Trim…**: a window opens on the monitor you dropped on, titled "Trim Video – ShiftDrop", in the Windows light or dark theme with your accent colour (the title bar matches); the picture, then the filmstrip under it, fill in within a second or two
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
- [ ] **Join**: drop three clips on **Join…**. Drag rows to reorder, or select one and press Alt+↑ / Alt+↓; Delete leaves one out. Same-size phone clips say "joined without re-encoding" and are quick; with the other-size clip it re-encodes to the first clip's size with black bars. One MP4 comes out
- [ ] **Join** a landscape and a portrait clip: under OTHER SHAPES, **Fit** (the default) keeps the whole picture with black bars; **Fill** crops the portrait clip to fill the frame, no bars, and the note under RESULT says so
- [ ] **Frames**: press M (or double-click the timeline) at three places and drag a mark to move it; "Save 3 frames" makes a folder next to the video with full-size PNGs named by their time
- [ ] **Split**: add two cuts with M, or choose Equal parts and 3; the parts land in one folder, numbered in order, and play end to end without a gap
- [ ] **Redact**: drag two areas on the picture, one Blur and one Solid; select one and press I and O to show it for part of the video only. The preview shows the blur; in the result each area is covered only for its own time
- [ ] Drop three videos on **Crop…**: the header says "File 1 of 3"; Next file and Skip this file walk through them; at the end one batch runs (one History entry) and the skipped file is left alone

Audio tools:
- [ ] **Normalize** a song: a small window asks "How loud should it be?" with **Speech** (−16 LUFS), **Music** (−14) and **Broadcast** (−23); Esc or Cancel does nothing. With several songs it says they all get the same loudness and asks once
- [ ] **Normalize** a quiet song and pick Speech: the top panel says "Loudness −3x.x → −16.0 LUFS"; normalizing the result again says "already at −16.0 LUFS" and writes nothing
- [ ] **Trim** on the waveform with the handles or I and O; "Also trim silence at both ends" cuts the quiet lead-in of the voice memo
- [ ] **Channels**: Make mono, Swap left and right, and the Left and Right volume sliders; while playing, A switches between the original and the result. On a mono file "Make mono" changes nothing and the main button is off
- [ ] **Bleep**: drag across the waveform to mark two parts (M adds one at the playhead); a 1 kHz tone or silence; Space plays with the bleeps, A compares with the original
- [ ] **Visualizer** (tab in the song's editor): Landscape, Portrait or Square; the moving waveform, or a still picture (the cover art, or one you choose); the MP4 has the song's sound and length

Always:
- [ ] Cancel (×) in the top panel during a long render: nothing is left in the folder (no `.sdpart`)
- [ ] An `.mp4` that holds only sound, dropped with a real video on **Crop…**: its page says "This file has no video to edit." and Skip moves on; the other video is still cropped
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

### R. Reduce size
- [ ] Drag a 3 MB photo, press Shift and scroll to **Reduce size** (Convert, Tools, Reduce size, File actions): eight slices, −90% to −20%; the centre shows "3 MB" (the photo's size)
- [ ] Hover **−90%**: the centre shows "≈ 307 KB" with "was 3 MB" under it; moving to **−50%** changes it to "≈ 1.5 MB"
- [ ] Drop on **−90%**: `photo (reduced).jpg` appears, at most 10% of the original size (WebP stays WebP; PNG becomes JPEG); if quality alone cannot get there the picture is made smaller too
- [ ] The same with a video (MP4 out, at most the shown size) and a song (M4A out)
- [ ] A PDF of scans on **−70%**: `(reduced).pdf` at most 30% of the size; a PDF of mostly text gets as small as it can and the panel says it could not get smaller; one that cannot shrink at all leaves no file
- [ ] Three files: the centre shows their total size, and hovering shows the total they come out at

### U. Settings and tray entries
- [ ] Right-click the tray icon: **Settings…**, **History…** and **Show Float shelf** are near the top
- [ ] **Settings…** opens a window (dark when Windows is dark) with Convert, Tools, Reduce size and File actions, each split by kind of file (Photos, Video, Audio, PDF, Any file…) with a tick on every line
- [ ] Untick **TIFF** under Convert › Photos: drag a photo, press Shift: TIFF is gone and the other formats share the circle evenly; tick it again: it is back
- [ ] Untick **Convert › Video**: a video's Convert wheel has no video formats; Photos shows a partly ticked box when only some of its slices are on; clicking a partly ticked line ticks everything under it
- [ ] Untick the whole **Tools** section: scrolling goes Convert, Reduce size, File actions (no Tools tab in the top panel); unticking the last section left is refused with a note
- [ ] **Turn everything on** ticks everything again; quitting and starting again keeps your choices (`[Features]` in settings.ini)
- [ ] **Show Float shelf** with an empty shelf: it opens saying "Drop files here to keep them"; drop a file on it: it stays; Clear hides it
- [ ] With two or more OCR languages installed: tray › **Text recognition language** lists Automatic and each language; pick one and **Copy text** on a picture in that language reads it; with one language installed the submenu is not there

### W. Files dragged out of browsers and mail apps
These drags carry no real file (no path), only a name and the contents. ShiftDrop saves them in
`%LOCALAPPDATA%\ShiftDrop\dropped` when they are dropped, runs the slice on that copy and writes the
result to the Desktop (or the folder chosen below); the copy is deleted when the job is done.
- [ ] Edge and Chrome: drag a picture off a web page (a PNG or JPG; WebP if the page serves it), press Shift: the wheel shows image formats for it (named after the picture), with the predicted name on the card; drop on **PNG**: `name (converted).png` appears on the Desktop and the panel says "Saved to Desktop"
- [ ] The same in Firefox, and with **Tools › 50%**: `name (edited).png` on the Desktop
- [ ] Do it twice: the second result is `name (converted 2).png`, the first one is untouched
- [ ] **File actions › Copy**: the picture is saved on the Desktop (`name.png`, or `name (2).png` if that is taken) and pasting in Explorer pastes it; **Float** puts that saved copy on the shelf; **Duplicate** writes `name (copy).png` on the Desktop; **Move to…** saves it into the folder you pick
- [ ] Outlook (classic): drag an attachment (a PDF or a photo) out of a mail with Shift: its slices show; convert it: the result is on the Desktop. Drag a whole mail onto **File actions › Copy**: a `.msg` is saved on the Desktop and opens in Outlook
- [ ] Thunderbird: drag an attachment with Shift: the same
- [ ] A large attachment (50 MB or more): the top panel shows "Saving 1 dropped file…" with progress before the job starts
- [ ] Tray › **Save browser drops to** › **Downloads**: the next result is in Downloads; **Ask every time**: a folder picker ("Save here") comes first, Cancel saves nothing; **Desktop** again: back to the Desktop. The choice survives a restart (`[Output] VirtualFiles` in settings.ini)
- [ ] After a few drops, `%LOCALAPPDATA%\ShiftDrop\dropped` is empty (quit with a job running, start again: it is emptied at startup)
- [ ] Real files from Explorer still convert next to themselves

### X. Toasts, more settings, Rename and screen readers (M12)
- [ ] Installed copy: convert a long video and drop a photo on **PNG** while it runs; when the video finishes a Windows toast (not a balloon) says so, with **Open folder** and **Open**. **Open folder** selects the output in Explorer, **Open** plays it
- [ ] Two outputs in that toast: only **Open folder**, and it selects both. Quit ShiftDrop and click a toast still in the notification centre: Explorer opens all the same, and no tray icon appears
- [ ] Portable copy (no Start menu shortcut): the same case shows the old balloon; Focus assist on (Do not disturb): no toast, the result is in the notification centre
- [ ] Settings › **Terminal**: Windows Terminal, PowerShell and Command Prompt each open at the dropped folder from **File actions › Terminal**; **Custom command** with `code {folder}` opens VS Code there; the command box is greyed out unless Custom is picked
- [ ] Settings › **Theme** › **Light** while Windows is dark: the wheel, the top panel, the card and Settings itself turn light at once; **Dark** the other way; **Follow Windows** follows it again
- [ ] Settings › **Output names** › each style, converting `photo.jpg` twice: `photo (converted).png` / `photo (converted 2).png`, `photo-converted.png` / `photo-converted-2.png`, `photo_converted.png` / `photo_converted_2.png`, and the folder style: `ShiftDrop\photo.png` then `ShiftDrop\photo (2).png`. The card's predicted name matches each. No existing file is ever replaced
- [ ] Folder style with a file called `ShiftDrop` (no extension) next to the photo: the convert fails with a message and the file is untouched
- [ ] All four settings survive a restart (`[Appearance] Theme`, `[FileActions] Terminal` / `TerminalCommand`, `[Output] Naming` in settings.ini)
- [ ] Select 12 photos, **File actions › Rename…**: a window lists them in Explorer's order; `Holiday {n:000}` previews `Holiday 001.jpg` … `Holiday 012.jpg`; `{date} {name}` puts each file's modified date first; Find `IMG_` Replace `trip-` changes only names that have it
- [ ] A pattern without `{n}` (two files would get the same name): those rows turn red, the status says why and **Rename** is greyed out; a name with `?` or ending in a dot is refused the same way; a name already taken in the folder is refused
- [ ] **Rename**: the top panel says "Renamed 12 items"; tray › **History** › the run › **Undo**: every file has its old name back. Renaming `a.txt` to `A.txt` works
- [ ] Narrator on: Shift+drag a photo: Narrator says "Convert wheel, 8 actions"; moving over slices reads each one ("PNG, lossless", "TIFF, not available: …"); scrolling to another section says its name; Shift+1–4 the same
- [ ] Narrator: dropping says what the top panel shows ("Converting 1 file to PNG…"), then its result line and the line under it; a failure is read too. With Accessibility Insights, the wheel shows as a list of list items with the highlighted one selected
- [ ] NVDA: the same announcements (2023.1 or later)

### P. Image canvas, Crop, Redact Photos, Share-safe, Remove metadata (M6)
Have ready: a phone photo with GPS (portrait, taken with the phone upright), a PNG with transparency, a screenshot that shows an email address, a phone number and a card number (use a test number such as 4111 1111 1111 1111), a group photo with faces, a short phone video, and an MP3 with tags.

Canvas (Crop or Redact)
- [ ] Drag the phone photo, hold **Ctrl**, drop on **Crop**: an editor opens in the app's colours (follows Windows light/dark), the photo fits the window and is upright
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
- [ ] Drag the phone photo, hold **Ctrl**, drop on **Share-safe**: a small "Checking the photo for personal details" window shows progress; with nothing found in the picture it saves straight away
- [ ] The top panel says something like "Removed GPS, camera, 14 fields"; `photo (share-safe).jpg` has no GPS, camera or date in Properties > Details, and still opens upright
- [ ] Drop the screenshot on Share-safe: a "Review before sharing" editor opens with covers on the email, phone and card number; remove one, add one, Apply: the result says "Covered N details · …"
- [ ] Esc in the review window saves nothing
- [ ] Drop the phone video on Share-safe: `video (share-safe).mp4` plays the same, is about the same size (no re-encode), and its location and creation date are gone (Inspect on it shows no GPS / location tag)
- [ ] Drop an already clean file (a Share-safe output): it is skipped with "already clean"

Remove metadata (File-actions wheel, **Alt**)
- [ ] Drop the phone photo on **Remove metadata**: `photo (no metadata).jpg`, no editor, the result line lists what went
- [ ] Drop the MP3: `song (no metadata).mp3` has no title, artist or cover art in Explorer, and still plays
- [ ] Drop a WebP and a PNG with text chunks: both come out clean (check with Inspect)

### V. Documents, spreadsheets, subtitles, Office bridge (M11)
Have ready: a Word document with headings, a bulleted and a numbered list, a table, a link and a
picture (`report.docx`); a Markdown file (`notes.md`); an RTF saved from WordPad with a picture; an
RTFD package from a Mac (a folder ending in `.rtfd`); an old `.pages` file; a text PDF; an Excel
workbook with two sheets, a date column and numbers with leading zeros (`book.xlsx`); the same saved
as Excel 97-2003 (`book.xls`); a CSV saved by European Excel (semicolons); an SRT and a VTT from
YouTube; a PowerPoint deck. For the Office bridge you need Microsoft Office or LibreOffice installed.

Documents
- [ ] Drag `report.docx` and hold **Shift**: Convert shows PDF, Markdown, HTML, EPUB and TXT (no DOCX), plus **Exact PDF** when Word or LibreOffice is installed
- [ ] Drop on **Markdown**: a folder `report (converted)` holds `report.md` and `images/`; the Markdown shows headings as `#`, lists, the table as a `|` table, the link, and the picture (open it in VS Code's preview)
- [ ] Drop on **HTML**: the same folder layout; the page opens in Edge with the picture, the table and working links. With no pictures in the document you get a single `report (converted).html`
- [ ] Drop on **EPUB**: `report (converted).epub` opens in an e-book app (Thorium, Calibre's viewer, or Edge's extension) with a table of contents; the picture shows
- [ ] Drop on **TXT**: plain text, bullets as •, the table as tab-separated lines, links as "text (address)"
- [ ] Drop on **PDF**: no print dialog appears; `report (converted).pdf` has A4 pages (Letter if Windows' region is the US), 2 cm margins, page numbers at the bottom, selectable text, the table's header row repeated when it runs onto a new page
- [ ] Turn off "Microsoft Print to PDF" (Windows Features), restart ShiftDrop: the PDF slice is dimmed and the centre says the printer is missing. Turn it back on
- [ ] Drop `notes.md` on **DOCX**: Word opens it with real headings (the navigation pane lists them), real lists and the table. On **HTML**: it looks like the Markdown preview
- [ ] Drop the WordPad RTF on **DOCX**: bold, italic, the picture and the larger headings come through
- [ ] Drop the `.rtfd` folder with **Shift**: the documents slices appear; **DOCX** includes its pictures
- [ ] Drop the `.pages` file on **DOCX**: the text arrives (from its preview PDF); a newer `.pages` with no preview PDF says so in the panel
- [ ] Drop the text PDF with **Shift**: besides M5's slices there are **DOCX**, **EPUB** and **HTML**; the DOCX has the text reflowed with headings and bullets

Spreadsheets
- [ ] Drag `book.xlsx` with **Shift**: ODS, CSV, TSV and PDF (no XLSX)
- [ ] Drop on **CSV**: a folder `book (converted)` with one CSV per sheet; double-click one: Excel opens it with accents right, dates as 2024-01-31, leading zeros kept as text
- [ ] Drop on **ODS**: LibreOffice Calc (or Excel) opens it with both sheets, the dates still dates, the numbers still numbers
- [ ] Drop on **PDF**: each sheet is a table under its name, wide sheets come out landscape, the header row repeats on each page
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

Office bridge (LibreOffice only, no Microsoft Office)
- [ ] The deck shows **Exact PDF** but not the slide pictures; the PDF comes out right, no LibreOffice window appears, and a LibreOffice window you already had open is not disturbed
- [ ] With neither installed, no Office bridge slices appear at all

- [ ] With Wi-Fi off, everything above works the same; Tray > History lists each job and Undo moves its output (or folder) to the Recycle Bin

### Q. Archives and the sandbox (M7)

Prepare a folder `Photos` with a few pictures, a subfolder and a file with a non-English name
(for example `Müller 日本.txt`); a ZIP, a TAR.GZ, a 7z and a RAR made elsewhere (7-Zip or WinRAR);
a password-protected ZIP; and an old ZIP made on a non-UTF-8 system if you have one.

Compress
- [ ] Drag `Photos` onto **Zip** on the File-actions wheel: `Photos (zipped).zip` appears next to the folder; open it in Explorer: the subfolder, the non-English name and the dates are all there
- [ ] Select three files in one folder and drop them on **Zip** on the File-actions wheel: one ZIP named after their folder holding the three files
- [ ] Drag `Photos` to the Convert wheel: it shows ZIP, TAR.GZ and 7z; **TAR.GZ** makes `Photos (archived).tar.gz`, **7z** makes `Photos (archived).7z`; both open in 7-Zip with the same files
- [ ] Zip a folder of a few GB: the top panel's progress moves smoothly; press × halfway: no `.zip` and no `.sdpart` file is left

Extract and convert
- [ ] Drag each archive (ZIP, TAR.GZ, 7z, RAR) to the Convert wheel: it shows Extract, ZIP, TAR.GZ, 7z (on a ZIP, TAR.GZ or 7z its own format is dimmed)
- [ ] **Extract** on each: a folder `name (extracted)` next to it with the same files, names and dates; an archive holding one top-level folder doesn't add an extra folder level
- [ ] The old non-UTF-8 ZIP: accented names come out right (not `M├╝ller`)
- [ ] **ZIP** on the RAR and the 7z: `name (converted).zip` with the same contents
- [ ] The password-protected ZIP on Extract: the top panel says it is password-protected; nothing is created
- [ ] A big archive on Extract, × halfway: no folder and no `.sdpart` is left next to it
- [ ] Undo the last extract from History: the folder goes to the Recycle Bin

Sandbox
- [ ] Extract an archive, then open `%LOCALAPPDATA%\ShiftDrop\logs`: no "sandbox:" warning line
- [ ] While a large extract runs, Process Explorer (or Task Manager > Details, column "Integrity level") shows `ShiftDropWorker.exe` at **AppContainer**, under ShiftDrop.exe, and it disappears when the job ends
- [ ] Kill ShiftDrop.exe in Task Manager during a large extract: ShiftDropWorker.exe ends at once
- [ ] Set `[Sandbox] Archives=0` in `settings.ini`, restart, extract: it still works, and the worker shows integrity level Medium
- [ ] Optional (open question 10), on a PC with a GPU: set `[Sandbox] Media=1`, restart, convert a video to MP4 and compress one (GPU encoder in use): both work, ffmpeg.exe shows **AppContainer** in Process Explorer; note any failure and the log lines
- [ ] Uninstall: the AppContainer profile is removed (`%LOCALAPPDATA%\Packages\shiftdrop.sandbox` is gone)

### V. AVIF, HEIC, SVG and multi-frame files (M5)
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
- [ ] The `.heic` on the Tools wheel's Rotate: the result is a JPG (on purpose: it opens everywhere)
- [ ] Without HEVC Video Extensions (uninstall it, or use a PC that lacks it): the HEIC slice is dimmed and the log says `wheel: 'HEIC' dimmed: install "HEVC Video Extensions"...`; the other slices still work
- [ ] Without HEIF Image Extensions: dragging the `.heic` dims every Convert slice and the centre says "Needs HEIF Image Extensions"; the HEIC slice for a JPG is dimmed, and the log line names both extensions to install
- [ ] Install the missing extension and restart ShiftDrop: the slices come back

SVG
- [ ] The SVG with a size on PNG: the PNG has exactly the SVG's width and height, transparent around the drawing
- [ ] The viewBox-only SVG on PNG: 1024 px on its long side, sharp edges, transparent background
- [ ] The SVG with text on PNG: the text is drawn (Arial / Segoe UI / Times look-alikes), not missing
- [ ] SVG on JPG: white background where the SVG is transparent; on WebP and AVIF: transparency kept
- [ ] SVG on **PDF** (Convert wheel): a one-page PDF with the drawing; several SVGs together give one page each
- [ ] Hover a slice with an SVG dragged: the card shows the output size (e.g. 1024×512)
- [ ] An `.svgz` (gzipped SVG) works the same as `.svg`
- [ ] A broken SVG (rename a `.txt` to `.svg`): the job fails with "not a readable SVG" and leaves nothing behind

Crop, Redact and Share-safe
- [ ] The `.avif` on Crop: the cropped result is an `.avif`; the `.heic` on Crop: a `.jpg`; the SVG on Crop: a `.png` with the transparency kept
- [ ] The `.avif` on Share-safe: a clean `.avif` comes out

Animated GIFs and multi-page TIFFs
- [ ] The animated GIF on WebP: `name (converted).webp` animates in Edge or Chrome at the same speed as the GIF; the top panel says "Animated WebP, N frames"
- [ ] The animated GIF on AVIF: `name (converted).avif` animates in Chrome
- [ ] The animated GIF on PNG: a folder `name (frames)` with `name frame 01.png`, `02`... Each frame looks complete (not just the changed bits); drop again and a second folder `name (frames 2)` appears, the first untouched
- [ ] The animated GIF on JPG: the same folder, frames on white
- [ ] The animated GIF on TIFF: one `.tiff`; the Photos app (or Windows Photo Viewer) pages through every frame
- [ ] The multi-page TIFF on PDF (Convert wheel): one PDF with every page, each its own size; with a JPG dropped alongside, the JPG is a page too
- [ ] The multi-page TIFF on PNG: a folder `name (frames)` with `name page 01.png` and so on
- [ ] A one-frame GIF on PNG: an ordinary `name (converted).png`, no folder
- [ ] Undo (History) on a frames folder moves the whole folder to the Recycle Bin

### W. Photo tools: Edit, Annotate, Background, Collage (M8)
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
- [ ] Drop the screenshot on **Annotate**: Arrow is chosen; drag: an arrow from the press to the release; Shift snaps it to 45°
- [ ] Text: click, type, Shift+Enter for a new line, Enter to finish; click it again (selected) to edit; Esc on an empty text removes it
- [ ] Box and Circle (Shift keeps them even; Filled toggle), Callout (press on the subject, drag to where the bubble goes, type), Number (1, 2, 3 in the order placed; deleting 1 renumbers), Magnifier (press on a detail, drag the loupe; its grips change the spot and the magnification)
- [ ] Pen and Highlighter draw smooth strokes; the highlighter is see-through and doesn't darken where it overlaps itself
- [ ] Insert picture: pick a PNG logo; it lands in the middle and can be moved and resized by its corners
- [ ] Select a mark: colour, width and size change it; To front and Delete work; arrows nudge it (Shift by ten); the letter keys V A T B O C N M P H switch tools
- [ ] Drop three screenshots: the switcher in the panel moves between them, each keeps its own marks; Save writes only those with marks (the rest are skipped)
- [ ] Save: `screenshot (annotated).png`; the marks look as in the editor at 100 %

Add a Background
- [ ] Drop the PNG with transparency on **Background**: gradient by default with a soft shadow and rounded corners
- [ ] Solid (pick a custom colour), Gradient (From, To, Direction), Blur (the photo's own blurred copy behind it), Picture (choose any picture; its blur slider)
- [ ] Shape Auto, 1:1, 4:5, 4:3, 3:2, 16:9, 9:16: the canvas takes the shape with the picture centred; Spacing, Rounded corners and Shadow change live; the panel shows the output size
- [ ] Save: `logo (background).png`; a JPEG source gives a JPEG with its EXIF

Create a Collage
- [ ] Drop the four photos on **Collage**: "Opening 4 photos…", then a grid; Row and Column keep every photo whole; Featured makes the first one large
- [ ] Drag a photo onto another: they swap; Shuffle and Reverse change the order; Undo puts it back
- [ ] Shape (grid and featured), Longer side (type 2000), Spacing, Rounded corners, background colour and "Show whole photos" all change the preview live
- [ ] Save: one `first-photo (collage).jpg` (named after the first photo in the collage), the longer side as typed, no camera or GPS in its details

GPU and speed
- [ ] On a laptop with a graphics card and on a VM without one (WARP): every preview works; the log names the device
- [ ] Unplug or update the graphics driver while an editor is open (or change the display): the preview comes back

## 4. M0/M1 checklist

### A. Startup (both builds)
- [ ] A tray icon appears (blue disc with a white ring) and a notification says ShiftDrop is running, naming the build (M0 or M1)
- [ ] Right-click the tray icon: the menu shows History…, Open recipes folder, Open logs folder, Start with Windows and Quit; each works
- [ ] Launching the exe a second time shows "ShiftDrop is already running" and no second icon
- [ ] `settings.ini` exists in `%LOCALAPPDATA%\ShiftDrop` with `Convert=Shift`, `Tools=None`, `Files=None` (an older file with `Tools=Ctrl` / `Files=Alt` is switched to None once, and the log says so)
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
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.5.0 installer\ShiftDrop.iss
.\build\installer\ShiftDrop-Setup-0.5.0.exe
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

