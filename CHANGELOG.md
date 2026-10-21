# Changelog

Notable changes to ShiftDrop, newest first. Versions follow [Semantic Versioning](https://semver.org/);
until 1.0, a minor version may change behaviour.

## [Unreleased]

### Changed

- The top panel is about 1.4 times larger, so it reads at a glance on large screens; it follows the
  tray Size setting and still fits a 1366 px screen.
- The dragged item's picture (its icon or thumbnail and name) stays visible over the wheel again,
  still without a "+ Copy" badge.
- "Hide cursor over the wheel" is gone: the pointer is never hidden.

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
