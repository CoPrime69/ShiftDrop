# Recipes

Every wheel slice is a recipe: a small JSON file that names one or more steps. The built-in slices
are recipes too. Your own recipes live in `%LOCALAPPDATA%\ShiftDrop\recipes\*.json` and are read at
startup (tray menu > Open recipes folder, then quit ShiftDrop from the tray and start it again).

```jsonc
{
  // Comments are allowed.
  "id": "web-ready",            // letters, digits, - _ . ; the same id as a built-in replaces it
  "label": "Web",               // text on the slice
  "description": "Resize to 2048 px, WebP quality 80, no metadata",
  "ring": "convert",            // convert (Shift), tools (Ctrl) or files (Alt); default convert
  "suffix": "converted",        // output "photo (converted).webp"; default converted
  "steps": [
    { "action": "image.resize", "params": { "maxEdge": 2048 } },
    { "action": "image.strip_metadata" },
    { "action": "image.encode", "params": { "format": "webp", "quality": 80 } }
  ]
}
```

Rules:
- Image recipes end with exactly one `image.encode`, and only the last step may be one.
- A file action (`file.*`) must be the recipe's only step.
- Unknown fields and out-of-range values are errors. A broken file is skipped and named in a
  notification and in the log; the other recipes still load.
- A wheel shows at most 8 slices. Built-ins come first, then your files sorted by file name.
- All steps run in memory, so a recipe never writes intermediate files and encodes only once.

## Actions

| Action | Parameters | Notes |
|---|---|---|
| `image.resize` | `maxEdge` 16–65535 (default 2048) | Longest edge at most this; never upscales |
| `image.scale` | `percent` 1–100 (default 50) | |
| `image.rotate` | `degrees` "90", "180", "270" (default "90") | Clockwise |
| `image.flip` | `axis` "horizontal", "vertical" | |
| `image.strip_metadata` | | Outputs never carry source metadata in M2; this step documents the intent |
| `image.encode` | `format` png, jpg, webp, tiff, bmp, same (default png); `quality` 1–100 (default 90) | `same` keeps the source format, or PNG if it can't be written |
| `file.copy` | | Clipboard; paste in Explorer |
| `file.move` | | Asks for a folder, then moves; undoable |
| `file.copy_path` | | Quoted paths, one per line |
| `file.terminal` | | Windows Terminal, else PowerShell, in the file's folder |
| `file.duplicate` | | "name (copy).ext" next to the original; undoable |

Orientation from EXIF is always applied first, and transparent pixels become white for formats
without alpha (JPG, BMP).
