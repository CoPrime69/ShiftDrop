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
  "ring": "convert",            // section: convert, tools, reduce or files; default convert
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
| `image.compress` | `maxKB` 0–10000000, `percent` 0–99, `quality` 1–100 (default 75) | Last step. JPEG (WebP stays WebP). With `maxKB` or `percent` (of the source's size): the highest quality that fits, scaling the picture down only when even quality 40 is too big; never writes a file over the target. With neither: one encode at `quality`, refused if it isn't smaller than the source |
| `media.convert` | `format` mp4, mov, mkv, webm, avi, wmv, gif, mp3, m4a, wav, flac, ogg, opus, aiff, wma | Only step. Streams the target can hold are copied (no quality loss, near instant) |
| `media.compress` | `targetMB` 0–100000, `percent` 0–99, `maxHeight` 0–4320 (default 1080, 0 = keep) | Only step. H.264 + AAC MP4 (AAC M4A for audio). With a target: bitrates and height planned from the duration, up to 3 tries, never over the target. With neither: a quality preset that must come out smaller |
| `file.copy` | | Clipboard; paste in Explorer |
| `file.move` | | Asks for a folder, then moves; undoable |
| `file.copy_path` | | Quoted paths, one per line |
| `file.terminal` | | Windows Terminal, else PowerShell, in the file's folder |
| `file.duplicate` | | "name (copy).ext" next to the original; undoable |

Orientation from EXIF is always applied first, and transparent pixels become white for formats
without alpha (JPG, BMP).
