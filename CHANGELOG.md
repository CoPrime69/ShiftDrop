# Changelog

Notable changes to ShiftDrop, newest first. Versions follow [Semantic Versioning](https://semver.org/);
until 1.0, a minor version may change behaviour.

## [Unreleased]

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

[Unreleased]: https://github.com/CoPrime69/ShiftDrop/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/CoPrime69/ShiftDrop/releases/tag/v0.5.0
