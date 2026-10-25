# Changelog

Notable changes to ShiftDrop, newest first. Versions follow [Semantic Versioning](https://semver.org/);
until 1.0, a minor version may change behaviour.

## [Unreleased]

## [0.7.1] - 2026-10-25

### Changed

- Version numbers follow Semantic Versioning 2.0.0 strictly. A build that isn't a release is a
  pre-release of the next version, such as 0.7.1-dev.3+af953b2 (3 commits after 0.7.0), so it sorts
  above the release it follows and below the next one: Check for updates offers it 0.7.1 when that
  comes out and never offers 0.7.0 again. File properties show four numbers, the last release and
  the build number (0.7.0.3), so Windows orders builds correctly too. Releases still say plain 0.7.0.

### Fixed

- Scrolling works again while dragging with Shift held: selecting text or moving a window with
  Shift down no longer stops the mouse wheel from scrolling the window under the cursor. Only an
  open wheel takes the mouse wheel.
- Float shelf: comes back where it was left (the edge it was pinned to, or the spot it floated at)
  after a restart, even when the resolution, scaling or monitors changed meanwhile; a monitor that
  is gone puts it on the nearest one, fully on screen. The first run still pins it to the right edge.

## [0.7.0] - 2026-10-22

### Added

- Edit for every document: Word, Excel, PowerPoint, OpenDocument, RTF, Markdown, HTML, text and
  CSV files get the Edit slice. The document is laid out into pages the way Convert makes a PDF, then
  opens in the PDF page editor (Redact, Annotate, Fill and sign, reorder, turn); Save writes
  "name (edited).pdf" next to it and leaves the document as it was. Formats only an office app can
  lay out need Office or LibreOffice.
- Float shelf: a chevron in the header shows Pinned items (kept across restarts) and Recent
  conversions (the last 10). Items there can be pinned, dragged out, opened or removed; removing one
  from Recent never deletes the file.
- Open, Show in folder and close on every result in the top panel and every Float shelf item: Open
  starts the file in its own app.
- Check for updates (Settings › Updates). Once a day ShiftDrop-Updater.exe asks GitHub for the latest
  release; a new version is offered a week after it comes out, with a toast and "Update to 0.7.1…" in
  the tray menu. Update now downloads the installer, checks its SHA-256 against the release's
  SHA256SUMS.txt, waits for any job to finish and installs it, keeping settings and history; if
  Setup fails, the old version starts again. Choose Install automatically, Ask me first (the default)
  or Off; Check now looks at once and skips the week.

### Changed

- The version is written in one place (VERSION). The app, its file properties, the installer and the
  release all take it from there, and CI fails if any of them disagree. Builds that aren't a release
  show the commit they came from, such as 0.6.0+b950d51 in About and in the installer's name, so a
  test build never looks like the release it follows.

### Fixed

- Settings: switching tabs and pages no longer flashes an in-between frame, scrolling is plain
  Windows scrolling, the Top panel size slider no longer leaves streaks and its 300% label sits at
  the end, and each Wheel tab shows its own actions (File actions showed the Reduce size ones).

## [0.6.0] - 2026-10-22

### Added

- Merge PDFs has More…: every page in one view to reorder across files, turn, leave out, preview
  and edit (Redact, Annotate, Crop, Light and colour) before Merge and export.
- An Edit slice next to Organize opens the same page view for one or more PDFs.
- Fill and sign for PDF pages: type into a form in text boxes with any installed font, add a
  signature (drawn, or from a photo, kept on this PC), ticks, crosses and the date. The page stays
  as it was; the typed text is real, searchable text in the PDF.
- Annotate text boxes edit in place (caret, selection, clipboard) with font, size, bold, italic,
  underline, colour, alignment and Auto-wrap.

### Changed

- A new look across the app in the ShiftDrop brand: new app and tray icons, the wheel, top panel,
  result card, Float shelf and notifications, Settings with pages, cards and a Wheel page, History,
  Rename, the inspector, the question windows and every editor.
- The accent colour follows your Windows accent by default; pick a preset or add colours of your own.
- The wheel's Size is Auto by default.
- Video and audio editors gain Remove a part with fades, Keep pitch, frame sequences, Split every,
  Add clips, Duck and six visualizer styles. History gains search, filters and Clear history;
  Rename can change the extension; Background can Fit to picture; the PDF page view can Add files.
- The Float shelf stays on screen while it's on, takes files dropped on it, and has Clear all.
- The top panel is about 1.4 times larger, so it reads at a glance on large screens; it follows the
  tray Size setting and still fits a 1366 px screen.
- The dragged item's picture (its icon or thumbnail and name) stays visible over the wheel again,
  still without a "+ Copy" badge.
- "Hide cursor over the wheel" is gone: the pointer is never hidden.
- The tray glyph and the app icon's small sizes (title bars, taskbar, Alt+Tab) show the tilted page
  with the teal corner too, drawn for each size so they stay crisp at 16 px.
- New text boxes in Fill and sign and Annotate start at 32 pt (32 px on photos).
- Releases are unsigned for now; code signing will be added later.

### Fixed

- Searchable PDFs open in Adobe Acrobat without "Cannot find or create the font": the invisible text
  layer now embeds a small font of its own. Text stays searchable and copies as before.

## [0.5.0] - 2026-10-21

The first public release.

### The wheel

- Drag files, press Shift, and a wheel of actions opens under the cursor. Four sections: Convert,
  Tools, Reduce size and File actions. Scroll, or press Shift+1 to 4, to switch; each section shows
  only what fits the files.
- Ctrl+Shift+Space and the Explorer right-click menu open the wheel on selected files.
- A panel at the top of the screen shows progress and the result. Every output lands next to the
  original as "name (converted).ext"; nothing is overwritten, and History can undo any run.
- Settings (tray menu) turns any section, file type or slice off.

### Convert

- Images, video, audio, PDFs, documents, spreadsheets, subtitles, Office files (with Office or
  LibreOffice installed), archives and folders. See the README for the full table.
- Video uses the GPU encoder when there is one, and copies streams that already fit the new
  container. A re-encoded video never comes out bigger than the original.

### Tools and file actions

- Image tools, photo editors (Edit, Annotate, Background, Collage), Redact, Share-safe, OCR, PDF
  Merge / Organize / Compress, video and audio editors, Reduce size from 90% to 20%.
- File actions: Copy, Move to, Copy path, Terminal, Duplicate, Rename, Float shelf, Share, Remove
  metadata, Zip.

### Also in this release

- PDF to Word, EPUB or HTML keeps the pictures; two-column PDFs are read in reading order.
- Password-protected ZIPs ask for the password. Very large spreadsheets and GIFs ask before a long job.
- HEIC photos stay HEIC through edits when Windows can save HEIC. SVGs with no size render at 2048 px.
- A notification for any job that took more than 10 seconds.
- Office exports run on their own thread, so Move and Copy never wait behind them.
- Dropping a ZIP on the ZIP slice is skipped instead of crashing.

[Unreleased]: https://github.com/CoPrime69/ShiftDrop/compare/v0.7.1...HEAD
[0.7.1]: https://github.com/CoPrime69/ShiftDrop/releases/tag/v0.7.1
[0.7.0]: https://github.com/CoPrime69/ShiftDrop/releases/tag/v0.7.0
[0.6.0]: https://github.com/CoPrime69/ShiftDrop/releases/tag/v0.6.0
[0.5.0]: https://github.com/CoPrime69/ShiftDrop/releases/tag/v0.5.0
