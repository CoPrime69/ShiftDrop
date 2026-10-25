# Contributing to ShiftDrop

Thanks for helping. Bug reports, tested fixes, new formats and better docs are all welcome.

## Before you start

- **Found a bug?** Open an issue with the bug template. Say what you dragged, which slice you
  dropped it on, what happened, and paste the lines from that day's log
  (`%LOCALAPPDATA%\ShiftDrop\logs\shiftdrop-YYYYMMDD.log`; tray menu > Open logs folder).
- **Want a feature?** Open an issue first, so we can agree on the shape before you write code. Check
  [the limitations in the README](README.md#limitations): some gaps are there on purpose.
- **Security problem?** Don't open an issue. Follow [SECURITY.md](SECURITY.md).

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## Getting set up

[docs/BUILDING.md](docs/BUILDING.md) takes you from a fresh clone to a running build and green tests.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains how the pieces fit, and is worth reading before a
change to the gesture, the wheel or the job pipeline.

## Rules the code keeps

These hold everywhere. A pull request that breaks one won't be merged, however useful it is.

- **Offline.** No network calls, no telemetry, no update checks, no downloads at run time. CI fails
  if `ShiftDrop.exe` or `ShiftDropWorker.exe` imports a networking DLL.
- **No GPL or AGPL code**, linked in or bundled. Permissive licences (MIT, BSD, Apache-2.0, zlib) are
  fine. LGPL is only allowed for `ffmpeg.exe`, which runs as a separate program.
- **No machine-learning models** bundled or downloaded. Windows' own OCR and face detection are fine,
  because they ship with Windows.
- **Never overwrite or lose a file.** Every output goes through `StagedOutput` (or `OutputPath`'s
  staging and commit), so it gets a fresh name, lands atomically and can be undone.
- **Never move the original on a drop.** The drop target always answers COPY.
- **Untrusted input is parsed in the sandbox worker** when it can be (see archives), and the app
  writes the files.
- **Leave the Win key alone.**

## Code style

- C++20, Win32 and COM, no frameworks. Builds clean at `/W4 /permissive-` with MSVC.
- RAII everywhere: `wil` handles and `Microsoft::WRL::ComPtr`. No bare `new` or `delete`.
- Four spaces, braces on their own line, `namespace sd` (a module gets `sd::<area>`). Match the file
  you are in.
- Comments say *why*, and what a reader can't see from the code. Keep them short.
- Nothing slow on the UI thread, and nothing at all in the mouse hook callback beyond recording
  state and posting a message.
- Text the user sees is plain and short: "3 files converted to PNG", not "Operation completed
  successfully".

## Adding a feature

Most new features are a *feature module*: a folder under `src/`, one entry in `src/Modules.cpp`, and
a test file. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#feature-modules) has the contract. Keep edits
to shared files small so work in parallel merges cleanly:

- `src/Modules.cpp`: one include and one entry.
- `CMakeLists.txt`: the module's sources and its test file.
- `vcpkg.json`: new dependencies (see below).
- `docs/TESTING.md`: a section for the hand checks, with the next free letter.
- `README.md`: the new slices in the feature list.

Changes to core files (`main.cpp`, `Overlay`, `Island`, `PipelineRunner`) should be limited to what
the module can't do without, and explained in the pull request.

### A new dependency

1. Check its licence (see the rules above) and that vcpkg has a port.
2. Add it to `vcpkg.json` and to `find_package` / `target_link_libraries` in `CMakeLists.txt`.
3. Add the port, and any port it pulls in, to the list in `scripts/make-notices.ps1`, so its
   copyright file reaches the installer.
4. Add a row to [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

### A new ffmpeg codec, format or filter

Add its name to `scripts/ffmpeg-components.txt`. The `FfmpegComponents` unit tests fail until every
codec, format and filter the argument builders use is listed there, and CI rebuilds the small
`ffmpeg.exe` when the list changes.

## Pull requests

Only the maintainer pushes to this repository. To send a change:

1. Fork the repository on GitHub and clone your fork.
2. Create a branch from `main` for your change.
3. Push the branch to your fork and open a pull request against `main` here.
4. CI runs once the maintainer approves it for your first pull request. The maintainer reviews the
   change, may ask for edits, and merges it.

- One topic per pull request. A refactor and a feature are two pull requests.
- CI must be green: build, unit tests, the offline check and the installer smoke test.
- Add or update unit tests for anything that can run without a person at the mouse.
- For anything you can only see by hand, run the matching sections of
  [docs/TESTING.md](docs/TESTING.md) and say which ones in the pull request.
- Write commit messages that say what changed and why, in the imperative: "Keep EXIF on cropped
  HEIC files", not "fixes".
- Add a line under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md) (Added, Changed, Fixed or
  Removed) for anything a person would notice. Don't change [VERSION](VERSION): the maintainer does
  that when releasing.

## Versions

ShiftDrop follows [Semantic Versioning 2.0.0](https://semver.org/): PATCH for fixes only, MINOR for
new features (before 1.0 a minor release may also change behaviour), MAJOR for breaking changes
(from 1.0 on). Releases are tagged `vX.Y.Z`; every other build is a pre-release of the next version,
such as `0.7.1-dev.3+af953b2`. [docs/BUILDING.md](docs/BUILDING.md#version-numbers) has the details
and how to release.

## Licence

ShiftDrop is under the [MIT licence](LICENSE). By sending a pull request you agree that your
contribution is released under the same licence.
