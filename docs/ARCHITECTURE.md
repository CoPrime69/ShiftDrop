# How ShiftDrop works

This page walks through ShiftDrop from the moment you press Shift to the moment a file lands on
disk, then covers the parts around it. It is written for people who want to change the code. The
design decisions behind the main choices are in [adr/](adr/).

- [The big picture](#the-big-picture)
- [Where the code is](#where-the-code-is)
- [Threads](#threads)
- [The gesture: mouse hook and probe](#the-gesture-mouse-hook-and-probe)
- [The drop target](#the-drop-target)
- [The wheel and the top panel](#the-wheel-and-the-top-panel)
- [Keyboard and click mode](#keyboard-and-click-mode)
- [Actions and recipes](#actions-and-recipes)
- [Running a job](#running-a-job)
- [Writing outputs](#writing-outputs)
- [Feature modules](#feature-modules)
- [The ffmpeg process](#the-ffmpeg-process)
- [The sandbox](#the-sandbox)
- [Plugins](#plugins)
- [History and undo](#history-and-undo)
- [Settings, data and logs](#settings-data-and-logs)
- [Speed](#speed)

## The big picture

Three programs ship in an install:

| Program | What it is |
|---|---|
| `ShiftDrop.exe` | The app: tray icon, mouse hook, wheel, windows, and most of the work |
| `engines\ShiftDropWorker.exe` | A small sandboxed helper that parses archives. No window, no COM, no file or network access of its own |
| `engines\ffmpeg.exe` | FFmpeg, started once per video or audio job |

A drop goes through these steps:

```
 mouse hook ──► probe window ──► OLE DragEnter ──► wheel ──► Drop
 (Shift held      (invisible,       (files arrive)     (slices for    │
  mid-drag)        hit-testable)                        these files)  ▼
                                                              batch on a job lane
                                                                       │
                       ┌───────────────────────────────────────────────┘
                       ▼
              PipelineRunner ──► image pipeline │ ffmpeg │ feature engine
                       │
                       ▼
              StagedOutput: staging file ──► no-replace rename ──► hash ──► history row
                       │
                       ▼
              top panel shows the result; History can undo it
```

## Where the code is

`CMakeLists.txt` builds four things:

- **`shiftdrop_core`**, a static library with everything that doesn't need the gesture UI: actions,
  recipes, the image pipeline, ffmpeg arguments, history, undo, output naming and every feature
  engine. The app and the tests both link it.
- **`shiftdrop_winrt`**, an object library with the Windows Runtime parts (PDF rendering, OCR, the
  share sheet, face detection, toasts). Each file installs itself into a hook in the core when it is
  linked, so the core and its tests build and run without it.
- **`shiftdrop_pdf_ui`**, the PDF Merge window and the page view (Organize, Merge's More…, Edit).
- **`ShiftDrop.exe`** and **`ShiftDropWorker.exe`**, plus **`shiftdrop_tests`**.

In `src/`, the files at the top level are the core and the gesture UI. Each feature has its own
folder:

| Path | What's there |
|---|---|
| `main.cpp` | `wWinMain`, single instance, the `App` class: gesture state, wheels, batches, tray, command line |
| `MouseHook` | The `WH_MOUSE_LL` hook |
| `Overlay`, `Canvas`, `Theme` | The wheel window and how it is drawn |
| `Accent` | The accent colour (Settings › Accent colour): the presets, their light and dark tones, and the one colour every window draws with (unit-tested) |
| `DropTarget`, `DropDescription`, `VirtualDrop` | The OLE drop target, the drag picture and its badge, files dragged out of browsers and mail |
| `Island`, `PanelLayout` | The top panel, and its size and layout (unit-tested) |
| `Accessibility` | The wheel and the top panel for screen readers (UI Automation) |
| `Action`, `Recipe`, `RecipeStore` | The action registry, recipe parsing, built-in and user recipes |
| `Engine.h`, `Modules.cpp` | The feature-module interface and the one list of modules |
| `PipelineRunner`, `ImagePipeline`, `ImageHooks.h` | Running a recipe on a file; the fused image pipeline |
| `Media`, `Ffmpeg` | Video and audio formats and their ffmpeg arguments; the ffmpeg child process |
| `StagedOutput`, `OutputPath` | Staging, naming and the no-replace commit |
| `HistoryStore`, `Hash`, `Undo`, `HistoryWindow` | SQLite history, SHA-256, undo, the History window |
| `JobQueue` | The worker threads |
| `FileActions`, `FileMove` | Copy, Copy path, Terminal, Move to folder |
| `Settings`, `Features`, `SettingsWindow`, `Startup`, `Tray` | settings.ini, the Settings window, start with Windows, the tray icon |
| `Ask`, `Secret.h` | Questions a running job asks the user (a password, print every row?) and passwords that wipe themselves |
| `Log`, `Brand.h`, `Utf.h`, `Messages.h`, `resource.h`, `ShiftDrop.rc`, `app.manifest` | Plumbing |
| `Strings`, `PathUtil`, `FileIo`, `ErrorText`, `WinRtError.h`, `TempPath`, `FormatText` | The small helpers every area shares: text, paths, whole-file reads and writes, error codes, %TEMP%, sizes and times |
| `keyboard/` | Arrow keys and Enter on the wheel, the Explorer-selection hotkey, the right-click entry, wheel layout |
| `preview/` | The predicted-output card |
| `inspect/` | The metadata inspector |
| `shelf/` | The Float shelf and convert-and-carry |
| `formats/` | AVIF, HEIC, SVG and multi-frame images |
| `pdf/`, `ocr/`, `share/` | The PDF engine and its windows, OCR, the share sheet |
| `canvas/`, `crop/`, `redact/`, `metadata/`, `sharesafe/`, `imagetools/` | The image editor canvas, Crop, Redact, metadata, Share-safe |
| `photo/` | Edit Photos, Annotate, Background, Collage |
| `mediaeditor/`, `videotools/`, `audiotools/` | The timeline editor and the video and audio tools |
| `docs/`, `sheets/`, `subtitles/`, `officebridge/` | Documents, spreadsheets, subtitles, the Office bridge |
| `archive/`, `sandbox/` | Archives, the AppContainer launcher and the worker |
| `rename/`, `toast/` | Rename, Windows toasts |

`tests/` has one GoogleTest file per module (`PdfTests.cpp` for `src/pdf`), plus `TestMain.cpp` and
shared helpers.

## Threads

| Thread | What runs there |
|---|---|
| UI thread (STA) | The hidden main window, tray icon, mouse hook, wheel, top panel, OLE drop target, History and Settings windows, editor windows |
| Pool workers (MTA) | Image recipes, PDF pages and most feature engines. One per logical processor minus one, at most 8 (`JobQueue::PoolThreads`) |
| Media worker | One thread for video: one ffmpeg run already uses every core, so videos go one at a time |
| Audio workers | Sound-only ffmpeg runs use one core each, so several run side by side (same count as the pool) |
| Shell worker (STA) | `IFileOperation` work such as Move to folder and Rename |
| Office worker (STA) | Word, Excel and PowerPoint exports through COM, apart from the shell worker so a long export never holds up a move |
| Shelf worker (STA) | Float shelf thumbnails, missing-file checks, clipboard pictures |
| Predictor worker | The predicted-output card, so the wheel never waits |

An engine picks its lane with `IEngine::Lane` (`Pool`, `Media`, `Audio`, `Shell` or `Office`), and
`JobLanes` in [JobQueue.h](../src/JobQueue.h) holds one queue per lane. Workers post
their results back to the main window with `PostMessage`; nothing touches a window from a worker.

## The gesture: mouse hook and probe

Windows has no API that tells an app "the user is dragging files". Only the drop target under the
cursor receives the drag data. ShiftDrop works around that in four steps
([ADR 0002](adr/0002-probe-overlay.md)).

1. **The hook** ([MouseHook.h](../src/MouseHook.h)). A `WH_MOUSE_LL` hook on the UI thread watches the
   primary button. When it is held and the mouse moves past `SM_CXDRAG`/`SM_CYDRAG`, the hook posts
   `DragBegin`. If the trigger key is down (`GetAsyncKeyState`), it posts `DragArm` once. On release
   it posts `DragEnd`. The callback does nothing else: no allocation, no locks, no I/O. Windows
   removes a low-level hook that is slow to return, so it has to stay that cheap. With swapped mouse
   buttons, the right button is treated as primary.
2. **The poll.** Between `DragBegin` and `DragEnd`, a 30 ms timer in `main.cpp` checks the key too, so
   pressing Shift while holding the mouse still also works.
3. **The probe.** On arm, the wheel window appears under the cursor, painted at about 2% alpha.
   You can't see it, but Windows can hit-test it. ShiftDrop doesn't know yet whether this is a file
   drag.
4. **OLE decides.** The drag source's `DoDragDrop` loop finds the probe under the cursor and calls
   `DragEnter`. If the data carries files, the probe turns into the real wheel. If not (text, a URL,
   a window being moved), it hides. If no `DragEnter` comes at all, the probe goes away at button-up.
   So Shift plus a drag that isn't a file drag never flashes a menu.

A watchdog timer reinstalls the hook if the cursor keeps moving but the hook has seen nothing for
over two seconds, which is what a silently removed hook looks like.

## The drop target

[DropTarget.h](../src/DropTarget.h) is the window's `IDropTarget`. It reads `CF_HDROP` for real files.
The Float shelf registers the same class with itself as the sink, so files dropped on it (from
Explorer, or virtual files from a browser, kept with `KeepVirtualFiles`) are read the same way.

Over a slice it always answers `DROPEFFECT_COPY`, or `LINK` when the source doesn't offer copy, and
never `MOVE`. Explorer treats Shift-drag as "move", and with `MOVE` the source would delete the
original after the drop. The centre and dimmed slices answer `NONE`, so releasing there cancels.
Outside the wheel, the drop goes to whatever is underneath, so a normal Shift-drag to move a file
into a folder still works.

`Drop` reads the paths, hides the wheel, queues the batch and returns at once, because the drag
source is blocked until it does.

**Files with no path.** A picture dragged off a web page or an attachment dragged out of Outlook
carries names and contents (`CFSTR_FILEDESCRIPTORW` and `CFSTR_FILECONTENTS`) but no path.
[VirtualDrop.h](../src/VirtualDrop.h) handles these. During the drag it reads only the names, so the
wheel can show the right slices. On the drop it saves the contents into a private folder under
`%LOCALAPPDATA%\ShiftDrop\dropped`, runs the slice on those copies, and sends the outputs to the
Desktop (or Downloads, or a folder the user picks; tray menu > Save browser drops to). The private
folder is deleted when the job ends, and any left over from a crash are deleted at startup.

An elevated window can't receive drops from a normal Explorer (UIPI). ShiftDrop warns at startup if
it is running as administrator.

## The wheel and the top panel

**The wheel** ([Overlay.h](../src/Overlay.h)) is a topmost, non-activating, layered popup. It is drawn
with Direct2D into a bitmap and pushed with `UpdateLayeredWindow`, so it has per-pixel alpha.
Pixels with alpha 0 can't be hit-tested, which is why the gaps between tiles are painted faintly:
otherwise a drop there would fall through to the window below. The window never takes focus, so the
drag source keeps it.

It has four sections, in this order: Convert, Tools, Reduce size, File actions. Shift opens it on the
section that fits the dragged files best. The mouse wheel steps through the sections (the hook
swallows the scroll so the window underneath doesn't move), and Shift+1 to Shift+4 jumps to one.
Each section shows only the slices that apply to the dragged files. A slice that applies but can't
run (already that format, a codec missing) is dimmed, and hovering it shows why in the centre.

Up to 8 slices sit on one ring, 9 to 16 on two, and more page through a "More" slice
([keyboard/WheelLayout.h](../src/keyboard/WheelLayout.h)). The Glass style captures the screen behind
the wheel when it opens, shrinks it and blurs it. Size and style come from the tray menu.

No label is cut off with "…" ([keyboard/WheelLabels.h](../src/keyboard/WheelLabels.h)). Each is
measured with DirectWrite and checked against its tile's real shape (both arcs and both straight
sides, a few DIPs in): one line at full size, else two lines broken at a space, then both a little
smaller down to a readable minimum. Only then is it shortened ("Under 10 MB" to "≤10 MB", then words
it shares with other slices), never into something another slice reads.

**The drag picture.** The drag source draws a picture of what is dragged (Explorer: the icon or
thumbnail and the full name; a browser: the image) in a topmost window of its own. The shell shows it
only over drop targets that hand their drag events to the drag-image helper (`IDropTargetHelper`)
and fades it away over any other window. The wheel's `DropTarget` forwards DragEnter, DragOver,
DragLeave and Drop to the helper with the wheel's window, so the picture follows the cursor over the
wheel. It sets an empty `DROPIMAGE_NOIMAGE` drop description while over the wheel, so there is no
"+ Copy" badge, and `DROPIMAGE_INVALID` on leave, so other windows show their usual badge. The wheel,
the top panel and the predicted-output card open after the drag began and are topmost too, so they
would cover the picture's window: `KeepDragImageOnTop` moves any of ours that are above it to just
below it (the window's handle is in the drag's data object as "DragWindow"). The mouse pointer is
never hidden. Over the centre the picture would hide the count, so the wheel reads where it is
(`DragImageRect`: its size and the cursor's spot in it, from "DragImageBits") and, while the cursor
is on the centre, shows the count in a small badge just beside it instead.

**The predicted-output card** ([preview/Predictor.h](../src/preview/Predictor.h)) appears under the
wheel when a slice is highlighted. On its own worker thread, it encodes small samples of a picture to
estimate the output size, or reads ffmpeg's plan for a video and multiplies bitrate by duration. It
also shows the output name `StagedOutput` will pick and what metadata is lost. Results are cached
per file, size, modified time and recipe.

**The top panel** ([Island.h](../src/Island.h)) hangs from the top centre of the monitor. It is drawn
1.4 times the wheel's scale at the same Size setting ([PanelLayout.h](../src/PanelLayout.h)), so it can be
read at a glance on large screens, and it never gets wider than the screen: on a narrow one the gaps
shrink first, then the hint goes, and only then are the wheel names shortened. While the
wheel is open it shows the sections and which one you are on. After a drop it shows the job's
progress with a cancel button, then the result. A success tucks itself away after two seconds; a
failure stays until clicked. A finished result can be clicked (Explorer opens with the outputs
selected), dragged straight into another app, pinned to the Float shelf or copied. The panel is
click-through except while it shows a result, and it is never a drop target.

Both windows follow the Windows light or dark theme (or the Settings override), use the accent
colour ([Accent.h](../src/Accent.h): the user's Windows accent colour by default, ShiftDrop's teal or another preset, or a colour of
your own, each in a tone that reads in light and dark), honour "Animation effects" being off, and are visible to screen readers through UI
Automation ([Accessibility.h](../src/Accessibility.h)).

## Keyboard and click mode

[keyboard/KeyboardController.h](../src/keyboard/KeyboardController.h) keeps everything keyboard out of
`main.cpp`.

- **During a drag**, the 30 ms poll reads the arrow keys and Enter with `GetAsyncKeyState`. Arrows
  move the highlight. Enter runs the slice on the dragged files, then sends an Esc so the drag source
  cancels the drag and nothing is dropped anywhere else. There is no keyboard hook.
- **Without a drag**, the global hotkey (Ctrl+Shift+Space by default, `[Keyboard]` in settings.ini)
  reads File Explorer's selection through `IShellWindows` and opens the wheel in *click mode* at the
  cursor. The right-click entry "ShiftDrop…" runs `ShiftDrop.exe --wheel <paths>`. Explorer starts
  one process per selected file, so the running copy collects the paths it receives by
  `WM_COPYDATA` for a moment, then opens one wheel for all of them.
- **In click mode**, hovering or the arrows pick a slice, and a click or Enter runs it. Arrows, Enter
  and Esc are taken with `RegisterHotKey` while the wheel is open, so they don't also move
  Explorer's selection, and are given back the moment it closes.

The installer and the tray menu add or remove the right-click entry under
`HKCU\Software\Classes` (`--register-shell`, `--unregister-shell`).

## Actions and recipes

An **action** is one step something can do, such as `image.resize` or `pdf.merge`. A **recipe** is
a named list of steps with parameters, and every slice on the wheel is a recipe, built-ins included
([ADR 0004](adr/0004-recipes-fused-pipeline.md)).

- [Action.h](../src/Action.h): the `ActionRegistry` holds every action with its parameter specs
  (type, range, allowed values, default). The core registers the image, media and file actions;
  each feature module registers its own.
- [Recipe.h](../src/Recipe.h): parses a recipe's JSON and validates it against the registry. Unknown
  fields, values out of range and steps in the wrong order are errors, and the message names the
  field: `steps[1].params.quality: 140 is above 100`.
- [RecipeStore.h](../src/RecipeStore.h): built-in recipes, then the user's files from
  `%LOCALAPPDATA%\ShiftDrop\recipes\*.json`. A user file with a built-in's id replaces it. A broken
  file is skipped, named in a notification and the log, and the rest still load.

[recipes.md](recipes.md) is the reference for writing your own.

## Running a job

When a slice gets a drop, `main.cpp` makes a batch: one history run, one job per file (or one job
for the whole drop when the engine is a batch engine), on the lane the recipe asks for.
[PipelineRunner](../src/PipelineRunner.h) runs each job:

- **Image recipes** go through the fused [ImagePipeline](../src/ImagePipeline.h). The steps chain
  WIC bitmap sources (decode, EXIF orientation, scale, flip and rotate, format conversion) and the
  pixels are pulled once into the encoder at the end. No intermediate file is written and there is
  one lossy encode at most. WIC handles PNG, JPEG, TIFF, BMP and GIF; libwebp does WebP, and
  [formats/](../src/formats) adds AVIF (libavif), SVG (LunaSVG), HEIC (Windows' own HEIF codec) and
  every frame of an animated GIF or multi-page TIFF, plugged in through `ImageHooks.h`.
- **Media recipes** (`media.convert`, `media.compress`) build an ffmpeg command line from the
  probed input in [Media.h](../src/Media.h) and run it (see [The ffmpeg process](#the-ffmpeg-process)).
  Streams the target container can hold are copied, not re-encoded.
- **File actions** (Copy, Copy path, Terminal, Duplicate, Move to folder, Rename) run on the UI or shell
  thread.
- **Everything else** goes to the feature engine that handles the recipe (see
  [Feature modules](#feature-modules)).

Each job gets a `std::stop_token`. The × in the top panel requests a stop: files not started yet
are skipped, a running ffmpeg is killed at once, and engines check the token as they go. Progress within a file (0 to 1) comes back through a
callback and moves the panel's bar.

Some tools need the user first: a trim range, a crop box, a page order. Their engine returns true
from `NeedsEditor` and opens its own window on the UI thread. When the user confirms, the recipe
goes back into the queue with their choices in its parameters.

Other questions only come up once a job is running: an archive turns out to be encrypted, a
spreadsheet bound for PDF has more than 20,000 rows, a GIF bound for a folder of pictures has more
than 100 frames. The worker asks through [Ask.h](../src/Ask.h): the question is posted to the UI
thread and shown there (a TaskDialog with command links, or a small password box) while the worker
waits. The UI thread never waits for a worker, questions are shown one at a time, and the job's stop
token closes an open question as Cancel, so nothing can deadlock. With no one to ask (the tests), a
job does what it did before the question existed.

## Writing outputs

Two rules: never overwrite anything, and never leave a half-written file under a real name, even if
the app is killed or the PC loses power ([ADR 0003](adr/0003-never-overwrite-commit.md)).

Every output goes through [StagedOutput](../src/StagedOutput.h):

1. Create a staging file `~photo.1a2b3c4d.sdpart` next to the source (or a staging folder for
   outputs that are folders, like the pages of a PDF). History records its path before any byte is
   written.
2. Write it.
3. Rename it to `photo (converted).png` with `MoveFileExW` and no `MOVEFILE_REPLACE_EXISTING`. If the
   name is taken, try `(converted 2)`, `(converted 3)` and so on. The rename fails rather than
   overwrite, even if another program creates the name between the check and the rename, and a
   rename within one folder is atomic.
4. Hash the result (SHA-256 through CNG) and mark the history item done.

If anything fails, the staging file is deleted. At startup, items still marked running from a
previous session are marked interrupted and their staging files deleted.

The naming style is a setting (Settings > Output names): `photo (converted).png`,
`photo-converted.png`, `photo_converted.png`, or a `ShiftDrop` subfolder. Every style keeps the
no-replace rule. [OutputPath.h](../src/OutputPath.h) is the one place that decides where an output
goes, including the redirect for files dragged out of browsers.

## Feature modules

Everything beyond the core image, media and file paths is a feature module: PDF, archives, the
video and audio tools, the photo editors, documents. A module lives in its own folder and plugs into
the core in one place, so features can be built side by side.

**Files.** `src/<area>/` (for example `src/pdf/`), plus `tests/<Area>Tests.cpp`. Nothing of a module
lives in another module's folder.

**Registration.** One `FeatureModule` (see [Engine.h](../src/Engine.h)) listed in
[Modules.cpp](../src/Modules.cpp):

- `registerActions` adds the module's actions with `ActionRegistry::Add`. Ids are `<area>.<verb>`
  (`pdf.merge`), and each is a recipe's only step.
- `builtInRecipes` returns a JSON array of slices in the format of `RecipeStore::BuiltInJson`: `id`,
  `label`, `description`, `ring`, `suffix`, and an optional `icon` as a Segoe Fluent Icons code
  point like `"E7AD"`. Ids are unique across the app; prefix them with the area when in doubt.
- `makeEngine` returns the module's `IEngine`.

**The engine.** `Handles`, `Applies`, `Accepts`, `IsNoOp` and `Lane` describe what it runs and where.
Then either `Run` (one output per file) or `IsBatch` plus `RunBatch` (one output from all files).
`AppliesToDrop` hides a slice for a drop of the wrong size, such as a collage of one picture.
`NeedsEditor` and `OpenEditor` open the tool's window first.

**Outputs** always go through `StagedOutput`. A batch output can pass an `OutputPlace` (folder and
stem) to be named after something other than its source, like `Photos (zipped).zip` for a drop of a
folder's files. An engine that needs an output folder asks `OutputFolder(source)`, never the source's
own folder, so the browser-drop redirect works.

**Untrusted input** is parsed in the sandbox worker when possible, and the app writes the files, as
`src/archive` does.

**ffmpeg** goes through `EngineServices::ffmpeg` (`FfmpegEngine::Run` and `Probe`), always with
arguments from `FfmpegArgs`, on `JobLane::Media` for video or `JobLane::Audio` for sound only.

**Things that need Windows Runtime or a window** go in the object libraries and reach the core
through a small hook, so the core library and its tests don't depend on them.

[CONTRIBUTING.md](../CONTRIBUTING.md#adding-a-feature) lists the shared files a module may touch.

## The ffmpeg process

[Ffmpeg.h](../src/Ffmpeg.h) runs `engines\ffmpeg.exe` (or `%SHIFTDROP_FFMPEG%`) as a child process:

- No console window, below-normal priority.
- Every argument list comes from `FfmpegArgs`, which adds `-protocol_whitelist file`, so the input
  can only be a local file, never a URL. ShiftDrop's own ffmpeg build is also configured with
  `--disable-network`.
- Progress comes from `-progress pipe:1`, read on a separate thread and turned into 0 to 1 from the
  input's duration.
- Each run is in a job object that kills the process when the handle closes. Cancelling, quitting or
  a crash of ShiftDrop never leaves an ffmpeg running.
- The first time a video needs H.264, each candidate encoder encodes a few frames once: NVENC, Quick
  Sync and AMF on the GPU, then Media Foundation, then OpenH264. The first that works is used from
  then on and named in the log.

`Probe` runs ffmpeg with an input and no output to read its streams, size and duration. Compress
and Fit to… plan bitrates and height from the duration, and try up to three times so the result is
never over the target.

Without `ffmpeg.exe` the app still runs; the video and audio slices are dimmed and say why.

## The sandbox

Parsing an archive means reading hostile input with a lot of C code. ShiftDrop keeps that out of
its own process ([sandbox/Sandbox.h](../src/sandbox/Sandbox.h)).

- **The worker** ([sandbox/Worker.h](../src/sandbox/Worker.h)) is `engines\ShiftDropWorker.exe`, a
  small exe with only the parsers in it. It starts in an **AppContainer with no capabilities**: it
  has no network access and can't open the user's files by name.
- It runs in a **job object** that kills it when ShiftDrop closes the handle, with a 2 GiB memory
  limit, two hours of CPU time, one process, and UI restrictions (no clipboard, no desktop
  switching, no global atoms). It starts suspended, goes into the job, then resumes, so it can never
  outlive ShiftDrop even for a moment.
- **Mitigation policies** block extension points and loading images from remote or low-integrity
  locations.
- **It gets handles, not paths.** ShiftDrop opens the archive read-only and passes that handle and a
  pipe. The worker streams entries back over the pipe; ShiftDrop checks every frame (sizes, names,
  counts), checks every entry name again, and creates every file itself through `StagedOutput`.
- **Passwords.** When an entry is encrypted, the worker asks over the same pipe (it goes both ways: a
  named pipe with a random name, connected before the worker starts). ShiftDrop shows the password
  box, and the answer goes back in one frame, held in a buffer that is wiped as soon as it is sent
  (`Secret`). It is never logged or written anywhere. A wrong password asks again; Cancel stops the
  job. libarchive decrypts ZIP's own ZipCrypto (and WinZip AES when it is built with a crypto
  library, which vcpkg's default here is not); encrypted 7z and RAR say they can't be opened without
  asking.
- **Entry checks** ([archive/Entries.h](../src/archive/Entries.h)): names with `..`, absolute paths,
  device names (`CON`, `NUL`…) or invalid characters are refused; links are skipped.
- **Bomb limits**: 64 GiB uncompressed in total, 1,000,000 entries, and after the first 1 GiB no more
  than 2000 times the archive's own size. A declared size that doesn't match the data fails.

When AppContainer is unavailable (an old or locked-down Windows, or Wine), the worker runs as a
plain child in the same job and the log says so. `[Sandbox] Archives=0` in settings.ini turns the
AppContainer off.

**ffmpeg in the sandbox** is optional and off by default (`[Sandbox] Media=1` turns it on). An
AppContainer process can't open files by name, so the run is rewritten to use handles: the input
becomes C runtime descriptor 3, the staging output descriptor 4, and ffmpeg may use the `fd`
protocol only ([sandbox/MediaSandbox.h](../src/sandbox/MediaSandbox.h)). Only runs that read one file
into one staging file qualify; anything else runs outside, and the log says why. The tests show
every built-in media recipe works this way with the CPU encoders. GPU encoders and `-hwaccel` need
driver access that a container with no capabilities may not get, and CI has no GPU to check them, so
it stays off until that is confirmed on real hardware.

## Plugins

Plugins are designed but not built. The design, in [plugins.md](plugins.md), runs each plugin action
as a WebAssembly module inside the same sandbox worker, with the app doing all file work, so a
plugin gets two walls around it and can't touch anything but its inputs and its own staging
outputs. The module interface above (`FeatureModule`, `IEngine`) is what a plugin host would plug
into.

## History and undo

[HistoryStore.h](../src/HistoryStore.h) keeps every run in `%LOCALAPPDATA%\ShiftDrop\history.db`
(SQLite in WAL mode): the recipe as it ran (its JSON), and for each file the source, staging path,
output, size, SHA-256, status, error and duration. Status strings are plain text, so any SQLite tool
can read the file. If the database can't be opened, conversions still work, without history or undo.

[Undo.h](../src/Undo.h) undoes one run, all or nothing ([ADR 0005](adr/0005-history-and-undo.md)):

- **Conversions and Duplicate**: every output is hashed again first. If any changed since ShiftDrop
  wrote it, nothing is undone, because deleting it would lose the user's edit. Otherwise the outputs
  go to the Recycle Bin through `IFileOperation`. Nothing is deleted permanently.
- **Move to folder** moves each item back, and is refused if something new sits at an original location.
- **Rename** gives each item its old name back, refused if another file has that name now.
- Outputs the user already deleted are skipped and reported.

The History window (tray menu > History…) lists runs newest first, with Undo and Show files.

## Settings, data and logs

Everything lives in `%LOCALAPPDATA%\ShiftDrop`, which uninstall leaves in place:

| Path | What it is |
|---|---|
| `settings.ini` | All settings (below) |
| `history.db` | Run history for undo |
| `recipes\*.json` | Your own recipes; `web-ready.json` is written as an example the first time |
| `shelf.json` | What is on the Float shelf, where it is, and whether it is switched on |
| `logs\shiftdrop-YYYYMMDD.log` | One log file per day |
| `dropped\` | Temporary copies of files dragged out of browsers and mail apps |
| `libreoffice\` | A private LibreOffice profile for the Office bridge, if LibreOffice is used |

`settings.ini` sections, most of which the tray menu or the Settings window write for you:

| Section | Keys |
|---|---|
| `[Gesture]` | `Convert`, `Tools`, `Reduce`, `Files`: the key that opens each section (`Shift`, `Ctrl`, `Alt` or `None`; never the Win key). Only Shift by default |
| `[Keyboard]` | `ExplorerHotkey`, for example `Ctrl+Shift+Space` |
| `[Appearance]` | `Size` (auto, small, medium, large; default medium), `Style` (solid, glass), `Theme`, `PanelSize` (the top panel's size in percent on top of `Size`, 50 to 300 in steps of 10, default 100; Settings › Top panel size), `Accent` (tide, blue, purple, pink, red, orange, green, graphite, custom, windows; default windows (the user's Windows accent colour), and anything else reads as windows; Settings › Accent colour), `AccentCustom` (the colour of your own as `#RRGGBB`, kept while a preset is chosen). An older `HideCursor` key is ignored and removed |
| `[Features]` | Sections turned off (`Convert=0` and so on) and single slices turned off (`Off=to-tiff,to-bmp`) |
| `[Output]` | `Naming` (output name style), `VirtualFiles` (desktop, downloads, ask) |
| `[FileActions]` | `Terminal` (wt, powershell, cmd, custom), `TerminalCommand` |
| `[Startup]` | `StartWithWindows` (1 or 0) |
| `[Sandbox]` | `Archives` (1 by default), `Media` (0 by default) |
| `[Ocr]` | `Language`: the text recognition language, or `auto` |

**Start with Windows** is a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
([Startup.h](../src/Startup.h)), on by default and kept pointing at the current exe. Turning it off in
Task Manager's Startup apps keeps working, because that switch lives elsewhere and ShiftDrop never
touches it.

**Toasts.** An installed copy has a Start menu shortcut with an app ID and a toast activator, so
ShiftDrop can show real Windows toasts with Open folder and Open buttons
([toast/](../src/toast)). A finished batch gets one when a later job has taken the top panel, or
when it took over 10 seconds. A portable copy without the shortcut falls back to a tray balloon.

**The log** ([Log.h](../src/Log.h)) is thread-safe UTF-8, one file per day. It records startup
(versions, which codecs WIC has, where ffmpeg is, which H.264 encoder won), each job, and every
warning. It is the first thing to read when something goes wrong. The mouse hook never logs.

**The command line** is used by the installer and the shell: `--autostart` (started at sign-in),
`--wheel <paths>` (the right-click entry), `--register-shell` and `--unregister-shell`, `--quit`
(asks a running copy to exit) and `--uninstall` (quits and removes the Run value, the right-click
entry, the toast registration and the sandbox profile).

## Speed

Conversions should feel instant on an ordinary PC, with or without a graphics card. What keeps them
quick:

- **Lanes.** Photos and songs run side by side on several workers; videos run one at a time, since
  one ffmpeg run already uses every core.
- **Images.** A JPEG that will be shrunk anyway is decoded at 1/2, 1/4 or 1/8 size by WIC, never below
  the final size. An opaque picture goes to the encoder as 24-bit BGR, with no alpha to add or
  flatten. libwebp gets a second thread. AVIF uses aom speed 8: on a 12 MP photo that was about
  three times faster than libavif's default for a file about 8% bigger.
- **PDF pages and pictures** are encoded on several threads (`pdf::ForEachParallel` in
  [pdf/Parallel.h](../src/pdf/Parallel.h)); reading and writing the PDF itself stays on one.
- **ffmpeg.** Copy streams whenever the target container takes them, read a file once where one pass
  is enough (Normalize measures with `ebur128` and applies one plain gain when the peaks allow it),
  and never pass `-hwaccel` without a GPU encoder.
- **Archives.** A ZIP stores photos, video, music and other archives as they are. Deflate would gain
  a percent or two on them, at about a second per 30 MB.
- **Measure it.** `SHIFTDROP_BENCH=1 shiftdrop_tests --gtest_filter=Bench.*` times every main
  conversion on generated files and prints a table ([tests/BenchTests.cpp](../tests/BenchTests.cpp)).
  In CI, label a pull request `bench` and the table lands on the run's summary page. Run it before
  and after a change that could make something slower.
