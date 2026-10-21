# ShiftDrop

[![CI](https://github.com/CoPrime69/ShiftDrop/actions/workflows/ci.yml/badge.svg)](https://github.com/CoPrime69/ShiftDrop/actions/workflows/ci.yml)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

Convert and edit files without opening an app. Start dragging a file, press **Shift**, and a wheel
of actions opens under the cursor. Drop the file on a slice: a photo becomes a WebP, a video shrinks
to fit under 25 MB, two PDFs become one. The result lands next to the original, which is never
touched.

ShiftDrop is a small native Windows app (C++20, Win32, COM) that lives in the tray. It works
offline and sends nothing anywhere.

<!-- Demo GIF goes here: docs/images/demo.gif, about 10 seconds. Drag a phone photo out of Explorer,
     press Shift, scroll from Convert to Reduce size, hover -70% (the centre shows the new size),
     drop, and the top panel says it is done. -->

## What a drop looks like

You drag `IMG_2041.jpg` out of Explorer and press Shift. The wheel opens on **Convert** and shows
only formats that make sense for a photo. Hover **WebP** and a card under the wheel tells you what
you'll get: about 1.2 MB instead of 3.4 MB, saved as `IMG_2041 (converted).webp`. Drop it there. A
panel at the top of the screen shows the progress, then "1 file converted to WebP". Click the panel
to see the file in Explorer. Changed your mind? Tray menu > History > Undo sends it to the Recycle
Bin.

## Features

The wheel has four sections. Each one shows only what fits the files you drag: a song gets audio
formats, a folder gets archive formats, and two PDFs get Merge.

### Convert

| Files | Convert to |
|---|---|
| Images (also from AVIF, HEIC and SVG) | PNG, JPG, WebP, AVIF, HEIC, TIFF, BMP, Web (resize + WebP), PDF (one PDF from all of them). An animated GIF or a multi-page TIFF keeps every frame |
| Video | MP4, MOV, MKV, WebM, AVI, WMV, GIF, MP3 |
| Audio | MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA |
| PDF | JPG or PNG (every page), TXT, Markdown, DOCX, EPUB, HTML |
| Documents (DOCX, Markdown, HTML, TXT, RTF, RTFD, Pages) | DOCX, PDF, Markdown, HTML, EPUB, TXT |
| Spreadsheets (XLSX, XLS, ODS, CSV, TSV) | XLSX, ODS, CSV, TSV, PDF |
| Subtitles (SRT, VTT) | SRT, VTT, TXT |
| Word, Excel and PowerPoint files | Exact PDF, and PNG or JPG slides, when Microsoft Office or LibreOffice is installed |
| Archives (ZIP, TAR, TGZ, GZ, 7z, RAR) | Extract, ZIP, TAR.GZ, 7z |
| Folders | ZIP, TAR.GZ, 7z |

Streams that already fit the new container are copied, not re-encoded, so a phone's `.mov` to MP4
takes a second or two.

### Tools

- **Images:** Rotate, Flip, 1920 px, 1080 px, 50%, Compress, Under 1 MB, Crop, and Inspect (every
  metadata field, searchable).
- **Photo editors**, with a live preview drawn on the GPU: Edit (light, colour, detail, dehaze,
  clarity, noise, grain), Annotate (arrows, text, shapes, callouts, numbers, magnifiers, pictures,
  pen, highlighter), Background (solid, gradient, blur or picture, with spacing, corners and shadow),
  and Collage for two or more pictures. Edit, Annotate and Background keep the photo's format and
  its camera, date and location details.
- **Privacy:** Redact covers text, faces and personal details such as emails and card numbers.
  Share-safe removes metadata, checks that the result really is clean, and shows you what it found
  first. Share-safe also works on video and audio.
- **Text:** Copy text (OCR from pictures and scanned PDFs, to the clipboard) and Searchable PDF.
- **PDF:** Merge, Organize (reorder, rotate, duplicate and delete pages), Compress, Smallest.
- **Video:** Compress, 50% size, Under 10 MB, Under 25 MB, and a timeline editor for Trim, Crop,
  Speed, Join, Frames, Split and Redact.
- **Audio:** the same Compress slices, Normalize, and a waveform editor for Trim, Channels, Bleep
  and Visualizer.

### Reduce size

90%, 80% and so on down to 20% smaller, for photos, video, audio and PDFs. The centre of the wheel
shows the size now, and hovering a slice shows the size it will come out at.

### File actions

Copy, Move to…, Copy path, Terminal (opens in the file's folder), Duplicate, Rename… (new names from
a pattern), Float (a shelf at the screen edge that holds files until you drag them into any app),
Share (the Windows share sheet), Remove metadata, and Zip (one ZIP of everything dropped).

### Also

- Pictures dragged straight out of a browser, and attachments dragged out of Outlook or Thunderbird,
  work too. Their results go to your Desktop, to Downloads, or to a folder you pick each time.
- Every slice is a recipe, a small JSON file, and you can add your own. See
  [docs/recipes.md](docs/recipes.md).
- Every run is in History and can be undone. Undo refuses if you edited the output since, so it
  never throws away your work.
- Long jobs show their progress in the top panel and can be cancelled there.

## Install

1. Download `ShiftDrop-Setup-<version>.exe` from the
   [Releases](https://github.com/CoPrime69/ShiftDrop/releases) page.
2. Run it. It installs for your user only, into `%LOCALAPPDATA%\Programs\ShiftDrop`, with no admin
   rights and nothing else to install. It adds a Start menu entry and can start ShiftDrop when you
   sign in.

The installer isn't code-signed yet (see the [code signing policy](#code-signing-policy)), so
Windows SmartScreen may say "Windows protected your PC". Choose **More info**, then **Run anyway**.

ShiftDrop needs Windows 10 22H2 or Windows 11, 64-bit. To uninstall, use Settings > Apps. Your
settings, recipes and history in `%LOCALAPPDATA%\ShiftDrop` are kept.

## How to use it

- **Drag and press Shift.** Start dragging one or more files, from Explorer, the desktop or any app
  that drags real files, and press Shift. The wheel stays open when you let go of Shift. Drop on a
  slice to run it. Drop on the centre or press Esc to cancel. Drag off the wheel and the drop goes
  wherever you release it, as usual.
- **Switch sections.** Turn the mouse wheel to step through Convert, Tools, Reduce size and File
  actions. No mouse wheel? Keep Shift held and press 1, 2, 3 or 4.
- **Use the keyboard.** While the wheel is open, the arrow keys pick a slice and Enter runs it.
- **Without dragging.** Select files in Explorer and press **Ctrl+Shift+Space**, or right-click them
  and choose **ShiftDrop…** (on Windows 11 it's under Show more options). The wheel opens at the
  cursor; click a slice.
- **The tray menu** has History (with Undo), Settings, the Float shelf, the wheel's size (Auto,
  Small, Medium, Large) and style (Solid or Glass), where browser drops are saved, Start with
  Windows, and the recipes and logs folders.
- **The Settings window** turns sections or single slices off, and sets the theme, the output name
  style (`photo (converted).png`, `photo-converted.png`, `photo_converted.png`, or a `ShiftDrop`
  subfolder) and which terminal File actions › Terminal opens.

Outputs are named `photo (converted).png`, then `photo (converted 2).png` and so on. An existing
file is never replaced.

## Privacy

- **Offline.** ShiftDrop makes no network connections: no telemetry, no accounts, no update checks.
  CI fails the build if either exe imports a networking library.
- **Video and audio** go through a bundled LGPL build of [FFmpeg](https://ffmpeg.org), run as a
  separate process. It is built without network support, and ShiftDrop only lets it open local
  files.
- **Archives** are read by a separate worker inside a Windows AppContainer with no permissions. It
  can't open your files or reach the network, and it only gets the one archive it reads. ShiftDrop
  checks every name before writing anything, refuses `..` and absolute paths, skips links, and stops
  zip bombs at a size limit.
- **OCR and face detection** use what Windows already has. Nothing is downloaded.
- Everything ShiftDrop keeps (settings, history, recipes, logs) stays in `%LOCALAPPDATA%\ShiftDrop`
  on your PC.

## Limitations

- **Office layouts.** Exact PDFs of Word, Excel and PowerPoint files need Microsoft Office or
  LibreOffice installed, and slides as pictures need PowerPoint itself. ShiftDrop drives the
  installed app invisibly and closes it again. Without one, those slices don't appear. If you install
  Office while ShiftDrop runs, restart ShiftDrop.
- **HEIC** needs Windows' "HEIF Image Extensions" to read and "HEVC Video Extensions" to save, both
  from the Microsoft Store. Without them the HEIC slices are dimmed and say what to install. Edits to
  a HEIC (Rotate, Crop and so on) are saved as JPG.
- **RAR** files can be extracted and converted to other archive formats, but not created.
- **Pages** files can be read only when they carry the preview PDF most of them have. ShiftDrop
  can't write Pages files.
- **PDF to DOCX, EPUB or HTML** gives the text, reflowed with its headings and lists. Pictures and
  page layout are not kept. There is no PDF to XLSX or PDF to PPTX.
- **No background removal**, or anything else that needs a machine-learning model.
- **H.264 video** is encoded with your graphics card's encoder, Windows' Media Foundation encoder or
  OpenH264. x264 and x265 are GPL, so they aren't included.
- **Elevated windows.** Windows blocks drops from a normal Explorer onto an app running as
  administrator, so don't run ShiftDrop elevated.
- **Metadata** can be inspected and removed, but not edited field by field.
- **Plugins** are designed ([docs/plugins.md](docs/plugins.md)) but not built.

## Building

You need Visual Studio 2022 with the C++ workload, Git and vcpkg. In a Developer PowerShell for VS
2022, with `VCPKG_ROOT` pointing at vcpkg:

```powershell
./scripts/get-ffmpeg.ps1
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
.\build\x64-release\Release\ShiftDrop.exe
```

[docs/BUILDING.md](docs/BUILDING.md) is the full guide: setting up vcpkg, where ffmpeg comes from,
the presets, the tests and the installer.

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md): build, test and package from a fresh clone
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it works inside, from the mouse hook to undo
- [docs/recipes.md](docs/recipes.md): writing your own slices
- [docs/TESTING.md](docs/TESTING.md): the manual test checklist
- [docs/plugins.md](docs/plugins.md): the plugin design
- [docs/adr/](docs/adr/): records of the main design decisions

## Contributing

Bug reports and pull requests are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. It covers
the rules the code keeps (offline, no GPL, never overwrite a file) and how a new feature plugs in.
Please follow the [code of conduct](CODE_OF_CONDUCT.md), and report security problems privately as
described in [SECURITY.md](SECURITY.md).

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/). Signing is being set up; until it is live, releases
are unsigned.

Only release builds are signed. They are built from a tagged commit by this repository's GitHub
Actions workflow, and each signing request is approved by hand.

Team roles:

- Committers and reviewers: [CoPrime69](https://github.com/CoPrime69)
- Approvers: [CoPrime69](https://github.com/CoPrime69)

Privacy: this program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it. See [Privacy](#privacy).

## Licence

ShiftDrop is released under the [MIT licence](LICENSE). It is built with third-party libraries
under their own licences, and the bundled `ffmpeg.exe` is under the LGPL. See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
