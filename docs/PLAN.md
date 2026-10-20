# ShiftDrop plan (REA stages 1–5)

Native Windows 11 rebuild of the *behaviour* of Tangerine (macOS, by @th2049). Clean room: we copy
behaviour and UX patterns only, never code, name, icons, artwork or marketing text.

Evidence labels, following REA's rule that every conclusion separates what was seen from what was
guessed: **[O]** observed in a public source, **[I]** inferred by us, **[U]** unknown / unverified.

> Post-M1 roadmap note: a separate thread ("Product research and feature plan") is doing competitor
> research and choosing signature features. Its output under `research/` will reshape the milestones
> after M1. M0 and M1 below are fixed.

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
| 4 | Keyboard choice while the wheel is open: arrow keys pick a slice, Enter applies, Esc cancels | [O] MacG | later (see Stage 4 note) |
| 5 | Dropping anywhere else, or Esc, cancels | [O] MacG; [I] for "elsewhere" | v1 |
| 6 | A second modifier opens **tools** for that file type (compress, crop, strip metadata, reorder PDF pages...) | [O] press kit says Command; App Store says Shift+Option. **[U] which is current** | later (Ctrl in our spec) |
| 7 | A third modifier opens **file actions** (AirDrop, copy path...) | [O] press kit: Control | later (Alt in our spec) |
| 8 | Several files of the same kind: the choice applies to all | [O] site | v1 |
| 9 | Mixed kinds: a notice instead of a wheel | [O] site. We go further: show only actions all files support | v1 |
| 10 | Output is written **next to the original**; nothing is moved or overwritten | [O] App Store, site | v1 |
| 11 | Output naming pattern | [U] not documented. Ours: `name (converted).ext`, then `(converted 2)`... | v1 |
| 12 | Runs fully on-device | [O] site, press kit | v1 |
| 13 | Breadth: ~275 conversion paths, ~26 tools, 6 file actions | [O] site, press kit (an older listing said 188) | grows per milestone |
| 14 | Feedback when done | [U] not documented (likely a notification or Finder reveal) | v1: notification, click reveals the file |
| 15 | Gesture timing: the wheel appears as soon as the key is pressed mid-drag | [O] reviews describe it as instant; [U] exact delay | v1: appears on the next mouse move or within 30 ms |
| 16 | Menu-bar app, settings, launch at login | [I] typical for this kind of app; [U] specifics | later (M-settings) |
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

M0 and M1 are delivered now. After M1, milestones will be re-cut around the signature features from
the research thread; the list below is the baseline to a shippable v1.

**M0. Gesture spike** (delivered)
- [ ] Tray icon with Quit (and Open logs folder)
- [ ] Holding Shift during a file drag shows a placeholder circle under the cursor
- [ ] Dropping on it logs every path and shows a notification listing the files
- [ ] Non-file drags, Esc and releasing elsewhere hide it; Explorer never moves the file

**M1. Real wheel + WIC images** (delivered)
- [ ] 4 slices (PNG, JPG, BMP, TIFF), hover highlight, centre = cancel
- [ ] Slices disabled for non-images and for files already in that format
- [ ] Drop converts on worker threads; output `name (converted).ext` next to the source, never overwrites
- [ ] Notification when the batch finishes; clicking it opens Explorer with the outputs selected
- [ ] EXIF orientation applied; transparency flattened onto white for JPG/BMP

**M2. Gesture hardening + modes**
- Ctrl (Edit) and Alt (File actions) wheels, configurable mapping, Alt menu-bar side effect handled
- Drag thumbnail and cursor polish, edge-of-screen behaviour decided, touch/pen check
- Measured: hook callback time, arm-to-wheel latency
- Done when: all three wheels open reliably from Explorer, Desktop and a file dialog on two monitors with different DPI

**M3. Feedback and job pipeline**
- Progress HUD/tray progress, cancel from the tray, failure details, history of recent outputs
- Real toast notifications (AUMID) with "Open folder" action
- Done when: a 50-file batch shows progress, can be cancelled, and every outcome is in the log

**M4. Audio/video/GIF via ffmpeg (LGPL)**
- MP4/MOV/WEBM/GIF, MP3/WAV/M4A/FLAC; H.264 via Media Foundation encoder; progress + kill on cancel
- Done when: each listed pair converts, cancel leaves no partial file, licence notices shipped

**M5. More images + PDF + archives + file actions**
- WebP/AVIF encode (libwebp BSD, libavif BSD), HEIC decode via WIC if present; PDF ↔ images (PDFium), zip/unzip, rename pattern, copy path, duplicate
- Done when: Alt wheel is complete and PDF→PNG pages / images→PDF work

**M6. Edit wheel**
- Compress (target quality/size), resize presets, rotate, strip metadata, trim (A/V)
- Done when: each edit works on single and multiple files

**M7. Settings, theme, login, packaging**
- Settings window (keys, enabled actions, output naming), start at login (HKCU Run), light/dark, Inno Setup installer + portable zip (+ MSIX later), code signing decision
- Done when: a clean Windows 11 VM installs, runs at login, uninstalls cleanly, with zero network calls (verified with a firewall log)

---

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

## Open questions

1. Tangerine's second modifier: Command (press kit) or Shift+Option (App Store)? Doesn't block us; our mapping is Ctrl/Alt.
2. Should the wheel keep showing after the cursor leaves it (Tangerine-like "until drop or cancel") or hide (current)? Hiding keeps Explorer's normal drop available.
3. Near a screen edge: keep the wheel centred (current, partly off-screen) or shift it inward?
4. Does the LL hook keep receiving events while an elevated window is under the cursor? Do drags from an elevated app reach us? (Test in TESTING.md, section E.)
5. Alt as a trigger: suppress the menu-bar activation it causes in the source app, or recommend against Alt?
6. Virtual-file drags (browsers, Outlook): where should outputs go? Proposal: Downloads.
7. Clicking a balloon that has moved to Action Center: does it reach us unpackaged? If not, real toasts move earlier.
8. Multi-page TIFF / animated GIF: M1 converts the first frame only. Convert all pages/frames later?
9. Keyboard selection (arrows + Enter while the wheel is open): needs a keyboard LL hook and cancelling the source's drag. Worth doing in M2?

## Sources

- [Tangerine: File Converter, Mac App Store](https://apps.apple.com/us/app/tangerine-file-converter/id6813034065?mt=12)
- [tangerineformac.com](https://tangerineformac.com/) and [press kit](https://tangerineformac.com/press)
- [MacGeneration review (FR), Oct 2026](https://www.macg.co/logiciels/2026/10/tangerine-un-convertisseur-de-fichiers-toujours-portee-de-main-sur-mac-311381)
- [Product Hunt: Tangerine](https://www.producthunt.com/products/tangerine-zero-click-file-converter) and [reviews](https://www.producthunt.com/products/tangerine-zero-click-file-converter/reviews)
- [@th2049 launch post on X](https://x.com/th2049/status/2097015553508725184)
- [REA repository](https://github.com/morluto/rea)

Note: the store and developer pages could not be fetched directly from this environment; the facts
above come from search-result excerpts of those pages and should be re-checked against the live pages.
