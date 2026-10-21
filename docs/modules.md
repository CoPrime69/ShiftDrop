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
  file) or `IsBatch` + `RunBatch` (one output from all files). Tools that need the user first
  (a trim range, a crop box) return true from `NeedsEditor` and open their own window in
  `OpenEditor` on the UI thread, then call `done` with the chosen params.
- **Outputs:** always through `StagedOutput` ([src/StagedOutput.h](../src/StagedOutput.h)): a
  staging file or folder next to the source, a no-replace rename to `name (suffix).ext`, a hash,
  and a history row, so every result can be undone and a crash leaves nothing half-written.
  A batch output can pass an `OutputPlace` (folder + stem) to be named after something other than
  its source, such as "Photos (zipped).zip" for a drop of a folder's files.
- **Untrusted input:** parse it in the sandbox worker (`src/sandbox`, `engines\ShiftDropWorker.exe`:
  AppContainer, no capabilities, kill-on-close job) and let the app write the files, as
  `src/archive` does.
- **ffmpeg:** through `EngineServices::ffmpeg` (`FfmpegEngine::Run`, `Probe`), always with
  `FfmpegArgs` (local files only), on `JobLane::Media`.
- **Rules that hold everywhere:** offline (no network calls), no ML models, no API keys, no GPL or
  AGPL code linked in or bundled, never overwrite a file, never use the Win key.

## Shared files a module may touch

Keep these edits to a few lines, so parallel work merges cleanly:
`src/Modules.cpp` (one include, one entry), `CMakeLists.txt` (the module's sources and test file),
`vcpkg.json` (new dependencies), `docs/PLAN.md` (decision rows, appended), `docs/TESTING.md` (a new
section), `README.md` (one line in the wheel table). Changes to core files (`main.cpp`, `Overlay`,
`Island`, `PipelineRunner`) are kept to what the module cannot do without, and described in the PR.
