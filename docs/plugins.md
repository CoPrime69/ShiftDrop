# Plugins (design, not built yet)

Nothing in this document is built. It fixes the contract, so plugins can be added later without
changing the sandbox or the module plumbing. Comments on it are welcome in an issue.

## Goal

Let people add their own actions (a new format, a rename rule, a checksum, a text clean-up) without
giving that code the rights of ShiftDrop itself. A plugin can read the files it was handed and write
the outputs it was asked for. It cannot open other files, reach the network, start processes, or see
the screen, the clipboard or the keyboard.

## Shape

- **One WebAssembly module per action.** A plugin folder holds `plugin.json` and one `.wasm` file for
  each action it offers. A module exports one entry point; it never shares memory with another
  action.
- **The interpreter runs inside the sandbox worker.** `ShiftDropWorker.exe` gains a `plugin-run` job
  next to `archive-read`. It embeds a small WebAssembly interpreter (wasm3, MIT, or a similar
  interpreter with no JIT; no JIT means no writable and executable memory, so plugin runs can add
  the prohibit-dynamic-code mitigation on top of the worker's current policy, which already blocks
  extension points and images from remote or low-integrity locations). The worker already runs in the AppContainer with no capabilities and in a
  kill-on-close job with memory and CPU limits, so a plugin gets two walls: the WebAssembly
  sandbox, and the AppContainer around the process if the interpreter is ever broken.
- **The parent does all file work.** Like archives, the worker never opens a path. ShiftDrop opens
  each input read-only, creates each output as a `.sdpart` staging file through `StagedOutput`, and
  hands the worker only those handles. Committing, naming ("name (suffix).ext"), history and undo
  stay in the app, so a plugin can never overwrite or delete anything.
- **A JSON manifest describes everything the wheel needs**, so the wheel and the predicted-output
  card never load or run plugin code.

## Manifest (`plugin.json`)

```json
{
  "id": "example.checksums",
  "name": "Checksums",
  "version": "1.0.0",
  "abi": 1,
  "actions": [
    {
      "id": "example.checksums.sha1-list",
      "module": "sha1-list.wasm",
      "label": "SHA-1 list",
      "description": "A text file with the SHA-1 of every file",
      "ring": "tools",
      "icon": "E943",
      "accepts": { "extensions": ["*"], "folders": false, "maxFiles": 500 },
      "output": { "kind": "file", "extension": ".txt", "suffix": "sha1", "batch": true },
      "params": [
        { "name": "uppercase", "type": "bool", "default": false }
      ],
      "limits": { "memoryMB": 256, "cpuSeconds": 120 }
    }
  ]
}
```

- `id` values are reverse-DNS strings; an action id must start with the plugin id, so it can never
  collide with a built-in action (`image.*`, `media.*`, `archive.*`, ...).
- `abi` is the host interface version below. ShiftDrop refuses a plugin whose `abi` it does not know.
- `accepts` is checked by the app (the engine's `Applies` / `Accepts`), not by the plugin.
- `output.kind` is `file` (one output per input, or one for the batch when `batch` is true) or
  `folder` (the plugin writes several named files into a staging folder, as Extract does).
- `params` use the same types and validation as recipe params (docs/recipes.md), so a user recipe
  can preset them.
- `limits` can only lower the worker's defaults (2 GiB, 2 h), never raise them.

Plugins live in `%LOCALAPPDATA%\ShiftDrop\plugins\<id>\`. The app reads manifests at start and
registers each action through one `FeatureModule` (`src/plugins/`) whose engine runs every plugin
action; no other module changes.

## Host interface (abi 1)

The module imports a few functions from a `shiftdrop` namespace and exports `run`. Every handle is a
small integer the host gives out; the module never sees a Windows handle or a path.

| Import | What it does |
|---|---|
| `input_count() -> i32` | Number of inputs handed in |
| `input_name(i, buf, len) -> i32` | The input's file name (UTF-8, no folder) |
| `input_size(i) -> i64` | Its size in bytes |
| `input_read(i, offset, buf, len) -> i32` | Read bytes; returns the count, 0 at the end |
| `output_create(name, len) -> i32` | Open an output; for `file` outputs the name is ignored, for `folder` it is a relative name that the host checks with the same rules as archive entries (no absolute paths, no `..`, no device names) |
| `output_write(o, buf, len) -> i32` | Append bytes; refused past the size cap |
| `output_close(o) -> i32` | Finish an output |
| `param_get(name, len, buf, buflen) -> i32` | A param's value as JSON |
| `progress(done, total)` | Progress for the top panel |
| `cancelled() -> i32` | 1 once the person pressed ×; the host also kills the worker after a grace period |
| `log(level, buf, len)` | A line in ShiftDrop's log, prefixed with the plugin id and rate-limited |
| `fail(buf, len)` | End the run with a message the top panel shows |

`run() -> i32` returns 0 for success. On any failure, trap or kill, the host deletes every staging
file of the run, exactly as for a cancelled archive extract.

Over the existing worker pipe this becomes a small request/response protocol: the worker sends
`read`, `create`, `write`, `close` and `progress` frames, and the parent answers. The parent checks
every frame (handle numbers, offsets, sizes, names, the per-run byte and file caps) as it does for
the archive entry stream, because the worker is untrusted.

## What a plugin cannot do

- Open a file by path, list a folder, or touch anything except its inputs and its own staging outputs.
- Use the network: the AppContainer has no capabilities, and the interpreter has no socket imports.
- Start processes, load DLLs, or run native code: modules are interpreted; the job allows one process.
- Change the wheel's behaviour, the history, or another action's recipes.
- Keep state between runs: every run starts a fresh worker.

## Trust and installation

- Installing a plugin is copying its folder; there is no download from inside ShiftDrop (offline rule).
- The tray menu lists installed plugins with their actions and lets each be turned off.
- A plugin's `.wasm` files are hashed (SHA-256) when first seen; a changed hash turns the plugin off
  until the person confirms it again.

## Open points

1. Pick the interpreter (wasm3 is MIT and small; check its maintenance status at that time) and a
   C++ toolchain story for plugin authors (wasi-sdk, with only the imports above, no WASI files).
2. Speed: an interpreter is 5–20 times slower than native code. Fine for text, names and checksums;
   image or media plugins would need a JIT, which conflicts with the prohibit-dynamic-code mitigation.
3. Whether plugins may produce a prediction for the predicted-output card (a separate `predict` export with no
   file access) or always show "~?".
