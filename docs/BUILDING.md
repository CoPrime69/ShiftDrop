# Building ShiftDrop

This guide goes from a fresh clone to a running build, green tests and an installer. Everything
below is what CI does too ([.github/workflows/ci.yml](../.github/workflows/ci.yml)), so when CI and
this page disagree, CI is right and this page needs a fix.

- [What you need](#what-you-need)
- [Set up vcpkg](#set-up-vcpkg)
- [Get ffmpeg](#get-ffmpeg)
- [Configure and build](#configure-and-build)
- [Run it](#run-it)
- [Run the tests](#run-the-tests)
- [Build the installer](#build-the-installer)
- [Build ffmpeg yourself](#build-ffmpeg-yourself)
- [How CI fits together](#how-ci-fits-together)
- [When something goes wrong](#when-something-goes-wrong)

## What you need

- Windows 10 22H2 or Windows 11, 64-bit.
- [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is fine) with the
  **Desktop development with C++** workload. It brings MSVC, the Windows SDK and CMake 3.24 or later.
  ShiftDrop only builds with MSVC; CMake stops with an error on any other compiler.
- [Git](https://git-scm.com/).
- For the installer only: [Inno Setup 6.3 or later](https://jrsoftware.org/isinfo.php).

Run every command below in **Developer PowerShell for VS 2022** (Start menu), from the repository
root. That shell puts `cmake`, MSVC and `dumpbin` on the PATH.

## Set up vcpkg

All libraries come from [vcpkg](https://vcpkg.io) in manifest mode: [vcpkg.json](../vcpkg.json) lists
them and pins their versions with `builtin-baseline`. You don't install them by hand; CMake asks
vcpkg to build them on the first configure.

The presets find vcpkg through the `VCPKG_ROOT` environment variable. They load
`$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake` as the CMake toolchain, so the variable must point
at a vcpkg folder before you configure. A standalone clone is the simplest:

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')   # for new shells
$env:VCPKG_ROOT = 'C:\dev\vcpkg'                                              # for this one
```

The copy of vcpkg that comes with Visual Studio works too, as long as `VCPKG_ROOT` points at it and it
knows the baseline commit (see [When something goes wrong](#when-something-goes-wrong)).

Everything is linked statically with the static CRT, so the result is a single `ShiftDrop.exe` that
needs no runtime installed. The triplet is `x64-windows-static`, with a small overlay in
[triplets/x64-windows-static.cmake](../triplets/x64-windows-static.cmake) that builds aom as an
encoder only (dav1d decodes AVIF).

Then clone ShiftDrop:

```powershell
git clone https://github.com/CoPrime69/ShiftDrop C:\dev\shiftdrop
cd C:\dev\shiftdrop
```

## Get ffmpeg

Video and audio go through `ffmpeg.exe`, which ShiftDrop runs as a separate program from an
`engines` folder next to its own exe. Fetch it **before** you configure, because CMake checks for it
at configure time:

```powershell
./scripts/get-ffmpeg.ps1
```

[scripts/get-ffmpeg.ps1](../scripts/get-ffmpeg.ps1) puts `ffmpeg.exe`, `FFMPEG-LICENSE.txt` and
`FFMPEG-SOURCE.txt` in `build\engines`. It picks one of two builds:

1. **ShiftDrop's own small build**, when `build\ffmpeg-custom\ffmpeg.exe` exists. CI makes it in its
   `ffmpeg` job. To use it locally, download the `ffmpeg-custom-<commit>` artifact of any CI run into
   `build\ffmpeg-custom`, for example with the GitHub CLI:
   `gh run download <run-id> -n ffmpeg-custom-<commit> -D build/ffmpeg-custom`.
   Or build it yourself, see [Build ffmpeg yourself](#build-ffmpeg-yourself).
2. **Otherwise, a pinned full LGPL build** from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds)
   (the `win64-lgpl` variant, no GPL parts). The script downloads the 170 MB zip once into
   `build\downloads`, checks it against a pinned SHA-256 and stops if it doesn't match. This build
   has everything the small one has and more, so it is about three times the size.

Options: `-Out` (default `build/engines`), `-Downloads` (`build/downloads`), `-Custom`
(`build/ffmpeg-custom`) and `-Btbn`, which skips the custom build even when it is there.

To check that an `ffmpeg.exe` has every codec, format and filter ShiftDrop uses:

```powershell
./scripts/check-ffmpeg.ps1 -Exe build/engines/ffmpeg.exe
```

It compares the exe against [scripts/ffmpeg-components.txt](../scripts/ffmpeg-components.txt).

You can skip ffmpeg entirely. The app still builds and runs, with the video and audio slices dimmed,
and the tests that need ffmpeg skip themselves.

## Configure and build

[CMakePresets.json](../CMakePresets.json) has the presets:

| Preset | Kind | What it does |
|---|---|---|
| `x64-release` | configure, build, test | Visual Studio 2022 generator, x64, triplet `x64-windows-static`, output in `build\x64-release`, Release configuration |
| `x64-release-m0` | configure, build | The same with `SHIFTDROP_M0=ON`: a placeholder build that draws a plain circle and only logs each drop. Useful when you work on the mouse hook or the drop target alone. Output in `build\x64-release-m0` |
| `ci` | configure, build, test | What CI uses: the Ninja generator, Release, every compile through [sccache](https://github.com/mozilla/sccache) (it must be on `PATH`), vcpkg's ports in `build\vcpkg_installed`, output in `build\ci`, tests four at a time. Run it from a Developer PowerShell |

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

The first configure builds every library in `vcpkg.json` from source, which takes a while. Later
configures reuse vcpkg's binary cache and take seconds.

CMake options, if you need them (pass with `-D` on the configure line):

| Option | Default | Meaning |
|---|---|---|
| `SHIFTDROP_TESTS` | `ON` | Build `shiftdrop_tests` and register the tests with CTest |
| `SHIFTDROP_M0` | `OFF` | The placeholder build (set by the `x64-release-m0` preset) |
| `SHIFTDROP_ENGINES_DIR` | `build/engines` | Where CMake looks for `ffmpeg.exe` |

The build makes:

- `build\x64-release\Release\ShiftDrop.exe`, the app.
- `build\x64-release\Release\engines\ShiftDropWorker.exe`, the sandboxed worker (copied there after
  every build, so the build folder runs like an install).
- `build\x64-release\Release\engines\ffmpeg.exe`, copied from `build\engines` when it was there at
  configure time.
- `build\x64-release\Release\shiftdrop_tests.exe`, the unit tests.

The placeholder build:

```powershell
cmake --preset x64-release-m0 -DSHIFTDROP_TESTS=OFF
cmake --build --preset x64-release-m0
```

CI builds it with the `ci` preset instead (`-DSHIFTDROP_M0=ON -DSHIFTDROP_TESTS=OFF`, then
`cmake --build --preset ci --target ShiftDrop`).

## Run it

```powershell
.\build\x64-release\Release\ShiftDrop.exe
```

A tray icon appears. Drag a file, press Shift, drop it on a slice. Only one copy runs at a time; quit
from the tray menu, or `Stop-Process -Name ShiftDrop`.

ShiftDrop keeps its data in `%LOCALAPPDATA%\ShiftDrop`, and a development build uses the same folder
as an installed copy. The log is the first place to look:

```powershell
Get-Content "$env:LOCALAPPDATA\ShiftDrop\logs\shiftdrop-$(Get-Date -Format yyyyMMdd).log" -Wait -Tail 30
```

Two environment variables help during development: `SHIFTDROP_FFMPEG` points the app at another
`ffmpeg.exe`, and `SHIFTDROP_WORKER` at another `ShiftDropWorker.exe`.

## Run the tests

```powershell
ctest --preset x64-release
```

The `x64-release` test preset prints the output of any test that fails. The tests are GoogleTest
tests in [tests/](../tests), one file per module, and they run without a person at the mouse. Some
skip themselves when the PC lacks what they need: the media tests without `ffmpeg.exe`, the OCR tests
when Windows OCR is not usable, the PDF rendering tests without Windows' PDF renderer.

When `build\engines\ffmpeg.exe` existed at configure time, CTest hands it to the tests through
`SHIFTDROP_FFMPEG`. If you fetched ffmpeg after configuring, configure again.

To run a few tests straight from the exe:

```powershell
.\build\x64-release\Release\shiftdrop_tests.exe --gtest_filter=Recipe*
```

Two groups print timings. The speed benchmark is skipped unless `SHIFTDROP_BENCH=1` is set; the
photo timings run with the other tests, and you can run them on their own:

```powershell
# Speed benchmark: times every main conversion on generated files and prints a table
$env:SHIFTDROP_BENCH = '1'
$env:SHIFTDROP_FFMPEG = (Resolve-Path build/engines/ffmpeg.exe).Path
.\build\x64-release\Release\shiftdrop_tests.exe --gtest_filter=Bench.*

# Photo tools' export times for a 12 MP photo
.\build\x64-release\Release\shiftdrop_tests.exe --gtest_filter=PhotoTiming.*
```

The offline check fails if either exe imports a networking DLL. It needs `dumpbin`, so run it from
the Developer PowerShell:

```powershell
./scripts/check-imports.ps1 -Exe build/x64-release/Release/ShiftDrop.exe
./scripts/check-imports.ps1 -Exe build/x64-release/Release/ShiftDropWorker.exe
```

What the unit tests can't see (the wheel, the windows, Explorer integration) is in the hand
checklist, [TESTING.md](TESTING.md).

## Build the installer

The installer is an [Inno Setup](https://jrsoftware.org/isinfo.php) script,
[installer/ShiftDrop.iss](../installer/ShiftDrop.iss). It installs per user into
`%LOCALAPPDATA%\Programs\ShiftDrop` with no admin rights. After a Release build:

```powershell
./scripts/get-ffmpeg.ps1       # if you haven't already
./scripts/make-notices.ps1
$version = (Select-String -Path src/Brand.h -Pattern '#define SD_VERSION L"([^"]+)"').Matches[0].Groups[1].Value
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /Qp "/DAppVersion=$version" installer\ShiftDrop.iss
```

- [scripts/make-notices.ps1](../scripts/make-notices.ps1) writes `build\installer\THIRD-PARTY-NOTICES.txt`
  from the copyright files vcpkg installed under `build\x64-release\vcpkg_installed`, plus ffmpeg's
  notices from `build\engines`. It stops if one is missing.
- The version comes from `SD_VERSION` in [src/Brand.h](../src/Brand.h).
- The result is `build\installer\ShiftDrop-Setup-<version>.exe`.

CI then runs [scripts/test-installer.ps1](../scripts/test-installer.ps1) on it: install, upgrade over a
running copy, keep a saved "start with Windows" choice, uninstall. It uses the real per-user install
locations and writes its logs under `$env:RUNNER_TEMP`, so on your own PC set that to a scratch
folder first, and expect it to install and remove ShiftDrop for your user:

```powershell
$env:RUNNER_TEMP = "$env:TEMP\sd-installer-test"
./scripts/test-installer.ps1 -Setup build/installer/ShiftDrop-Setup-$version.exe
```

## Build ffmpeg yourself

[scripts/build-ffmpeg.sh](../scripts/build-ffmpeg.sh) cross-compiles ShiftDrop's small `ffmpeg.exe`
with MinGW-w64 on Linux. It runs on Ubuntu 24.04 (what CI uses), any recent Debian or Ubuntu, or WSL:

```bash
sudo apt-get install mingw-w64 nasm meson ninja-build cmake pkg-config autoconf automake libtool git curl xz-utils
scripts/build-ffmpeg.sh build/ffmpeg-work build/ffmpeg-custom
```

Both folders are optional arguments and default to those shown. It takes about 15 minutes on four
cores.

Every source is pinned: a git tag and the commit it must resolve to, or a tarball and its SHA-256,
so a moved tag or a changed download stops the build. Only what
[scripts/ffmpeg-components.txt](../scripts/ffmpeg-components.txt) lists is built in, and the build is
LGPL-only: no `--enable-gpl`, no `--enable-nonfree`, and the script checks FFmpeg's `config.h` to be
sure. The output folder gets:

- `ffmpeg.exe`
- `FFMPEG-LICENSE.txt`: the licences of FFmpeg and every library in it
- `FFMPEG-SOURCE.txt`: versions, sources and the exact configure line
- `ffmpeg-sources.tar.xz`: the complete corresponding source

Put that folder at `build\ffmpeg-custom` on the Windows side and run `./scripts/get-ffmpeg.ps1`.

To add a codec, format or filter, add its name to `ffmpeg-components.txt`. The `FfmpegComponents`
unit tests check that the list covers every name the app's argument builders use, and CI rebuilds
ffmpeg whenever the list or the script changes. [scripts/time-ffmpeg.ps1](../scripts/time-ffmpeg.ps1)
times two builds against each other with the app's own arguments:

```powershell
./scripts/get-ffmpeg.ps1 -Btbn -Out build/engines-btbn
./scripts/time-ffmpeg.ps1 -New build/engines/ffmpeg.exe -NewName ours -Old build/engines-btbn/ffmpeg.exe -OldName BtbN
```

## How CI fits together

[.github/workflows/ci.yml](../.github/workflows/ci.yml) runs on every push to `main`, every pull
request and every `v*` tag. Its jobs:

1. **ffmpeg** (Ubuntu): runs `build-ffmpeg.sh` and uploads the result as `ffmpeg-custom-<commit>`.
   It is cached on the script and the components list, so it only rebuilds when one of them changes.
   Setting the repository variable `SHIFTDROP_FFMPEG` to `btbn` skips it, and the build then ships
   the pinned BtbN build instead.
2. **build** (Windows Server 2022): gets ffmpeg, checks its components, configures with the `ci`
   preset and compiles everything once: the app, the worker and the tests. It runs the offline
   check, writes the third-party notices and uploads the binaries for the jobs below.
3. **tests (1 of 3)**, **(2 of 3)**, **(3 of 3)** (Windows, at the same time): each downloads the
   test exe and runs every third test with CTest (`-I <shard>,<total>,3`), so the three together run
   each test exactly once; each checks that first. Every test runs in its own process, four at a
   time.
4. **installer** (Windows, beside the shards): builds the installer from the uploaded exes, runs the
   installer smoke test and uploads the installer and the exe.
5. **placeholder** (Windows, beside build): builds the placeholder gesture build
   (`SHIFTDROP_M0=ON`), the app target only.
6. **timings** (tags, runs started by hand, pull requests labelled `bench`): the speed benchmark,
   the photo tools' 12 MP export times and our ffmpeg against the BtbN build.
7. **release** (tags only): publishes the installer once every job above passed.

**A push to main reuses its pull request's run.** Every pull-request run marks the tree it tested
(an artifact named `tested-tree-<tree>`). When a push to main has the tree of a pull-request run
that passed, the **reuse** job takes that run's installer and exe over as this run's artifacts and
nothing else runs: merge by pushing the merge commit to the pull request's branch, wait for it to
pass, then fast-forward main to it. A push no passed run covers builds in full, and so does every
tag. "Run workflow" with "reuse" ticked tries the same on any branch.

Because of that, main itself no longer compiles in CI, yet pull requests read main's compiler cache
(GitHub Actions caches belong to a branch; a pull request reads its own and main's).
[.github/workflows/cache.yml](../.github/workflows/cache.yml) keeps it current: after each push to
main it runs ci.yml's build, ffmpeg and placeholder jobs only (no tests, no installer), and nothing
waits for it.

The Windows jobs that compile share their setup and configure steps through
[.github/actions/configure](../.github/actions/configure/action.yml). Every compile goes through
sccache, which keeps the object files in the GitHub Actions cache, so a push that changes a few
files only recompiles those. The installed vcpkg ports are cached as a folder, keyed on
`vcpkg.json`, the triplet and the compiler version; when that cache is there, configure doesn't run
vcpkg at all. Otherwise vcpkg installs them from its own binary cache, also kept between runs. Each
test shard keeps its tests' times from the last run, so it starts the slowest ones first.

The unit tests run the photo timing test on every push all the same; the timings job only prints
its numbers.

The speed benchmark runs only when asked: label a pull request `bench`, or start the workflow by hand
("Run workflow" on the Actions page) with "bench" ticked. Its table goes on the run's summary page.

A tag `v<version>` that matches `SD_VERSION` in `src/Brand.h` publishes a GitHub Release with the
installer and, for the custom ffmpeg build, the ffmpeg source tarball. A tag that doesn't match
fails the release step.

## When something goes wrong

**CMake can't find `vcpkg.cmake`, or the path starts with `/scripts/buildsystems`.** `VCPKG_ROOT` is
not set in this shell. Set it as in [Set up vcpkg](#set-up-vcpkg) and configure again.

**vcpkg says it can't find the baseline commit.** Your vcpkg clone is older than the
`builtin-baseline` in `vcpkg.json`. Update it, as CI does: `git -C $env:VCPKG_ROOT fetch origin`
(or `git pull` in a standalone clone).

**"ShiftDrop is built with MSVC (Visual Studio 2022)".** You configured with another generator or
compiler. Use the presets from a Developer PowerShell.

**Configure prints "No ffmpeg.exe in ...".** Run `./scripts/get-ffmpeg.ps1`, then configure again.
Until then the video and audio slices are dimmed and the media tests are skipped.

**`make-notices.ps1` says a copyright file is missing.** It reads the Release configure's
`vcpkg_installed` folder. Configure with the `x64-release` preset first.

**Starting over.** Delete the `build` folder. vcpkg's binary cache lives outside it, so the next
configure is still quick.
