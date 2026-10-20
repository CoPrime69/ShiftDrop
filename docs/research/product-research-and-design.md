# ShiftDrop: product research, signature features and design

**REA stages covered:** Observe (competitors and user complaints) → Break down behaviour (gaps) → Infer architecture → Map to Windows → Plan. No code in this document. M0/M1 (gesture, overlay, WIC image conversion) remain the foundation and are finished first in the "ShiftDrop plan and M0" thread; everything here starts at M2.

Date: 2026-10-20. Status: **approved by Prakhar on 2026-10-20, with one rule added: no ML models that we bundle or download, and no API keys anywhere in the product. Windows' built-in OCR (Windows.Media.Ocr) is allowed.** Revision 2 applies that rule: AI background removal, face blur, subtitles and upscaling are gone, and so are the ML worker, model handling and the ONNX Runtime/DirectML dependencies. OCR, searchable PDF and rule-based PII redaction stay because they only use Windows' own OCR.

Contents
1. [Competitor research](#1-competitor-research)
2. [Idea list](#2-idea-list)
3. [Scoring and the signature features](#3-scoring-and-the-signature-features)
4. [Positioning and name](#4-positioning-and-name)
5. [Architecture impact](#5-architecture-impact)
6. [Roadmap after M1](#6-roadmap-after-m1)
7. [Interview showcase plan](#7-interview-showcase-plan)
8. [Decisions log](#8-decisions-log)
9. [Open questions](#9-open-questions)
10. [Sources](#10-sources)

---

## 1. Competitor research

A note on evidence: Product Hunt, the App Store and the vendors' own sites blocked direct reading from this environment, so Product Hunt and App Store points come from search-result summaries of those pages, not from reading every comment. GitHub issue lists were read directly. Reddit and X searches returned almost nothing about these tools; most of them launched in 2026 and have very few public reviews. Where a claim is the vendor's own, it says so.

### 1.1 Tool by tool

| Tool | Platform | Trigger | Formats / actions | Offline? | Price |
|---|---|---|---|---|---|
| **Tangerine** (Leap Studio, @th2049) | macOS 13+, Apple silicon | Drag a file, tap Shift (convert), Cmd (file tools), Ctrl (actions); radial wheel stays open until drop or cancel | Vendor: 275 conversions, 26 file tools, 6 file actions | Yes (vendor: files never uploaded) | $29.99 one-time per press kit; App Store listing is free + "Full Unlock" IAP |
| **Arazio** | macOS 13+, Apple silicon | Select in Finder, hold Shift → radial menu of valid targets; Option → tools | Vendor: 283 conversions across 43 formats, 14 tools (compress, crop, trim, split, merge, extract audio) | Yes; vendor says a build test fails if networking code appears in the engines | $14.99 lifetime, sold direct because the App Store sandbox won't write next to the original |
| **TeraConvert** | Free browser version + paid Mac app | Normal app window (no gesture) | Images, video, audio, PDF, docs, OCR, batch; built on FFmpeg, ImageMagick, Ghostscript, Pandoc, LibreOffice, Tesseract | Yes (works with Wi-Fi off) | $19–29 one-time (listings disagree) |
| **DragShift** (llamaboi55, MIT, ~259★) | Windows, macOS, Linux (X11) | Drag, press Shift/Ctrl/Alt → "liquid-glass" radial menu; also right-click and Send To | Images ↔ formats/PDF + compress, resize, crop, annotate, redact, metadata; PDF split/merge/protect; DOCX/TXT/MD → PDF | Not stated | Free |
| **Filee** (KnifeLemon, MIT, ~7.5k★) | Windows 10/11 (full), macOS/Linux preview | Hold Ctrl and drag → donut menu at cursor; also right-click, hotkey, drop zone, CLI, **watch folders** | Claims 244–255 formats incl. Office, HWP, e-books, CAD, fonts, 3D | Converts locally; downloads optional engines and checks updates online | Free |
| **ZestDrop** (MIT, ~23★) | Windows x64 tray app | Drag + Shift (formats), Ctrl+Shift (tools), F8/F9 mid-drag | Image edits incl. OCR, pixelate, GPS remove/set; video trim/speed/compress; audio loudness, beep redaction; PDF redaction; archives | Not stated; Office exports need desktop Office | Free |
| **tangerine-windows** (shakirgv, MIT, 4★) | Windows | Ctrl+Shift drag → radial menu | Images, PDF, docs, video, audio; Windows OCR; "remove background" only for plain backgrounds; 2× upscale | Yes ("no uploads") | Free |
| **File Converter** (Tichau, GPL-3, ~15.4k★) | Windows | Explorer right-click menu | Audio/video/image/PDF/CD via ffmpeg, ImageMagick, Ghostscript; presets; AVIF since v2.2 | Yes | Free, donations |
| **PowerToys** Image Resizer / Peek / Advanced Paste / File Locksmith (Microsoft, MIT) | Windows | Right-click (Resizer, Locksmith); Ctrl+Space (Peek); Win+Shift+V (Advanced Paste) | Resize only; quick preview; paste-as text/markdown/JSON + OCR, optional AI; shows which process locks a file | Mostly; Advanced Paste AI is cloud unless you set up Foundry Local/Ollama | Free |
| Others seen | Satsuma (macOS OSS Tangerine reimplementation), Video-Compressor-for-Discord / ffmpeg4discord (target-size video), DropIt (rule-based file actions), Dropzone (Mac drag shelf) | | | | |

### 1.2 What users complain about or ask for

**Tangerine** (thin evidence: one Product Hunt review, a few App Store reviews)
- The Option/advanced menu is "dense on first use" and needs onboarding (Product Hunt review).
- Requested: batch rename with patterns (date, sequence, find/replace) (Product Hunt review); PDF → EPUB (App Store review); a built-in preview for complex file types (third-party listing).
- Unanswered question: does batch editing keep Finder tags and extended attributes? (Windows analogue: alternate data streams such as Zone.Identifier, and file timestamps.)
- No Windows or Linux version.

**Arazio, TeraConvert:** no public user complaints found; only vendor pages. TeraConvert's revenue listing shows very low traction.

**DragShift:** no open issues. README limitations: unsigned builds trigger SmartScreen and Gatekeeper, the gesture doesn't work on Wayland, PDF → Word needs Microsoft Word, the glass effect needs Screen Recording permission on macOS.

**Filee:** release notes fix a crash after adding the Windows 11 menu entry and "the donut on a drag without files". Unsigned on Windows. Several formats need optional engines that are downloaded.

**tangerine-windows:** its README states the core Windows problem plainly: Windows doesn't expose the dragged file paths during a system-wide drag, so the menu opens first and reads the files only when the drag enters it. It also says background removal isn't AI-based and OCR needs a Windows language pack.

**File Converter (Tichau)**, open issues sorted by 👍:
1. AVIF support (#226; shipped in v2.2 per changelog)
2. Windows 11 context-menu compatibility (#211), plus "menu disappears" bugs (#653, #554, #798)
3. Dark mode (#240, #771)
4. Opus audio (#111)
5. Conversions getting stuck / ffmpeg freezes (#749, #722, #700, #739, #780)
6. **Use the GPU (#392)**
7. JPEG XL (#427), portable version (#121), PNG→ICO loses transparency (#63)

**PowerToys Image Resizer**, open issues sorted by 👍:
1. **Convert to another format (#2686)** and "do more, File Converter capabilities" (#43724)
2. Better HEIC support (#1934, #45474)
3. **Compress a picture to a target file size, e.g. 1 MB (#8790)**
4. Folders, applied recursively (#7741); AVIF (#13875); WebP (#23230, #36904)
5. **Resize to clipboard (#49791)**
6. Trust bugs: "Remove metadata" leaves GPS coordinates (#46317); EXIF removed when the option is off (#47693); the context-menu entry silently stops working after updates (#45280, #23749)
- Peek works erratically in some folders (#45682). Advanced Paste's AI is cloud by default; local models arrived in 0.96.

**Generic demand outside these tools:** "compress video under 25 MB for Discord" is served by single-purpose tools (Video-Compressor-for-Discord, ffmpeg4discord) and many web uploaders. Discord's free limit itself is reported inconsistently (10, 20 or 25 MB), which argues for editable presets rather than hard-coded numbers.

### 1.3 Gap analysis: what nobody does well on Windows

1. **You can't see the result before you commit.** Every tool is fire-and-forget. None shows output size or a preview while you choose; Tangerine users ask for preview.
2. **No "make it fit" in a gesture.** Target-size compression is one of PowerToys' most-upvoted image asks and a whole category of single-purpose video tools, but none of the drag tools does it for images, video and PDF together.
3. **No chains, no undo.** Presets exist (Tichau) and watch folders exist (Filee), but no tool runs a multi-step recipe from one slice, and none offers history or undo.
4. **Privacy claims aren't verifiable.** PowerToys' "remove metadata" leaked GPS. Arazio's "network code fails the build" is the only attempt at proof, and it's Mac-only. No Windows tool shows a before/after metadata diff or sandboxes its engines.
5. **On-device AI is shallow.** Windows OCR shows up (ZestDrop, tangerine-windows), but background removal is non-AI, and nobody makes searchable PDFs or redacts personal data in one gesture. *(Rev 2: ShiftDrop answers this only with Windows' built-in OCR and rules; anything needing a model of our own is out of scope.)*
6. **Weight and stability.** Every Windows drag tool found is Python/Qt or .NET/WPF/Avalonia, and several bundle a Python runtime (tangerine-windows' build needs ~1 GB). Long ffmpeg jobs hang in Tichau. *Inferred, not measured:* none of them can match a native tray app's idle memory and startup; we'll measure this (section 7).
7. **GPU is unused.** Tichau's #392 is open; no drag tool uses NVENC/QSV/AMF or DirectML.
8. **Not extensible, not accessible.** No plugin model in any of them. Radial menus are mouse-only and none mention keyboard or screen-reader support.
9. **Unsigned builds** everywhere (DragShift, Filee, tangerine-windows), which means SmartScreen warnings on first run.

**Honest Windows vs macOS difference:** on macOS, Tangerine can read the dragged files' types the moment you tap Shift. On Windows, a system-wide OLE drag's data object is only handed to a window when the drag enters it. So ShiftDrop's wheel must appear first as the drop target and specialise its slices once the drag enters it (M0's "Probe" mode already does this). The closest alternative for "select then press a key" (Arazio-style) is reading the Explorer selection through `IShellWindows` → `IFolderView2`, which we use for keyboard mode.

---

## 2. Idea list

38 ideas. IDs are used in the scoring table.

**On-device AI (still offline)** *(Rev 2: A1, A2, A6 and A8 stay because they use only Windows' built-in OCR plus rules. A3, A4, A5 and A7 are excluded because they need an ML model of our own. A9 uses classical image processing, so it stays eligible.)*
- A1 OCR → text on clipboard (Windows.Media.Ocr)
- A2 Searchable PDF from scans/photos (OCR + invisible text layer)
- A3 AI background removal (ONNX Runtime + DirectML)
- A4 AI upscaling 2×/4× (Real-ESRGAN class models)
- A5 Subtitles from audio/video (whisper.cpp → SRT/VTT, optional burn-in)
- A6 PII detection + redaction on images and PDFs (Windows OCR + pattern rules such as email, phone, card numbers with Luhn, IBAN, Aadhaar/PAN)
- A7 Face and licence-plate blur
- A8 Content-based rename (date/invoice number/title from Windows OCR + rules)
- A9 Document photo auto-crop + deskew (classical CV, no model)

**Workflow power**
- W1 Recipes: a chain (resize → WebP → strip EXIF) as one slice
- W2 Fit-to-target: "under 10 MB / 25 MB / custom" for images, video, PDF
- W3 Destination-aware slices (adapt to the app you're dragging toward)
- W4 Convert-and-carry: after the drop, the result follows the cursor as a drag chip you can drop straight into Discord/Outlook/a browser
- W5 Drop to clipboard instead of disk
- W6 Folder and multi-file drops with a batch summary
- W7 Watch folders that apply a recipe
- W8 Batch rename with patterns

**Trust & UX**
- T1 Live preview + predicted output size on hover
- T2 Undo + history timeline
- T3 Verified metadata scrub with a before/after diff
- T4 Keyboard-only mode (hotkey on the Explorer selection; number keys pick slices)
- T5 Screen-reader support (UI Automation provider on the wheel)
- T6 Frecency ordering of slices per file type
- T7 First-run coach marks / cheat sheet (Tangerine's "dense menu" complaint)
- T8 Provably offline: engines in AppContainer sandboxes with no network capability + CI check that the main exe imports no networking DLLs

**Extensibility**
- E1 Sandboxed WASM plugins (wasmtime) with a manifest
- E2 Lua scripting for recipes/slices
- E3 CLI + recipe files for automation
- E4 Windows 11 top-level context-menu entry (IExplorerCommand + sparse package)

**Performance**
- P1 GPU video encoding (NVENC/QSV/AMF through ffmpeg) with automatic fallback
- P2 Zero-copy, tiled WIC pipeline (fused steps, no intermediate files)
- P3 Public benchmark harness against competitors
- P4 Tiny footprint: instant start, near-zero idle memory
- P5 Robust job supervision (Job Objects, kill-tree on cancel, watchdog for stalled ffmpeg)

**Other, from the research**
- O1 Signed builds + winget/MSIX distribution
- O2 Modern formats: AVIF, JPEG XL, HEIC, Opus
- O3 Portable mode (Tichau #121)
- O4 Dark mode / follow system theme (Tichau #240, #771)
- O5 PNG → ICO multi-size with transparency done right (Tichau #63)

---

## 3. Scoring and the signature features

Columns, 1–5: **UV** user value · **Uniq** unique vs section 1 · **Show** interview showcase depth · **Feas** feasibility in C++ for one developer · **Fit** "zero clicks, fully offline". **Σ** max 25.

| ID | Idea | UV | Uniq | Show | Feas | Fit | Σ |
|---|---|---|---|---|---|---|---|
| T1 | Live preview + predicted size on hover | 5 | 5 | 5 | 3 | 5 | **23** |
| W2 | Fit-to-target compression | 5 | 4 | 4 | 4 | 5 | **22** |
| T2 | Undo + history timeline | 4 | 5 | 3 | 5 | 5 | **22** |
| W4 | Convert-and-carry drag chip | 4 | 5 | 4 | 4 | 5 | **22** |
| T8 | Provably offline (AppContainer + CI import check) | 4 | 5 | 5 | 3 | 5 | **22** |
| W1 | Recipes (chains in one slice) | 4 | 4 | 4 | 4 | 5 | 21 |
| P1 | GPU video encoding | 4 | 4 | 4 | 4 | 5 | 21 |
| P4 | Tiny footprint, instant start | 4 | 4 | 4 | 4 | 5 | 21 |
| P5 | Robust job supervision | 4 | 3 | 4 | 5 | 5 | 21 |
| T3 | Verified metadata scrub + diff | 4 | 4 | 3 | 4 | 5 | 20 |
| A2 | Searchable PDF | 4 | 4 | 4 | 3 | 5 | 20 |
| A3 | ~~AI background removal~~ | 5 | 3 | 5 | 3 | 4 | 20 · excluded (needs model) |
| A5 | ~~Subtitles (whisper.cpp)~~ | 4 | 5 | 5 | 3 | 3 | 20 · excluded (needs model) |
| A6 | PII redaction | 4 | 5 | 5 | 2 | 4 | 20 |
| P2 | Zero-copy tiled WIC pipeline | 3 | 3 | 5 | 4 | 5 | 20 |
| P3 | Benchmark harness | 2 | 4 | 5 | 4 | 5 | 20 |
| A1 | OCR → clipboard | 4 | 2 | 3 | 5 | 5 | 19 |
| W5 | Drop to clipboard | 4 | 3 | 2 | 5 | 5 | 19 |
| T5 | Screen-reader support | 3 | 5 | 4 | 3 | 4 | 19 |
| T6 | Frecency slice order | 3 | 4 | 2 | 5 | 5 | 19 |
| E1 | WASM plugins | 3 | 5 | 5 | 2 | 4 | 19 |
| O2 | Modern formats (AVIF/JXL/HEIC/Opus) | 4 | 3 | 3 | 4 | 5 | 19 |
| A7 | ~~Face / plate blur~~ | 3 | 4 | 4 | 3 | 4 | 18 · excluded (needs model) |
| W6 | Folder + batch drops | 4 | 2 | 3 | 4 | 5 | 18 |
| T4 | Keyboard-only mode | 3 | 4 | 3 | 4 | 4 | 18 |
| E2 | Lua scripting | 3 | 4 | 3 | 4 | 4 | 18 |
| O1 | Signed builds + winget | 4 | 4 | 2 | 3 | 5 | 18 |
| A4 | ~~AI upscaling~~ | 3 | 3 | 4 | 3 | 4 | 17 · excluded (needs model) |
| A8 | Content-based rename (OCR + rules) | 3 | 4 | 3 | 3 | 4 | 17 |
| A9 | Auto-crop + deskew | 3 | 3 | 3 | 3 | 5 | 17 |
| W3 | Destination-aware slices | 3 | 5 | 3 | 2 | 4 | 17 |
| W8 | Pattern batch rename | 3 | 3 | 2 | 5 | 4 | 17 |
| E4 | Win11 top-level context menu | 4 | 3 | 4 | 3 | 3 | 17 |
| T7 | First-run coach marks | 3 | 2 | 1 | 5 | 5 | 16 |
| O3 | Portable mode | 3 | 2 | 1 | 5 | 5 | 16 |
| O4 | Dark mode / system theme | 3 | 2 | 1 | 5 | 5 | 16 |
| W7 | Watch folders | 3 | 2 | 3 | 4 | 3 | 15 |
| E3 | CLI + recipe files | 3 | 2 | 2 | 5 | 3 | 15 |

### 3.1 The six signature features (approved, Rev 2)

Together they say one thing: **ShiftDrop shows you the result before you drop, makes it fit where it's going, can always be undone, and can prove nothing left your PC.** Everything runs with no ML models of our own and no API keys; the only recognition engine is the OCR built into Windows.

| # | Signature feature | Built from | Why it made the cut |
|---|---|---|---|
| **S1** | **Know before you drop.** Hovering a slice shows a live thumbnail of the result, the predicted size ("≈1.4 MB, −62%") and what metadata will change | T1 + T3 (diff part) | Highest score. Directly answers gap 1 and Tangerine's preview request. Nobody does it. It's also the hardest engineering in the app: cancellable low-priority work, estimation, caching, all under a frame budget. |
| **S2** | **Fit to…** "Under 10 MB", "Under 25 MB", "Email (≤20 MB)" or custom, for images, video and PDF. The result can be carried straight on to the destination app or copied to the clipboard. GPU encoders used when present | W2 + W4 + W5 + P1 | Top PowerToys ask (#8790) and a whole category of single-purpose web tools. Convert-and-carry makes "drag from Explorer into Discord" a single gesture, and replaces the infeasible destination-aware idea. Rate control and GPU encoder fallback make a great interview story. |
| **S3** | **Recipes with an undo timeline.** Any chain of actions becomes one slice; every run is recorded and can be undone | W1 + T2 (+ P2 fusion) | Forces the right architecture (action/pipeline model), which everything else plugs into. Undo is unique and cheap because we never overwrite. Step fusion (resize → encode without an intermediate file) is the zero-copy performance story. |
| **S4** | **Text from anything (Windows OCR).** OCR to clipboard from an image or PDF page, and a searchable PDF from scans or photos | A1 + A2 | Rev 2 narrows this from "on-device vision": background removal is gone because it needs a model, but Windows.Media.Ocr ships with Windows, so no model files, downloads or keys. Writing our own PDF with an invisible text layer is a good low-level story. |
| **S5** | **Share-safe.** One slice that produces a copy you can safely send: every metadata block removed (EXIF, GPS, XMP, IPTC, maker notes, embedded thumbnails, PDF document info and XMP, the Zone.Identifier stream) and re-read to prove it, plus emails, phone numbers, card numbers (Luhn-checked) and IDs found by Windows OCR + pattern rules and boxed for review before a redacted copy is written | T3 + A6 | PowerToys leaking GPS ([#46317](https://github.com/microsoft/powertoys/issues/46317)) proves the trust gap is real. Builds on S4's OCR. Rev 2 drops face blur (it needed a face model). Very strong demo: drop a screenshot of a bank statement, get a redacted copy. |
| **S6** | **Sealed engines and plugins.** ffmpeg and plugins run in AppContainer sandboxes with no network capability; third parties add slices as WASM plugins; CI fails if the main exe links networking | T8 + E1 + P5 | Turns "offline" from a promise into something you can show. Sandboxing, IPC and a plugin ABI are senior-level talking points. The same worker model gives crash isolation and near-zero idle memory. Plugins may not bundle ML models or call APIs either (rule in the manifest spec). |

**Baseline quality (planned, not signature):** tiny footprint (P4), robust supervision (P5), keyboard mode and screen reader (T4, T5), frecency (T6), modern formats (O2), folder/batch drops (W6), dark mode (O4), signed builds (O1), coach marks (T7). These are spread across milestones rather than headlined.

### 3.2 Strongest rejected ideas, and why

- **Everything that needs an ML model of our own: background removal (A3, 20), subtitles with whisper.cpp (A5, 20), face and plate blur (A7, 18), AI upscaling (A4, 17).** Excluded by Prakhar's rule (Rev 2): the product must run with no bundled or downloaded models and no API keys. Windows' built-in OCR is the one allowed exception.
- **Destination-aware slices (17).** Windows doesn't tell us where a drag is heading; once the wheel is up, the cursor is over our own window. Guessing from the foreground window is unreliable. Replaced by convert-and-carry plus editable presets (S2).
- **Windows 11 top-level context menu (17).** Needs package identity (sparse package + signing) and copies Tichau's model; the gesture is our identity. Keyboard mode covers the "no drag" case.
- **Watch folders and CLI (15).** Filee already has watch folders, and neither is zero-click. Recipes are stored as files, so a CLI is a cheap add-on later.
- **Lua instead of WASM (18).** Simpler, but Lua scripts run with whatever the host exposes and in-process; WASM gives a real memory/CPU sandbox and lets plugin authors use Rust/C/Zig. Lua stays the fallback if wasmtime integration goes badly (section 5.7).

---

## 4. Positioning and name

**One-line pitch:** *Reshape any file mid-drag: see the result before you drop, make it fit where it's going, undo anything, and prove it never left your PC.*

**Why this exists:** Windows already has good converters, but they make you trust them blindly. Tichau's File Converter and PowerToys live in a right-click menu and run fire-and-forget; the new drag-and-drop tools (Filee, DragShift, ZestDrop) copied the Mac gesture on top of Python or .NET runtimes and stop at "convert". None of them show you what you'll get, none can shrink a video to fit Discord in one motion, none can undo, and none can prove they stayed offline (PowerToys' metadata stripper even leaked GPS). ShiftDrop is a native Win32 tool that answers the questions you actually have mid-drag ("how big will it be?", "will it fit?", "is anything private left in it?") before you let go, and runs every engine in a sandbox that can't reach the network.

### Is "ShiftDrop" taken?
- No software product called ShiftDrop turned up. `shiftdrop.com` is a parked premium domain for sale.
- **Weaknesses:** it is very close to **DragShift**, an existing open-source tool doing the same thing (the same two words reversed), and close to **ZestDrop**. It also names the Shift key, which is configurable and not the point of the product.
- **Recommendation:** rename before the repo goes public. Candidates (a quick search found no conflicting software for the first two; the rest were not checked yet):
  1. **Dropwise**: "drop it wisely"; ties to know-before-you-drop.
  2. **DropWheel**: describes the interaction; plain but clear.
  3. **Midair**: everything happens mid-drag. *Not checked yet; likely crowded as a word.*
  4. **Prism**: one file in, many shapes out. *Not checked; almost certainly used elsewhere.*
  5. **Kerf** was considered and **rejected**: already used by woodworking and CAD apps.

The code keeps the name in one place (the M0 thread was told this), so a rename is cheap.

---

## 5. Architecture impact

### 5.1 From `IConverter` to actions and pipelines

Today `IConverter` has `supports(ext)`, `actions()` and `run(job, progress, stop)`, and an `Action` is just id + label + extension. The signature features need more, so the model splits into four concepts (described here as fields, not code):

- **ActionDescriptor**: stable id, label, icon, accepted input kinds (by extension and sniffed content), output kind, a parameter schema (typed: int/float/enum/size/bool with defaults and ranges), cost hints (CPU, GPU, ML, long-running), capability flags (previewable, estimable, fusable, needs-sandbox), and the engine that owns it.
- **Action** (the new interface each engine implements): `accepts(file facts)`, `estimate(file facts, params)` returning a size range and confidence, optional `preview(file facts, params, max pixels)` returning a small bitmap, and `run(context, inputs, params, progress, stop token)` producing artefacts. The current `IConverter::run` maps onto `run`; WicConverter becomes the first Action provider with no behaviour change.
- **Recipe**: a named, ordered list of steps (action id + params), an output naming rule, and where it appears (which modifier ring, which file kinds). Stored as JSON files in `%LOCALAPPDATA%\ShiftDrop\recipes\`. A single action is just a one-step recipe, so the wheel only ever shows recipes.
- **PipelineRunner**: resolves a recipe for the dropped files, plans it (fusing adjacent in-memory image steps so resize → encode passes an `IWICBitmapSource` straight through without writing a file), runs steps on the right queue, writes to a staging file, then commits with an atomic rename to `name (converted).ext` (never overwrite stays). Every commit writes a **HistoryEntry** (inputs with file IDs and hashes, outputs, recipe snapshot, timings, engine versions).

**Undo** is cheap because we never overwrite: undoing a run sends its outputs to the Recycle Bin through `IFileOperation` with `FOFX_RECYCLEONDELETE`, after checking that their hashes still match (so we never bin a file the user edited since). History lives in SQLite.

**Fit-to-target** is an action whose `run` is a solver: for images, a bounded search over encoder quality (and scale if quality alone can't reach it), with the first probe taken from S1's estimate; for video, bitrate = (target bytes × 0.97 safety − audio budget − container overhead) ÷ duration, then a two-pass encode (or the GPU encoder's CBR/VBV mode), re-encoding once with a corrected bitrate if the result overshoots; for PDF, re-rasterising images at decreasing DPI/quality.

### 5.2 Process model and the sandbox boundary

```
shiftdrop.exe  (broker; medium integrity; the only process the user starts)
│
├─ UI thread ── MouseHook (WH_MOUSE_LL) · Overlay (D2D + DComp) · DropTarget (OLE)
│               Tray · History window · UIA provider for the wheel
│
├─ Core ─────── ActionRegistry · RecipeStore · PipelineRunner · Scheduler
│               (queues: preview [lowest priority, cancellable], CPU pool, GPU queue)
│               PreviewService + cache · HistoryStore (SQLite) · OutputCommitter
│               SandboxManager (spawns/kills workers, owns Job Objects)
│
├─ In-process engines (trusted Windows components, no third-party parsers)
│     WicEngine · OcrEngine (Windows.Media.Ocr, built into Windows) · PdfEngine
│     (Windows.Data.Pdf render + our own small PDF writer) · MetadataEngine
│     (WIC metadata query reader/writer + our PDF/ADS scrubbers) · PiiDetector (rules)
│
└─ Sandboxed workers: AppContainer, zero capabilities (so no network, no user files),
   inside a Job Object with KILL_ON_JOB_CLOSE, memory cap and an idle timeout
      sd-media.exe  → supervises ffmpeg.exe / ffprobe.exe children
      sd-plugin.exe → wasmtime host running .wasm plugins

IPC: one anonymous pipe pair per worker, length-prefixed binary messages
     (request id, op, params, progress, result). Bulk pixels go through a
     shared-memory section. Files reach the sandbox only as handles the broker
     opens and duplicates in, or via a staging folder ACL'd to that
     AppContainer's SID; the broker moves the output to its final place.
```

Why this shape:
- **No network, provably:** an AppContainer without the `internetClient` capability can't open sockets at all. CI adds a second proof: a `dumpbin /imports` check fails the build if `shiftdrop.exe` imports `ws2_32`, `winhttp`, `wininet` or similar.
- **Untrusted parsers stay out of the broker.** ffmpeg demuxers and plugins are the attack surface; a crafted file can crash a worker but not the tray app.
- **Idle memory stays tiny:** workers exit after an idle timeout, so the always-on broker holds only the hook, overlay and tray.
- **Cancellation is real:** closing the Job Object kills ffmpeg and its children together (fixes the "stuck ffmpeg" class of bugs seen in Tichau).

Windows-specific caveats (said plainly): AppContainer was built for UWP. **NVENC/QSV/AMF hardware encoders inside an AppContainer are unverified**. If they fail, the fallback for sd-media is a Job Object + restricted, low-integrity token without AppContainer (crash isolation and kill-tree stay; the "no network" proof for that one worker then relies on ffmpeg being built without network protocols, checked in CI with `ffmpeg -protocols`).

### 5.3 Preview generation that stays cheap (S1)

1. The wheel is the OLE drop target, so on `DragEnter` we receive the `IDataObject` and read the paths (this is the earliest moment Windows allows).
2. **File facts** are gathered once per drag on the preview queue: size, type sniffed from header bytes, dimensions/duration (WIC frame size or a cached ffprobe result), metadata summary.
3. When a slice is hovered for more than ~80 ms, the PreviewService schedules `estimate` and `preview` for that slice only. Moving to another slice cancels the previous request through its stop token. Nothing runs on the UI thread.
4. **Images:** decode at reduced size using the decoder's own scaling (`IWICBitmapSourceTransform` lets the JPEG decoder skip DCT work), run the recipe on a ≤ 512 px proxy, encode it, and extrapolate full size from bytes-per-pixel with a per-codec correction factor calibrated by the benchmark corpus. Shown as a range with confidence. If the image is small (say under 4 MP), encode it fully instead and show the exact size.
5. **Video:** size is computed, not guessed (bitrate × duration, exact for Fit-to-target); the thumbnail is one frame grabbed by ffprobe/ffmpeg in sd-media and cached.
6. **Cache** key: volume serial + file ID (`GetFileInformationByHandleEx(FileIdInfo)`) + last-write time + recipe hash + params. LRU in memory, capped in MB.
7. **Budget:** wheel paints instantly with a skeleton; estimate within 150 ms p95 for a 24 MP JPEG; preview bitmaps drawn by Direct2D on the next frame. A request that misses its budget shows "estimating…" rather than blocking.

### 5.4 No models: what replaced them (Rev 2)

- **No model files, no model registry, no ML worker.** Rev 1's sd-ml.exe, ONNX Runtime, DirectML and the "AI pack" are gone, which also removes the model-delivery and licence questions.
- **OCR comes from Windows.** `Windows.Media.Ocr` (through C++/WinRT) runs in-process on a worker thread. It uses the OCR languages installed with Windows; we list them and say clearly when the needed language isn't installed (the user adds it in Windows Settings, not through us). OCR is lazy: the WinRT engine is created on the first OCR job and released after an idle timeout.
- **PII detection is rules, not a model:** regexes plus validators (Luhn for cards, IBAN mod-97, Verhoeff for Aadhaar, PAN format, E.164-ish phones, emails) run over OCR words with their bounding boxes, so a match maps straight to rectangles to redact. Everything is shown for review; we never claim to find everything.
- **Searchable PDF:** pages rasterised (or the source images used directly), OCR words placed as invisible text (PDF text render mode 3) by our own PDF writer.

### 5.5 GPU abstraction (video only after Rev 2)

- **ComputeDevices** (in the broker): enumerates adapters once with `IDXGIFactory6::EnumAdapterByGpuPreference`, records vendor and a stable LUID; the user can pin an adapter.
- **Video:** sd-media probes encoders once per driver version (run a 1-frame encode with `h264_nvenc`, `h264_qsv`, `h264_amf`, `hevc_*`, `av1_*`) and caches the results; Fit-to-target picks the best working one and falls back to software.
- **Preview/overlay** keeps its own D3D11 device for Direct2D/DirectComposition.
- There is no ML inference, so DirectML and Windows ML are no longer needed.

### 5.6 Plugins (S6)

- **Runtime:** wasmtime C API (Apache-2.0 WITH LLVM-exception). **There is no vcpkg port**, so it would come from a pinned, hash-checked prebuilt release through CMake (or an overlay port we maintain). WAMR is the alternative with the same licence and a smaller footprint.
- **Manifest** (`plugin.json` in a folder or `.sdplugin` zip): id, name, version, author, licence, `api_version`, accepted inputs (extensions + MIME), slices (id, label, icon, output extension, params schema), and limits (max memory MB, max fuel/time).
- **ABI:** no WASI filesystem or sockets at all. The host decodes inputs itself and passes data in through a small imported API: get input bytes (or RGBA pixels decoded by WIC), get params, report progress, log, write output bytes. Plugins never see paths. CPU limited by fuel/epoch interruption, memory by a resource limiter, and the whole thing runs in sd-plugin.exe inside an AppContainer anyway (two walls).
- **Signing/trust:** plugins are installed by hand from a folder; the History panel shows which plugin produced which output.

### 5.7 Updated module list and new dependencies

New modules (each its own .h/.cpp, same style as today): `Action.h` (replaces `IConverter.h`), `ActionRegistry`, `Recipe` + `RecipeStore`, `PipelineRunner`, `Scheduler`, `PreviewService`, `SizeEstimator`, `FitToTarget`, `HistoryStore`, `Undo`, `MetadataEngine`, `OcrEngine`, `PdfEngine` + `PdfWriter`, `PiiDetector`, `Redactor`, `ComputeDevices`, `SandboxManager`, `IpcChannel`, `ExplorerSelection` (keyboard mode), `WheelAutomation` (UIA), and two worker executables `sd-media` and `sd-plugin`.

| Dependency | Use | Licence | Source |
|---|---|---|---|
| wil (existing) | RAII for Win32/COM | MIT | vcpkg |
| cppwinrt | Windows.Media.Ocr, Windows.Data.Pdf (both built into Windows) | MIT | vcpkg 3.0.260818.1 |
| sqlite3 | history, undo, preview stats | Public domain ("blessing") | vcpkg 3.53.4 |
| nlohmann-json | recipes and plugin manifests | MIT | vcpkg 3.12.0 |
| libwebp / libavif (+dav1d, aom or svt) / libjxl | encoders when WIC lacks them | BSD-3 / BSD-2 + Apache-2.0 / BSD-3 | vcpkg 1.6.0 / 1.4.2 / 0.12.0 |
| wasmtime C API (or WAMR) | sd-plugin only | Apache-2.0 WITH LLVM-exception | pinned prebuilt release, not in vcpkg |
| ffmpeg.exe / ffprobe.exe | sd-media children, not linked | LGPL-2.1+ build, or GPL if x264/x265 included (open question 3) | bundled binaries |
| gtest, benchmark | tests, micro-benchmarks | BSD-3, Apache-2.0 | vcpkg 1.18.0, 1.9.5 |
| libFuzzer + ASan | fuzzing | built into MSVC (`/fsanitize=fuzzer,address`) | toolchain |

**Single-exe rule:** the broker can stay one static exe. The two workers and ffmpeg can't, so the shipped app becomes "one exe + a small sidecar folder" (no models, no ML runtimes). Open question 2.

### 5.8 Risks and how we'd cut scope

| Risk | Likelihood | Cut / fallback |
|---|---|---|
| Size estimates are too inaccurate to trust | Medium | Show only exact sizes for small images and computed video sizes; show "≈" ranges elsewhere; drop the live thumbnail before dropping the size. |
| Fit-to-target overshoots on odd files | Medium | One corrective re-encode; if still over, report "couldn't fit, closest is 26.1 MB" and keep the file. Never silently exceed. |
| AppContainer blocks GPU encoders or confuses ffmpeg | Medium | Restricted-token + Job Object fallback for sd-media only (section 5.2); keep AppContainer for plugins. |
| wasmtime integration/size too costly | Medium | Switch to WAMR; worst case ship Lua recipes (in sd-plugin, still sandboxed) and keep the manifest format. |
| Windows OCR language missing or weak on a document | Medium | Show which language was used and which are installed; point to Windows Settings; never fail silently. Searchable PDF still saves (text layer may be sparse). |
| PII detection misses things | High | Always show a review step with boxes, and let the user add boxes by dragging; position it as "assisted redaction", never "guaranteed". No model will be added (Rev 2 rule). |
| PDF redaction leaves recoverable text | Medium | Redact by rasterising affected pages and rebuilding with our writer (+ OCR text layer from the redacted image), so no hidden text survives. |
| Scope: six features for one developer | High | Each milestone ships its feature for one file family first (images), then extends. S6 plugins are the first thing to slip; S5 can ship with metadata scrub only and add PII boxes after. |
| LowLevelHooksTimeout: Windows silently removes a slow LL hook | Low (design avoids it) | Hook callback stays allocation-free; a watchdog re-installs the hook if events stop while the mouse moves; measured in CI micro-benchmark. |

---

## 6. Roadmap after M1

M0 (gesture + overlay) and M1 (WIC image conversion) finish first in their own thread. Each milestone below ships one signature feature end to end and is hand-testable.

### M2 — Pipeline core, Recipes and Undo (S3)
Refactor `IConverter` → Action model with no behaviour change, then add recipes, the runner, history and undo. Set up CI and tests here so every later milestone inherits them.
Done when:
- [ ] All M1 conversions still work through the new Action interface (regression checklist from M1 passes).
- [ ] A recipe JSON "Web-ready" (resize ≤ 2048 px → WebP q80 → strip EXIF) appears as one slice and produces one file, with no intermediate file on disk.
- [ ] History window lists the last runs; "Undo" sends the outputs to the Recycle Bin; undo refuses if the output was modified since.
- [ ] Killing the app mid-job leaves no partial `name (converted).ext` (staging + atomic rename).
- [ ] GitHub Actions builds Release x64 with vcpkg caching, runs unit tests, and runs the "no networking imports" check.
- [ ] ADRs 0001–0005 written.

### M3 — Fit to… (S2)
Adds sd-media (Job Object supervision only; AppContainer comes in M7), ffmpeg, GPU encoder probing, convert-and-carry, drop to clipboard.
Done when:
- [ ] "Under 10 MB" on a 200 MB 1080p clip produces ≤ 10 MB on the first try for ≥ 9 of 10 corpus clips, and never above target.
- [ ] Same for images ("under 1 MB" from a 12 MP photo) and PDFs with scanned pages.
- [ ] NVENC/QSV/AMF used when present, logged, with automatic software fallback; time-to-convert recorded for both.
- [ ] After a Fit drop, a chip follows the cursor and can be dropped into a browser upload box or Discord; Esc puts the output on the clipboard instead.
- [ ] Cancel from the tray kills ffmpeg within 500 ms and leaves no files.
- [ ] Presets are editable in settings (Discord's limit changes; we don't hard-code it).

### M4 — Know before you drop (S1)
Done when:
- [ ] Hovering any slice shows predicted size within 150 ms p95 (24 MP JPEG on a mid-range laptop), as a range with "≈" when estimated.
- [ ] Estimate accuracy on the corpus: median error ≤ 10 %, p90 ≤ 25 %, reported by a test.
- [ ] Live thumbnail of the result for image recipes; one-frame thumbnail for video.
- [ ] Metadata diff tooltip ("removes: GPS, camera serial; keeps: orientation").
- [ ] Moving rapidly across slices never makes the wheel stutter (frame time logged; no frame > 16 ms on the UI thread).
- [ ] Keyboard: number keys pick the hovered slice; Esc cancels.

### M5 — Text from anything (S4)
Done when:
- [ ] OCR slice puts text on the clipboard from an image or a PDF page, using the best installed Windows OCR language (and says which).
- [ ] "Searchable PDF" turns a folder of phone scans into one PDF whose text can be selected and searched in Edge/Acrobat.
- [ ] Missing OCR language → clear message pointing to Windows Settings; no network access is attempted (verified with Process Monitor).
- [ ] No model files and no ML runtime DLLs anywhere in the build output (CI check).

### M6 — Share-safe (S5)
Done when:
- [ ] "Share-safe" on a photo removes GPS/serial/owner metadata, and a re-read of the output proves it (test corpus including the PowerToys #46317 case).
- [ ] On a screenshot or PDF, emails, phone numbers, card numbers (Luhn-checked) and IDs found through Windows OCR + rules are boxed for review; the user can add or remove boxes; confirm writes a redacted copy.
- [ ] Redacted PDFs contain no hidden text under the boxes (checked by extracting text from the output).
- [ ] Before/after diff shown in the history entry.

### M7 — Sealed engines and plugins (S6)
Done when:
- [ ] sd-media and sd-plugin run in AppContainers with no capabilities (or the documented fallback for sd-media); a test proves a socket call from inside fails.
- [ ] A sample plugin written in Rust ("pixel sort" or "add border") installs from a folder, appears as a slice, and runs with memory and fuel limits; an infinite-loop plugin is stopped and reported.
- [ ] Plugin manifest documented with a JSON Schema; the history shows which plugin made which file.
- [ ] Crashing a worker (fuzzed input) never takes down the tray app.

### M8 — Polish, release and showcase
Done when:
- [ ] Signed release (SignPath Foundation or other cert), winget manifest, portable zip.
- [ ] Screen reader announces the wheel and slices (Narrator test); full keyboard mode from an Explorer selection.
- [ ] Benchmarks published vs File Converter, PowerToys Image Resizer and Filee (section 7).
- [ ] README with architecture diagram and GIF; ADR index complete.

---

## 7. Interview showcase plan

### 7.1 Things to measure (and how)

| Metric | Target | How it's measured |
|---|---|---|
| Cold start → tray icon ready | < 150 ms | QPC timestamp at `wWinMain` entry and after `Shell_NotifyIcon` succeeds, written to the log; plus ETW/WPR trace for the README. |
| Idle memory | < 15 MB private bytes, workers not running | `GetProcessMemoryInfo` after 60 s idle; checked in CI. |
| Hook responsiveness | LL hook callback p99 < 50 µs; modifier press → first wheel frame < 1 frame (16.7 ms) | QPC inside the callback; frame time from DComp/DXGI present stats. |
| Preview latency + accuracy | p95 < 150 ms; median error ≤ 10 % | PreviewService telemetry written to the local log only (no network); corpus test. |
| Time to convert | Beat or match the competitors on the corpus | Benchmark harness: 100 × 12 MP JPEG → WebP, 20 × 1080p clips → "under 25 MB", 10 scanned PDFs → searchable PDF; median and p95, CPU vs GPU, threads 1…N. |
| Binary size | Broker exe < 3 MB | CI artefact size check. |

Competitor runs: same machine, same corpus, scripted where the tool has a CLI (File Converter does not, so time it by hand with a stopwatch protocol and say so). Publish the method with the numbers.

### 7.2 Tests and fuzzing
- **Unit tests (gtest):** output naming and collision rules; recipe parsing/validation; fit-to-target solver on synthetic rate curves; size estimator against stored ground truth; history/undo state machine; PII rules (Luhn, IBAN, phone formats); PDF writer round-trip.
- **Golden-image tests:** outputs compared with a PSNR/SSIM threshold, not byte equality (encoders change).
- **Fault injection:** fail the disk mid-commit, kill a worker mid-job, cancel at every step; assert no partial files and a correct history.
- **Fuzzing (libFuzzer via MSVC `/fsanitize=fuzzer,address`):** recipe and plugin manifest JSON, IPC message decoder, ffmpeg progress-line parser, WIC decode of arbitrary bytes from memory, PDF writer inputs. 60 s smoke per target on every PR, longer nightly; crashes become regression tests.
- **Manual checklist** stays for the gesture and overlay (CI runners have no interactive desktop for a global hook).

### 7.3 GitHub Actions CI
`windows-2022` runner: vcpkg manifest install with binary caching → CMake preset Release x64 → ctest → MSVC `/analyze` → ASan build + fuzz smoke → import-table "no networking" check → size check → upload artefacts. Nightly: long fuzzing + benchmark run, results committed to a `bench/` history so the README chart updates. Tag: signed release build.

### 7.4 README
Hero GIF (drag → Shift → hover shows "≈8.7 MB" → drop on "Under 10 MB" → carry into Discord); the one-line pitch; a "How it stays offline" section with the sandbox diagram; a benchmark table with method; a Mermaid architecture diagram (renders on GitHub); a feature table vs competitors; links to ADRs; build instructions in PowerShell.

### 7.5 ADRs to write
0001 Native C++/Win32 over .NET · 0002 WH_MOUSE_LL + OLE drop target rather than a shell extension · 0003 Layered/DirectComposition overlay without WS_EX_TRANSPARENT · 0004 Action/Recipe/Pipeline model · 0005 Never overwrite; undo via Recycle Bin · 0006 ffmpeg out of process · 0007 No ML models; Windows OCR + rules only · 0008 AppContainer worker sandbox · 0009 WASM plugins (wasmtime vs WAMR vs Lua) · 0010 Offline enforcement in CI · 0011 Size estimation by proxy encode.

### 7.6 "Hard problems I solved" stories
1. **A global mouse hook that can't lag Windows.** LL hooks run on your thread and Windows silently drops them if they're slow; Windows also hides a drag's files until it enters your window. Story: allocation-free callback + PostMessage, measured p99 latency, the hook watchdog, and turning the wheel itself into the drop target.
2. **Hitting an exact file size.** Bitrate maths vs reality: container overhead, audio budget, VBV behaviour differing between NVENC/QSV/AMF and x264, and an image quality search seeded by the preview estimate. Story ends with the accuracy chart.
3. **Sandboxing untrusted parsers and plugins on Windows.** AppContainer SIDs, ACL'd staging folders and handle brokering, Job Objects with kill-on-close, wasmtime fuel limits, and how we prove "no network" in CI and at runtime.
4. **Instant preview without stealing the CPU.** Hover-intent delay, cancellation via stop tokens, a priority scheduler, decoding at reduced scale, the file-ID cache, and how calibration brought the median error from X % to Y %.

---

## 8. Decisions log

Earlier decisions (stack, gesture, overlay, drop target, jobs, output naming) are in the M0 thread's plan and project memory and still stand. New in this document, **proposed until Prakhar approves**:

| # | Decision | Reason |
|---|---|---|
| R1 | **Approved 2026-10-20.** Six signature features: S1 Know before you drop, S2 Fit to…, S3 Recipes + Undo, S4 Text from anything (Windows OCR), S5 Share-safe, S6 Sealed engines + plugins | Highest combined score and they reinforce one identity (section 3.1). |
| R2 | `IConverter` evolves into Action + Recipe + PipelineRunner; a single action is a one-step recipe | Recipes, preview and fit-to-target all need estimate/preview/params. |
| R3 | Undo = recycle outputs (hash-checked); history in SQLite | We never overwrite, so undo needs no backups. |
| R4 | Broker + sandboxed worker processes (AppContainer, Job Objects) for ffmpeg and plugins | Provable offline, crash isolation, tiny idle memory. |
| R5 | **Prakhar, 2026-10-20:** no ML models that we bundle or download, and no API keys, anywhere (plugins included). Windows' built-in OCR is allowed | Product must be easy to run. Removes ONNX Runtime, DirectML, the ML worker and model delivery. |
| R6 | PII detection = Windows OCR + validated pattern rules, always with a review step | Follows R5; honest about what rules can find. |
| R7 | Background removal, face blur, subtitles and upscaling dropped | Each needs a model (R5). |
| R8 | WASM plugins with no filesystem access; host passes bytes/pixels | Smallest sandbox surface, simplest ABI. |
| R9 | PDF redaction by rasterise-and-rebuild | Guarantees no hidden text survives. |
| R10 | Recommend renaming before the repo goes public | "ShiftDrop" is nearly "DragShift", an existing tool. |

## 9. Open questions

1. **Name:** keep ShiftDrop, or pick Dropwise / DropWheel / another (I'll run a fuller trademark and domain check on the one you like)?
2. **Single exe:** accept "one broker exe + a small sidecar folder" (two worker exes and ffmpeg)?
3. **ffmpeg licence:** LGPL build (hardware encoders + OpenH264/SVT-AV1 software fallback) or GPL build with x264 (better software quality, GPL obligations for the bundle)? Also: what licence will ShiftDrop's own source use (MIT, source-available, closed)?
4. **Code signing:** SignPath Foundation (free for OSS, requires open source), a paid certificate, or unsigned for now?
5. **Minimum Windows version:** Windows 11 22H2+ only (acrylic backdrop, newer APIs), or also Windows 10 22H2?
6. **Plugin runtime:** wasmtime (more capable, larger, not in vcpkg) or WAMR (smaller)? Default if no preference: wasmtime.
7. **Competitor benchmarks:** OK to publish numbers naming File Converter, PowerToys and Filee in the README?

---

## 10. Sources

Tangerine: [X launch post](https://x.com/th2049/status/2097015553508725184) · [Product Hunt](https://www.producthunt.com/products/tangerine-zero-click-file-converter) · [Product Hunt reviews](https://www.producthunt.com/products/tangerine-zero-click-file-converter/reviews) · [Mac App Store](https://apps.apple.com/us/app/tangerine-file-converter/id6813034065?mt=12) · [Website](https://tangerineformac.com/) · [Press kit](https://tangerineformac.com/press) · [Satsuma (OSS reimplementation)](https://github.com/plyght/satsuma)

Arazio: [Product Hunt](https://www.producthunt.com/p/arazio/arazio) · [TrustMRR](https://trustmrr.com/startup/arazio) · [Website](https://arazioformac.com/)

TeraConvert: [Product Hunt](https://www.producthunt.com/products/teraconvert) · [AlternativeTo](https://alternativeto.net/software/teraconvert/about/) · [TrustMRR](https://trustmrr.com/startup/teraconvert)

Windows drag tools: [DragShift](https://github.com/llamaboi55/DragShift) · [Filee](https://github.com/KnifeLemon/Filee) · [Filee releases](https://github.com/KnifeLemon/Filee/releases) · [ZestDrop](https://github.com/Maxaccurate/ZestDrop) · [tangerine-windows](https://github.com/shakirgv/tangerine-windows)

File Converter (Tichau): [Repo](https://github.com/Tichau/FileConverter) · [Issues by 👍](https://github.com/Tichau/FileConverter/issues?q=is%3Aissue+is%3Aopen+sort%3Areactions-%2B1-desc) · [#211 Win11](https://github.com/Tichau/FileConverter/issues/211) · [#598 top-level menu](https://github.com/Tichau/FileConverter/issues/598) · [#805 JXL](https://github.com/Tichau/FileConverter/issues/805) · [Changelog](https://github.com/Tichau/FileConverter/blob/integration/CHANGELOG.md)

PowerToys: [Image Resizer issues by 👍](https://github.com/microsoft/PowerToys/issues?q=is%3Aissue+is%3Aopen+label%3A%22Product-Image+Resizer%22+sort%3Areactions-%2B1-desc) · [#8790 target size](https://github.com/microsoft/PowerToys/issues/8790) · [#2686 convert format](https://github.com/microsoft/PowerToys/issues/2686) · [#46317 GPS not removed](https://github.com/microsoft/powertoys/issues/46317) · [#45280 resizer broken](https://github.com/microsoft/PowerToys/issues/45280) · [#45682 Peek erratic](https://github.com/microsoft/PowerToys/issues/45682) · [Advanced Paste overview](https://powertoy.net/utilities/advanced-paste/) · [0.96.1 local AI notes](https://windowsforum.com/threads/powertoys-0-96-1-restores-image-resizer-on-windows-10-and-stabilizes-ai-features.391269/)

Target-size demand: [Video-Compressor-for-Discord](https://github.com/Mightypotatoast/Video-Compressor-for-Discord) · [ffmpeg4discord](https://pypi.org/project/ffmpeg4discord) · [Discord limit claims](https://discordtexttools.com/file-compressor/)

Other: [Drop Zone / DropIt alternatives](https://alternativeto.net/software/drop-zone) · [shiftdrop.com (parked)](https://shiftdrop.com/) · [DirectML EP status](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html) · [DirectML on Microsoft Learn](https://learn.microsoft.com/windows/ai/directml) · vcpkg port manifests checked directly in [microsoft/vcpkg/ports](https://github.com/microsoft/vcpkg/tree/master/ports) (onnxruntime, libwebp, libavif, libjxl, sqlite3, cppwinrt, nlohmann-json, gtest, benchmark present; wasmtime, wamr, pdfium absent)
