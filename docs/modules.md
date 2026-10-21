# Feature modules

Everything beyond the core image, media and file paths is a **feature module**: PDF, archives, video
and audio tools, photo editors, documents. A module lives in its own folder and plugs into the core
in one place, so features can be built side by side without touching each other.

## The contract

- **Files:** `src/<area>/` (for example `src/pdf/`), plus `tests/<Area>Tests.cpp`. Nothing of a
  module lives in another module's folder.
- **Registration:** one `FeatureModule` (see [src/Engine.h](../src/Engine.h)) listed in
  [src/Modules.cpp](../src/Modules.cpp):
  - `registerActions` adds the module's actions with `ActionRegistry::Add`. Ids are
    `<area>.<verb>` (`pdf.merge`), kind `StepKind::Engine`, so each is a recipe's only step.
  - `builtInRecipes` returns a JSON array of slices, in the format of `RecipeStore::BuiltInJson`
    (`id`, `label`, `description`, `ring`, `suffix`, optional `icon` as a Segoe Fluent Icons code
    point like `"E7AD"`). Ids are unique across the app; prefix them with the area when in doubt.
  - `makeEngine` returns the module's `IEngine`.
- **Engine:** `Handles`, `Applies`, `Accepts`, `IsNoOp`, `Lane`, then either `Run` (one output per
  file) or `IsBatch` + `RunBatch` (one output from all files). `AppliesToDrop` hides a slice for
  a drop of the wrong size (a collage needs two pictures). Tools that need the user first
  (a trim range, a crop box) return true from `NeedsEditor` and open their own window in
  `OpenEditor` on the UI thread, then call `done` with the chosen params.
- **Outputs:** always through `StagedOutput` ([src/StagedOutput.h](../src/StagedOutput.h)): a
  staging file or folder next to the source, a no-replace rename to `name (suffix).ext`, a hash,
  and a history row, so every result can be undone and a crash leaves nothing half-written.
  A batch output can pass an `OutputPlace` (folder + stem) to be named after something other than
  its source, such as "Photos (zipped).zip" for a drop of a folder's files.
  Files dragged out of a browser or a mail app are saved in a private drop folder first, and their
  outputs go to the Desktop (or the folder the user chose): an engine that needs an output folder
  asks `OutputFolder(source)` (OutputPath.h), never the source's own folder.
- **Untrusted input:** parse it in the sandbox worker (`src/sandbox`, `engines\ShiftDropWorker.exe`:
  AppContainer, no capabilities, kill-on-close job) and let the app write the files, as
  `src/archive` does.
- **ffmpeg:** through `EngineServices::ffmpeg` (`FfmpegEngine::Run`, `Probe`), always with
  `FfmpegArgs` (local files only), on `JobLane::Media` (video) or `JobLane::Audio` (sound only).
- **Rules that hold everywhere:** offline (no network calls), no ML models, no API keys, no GPL or
  AGPL code linked in or bundled, never overwrite a file, never use the Win key.

## Shared files a module may touch

Keep these edits to a few lines, so parallel work merges cleanly:
`src/Modules.cpp` (one include, one entry), `CMakeLists.txt` (the module's sources and test file),
`vcpkg.json` (new dependencies), `docs/PLAN.md` (decision rows, appended), `docs/TESTING.md` (a new
section), `README.md` (one line in the wheel table). Changes to core files (`main.cpp`, `Overlay`,
`Island`, `PipelineRunner`) are kept to what the module cannot do without, and described in the PR.

## Speed

Conversions should feel instant on any PC, with or without a graphics card. What keeps them quick:

- **Lanes** (`Lane` in [src/Engine.h](../src/Engine.h)): `Pool` and `Audio` have one worker per
  logical processor but one (at most 8, `JobQueue::PoolThreads`), so a batch of photos or songs
  runs side by side. `Media` runs one video at a time, since one ffmpeg run already uses every
  core. `Shell` is the single STA thread. Pick the lane by what the work keeps busy.
- **Images:** a JPEG that is shrunk anyway is decoded at 1/2, 1/4 or 1/8 size in WIC (never below
  the final size, so the result is the same size), an opaque picture goes to the encoder as 24-bit
  BGR (no alpha to add or flatten), and libwebp gets a second thread.
- **PDF pages and pictures:** `pdf::ForEachParallel` ([src/pdf/Parallel.h](../src/pdf/Parallel.h))
  encodes pages (PDF to images) and pictures (Compress) on several threads; reading and writing the
  PDF itself stays on one.
- **ffmpeg:** copy streams whenever the target container takes them, read a file once where one
  pass is enough (Normalize measures with `ebur128` and applies one plain gain when the peaks allow
  it), and never pass `-hwaccel` without a GPU encoder.
- **Archives:** a ZIP stores photos, video, music and other archives as they are (Deflate gains a
  percent or two on them, at a second per 30 MB).
- **Measure it:** `SHIFTDROP_BENCH=1 shiftdrop_tests --gtest_filter=Bench.*` times every main
  conversion on generated files and prints a table ([tests/BenchTests.cpp](../tests/BenchTests.cpp)).
  In CI, label a pull request `bench` (or "Run workflow") and the table lands on the run's summary
  page. Run it before and after a change that could make something slower.
