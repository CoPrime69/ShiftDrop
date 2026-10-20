# ShiftDrop plan (REA stages 1–5)

Native Windows 11 rebuild of the *behaviour* of Tangerine (macOS, by @th2049). Clean room: we copy
behaviour and UX patterns only, never code, name, icons, artwork or marketing text.

Evidence labels, following REA's rule that every conclusion separates what was seen from what was
guessed: **[O]** observed in a public source, **[I]** inferred by us, **[U]** unknown / unverified.

> Roadmap note: milestones after M1 follow the approved research plan (`research/`) plus full parity
> with Prakhar's detailed Tangerine specification (2026-10-20). Both are mapped in Stage 5b.

---

## How we applied the REA process

REA ([morluto/rea](https://github.com/morluto/rea)) is an MCP toolkit for inspecting shipped
binaries and runtime behaviour. Its staged workflow is: turn the request into questions and the
evidence each needs, start from the smallest overview, follow each question from entry point to
result, corroborate with the right kind of evidence, and keep a findings ledger with what is still
unknown. We used that discipline, with one limit:

- Tangerine is a macOS app and this work runs on Linux/Windows, so there was **no binary or runtime
  to inspect**. All observations come from public material (store listing, developer site and press
  kit, a MacGeneration review, Product Hunt, the developer's X post).
- Optional corroboration later: if you download the free Tangerine on a Mac, REA's archive/app
  inventory (`open_binary` on the `.app`/DMG, then `project_apple_application_graph`) would show
  which frameworks it links (for example AVFoundation vs a bundled ffmpeg). That is
  architecture-level evidence only; no decompiled code goes anywhere near our repo.
- There is an unofficial "tangerine-windows" project on GitHub. We have not read its code and will
  not, to keep the rebuild clean.

---

## Stage 1–2. Observe and break down: feature list

| # | Behaviour | Evidence | v1 / later |
|---|---|---|---|
| 1 | Start dragging a file in Finder, press **Shift**: a wheel of output formats appears around the pointer | [O] App Store, site, MacG | v1 |
| 2 | The wheel offers only formats that fit the file's type | [O] MacG, site | v1 |
| 3 | The wheel stays open after you let go of the key; you keep dragging and **drop on a slice** to run it | [O] site | v1 |
| 4 | Keyboard choice while the wheel is open: arrow keys pick a slice, Enter applies, Esc cancels | [O] MacG, Prakhar's spec | M4 |
| 5 | Dropping anywhere else, or Esc, cancels | [O] MacG; [I] for "elsewhere" | v1 |
| 6 | A second modifier opens **tools** for that file type (22 named tools, see Stage 5b) | [O] Prakhar's spec and press kit: Command | Ctrl in ShiftDrop; tools M3–M10 |
| 7 | A third modifier opens **file actions** (AirDrop, Float, Copy, Move, Copy Path, Terminal) | [O] Prakhar's spec, press kit: Control | Alt in ShiftDrop; M2, M3, M5 |
| 8 | Several files of the same kind: the choice applies to all | [O] site | v1 |
| 9 | Mixed kinds: a notice instead of a wheel | [O] site. We go further: show only actions all files support | v1 |
| 10 | Output is written **next to the original**; nothing is moved or overwritten | [O] App Store, site | v1 |
| 11 | Output naming pattern | [U] not documented. Ours: `name (converted).ext`, then `(converted 2)`... | v1 |
| 12 | Runs fully on-device | [O] site, press kit | v1 |
| 13 | Breadth: ~275 conversion paths, ~26 tools, 6 file actions | [O] site, press kit (an older listing said 188) | grows per milestone |
| 14 | Feedback when done | [U] not documented (likely a notification or Finder reveal) | v1: notification, click reveals the file |
| 15 | Gesture timing: the wheel appears as soon as the key is pressed mid-drag | [O] reviews describe it as instant; [U] exact delay | v1: appears on the next mouse move or within 30 ms |
| 16 | Menu-bar app, settings, launch at login | [I] typical for this kind of app; [U] specifics | M2 (start at login), M12 (settings window) |
| 17 | Requires Apple silicon, macOS 13+; $29.99 one-time | [O] store, press kit | n/a |

Our spec adds: configurable trigger keys (never Win), light/dark theme, glass look, toast that opens
the file location, logs, tray app, start at login (opt-out).

---

## Stage 3. Inferred architecture and its Windows mapping

How Tangerine probably works [I], and what we use instead:

| macOS mechanism [I] | Windows equivalent (ours) | Risk / limitation |
|---|---|---|
| Global event monitor (`NSEvent.addGlobalMonitorForEvents` for `leftMouseDragged` + `flagsChanged`, or a `CGEventTap`); needs Accessibility / Input Monitoring permission | `WH_MOUSE_LL` hook on the UI thread + `GetAsyncKeyState` for the modifier; a 30 ms poll covers "key pressed while the mouse is still" | Hook must return in microseconds or Windows silently removes it (`LowLevelHooksTimeout`). We keep it allocation-free and add a watchdog that reinstalls it. No permission prompt on Windows |
| Read the **drag pasteboard** (`NSPasteboard(name: .drag)`) to learn which files are being dragged *before* the drop | **No equivalent.** Windows has no global "current drag data". We learn the files only when OLE calls our `IDropTarget::DragEnter`, so the window must be under the cursor first | This is the core platform gap. Solved with the invisible "probe" disc (Stage 4). Keyboard-only selection (#4) also depends on it |
| Borderless `NSPanel` (non-activating, high window level) with `NSVisualEffectView` vibrancy | Layered popup `WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, Direct2D into a DIB, `UpdateLayeredWindow` with per-pixel alpha | No public API gives true blur behind a *shaped* per-pixel-alpha window. See "Glass look" below |
| Panel registered for dragged types (`registerForDraggedTypes([.fileURL])`), `performDragOperation` gets URLs | `RegisterDragDrop` with our own `IDropTarget`; read `CF_HDROP` with `DragQueryFileW` | Explorer's Shift/Ctrl/Alt meanings; UIPI; non-file drags (Stage 4) |
| Return `.copy` from `draggingUpdated` so Finder doesn't move the file | Always answer `DROPEFFECT_COPY` (or `LINK`), never `MOVE` | With `MOVE` the source (Explorer) deletes the original after the drop |
| Image conversion via ImageIO / Core Image (HEIC native) | **WIC**: built-in PNG/JPEG/BMP/TIFF/GIF; WebP, HEIC, AVIF only if the Store codec extensions are installed (checked at runtime) | HEIC needs "HEIF Image Extensions" + the paid "HEVC Video Extensions" on many PCs. Encoding WebP/HEIC is not available from WIC on a stock PC |
| Audio/video via AVFoundation | Bundled LGPL `ffmpeg.exe` via `CreateProcessW` (no console), progress parsed from `-progress pipe:1`, killed on cancel | LGPL build has no x264/x265. H.264/HEVC encode via Media Foundation (`h264_mf`) or OpenH264. Licence notices required |
| PDF via PDFKit | PDFium (BSD/Apache) for rendering, QPDF (Apache-2.0) for page operations. No Ghostscript/MuPDF (AGPL) | Later milestone |
| Background removal via Vision (`VNGenerateForegroundInstanceMaskRequest`) | ONNX Runtime + DirectML with a permissively licensed model | Later; model size and licence vetting |
| Notification via `UNUserNotificationCenter`, reveal via `NSWorkspace.activateFileViewerSelecting` | `Shell_NotifyIcon` balloon (shown as a toast on Win10/11), `SHOpenFolderAndSelectItems` on click | Clicking works while the toast is on screen; from Action Center it may not reach an unpackaged app [U]. A real toast with an AUMID comes with packaging |
| Login item (`SMAppService`) | `HKCU\...\Run` value (or MSIX StartupTask when packaged) | Later milestone |
| Single instance | Named mutex `Local\ShiftDrop.SingleInstance...` + `FindWindow` to notify the running copy | — |

### Glass look (acrylic / Mica)

`DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_TRANSIENTWINDOW)` only affects windows DWM
composes itself; a per-pixel-alpha layered window hands DWM a finished bitmap, so the attribute has
no effect, and system backdrops are rectangular anyway. **v1 uses a translucent themed fill.** A
spike after M1 can try a non-layered `WS_EX_NOREDIRECTIONBITMAP` window with DirectComposition and a
circular clip, which may allow a real backdrop [U].

---

## Stage 4. The riskiest unknown: "drag in progress + modifier → overlay drop target"

### Approach (implemented in M0/M1)

1. **Hook** (`MouseHook.cpp`): on primary-button down, remember the point. On a move past
   `SM_CXDRAG/SM_CYDRAG` post `DragBegin`. While dragging, if the trigger key is down
   (`GetAsyncKeyState`) post `DragArm` once. On button up post `DragEnd`. Nothing else happens in
   the callback.
2. **Poll** (`main.cpp`): between `DragBegin` and `DragEnd`, a 30 ms timer checks the key too, so
   pressing Shift while holding the mouse still also works.
3. **Probe**: on arm, show the overlay centred on the cursor, painted at ~2% alpha. It is invisible
   but hit-testable (alpha > 0). We do not know yet whether this is a file drag.
4. **OLE decides**: the drag source's `DoDragDrop` loop finds our window under the cursor on the next
   mouse move and calls `DragEnter`. If the data has `CF_HDROP`, we read the paths and draw the real
   wheel. If not (text, URL, browser image), we hide and answer `DROPEFFECT_NONE`. If no `DragEnter`
   ever comes (window move, text selection: no OLE drag at all), the probe is hidden at button-up.
   So a Shift+drag that is not a file drag never flashes a menu.
5. **Hover/drop**: `DragOver` hit-tests the slice (angle + radius) and highlights it. Slices answer
   `DROPEFFECT_COPY`; the centre and disabled slices answer `NONE`, so releasing there turns into
   `DragLeave` (cancel). `Drop` reads paths, hides, queues jobs, returns immediately (the source is
   blocked until we return).

### Edge cases

| Case | What happens | Status |
|---|---|---|
| **Explorer semantics**: Shift = move, Ctrl = copy, Alt = link, Ctrl+Shift = shortcut | Over the wheel we always answer COPY (LINK if COPY isn't offered), never MOVE, so the original is never deleted. **Outside** the wheel the drop goes to whatever is underneath, so Shift-drag-to-move still works if you drag away from the wheel and drop on a folder | Implemented; verify in TESTING.md |
| Alt as trigger | Pressing/releasing Alt can activate the menu bar of the source window | Open question; Alt is configurable but not default |
| Esc | The drag *source* cancels the drag and OLE sends us `DragLeave`, so the wheel hides | Implemented |
| Leaving the wheel | Alpha-0 corners are not hit-testable, so the cursor leaving the disc gives `DragLeave` and the wheel hides. Pressing the key again in the same drag brings it back | Implemented |
| **Per-monitor DPI** | Manifest is PerMonitorV2; hook points, `DragOver` points and `UpdateLayeredWindow` are all physical pixels. Wheel size uses `GetDpiForMonitor` at the cursor | Implemented; verify on mixed-DPI monitors |
| Near a screen edge | The wheel stays centred on the cursor and may be partly off-screen (shifting it would put the cursor on a slice) | Open question |
| **Elevated windows (UIPI)** | A drag from a normal Explorer cannot drop onto an elevated window; if ShiftDrop runs elevated it warns at startup. A drag *from* an elevated app to us, and whether the LL hook sees input over elevated windows, are **[U]**; the watchdog logs when the hook goes quiet while the cursor moves. Real fix if needed: `uiAccess=true`, which needs a signed exe installed under Program Files | Documented; verify |
| **Browsers** | Chrome/Edge/Firefox image drags usually carry `FileGroupDescriptorW` + `FileContents` (virtual files) or a URL, not `CF_HDROP`. Downloads-bar / file:// drags may carry `CF_HDROP`. v1 ignores non-`CF_HDROP` drags | Later: materialise virtual files to a temp folder and write output to Downloads |
| Other apps (Outlook attachments, 7-Zip, VS Code) | Same as browsers: `CF_HDROP` works, virtual files don't yet | Later |
| Text drags | No `CF_HDROP`: the wheel hides | Implemented |
| **Hook removed for being slow** | Windows removes `WH_MOUSE_LL` silently after a timeout. Callback is allocation/IO-free on a thread that only pumps messages; watchdog reinstalls when the cursor moves but the hook saw nothing for >2 s | Implemented |
| Left-handed mouse (swapped buttons) | LL hook reports physical buttons, so we treat the right button as primary when swapped | Implemented, unverified |
| Touch / pen drags | Not handled in v1 [U] | Later |
| Drag image disappears over our window | We forward events to the shell's `IDropTargetHelper`, which keeps Explorer's thumbnail visible | Implemented |
| Single instance | Named mutex; a second launch tells the first one to show "already running" | Implemented |

---

## Stage 5a. Tech stack decision

Decided by Prakhar (2026-10-20): **C++20, raw Win32 + COM**, no MFC/ATL/Qt/.NET. MSVC (VS 2022) +
CMake + vcpkg manifest mode. Unicode only, PerMonitorV2, static CRT (single exe). RAII via wil and
`Microsoft::WRL::ComPtr`.

Why this is a good fit for this app specifically:
- The hard parts are Win32/COM anyway (LL hooks, OLE drop target, layered windows, Shell_NotifyIcon,
  WIC, DirectWrite). Native code removes every interop layer between us and them.
- Hook latency: no GC pauses on the thread that services `WH_MOUSE_LL`.
- Startup and idle memory of a single small exe beat any managed or XAML runtime, which matters for
  something that runs all day in the tray.
- Trade-offs accepted: settings UI is more work in raw Win32 (plan: a small dialog-based window,
  or a WebView2-free custom-drawn one); fewer safety nets, so we lean on RAII, `/W4`, and fuzzing
  the converters later.

Earlier option of C#/WPF or WinUI 3 is dropped. (For the record: WinUI 3 window-level Mica does not
help a shaped overlay either, and both would still need Win32 interop for the hook and OLE.)

---

## Stage 5b. Milestones

M0 and M1 are delivered. From M2 on, the roadmap is the **approved plan** in
[research/product-research-and-design.md](research/product-research-and-design.md) (section 6). Each
milestone there ships one signature feature end to end (S1 to S6, M2 to M7).

Prakhar's detailed Tangerine specification (2026-10-20) is the **parity target**. Every format, tool
and file action it names is mapped below. Work that didn't fit the signature milestones gets four new
milestones (M8 to M11), so polish and release move from M8 to **M12**. Every milestone lands as its
own pull request.

Each milestone, at a glance:

| Milestone | Signature feature | Parity work that lands with it |
|---|---|---|
| M0/M1 (delivered) | Gesture, wheel, WIC images | Shift wheel, batches, output next to the source |
| M2 | Recipes + Undo (S3) | Tools and File-actions wheels; Copy, Move, Copy Path, Terminal; start with Windows |
| M3 (three PRs: M3a, M3b, M3c) | Fit to… (S2), media engine | M3a: all audio/video formats, progress and cancel. M3b: Compress, Fit to…, carry, clipboard. M3c: Float shelf |
| M4 | Know before you drop (S1) | Keyboard model, metadata inspector |
| M5 (delivered; AVIF/HEIC/SVG still open) | Text from anything (S4), PDF engine | PDF ↔ images, Merge PDFs, Organize PDF Pages, AVIF/HEIC/SVG, Share |
| M6 | Share-safe (S5), image canvas | Edit Metadata, Redact Photos, Crop Images |
| M7 | Sealed engines + plugins (S6) | Archives (ZIP/TAR/GZIP, RAR input) |
| **M8 (new)** | — | Photo tools: Edit Photos, Annotate, Add a Background, Collage |
| **M9 (new)** | — | Video tools: Trim, Crop, Speed, Join, Save Frames, Split, Redact Video |
| **M10 (new)** | — | Audio tools: Normalize, Visualizer, Trim Audio, Channels, Bleep |
| **M11 (new)** | — | Documents, spreadsheets, presentations, subtitles, PDF → DOCX/EPUB/PPTX/TXT/MD |
| M12 (was M8) | Polish, release and showcase | Settings window, real toasts, winget and a portable zip (the installer itself shipped with M2) |

Each milestone in detail:

**M2. Recipes + Undo (S3)**
- Action, Recipe and Pipeline model. The "Web-ready" recipe is one slice and writes no intermediate file.
- Staging file plus atomic rename. History in SQLite. Undo moves outputs to the Recycle Bin and is refused if an output changed.
- CI, unit tests, ADRs 0001–0005. WebP in and out through libwebp.
- **(spec/parity)** Three wheels, each on a configurable key:
  - Shift = Convert.
  - Ctrl = Tools: Tangerine's Command; quick edits now (resize, rotate, flip), the 22 tools as their milestones land.
  - Alt = File actions: Tangerine's Control.
- **(spec)** Start with Windows: on by default, a "Start with Windows" tick in the tray menu turns it off (moved forward from M12 at Prakhar's request).
- File actions in M2: Copy (to the clipboard), **Move** (pick a folder, then move the originals; this replaces the earlier "Cut"), Copy Path (one per line), Terminal, and Duplicate (ours). File actions also accept folders and mixed selections.

**M3. Fit to… (S2) and the media engine**

Split into three pull requests so each one can be tried by hand:
- **M3a (this PR): media engine and conversions.** Bundled LGPL ffmpeg.exe run as a sandbox-friendly
  child process, video and audio formats on the Convert wheel (only the formats that fit the dragged
  files), per-file progress and a cancel button in the top panel.
- **M3b:** Fit to… target sizes, Compress (tool 1), convert-and-carry and drop to clipboard.
- **M3c (done):** the Float shelf, convert-and-carry from the top panel, and copy to the clipboard (files, plus the picture for a single image).

The full list:
- sd-media worker plus LGPL ffmpeg.
- Target-size presets for images, video and PDF, with GPU encoders and a software fallback.
- Convert-and-carry chip, drop to clipboard, tray progress and cancel.
- **(parity)** Convert wheel for media:
  - Video: MP4, MOV, MKV, WebM, AVI, WMV, GIF, and MP3 audio export.
  - Audio: MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA.
- **(parity)** Tool 1, **Compress**: target size, percentage, preset, and resize while compressing (images and media now, PDF in M5).
- **(parity)** File action **Float**: a floating shelf that holds files, pins to a screen edge, and lets you drag files out into any app. It's built on convert-and-carry.

**M4. Know before you drop (S1)**
- Predicted size and live thumbnail on hover, plus a metadata diff.
- The overlay moves to DirectComposition, which is where the real acrylic spike happens.
- **(parity)** Keyboard model:
  - While the wheel is open, the arrow keys move between slices, Enter applies and Esc cancels.
- **(parity)** "Enter on a file opens the format menu" can't be done the same way: Enter in Explorer opens the file and we can't take it over. Closest alternatives:
  - A configurable global hotkey (default Ctrl+Shift+Space) opens the wheel for Explorer's current selection.
  - A "ShiftDrop…" entry in the right-click menu does the same.
- **(parity)** Tool 2, **Edit Metadata**, part 1: an inspector that reads metadata and groups fields by file, track and chapter, with search.

**M5. Text from anything (S4) and the PDF engine** (delivered, except AVIF, HEIC and SVG)
- [x] Windows OCR to the clipboard; searchable PDF.
- [x] PDF engine:
  - Windows.Data.Pdf renders pages.
  - Text comes from our own reader of the pages' text operators, built on QPDF's content-stream parser (not PDFium; see the decisions log).
  - QPDF (Apache-2.0) restructures files.
  - Our own writer creates image PDFs.
- [x] **(parity)** PDF → JPG/PNG: all pages, 300 DPI by default. Images → PDF. PDF → TXT and Markdown, using the text layer, with OCR when a page has none.
- [x] **(parity)** Tool 21, **Merge PDFs**: starts in the order Explorer gave us; drag to reorder.
- [x] **(parity)** Tool 22, **Organize PDF Pages**: a page grid to reorder, rotate, duplicate and remove pages.
- [x] **(parity)** Compress for PDF.
- [ ] **(parity)** AVIF in and out (libavif with aom/dav1d, BSD). HEIC (see exclusions). SVG input via lunasvg (MIT). Not done in M5; still open.
- [x] **(parity)** File action **AirDrop** becomes **Share**: the Windows share sheet (Nearby Share, Phone Link, mail, apps). This is the closest Windows equivalent of a recipient picker.

**M6. Share-safe (S5) and the image canvas**
- Verified metadata scrub, and rule-based PII redaction with a review step.
- Builds the shared Direct2D image canvas window that M8 reuses.
- **(parity)** Tool 2, **Edit Metadata**, part 2: edit and remove fields, including hidden camera and processing data.
  - Images go through the Windows property system and WIC metadata writers.
  - Media goes through ffmpeg.
- **(parity)** Tool 7, **Redact Photos**: solid or blur covers, with several movable regions.
  - Text is found with Windows' built-in OCR.
  - Faces are found with Windows' built-in face detector (Windows.Media.FaceAnalysis). It ships with Windows; we neither bundle nor download a model.
- **(parity)** Tool 6, **Crop Images**: exact pixel size, plus preset and custom aspect ratios.

**M7. Sealed engines and plugins (S6)**
- AppContainer workers, WASM plugins, and proof that nothing touches the network.
- **(parity)** Archives, through libarchive (BSD-2) inside the sandboxed worker, because archive parsing is untrusted input:
  - ZIP, TAR and GZIP: create, extract, and convert between them.
  - RAR and 7z: extract and convert from.

**M8. Photo tools (new)**

Everything here runs on the M6 canvas with a live GPU preview, using Direct2D effects plus our own shaders. No models.
- Tool 3, **Edit Photos**: exposure, light, colour and detail, plus dehaze (dark-channel prior), clarity (local contrast), grain and noise reduction (edge-preserving filter).
- Tool 4, **Annotate Photos**: arrows, text, shapes, callouts, numbered labels, magnifiers, inserted images, freehand drawing and highlighter.
- Tool 5, **Add a Background**: solid, gradient or image background, with aspect ratio, spacing, corner radius, blur and shadow.
- Tool 8, **Create a Collage**: grid, row, column and featured layouts, with order, output size, spacing, corners and background.

**M9. Video tools (new, done)**

One timeline window covers all seven tools. It previews with Windows' Media Foundation, steps frame by frame, and accepts typed timecodes. Rendering goes through ffmpeg in the sandboxed worker.
- Tool 9, **Trim Video**.
- Tool 10, **Crop Video**: exact output size, original audio kept.
- Tool 11, **Change Video Speed**: keeps the audio pitch (atempo), and handles silent videos.
- Tool 12, **Join Videos**: MP4, MOV and MKV in, one MP4 out; the first clip sets the canvas.
- Tool 13, **Save Video Frames**: exact frames as full-resolution PNGs.
- Tool 14, **Split Video**: at custom points or into equal parts; the clips go into one folder.
- Tool 15, **Redact Video**: solid or blur regions, each with its own time range.

**M10. Audio tools (new, done)**

One waveform window with playback through Windows (XAudio2), and A/B compare.
- Tool 16, **Normalize Volume**: EBU R128 loudness range and true peak, with before and after levels.
- Tool 17, **Audio Visualizer**: an animated waveform or a still-image MP4, landscape, portrait or square.
- Tool 18, **Trim Audio**: at waveform precision with exact times, plus optional removal of silence at the start and end.
- Tool 19, **Convert Audio Channels**: mono or stereo, separate left and right volumes, and per-channel preview.
- Tool 20, **Bleep Audio**: movable bleep ranges, with a compare against the original.

**M11. Documents, spreadsheets, presentations, subtitles (new)**

All our own code on small permissively licensed libraries: miniz, pugixml, md4c, libxlsxwriter and libxls. Formats:
- Documents: DOCX ↔ Markdown, TXT, HTML and EPUB. RTF input (Windows' RichEdit). HTML export.
- Documents to PDF: our own layout, drawn with DirectWrite, through Windows' built-in "Microsoft Print to PDF".
- Spreadsheets:
  - XLSX, ODS, CSV and TSV convert into each other.
  - XLS input.
  - PDF export as a plain table layout.
- Subtitles: SRT ↔ VTT, and either to TXT.
- PDF → DOCX and EPUB as reflowed text and images. PDF → PPTX with one page image per slide.
- **Office bridge:** when Microsoft Office or LibreOffice is *already installed*, we drive it for layout-faithful results. This covers PPT/PPTX → PDF/JPG/PNG, DOCX/XLSX → PDF and PDF → DOCX. Nothing is bundled, and those slices only appear when the app is found.

**M12. Polish, release and showcase (was M8)**
- Signed build (if a certificate is chosen), winget, screen reader support, benchmarks, README.
- **(spec)** Settings window: keys per wheel, enabled actions, output naming, theme override, and the terminal to open (Windows Terminal, PowerShell, cmd, or a custom command; Tangerine offers Terminal or iTerm2).
- **(spec)** A rename-pattern file action, real toasts with "Open folder", and a portable zip. (Start at login and the Inno Setup installer moved to M2.)

### Parity map: interaction model

| # | Tangerine (spec) | ShiftDrop | Milestone |
|---|---|---|---|
| 1, 9 | Drag a local file or selection, from Finder, the desktop or any app | Explorer, the desktop, and any app that drags real files (CF_HDROP) | Done (M0/M1) |
| 2 | Shift → format wheel | Shift → Convert wheel | Done (M1) |
| 3 | Command → tools wheel | Ctrl → Tools wheel (Ctrl is the Windows counterpart of Command; Win key never used) | M2 |
| 4 | Control → file-actions wheel | Alt → File-actions wheel | M2 |
| 5 | Pick a slice and drop to run it | Same | Done (M1) |
| 6 | Wheel stays open until drop or cancel | Stays open after the key is released; see open question 2 for leaving the wheel | Done (M1) |
| 7 | Enter / arrows / Esc | Esc done; arrows and Enter | M4 |
| 8 | Enter on a file opens the format menu | Not possible in Explorer; hotkey on Explorer's selection, plus a right-click entry | M4 |
| 8 | Command/Control switch wheels | Pressing another wheel's key switches wheels mid-drag; turning the mouse wheel also steps through the wheels | Done (wheel redesign) |
| 10 | Batches | Same; mixed drops show only the actions every file supports | Done (M1) |
| 11 | Output saved next to the original | Same, never overwrites, crash-safe, undoable | Done (M1), M2 |
| 12 | File actions work on folders, mixed selections, files with no tools | Same | M2 |

### Parity map: conversions

| Category | Tangerine formats | ShiftDrop engine | Milestone |
|---|---|---|---|
| Images | JPG, PNG, TIFF, BMP | WIC | Done (M1) |
| | WebP | libwebp | M2 |
| | AVIF | libavif + aom/dav1d | M5 (not done yet) |
| | HEIC | Windows' HEIF codec when installed; see exclusions | M5 (not done yet) |
| | SVG (input only) | lunasvg | M5 (not done yet) |
| | PDF export | our PDF writer | Done (M5) |
| Audio | MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA | LGPL ffmpeg (LAME, native AAC, Vorbis, Opus) | M3 |
| Video | MP4, MOV, MKV, WebM, AVI, WMV, GIF, MP3 export | LGPL ffmpeg. H.264/HEVC via the GPU encoder or Windows' Media Foundation encoder; WebM via libvpx/AV1 | M3 |
| PDF | JPG, PNG (all pages, 300 DPI) | Windows.Data.Pdf | Done (M5) |
| | TXT, MD | text layer (QPDF content-stream parser), Windows OCR fallback | Done (M5) |
| | EPUB, DOCX | reflowed text and images (our writer); faithful DOCX via Word if installed | M11 |
| | PPTX | one page image per slide (our writer) | M11 |
| | XLSX | **Deferred** (see exclusions) | — |
| | PAGES | **Excluded** (see exclusions) | — |
| Documents | DOCX, MD, TXT, EPUB; HTML export | our converters (miniz, pugixml, md4c) | M11 |
| | RTF input | Windows RichEdit | M11 |
| | RTFD input | read as the folder it is (its RTF and pictures) | M11 |
| | Text clippings input | **Excluded** (macOS-only) | — |
| | PAGES | **Excluded** for output; input best-effort from its embedded preview PDF | M11 (input) |
| Presentations | PPT/PPTX → PDF, JPG, PNG | Office bridge only (PowerPoint or LibreOffice installed) | M11 |
| | PDF → PPTX | one page image per slide | M11 |
| Spreadsheets | XLSX, ODS, CSV, TSV; PDF export; XLS input | libxlsxwriter, pugixml, libxls; plain-table PDF; Office bridge for faithful PDF | M11 |
| Subtitles | SRT, VTT; TXT export | our code | M11 |
| Archives | ZIP, TAR, GZIP; RAR input | libarchive (reads RAR/RAR5 under BSD, no unRAR licence) | M7 |

"275 conversion paths" is a count of input × output pairs, not a list we can check item by item; the
table above covers every format the spec names.

### Parity map: the 22 named tools

| # | Tool | Milestone | Notes |
|---|---|---|---|
| 1 | Compress | M3 (images, media), M5 (PDF, done) | Target size, percentage, preset, resize |
| 2 | Edit Metadata | M4 (inspect, search), M6 (edit, remove) | Grouped by file / track / chapter |
| 3 | Edit Photos | M8 | Dehaze, clarity, grain and noise reduction are classic filters, no models |
| 4 | Annotate Photos | M8 | |
| 5 | Add a Background | M8 | |
| 6 | Crop Images | M6 | |
| 7 | Redact Photos | M6 | Faces: Windows.Media.FaceAnalysis; text: Windows OCR |
| 8 | Create a Collage | M8 | |
| 9 | Trim Video | M9 | |
| 10 | Crop Video | M9 | |
| 11 | Change Video Speed | M9 | |
| 12 | Join Videos | M9 | |
| 13 | Save Video Frames | M9 | |
| 14 | Split Video | M9 | |
| 15 | Redact Video | M9 | Manual regions and time ranges (no tracking model) |
| 16 | Normalize Volume | M10 | |
| 17 | Audio Visualizer | M10 | |
| 18 | Trim Audio | M10 | |
| 19 | Convert Audio Channels | M10 | |
| 20 | Bleep Audio | M10 | |
| 21 | Merge PDFs | Done (M5) | |
| 22 | Organize PDF Pages | Done (M5) | |

Tangerine advertises 26 tools but names 22. The other four are not planned or guessed at (open
question 1).

### Parity map: file actions

| # | Tangerine | ShiftDrop | Milestone |
|---|---|---|---|
| 1 | AirDrop | **Share** via the Windows share sheet (Nearby Share, Phone Link, apps). AirDrop itself doesn't exist on Windows | M5 |
| 2 | Float (shelf, pin to an edge, drag out) | Same | M3 |
| 3 | Copy | Copy to the clipboard, paste in Explorer | M2 |
| 4 | Move (pick a folder, move the originals) | Same (folder picker, then an undoable move) | M2 |
| 5 | Copy Path (one per line) | Same | M2 |
| 6 | Terminal (Terminal or iTerm2) | Windows Terminal, falling back to PowerShell; choice in Settings | M2 (choice in M12) |
| — | (ours) Duplicate, rename pattern | | M2, M12 |

### Platform, pricing, privacy

| Tangerine | ShiftDrop |
|---|---|
| Local processing, no uploads | Same, plus no network access at all: CI import check from M2, sandbox proof in M7 |
| Outputs are new copies | Same |
| macOS 13+, Apple silicon, App Store | Windows 10 22H2+ x64 (Windows 11 features when present); winget + installer + portable zip (M12) |
| $29.99, 20-file trial; file actions don't count | Not applicable: no trial, no licence checks, no allowance |

### Excluded, deferred or different on Windows

| Item | What we do | Why |
|---|---|---|
| PAGES output | **Excluded** | Apple's private iWork format: undocumented, no library to write it. Input is best effort from the preview PDF most .pages files contain (M11) |
| PDF → XLSX | **Deferred** | No offline library extracts tables reliably; a wrong table is worse than none. Revisit after M11 |
| PPT/PPTX rendering | Only with PowerPoint or LibreOffice installed | Rendering slides needs a full layout engine; we use the user's own Office instead of bundling one |
| Text clippings input | **Excluded** | macOS-only format, never found on Windows |
| HEIC output | Only when Windows' HEIF and HEVC extensions are installed | The open HEVC encoder (x265) is GPL. HEIC input also works through libheif + libde265 (LGPL, shipped as a separate DLL) |
| H.264 / HEVC video | GPU encoder or Windows' Media Foundation encoder | x264/x265 are GPL and can't go in the LGPL build |
| RAR | Extract only | Creating RAR files needs RARLAB's tools |
| AirDrop | Windows share sheet | AirDrop doesn't exist on Windows |
| Enter on a file in Finder | Hotkey on Explorer's selection, plus a right-click entry | Explorer uses Enter to open files and we can't take it over |
| Terminal or iTerm2 | Windows Terminal, PowerShell, cmd or a custom command | Windows equivalents |
| Face detection (Redact Photos) | Windows.Media.FaceAnalysis | Built into Windows; nothing bundled or downloaded. Coordinator default, can be overruled |

### Original-spec coverage

| Original spec item | Status |
|---|---|
| Shift during a drag shows a wheel at the cursor; drop on a slice; Esc or release elsewhere cancels | Done (M0/M1) |
| Convert: PNG / JPG / BMP / TIFF | Done (M1); WebP in M2; AVIF / HEIC in M5 |
| Convert: MP4 / MOV / WEBM / GIF, MP3 / WAV / M4A / FLAC | M3 |
| Convert: PDF ↔ images | M5 |
| Ctrl = Edit: resize, rotate | M2 |
| Edit: compress | M3 (Fit to…) and M6 (quality presets) |
| Edit: trim | Done (M9 video, M10 audio) |
| Edit: strip metadata | M2 (outputs carry none), verified scrub in M6 |
| Edit: remove background | **Dropped**: needs an ML model, excluded by your no-models rule |
| Alt = File actions: copy path, duplicate | M2 (plus Copy, Move, Terminal) |
| File actions: zip / unzip | M7 |
| File actions: rename pattern | M12 |
| Configurable modifier keys, never Win | INI today (M0); per-wheel keys in M2; settings UI in M12 |
| Multiple files; mixed types show only common actions | Done (M1) |
| Output next to the source, "name (converted).ext", never overwrite | Done (M1); crash-safe commit in M2 |
| Notification on completion that opens the file location | Done (M1, balloon); real toast in M12 |
| Tray app, single instance | Done (M0) |
| Start at login (can be turned off) | M2 (tray toggle; settings window in M12) |
| Settings window | M12 |
| Light / dark theme | Done for the wheel (M1); settings window follows it in M12 |
| Acrylic / Mica glass look | Translucent fill now; real backdrop spike in M4 |
| No network calls, no telemetry | True today; enforced in CI from M2, proven by the sandbox in M7 |
| Background job queue with cancellation and progress | Done (M1, worker pool); progress within a file and a cancel button in the top panel in M3a |
| Logs in %LOCALAPPDATA%\ShiftDrop\logs | Done (M0) |
| Per-monitor DPI, Explorer move/copy semantics, elevated windows, browsers, hook timeouts | Handled or documented in Stage 4 (M0/M1) |
| Packaging: MSIX or Inno Setup + portable zip | M12 (Inno Setup + portable zip; MSIX later) |

## Decisions log

| Date | Decision | Why |
|---|---|---|
| 2026-10-20 | C++20, raw Win32 + COM, MSVC + CMake + vcpkg (manifest), static CRT, PMv2, RAII via wil/ComPtr | Prakhar's call; best fit for hooks/OLE/latency |
| 2026-10-20 | Probe overlay: invisible hit-testable disc first, real wheel only after `DragEnter` with `CF_HDROP` | Windows has no global drag-data API; avoids false menus on text selection or window moves |
| 2026-10-20 | Always `DROPEFFECT_COPY` (LINK fallback), never MOVE | Explorer would delete the original after a Shift (move) drop |
| 2026-10-20 | Wheel is local to the cursor; drops outside it go to the window underneath | Keeps Explorer's normal Shift-drag-to-move working when you drag away |
| 2026-10-20 | Centre of the wheel = cancel (in M1); in the M0 build the whole circle accepts | Matches "release elsewhere cancels"; M0 needs a drop target to test |
| 2026-10-20 | Translucent themed fill instead of system acrylic | DWM backdrops don't apply to `UpdateLayeredWindow` windows |
| 2026-10-20 | Balloon notifications (`Shell_NotifyIcon NIF_INFO`) for M0/M1; real toasts later | No AUMID/packaging needed yet |
| 2026-10-20 | Output `name (converted).ext`, then `(converted 2)`...; created with `STGM_FAILIFTHERE` | Never overwrites, even under a race |
| 2026-10-20 | WIC decoders discovered at runtime; slices disabled when a file can't be decoded | HEIC/WebP/AVIF depend on optional Store codecs |
| 2026-10-20 | Product name in one place (`src/Brand.h`, plus `.rc` VERSIONINFO and CMake target) | Name may change after the research thread checks it |
| 2026-10-20 | Settings in `%LOCALAPPDATA%\ShiftDrop\settings.ini` (INI via `GetPrivateProfileString`) | No JSON dependency for one key; can move to JSON when settings grow |
| 2026-10-20 | Roadmap after M1 = approved research plan (S1–S6, M2–M8); original-spec gaps folded in (see coverage table) | Prakhar approved the plan and asked for everything approved to be built |
| 2026-10-20 | No ML models we bundle or download, no API keys, anywhere (plugins included); Windows' built-in OCR allowed | Prakhar's rule; drops background removal |
| 2026-10-20 | GitHub repo CoPrime69/ShiftDrop is the source of truth; one PR per milestone, stacked if the previous one isn't merged | Prakhar created the repo |
| 2026-10-20 | Defaults you can overrule: keep the name ShiftDrop (rename is a one-line change in Brand.h); LGPL ffmpeg build; no code signing yet; minimum Windows 10 22H2 (Windows 11 features used when present); broker exe + small sidecar folder for workers and ffmpeg; wasmtime for plugins | Research doc recommendations / open questions, decided so work can continue |
| 2026-10-20 | M2 outputs never carry source metadata (pixels are re-encoded into a fresh container); verified scrub and opt-in carry-over come in M6 | Same behaviour as M1; keeps M2 focused on the pipeline |
| 2026-10-20 | WebP decode and encode through libwebp (BSD-3), not WIC | Stock Windows has no WebP encoder; consistent results on every PC |
| 2026-10-20 | Prakhar's detailed Tangerine spec is the parity target: every named format, the 22 named tools and the 6 file actions are mapped to milestones; the 4 unnamed tools are not guessed | Prakhar asked for everything the Mac app does; the spec itself says not to invent the missing four |
| 2026-10-20 | Roadmap is now M2–M12: new M8 photo tools, M9 video tools, M10 audio tools, M11 documents/spreadsheets/subtitles; polish and release move from M8 to M12 | The tools need shared editor windows that don't fit the signature milestones |
| 2026-10-20 | Shift = Convert, Ctrl = Tools (Tangerine's Command), Alt = File actions (Tangerine's Control) | Ctrl is the Windows counterpart of Command; the Win key is reserved by Windows. Coordinator default, can be overruled |
| 2026-10-20 | Face detection for Redact Photos uses Windows.Media.FaceAnalysis | Built into Windows, nothing bundled or downloaded, so it fits the no-models rule the same way Windows OCR does. Coordinator default, can be overruled |
| 2026-10-20 | Formats with no shippable offline engine are excluded or deferred (PAGES output, PDF → XLSX, text clippings, RAR creation); PPT/PPTX rendering and faithful Office output only through Office or LibreOffice already installed | Nothing GPL, proprietary or huge gets bundled; see "Excluded, deferred or different on Windows" |
| 2026-10-20 | H.264/HEVC encoding through GPU encoders or Windows' Media Foundation encoder, not x264/x265 | x264/x265 are GPL; the ffmpeg build stays LGPL |
| 2026-10-20 | Archives through libarchive (BSD-2), including RAR/RAR5 extraction | Permissive licence, reads RAR without the unRAR licence |
| 2026-10-20 | Document → PDF through Windows' built-in "Microsoft Print to PDF" with our own DirectWrite layout | Real vector PDFs with embedded fonts and no bundled layout engine. WebView2 rejected because it can make network calls |
| 2026-10-20 | Alt wheel: Move (folder picker, undoable) replaces Cut | Matches Tangerine's Move; Cut was a weaker stand-in |
| 2026-10-20 | Start with Windows lands in M2: an HKCU Run value, on at first run, tray toggle; the choice is kept in settings.ini so "off" stays off, and Windows' own Startup-apps switch is left alone | Prakhar wants it in the tray at startup; HKCU needs no admin |
| 2026-10-20 | Move to… and Undo use IFileOperation on a dedicated STA thread; the move never overwrites (shell renames on collision) and its undo moves items back, refused if the original place is taken | IFileOperation is STA-only; moving across drives and moving folders need the shell |
| 2026-10-20 | The Ctrl wheel is called Tools (recipe ring id "tools") | Matches the Mac app's tools wheel |
| 2026-10-20 | Every CI build produces a per-user Inno Setup installer (no admin, no Visual Studio, no VC++ redistributable thanks to the static CRT); CI installs, upgrades and uninstalls it on a clean runner; a `v*` tag publishes it as a GitHub Release. Unsigned for now, so SmartScreen asks once | Prakhar: anyone must be able to install and run it; moved forward from M12 |
| 2026-10-20 | Wheel redesign: separate rounded tiles with icons, the hovered tile lifts in the Windows accent colour, the centre shows the count and the hovered slice's detail, spring open and a rotate-crossfade when switching wheels | Prakhar found the M2 wheel shabby; Open-Tangerine is a visual reference only (GPL-3.0, nothing copied) |
| 2026-10-20 | Switching wheels mid-drag moves forward from M4: press another wheel's key, or scroll (the mouse hook swallows wheel events only while the wheel is open) | Prakhar: people should not have to remember the keys |
| 2026-10-20 | A top panel hangs from the top centre of the cursor's monitor: wheel tabs while a wheel is open, then the job's progress and result (success tucks away after 2 s, failure stays until clicked; click shows the files). Replaces the tray balloon for jobs started from the wheel | Windows has no notch; same role as Tangerine's notch panel |
| 2026-10-20 | Tray menu Size (Auto, Small, Medium, Large) and Style (Solid, Glass). Auto scales with the monitor's height (1080p = 1.0, clamped 0.9–1.3) on top of its DPI | Prakhar: it must fit 1080p and 2K screens, with a manual choice. Glass is a blurred screen snapshot, since true acrylic is not available to layered windows |
| 2026-10-20 | Over the wheel: Explorer's drag picture stays, without the "+ Copy" badge (drop description DROPIMAGE_NOIMAGE); the centre answers COPY | Prakhar: the picture shows what is dragged, but "Copy" is misleading. A "not allowed" cursor the moment the wheel opens looked broken. Releasing on the centre still cancels |
| 2026-10-20 | Explorer's drag picture is never redrawn: its full name always shows | Prakhar tried a one-line redraw (IDragSourceHelper::InitializeFromBitmap); the picture vanished on his PC, and he chose Explorer's full name with no setting |
| 2026-10-20 | Tray "Hide cursor over the wheel" (default on): AttachThreadInput to the drag source's thread + ShowCursor(FALSE), undone on leave/drop/close | The pointer belongs to the drag source during a drag; sharing its input state is the only way to hide it. Windows keeps each thread's part of the display count, so detaching (or our thread ending) always gives the pointer back |
| 2026-10-20 | Shift opens the best wheel for the files: if nothing on Convert applies (a PDF today), the first wheel that has something opens instead. Scrolling skips wheels with nothing for the files; Ctrl and Alt still open their own wheel | Prakhar: fewer scrolls, the right options at once. As PDF (M5) and media (M3) conversions land, Shift opens them for those files |
| 2026-10-20 | Leaving the wheel: it fades with distance past the tiles, then shrinks and fades out over 200 ms | Prakhar: the exit felt abrupt |
| 2026-10-20 | M3 ships as three PRs: M3a media conversions + progress + cancel, M3b Fit to… / Compress / carry / clipboard, M3c Float shelf | Small milestones, each testable by hand |
| 2026-10-20 | Media engine = BtbN's win64 **LGPL** FFmpeg build (n8.1.3), shipped unmodified as `engines\ffmpeg.exe`, pinned by URL + SHA-256 in `scripts/get-ffmpeg.ps1`; licence and source pointer installed beside it. ffprobe is not shipped (the duration comes from ffmpeg's own log) | LGPL keeps ShiftDrop's own licence free; no x264/x265 (GPL). One exe instead of two saves ~130 MB unpacked |
| 2026-10-20 | ffmpeg runs as a child process: no console, below-normal priority, only its pipes and NUL inherited, in a kill-on-close job object (cancel, exit or crash never leaves it running), and `-protocol_whitelist file` so it can only read local files | Offline guarantee and clean cancel; a separate process is also the first step toward S6's sealed engines |
| 2026-10-20 | H.264 encoder chosen once per run of the app by test-encoding a few black frames: NVIDIA NVENC, Intel Quick Sync, AMD AMF, Windows Media Foundation, then OpenH264 (always works) | GPU encoders are fast and leave the CPU free, but each needs its vendor's GPU and driver |
| 2026-10-20 | Video and audio run one file at a time on their own worker; images keep the parallel pool | ffmpeg already uses every core; two at once would only fight |
| 2026-10-20 | MKV copies the streams (no re-encode, instant, lossless) and falls back to H.264/AAC if a stream doesn't fit Matroska. Odd video sizes lose one pixel (crop) for 4:2:0 encoders. GIF: 12 fps, up to 480 px wide, palette made for the clip | Fast where possible, never fails on odd phone-video sizes, GIFs that look good and stay small |
| 2026-10-20 | Each wheel shows only the recipes meant for the dragged files: video formats for videos, audio formats for audio, image formats for images (MP3 for both video and audio); a mix gets what fits all of them | Prakhar: the first wheel should already be the right one for the file type |
| 2026-10-20 | The top panel's progress moves within a file for video and audio (percentage for a single file) and has a × that cancels the batch: the file in progress is abandoned and its staging file deleted, finished files stay | Long jobs need a way out; nothing half-written is ever left next to the originals |
| 2026-10-20 | The installer grows from about 2.5 MB to about 45 MB because of ffmpeg.exe | Video and audio need it; a download-on-first-use engine would be a network call, which the offline rule forbids |
| 2026-10-20 | Convert copies the streams a container can hold (H.264/HEVC + AAC/MP3 into MP4/MOV, anything into MKV, ...) instead of re-encoding; it probes the file first and falls back to a re-encode if the copy fails. Re-encodes decode on the GPU (`-hwaccel auto`) and use faster encoder presets | Prakhar: fast conversion. A phone video to MP4 takes a second instead of a minute, with no quality loss |
| 2026-10-20 | Compress and Fit to size (tool 1): images search the JPEG/WebP quality (binary search, 40–92) for the best fit and shrink the picture only when even 40 is too big; media plans video/audio bitrates and height from the duration (0.08 bits per pixel per frame ladder) and retries up to 3 times. Neither ever keeps a result over the target, and a no-target preset must come out smaller than the source | A target size is a promise: "under 10 MB" must be under 10 MB. Presets: images Compress, Under 1 MB; video and audio Compress (up to 1080p), 50% size, Under 10 MB (chat apps), Under 25 MB (email) |
| 2026-10-20 | The H.264 encoder test runs on the media thread right after start | The first video conversion no longer waits about a second for it |
| 2026-10-20 | Features are built as modules (docs/modules.md): one `FeatureModule` per area in `src/<area>/`, plugged in through `src/Modules.cpp`, with an `IEngine` that runs its recipes and `StagedOutput` for every output | Lets several milestones be built at the same time without touching each other, and keeps every output crash-safe and undoable |
| 2026-10-20 | Float is an instant file action (`file.float`, a FeatureModule in `src/shelf`) handled on the UI thread through a small `SetInstantFileAction` hook in FileActions, not an engine | It writes nothing and runs no job; the shelf window must live on the UI thread anyway |
| 2026-10-20 | The Float shelf is a layered Direct2D window like the wheel and the top panel: newest file on top, thumbnails from IShellItemImageFactory on its own STA worker, at most five rows visible (the rest scroll), a "drag all / drag selected" handle at the bottom | One look across the app; thumbnails never block the UI thread |
| 2026-10-20 | Dropped within 32 DIP of a work-area edge, the shelf pins flush to it (side edges win a corner tie) and folds into a 22 x 84 DIP tab when the mouse leaves; it peeks open for 1.8 s when files arrive. Away from the edges it floats and never folds | A pinned shelf should take no room until it is wanted; a floating one was put there on purpose |
| 2026-10-20 | Dragging out of the shelf or the top panel offers copy or link only, never move; the shelf keeps its files after a drag out | Same rule as the wheel: no target can take the original away. Files leave the shelf only by ×, Clear, or when they disappear from disk |
| 2026-10-20 | The shelf follows the light/dark app theme (the top panel stays the dark island); Glass makes it translucent without a blur | The shelf is a window you work in and stays on screen; a blurred snapshot would go stale as it moves |
| 2026-10-20 | Shelf state (files, edge, position) in `%LOCALAPPDATA%\ShiftDrop\shelf.json`, rewritten through a temp file + rename; files are checked every 4 s while it shows and dropped only on "not found" (an offline network drive keeps its items) | Survives restarts and crashes; unplugged drives don't wipe the shelf |
| 2026-10-20 | Carry from the top panel: a result with outputs can be dragged straight into any app, and has a pin (Float) and a copy button; it stays while hovered and for 5 s instead of 2 | Convert-and-carry without a separate chip window; the buttons make it discoverable |
| 2026-10-20 | Drop to clipboard is a Copy button (and Ctrl+C on the shelf), not a global Ctrl+C while the result shows | The panel never has the keyboard; grabbing Ctrl+C from every app for a few seconds would eat real copies |
| 2026-10-20 | Copy puts CF_HDROP + "Preferred DropEffect: copy" on the clipboard at once; for a single image the picture follows a moment later as "PNG" (keeps transparency) and CF_DIB (flattened on white), only if nothing else was copied meanwhile | Paste in Explorer gets the file, paste in a chat or document gets the picture; decoding big photos stays off the UI thread |
| 2026-10-20 | M5: PDF text comes from our own reader of the pages' text operators, on QPDF's content-stream parser: ToUnicode maps, WinAnsi / MacRoman / Standard encodings with /Differences, /Widths and /W. A page with fewer than 8 visible characters counts as a scan and goes to Windows OCR | vcpkg has no PDFium port (PDFium builds with Chromium's own toolchain), and Windows.Data.Pdf has no text API. QPDF is already here for merge and organize; ordinary PDFs read well and OCR covers the rest |
| 2026-10-20 | M5: Searchable PDF = an invisible text layer (render mode 3) over the page picture, in a glyphless Type0 font (Identity-H, codes are UTF-16 units, identity ToUnicode, every glyph 500/1000 em, stretched to each word's box with Tz) | Any language Windows OCR reads survives copy and paste; nothing to embed, so the file barely grows |
| 2026-10-20 | M5: Images → PDF makes one page per image at the image's own DPI (96 when it has none). JPEGs pass through unchanged (DCTDecode; EXIF orientation by the page's drawing matrix); others are lossless Flate with a PNG predictor and a soft mask for transparency | No quality loss, small files, photos the right way up |
| 2026-10-20 | M5: Compress PDF re-encodes 8-bit Gray/RGB pictures as JPEG at the preset's quality, scaled down to the preset's DPI where they are stored larger than they are drawn; masks, alpha and unusual colour spaces are left alone; a picture is replaced only when the new one is at least 10% smaller; object streams on. A result that isn't smaller is skipped | Same "never bigger" rule as image and media Compress; nothing that could change how a page looks is touched |
| 2026-10-20 | M5: PDF → images makes one file per page; more than one page goes into a folder "name (pages)" holding "name page 01.png" and so on | One drop leaves one thing next to the original; pages sort in order |
| 2026-10-20 | M5: Share opens the Windows share sheet from the engine's editor step (UI thread), owned by a hidden 1 × 1 window; only a failure runs a batch, to show why in the top panel | The share sheet must come from a window on the UI thread; no new core hook was needed |
| 2026-10-20 | M5: Merge PDFs and Organize PDF Pages ask first, in small windows of their own (Direct2D, light/dark, keyboard: Alt+arrows move, R / Shift+R rotate, Ctrl+D duplicates, Delete, Ctrl+Z, Enter, Esc). Organize keeps the first PDF's outline and metadata | The same editor step the video tools use; the page plan travels in the recipe's params |
| 2026-10-20 | M5: the WinRT code (Windows.Data.Pdf, Windows.Media.Ocr, share sheet) and the editor windows are object libraries that plug into the core through small hooks; the core and its tests build without them | Unit tests run anywhere; tests that need Windows' renderer or OCR skip themselves where they are missing |
| 2026-10-20 | M5: an engine can give the top panel its own line for a single result ("Copied 214 words", "2.1 MB to 840 KB (61% smaller)"); a result that is not a file (Copy text) doesn't say "Saved next to the originals" | Copy text writes nothing to disk; for Compress the size saved is what people want to see |
| 2026-10-20 | All eleven video and audio tools that need a choice share one editor window (`src/mediaeditor`): Direct2D at the window's DPI, light/dark and accent aware, keyboard-usable, with a preview or waveform, a zoomable filmstrip timeline, typed timecodes and the tool's options on the right. Engines and the editor's maths are in `shiftdrop_core` (`src/videotools`, `src/audiotools`); the window is in the app and registers itself with `EditorApi` from a static initializer | The tools differ only in their options and overlays; one window keeps them consistent. The tests link the engines without any UI, and a build without the window says so instead of crashing |
| 2026-10-20 | The Tools wheel keeps its eight-slice limit: a video gets Compress, 50% size, Under 10 MB, Under 25 MB, Trim…, Crop…, Join…, Split…; a song gets the four Compress slices, Normalize, Trim…, Channels…, Bleep…. The editor has a tool bar (tabs, Ctrl+1…Ctrl+7) with every tool of its kind, so Speed, Frames, Redact and Visualizer are one click away, and confirming runs the tool chosen last under its own recipe (name, suffix, History) | Seven video tools and four Compress presets do not fit eight slices, and smaller labels stop being readable. A recipe file that reuses a slice's id swaps that slice for another tool or preset |
| 2026-10-20 | Per-file choices (a trim range, crop box, redaction areas, frame marks) travel as `params.perFile[path]` and are merged by `ParamsForFile` before each file runs; the whole drop stays one batch with one History run | The pipeline, history and undo stay unchanged: one recipe per drop |
| 2026-10-20 | Preview frames, filmstrip thumbnails, waveforms and playback sound come from the bundled ffmpeg (separate processes on the editor's own threads), not Media Foundation; quick frames seek to the keyframe before the time and use `-fps_mode passthrough` | One decoder for every format the tools accept (MKV, WebM, AVI included) and the same frames the render will use. Without passthrough the keyframe before the time came out with a negative timestamp and was dropped |
| 2026-10-20 | Playback is XAudio2 2.9, loaded at run time from System32 (`LoadLibraryExW` + `XAudio2Create`), fed 48 kHz stereo chunks decoded ahead by ffmpeg with the tool's filter (speed, channels, bleeps), so A/B compare plays the real result; with no sound device a timer clock moves the playhead | No new import in the exe, and no audio device is needed to edit |
| 2026-10-20 | Redact Video blurs with `gblur` and covers with `drawbox`, each area enabled only for its own time range; the bundled build has no `boxblur` | `boxblur` is GPL and not in our LGPL ffmpeg |
| 2026-10-20 | Normalize Volume is two-pass EBU R128 `loudnorm` (linear) at −16 LUFS and −1 dBTP; within 0.5 LU it is skipped as "already at…"; the top panel shows the loudness before and after (`ItemResult::message`, from M5). Trim Audio's silence option measures with `silencedetect` (below −50 dB for 0.1 s) and then cuts exactly | Two passes hit the target within a fraction of a LU; one-pass loudnorm drifts. A measured cut is exact where `silenceremove` would also cut pauses inside |
| 2026-10-20 | Core changes for M9/M10, kept small: `FfmpegEngine::Run` takes the expected output length for progress, and Media exposes `H264QualityArgs` | A trim, a speed change or a join is not as long as its input; the tools reuse the H.264 encoder choice instead of their own |

## Open questions

1. Tangerine advertises 26 tools but names 22. If you can see the four others in the Mac app, send their names and I'll slot them in.
2. Should the wheel keep showing after the cursor leaves it (Tangerine-like "until drop or cancel") or hide (current)? Hiding keeps Explorer's normal drop available.
3. Near a screen edge: keep the wheel centred (current, partly off-screen) or shift it inward?
4. Does the LL hook keep receiving events while an elevated window is under the cursor? Do drags from an elevated app reach us? (Test in TESTING.md, section E.)
5. Alt as a trigger: suppress the menu-bar activation it causes in the source app, or recommend against Alt?
6. Virtual-file drags (browsers, Outlook): where should outputs go? Proposal: Downloads.
7. Clicking a balloon that has moved to Action Center: does it reach us unpackaged? If not, real toasts move earlier.
8. Multi-page TIFF / animated GIF: M1 converts the first frame only. Convert all pages/frames later?
9. Hotkey for "open the wheel on Explorer's selection" (Tangerine's Enter on a file): is Ctrl+Shift+Space a good default?
10. Converting a video to MP4/MOV/MKV now copies the streams when the target can hold them (same size, instant). When it must re-encode (AVI to MP4, say), it uses good quality, so the file can grow; Compress and the size presets are there to shrink. Fine, or should Convert also aim for the original's size?
11. Float shelf: should dragging a file out of the shelf remove it from the shelf (like a tray you empty) or keep it (current)? And should a renamed file follow its new name instead of leaving the shelf?
12. Should the tray menu get a "Show Float shelf" entry (it hides itself when empty, and Float on any file brings it back)?
13. M5: PDF → Markdown finds headings by font size and lists by bullet characters. Tables and multi-column pages come out as plain lines. Good enough, or should columns be detected?
14. M5: Organize PDF Pages with several PDFs dropped shows all their pages in one grid and saves one PDF. Is that wanted, or should it open one window per PDF?
15. M5: Copy text uses the first OCR language Windows has for the user's profile languages. Should the tray menu offer a language choice when several are installed?
16. Tools wheel for video: Trim, Crop, Join and Split have slices, and Speed, Frames and Redact are tabs in the editor. Is that the right four? For a song: Normalize (−16 LUFS), Trim, Channels and Bleep, with the Visualizer as a tab. Or should the wheel get a second page (a "More…" slice) once M8 photo tools also need room?
17. Normalize defaults to −16 LUFS (podcasts, speech). Music for streaming is usually −14 LUFS. Which default, and should the slice open a small chooser instead?
18. Join Videos re-encodes when the clips differ and pads other shapes with black bars, at the first clip's size. Should it offer "crop to fill" too?

## Sources

- [Tangerine: File Converter, Mac App Store](https://apps.apple.com/us/app/tangerine-file-converter/id6813034065?mt=12)
- [tangerineformac.com](https://tangerineformac.com/) and [press kit](https://tangerineformac.com/press)
- [MacGeneration review (FR), Oct 2026](https://www.macg.co/logiciels/2026/10/tangerine-un-convertisseur-de-fichiers-toujours-portee-de-main-sur-mac-311381)
- [Product Hunt: Tangerine](https://www.producthunt.com/products/tangerine-zero-click-file-converter) and [reviews](https://www.producthunt.com/products/tangerine-zero-click-file-converter/reviews)
- [@th2049 launch post on X](https://x.com/th2049/status/2097015553508725184)
- [REA repository](https://github.com/morluto/rea)

Note: the store and developer pages could not be fetched directly from this environment; the facts
above come from search-result excerpts of those pages and should be re-checked against the live pages.
