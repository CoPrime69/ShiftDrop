// What the wheel offers each sample file: the app's own sections, slice labels, icons and wording
// (its recipes in src/RecipeStore.cpp and the feature modules). Sizes are examples for the demo;
// nothing is converted in the browser.

export const SECTIONS = [
  { key: 'convert', title: 'Convert', icon: 'convert' },
  { key: 'tools', title: 'Tools', icon: 'tools' },
  { key: 'reduce', title: 'Reduce size', icon: 'reduce' },
  { key: 'files', title: 'File actions', icon: 'files' },
];

export function formatSize(mb) {
  if (mb >= 100) return `${Math.round(mb)} MB`;
  if (mb >= 1) return `${mb.toFixed(1)} MB`;
  return `${Math.max(1, Math.round(mb * 1024))} KB`;
}

const out = (base, suffix, ext) => `${base} (${suffix}).${ext}`;

// ---- Builders ----------------------------------------------------------------------------------------

function convertSlice(file, label, ext, mb, { icon, detail, done } = {}) {
  return {
    label, icon, detail: detail ?? `Save ${/^[AEFHILMNORSX]/.test(label) ? 'an' : 'a'} ${label} copy next to it`,
    card: { title: label, file: out(file.base, 'converted', ext), size: mb },
    done: done ?? `1 file converted to ${label}`,
  };
}

function reduceSlices(file) {
  return [90, 80, 70, 60, 50, 40, 30, 20].map((p) => {
    const mb = file.mb * (1 - p / 100);
    return {
      label: `\u2212${p}%`, headline: formatSize(mb), detail: `${p}% smaller`,
      card: { title: file.ext.toUpperCase(), file: out(file.base, 'reduced', file.ext), size: mb },
      done: `1 file made ${p}% smaller`,
    };
  });
}

function fileSlices(file, { metadata = false } = {}) {
  const name = `${file.base}.${file.ext}`;
  const s = [
    { label: 'Copy', icon: 'copy', detail: 'Copy to the clipboard', done: `${name} copied` },
    { label: 'Move to folder', icon: 'move', detail: 'Move to a folder you pick', done: 'Pick a folder for it' },
    { label: 'Copy path', icon: 'link', detail: 'Copy the full path', done: `Copied C:\\Users\\You\\Downloads\\${name}` },
    { label: 'Terminal', icon: 'terminal', detail: 'Open a terminal here', done: 'Terminal opened in Downloads' },
    { label: 'Duplicate', icon: 'duplicate', detail: 'Make a copy next to it', card: { title: 'Duplicate', file: out(file.base, 'copy', file.ext), size: file.mb }, done: `Saved ${out(file.base, 'copy', file.ext)}` },
  ];
  if (metadata) s.push({ label: 'Remove metadata', icon: 'tag', detail: 'A copy without EXIF, GPS or tags', card: { title: file.ext.toUpperCase(), file: out(file.base, 'no metadata', file.ext), size: file.mb * 0.99 }, done: 'Location, camera and dates removed' });
  s.push(
    { label: 'Rename', icon: 'rename', detail: 'Rename with a pattern: {name}, {n}, {date}', done: 'Pick a name pattern' },
    { label: 'Float', icon: 'float', detail: 'Keep on the Float shelf', done: 'On the Float shelf' },
    { label: 'Zip', icon: 'zip', detail: 'One ZIP of everything', card: { title: 'ZIP', file: `${file.base}.zip`, size: file.mb * (file.kind === 'photo' || file.kind === 'video' ? 0.97 : 0.4) }, done: `Saved ${file.base}.zip` },
    { label: 'Share', icon: 'share', detail: 'Nearby sharing, Phone Link, mail, apps', done: 'Windows share opened' },
  );
  return s;
}

const IMAGE_FORMATS = [
  ['PNG', 'png', 2.9], ['JPG', 'jpg', 1], ['WebP', 'webp', 0.35], ['AVIF', 'avif', 0.26],
  ['HEIC', 'heic', 0.32], ['TIFF', 'tiff', 10.2], ['BMP', 'bmp', 10.2],
];

function photo(file) {
  const jpgMb = file.ext === 'png' ? file.mb / 2.9 : file.mb;
  const convert = IMAGE_FORMATS.filter(([, ext]) => ext !== file.ext).map(([label, ext, k]) => convertSlice(file, label, ext, jpgMb * k));
  convert.push(
    convertSlice(file, 'Web', 'webp', jpgMb * 0.18, { icon: 'globe', detail: 'Resized for the web, as WebP', done: '1 file made ready for the web' }),
    convertSlice(file, 'PDF', 'pdf', jpgMb * 1.03, { icon: 'pdf', detail: 'One PDF, a page per image', done: '1 PDF made' }),
  );
  const edited = (suffix) => out(file.base, suffix, file.ext);
  const tools = [
    { label: 'Rotate right', icon: 'rotate', detail: 'Rotate 90\u00b0 clockwise', card: { title: 'Rotate right', file: edited('edited'), size: file.mb }, done: '1 file rotated' },
    { label: '1920 px', icon: 'fit', detail: 'Fit within 1920 px', card: { title: '1920 px', file: edited('edited'), size: file.mb * 0.42 }, done: '1 file resized to 1920 px' },
    { label: 'Flip', icon: 'flip', detail: 'Mirror left to right', card: { title: 'Flip', file: edited('edited'), size: file.mb }, done: '1 file flipped' },
    { label: '50%', icon: 'shrink', detail: 'Half the width and height', card: { title: '50%', file: edited('edited'), size: file.mb * 0.3 }, done: '1 file scaled to 50%' },
    { label: 'Rotate left', icon: 'rotateLeft', detail: 'Rotate 90\u00b0 counter-clockwise', card: { title: 'Rotate left', file: edited('edited'), size: file.mb }, done: '1 file rotated' },
    { label: '1080 px', icon: 'fit', detail: 'Fit within 1080 px', card: { title: '1080 px', file: edited('edited'), size: file.mb * 0.16 }, done: '1 file resized to 1080 px' },
    { label: 'Compress', detail: 'Smaller JPEG, quality 75', card: { title: 'JPG', file: out(file.base, 'compressed', 'jpg'), size: jpgMb * 0.55 }, done: '1 file compressed' },
    { label: 'Under 1 MB', detail: 'Best quality that fits in 1 MB', card: { title: 'JPG', file: out(file.base, 'compressed', 'jpg'), size: 0.97 }, done: '1 file now under 1 MB' },
    { label: 'Crop', icon: 'crop', detail: 'Crop to a box, a shape or an exact size', done: 'Crop opened' },
    { label: 'Redact', icon: 'redact', detail: 'Cover faces, text and details for good', done: 'Redact opened' },
    { label: 'Private', icon: 'private', detail: 'A copy without location or camera details', card: { title: 'Private', file: edited('private'), size: file.mb * 0.99 }, done: 'Checked clean: nothing personal left' },
    { label: 'Edit', icon: 'edit', detail: 'Light, colour and detail', done: 'Edit opened' },
    { label: 'Mark up', icon: 'markup', detail: 'Arrows, text, shapes, drawing', done: 'Mark up opened' },
    { label: 'Frame', icon: 'frame', detail: 'On a backdrop with a shadow', done: 'Frame opened' },
    { label: 'Copy text', icon: 'text', detail: 'Read the text and copy it', done: 'Copied 3 lines of text' },
    { label: 'Inspect', icon: 'inspect', detail: 'Every metadata field, searchable', done: '48 metadata fields shown' },
  ];
  return { convert, tools, reduce: reduceSlices(file), files: fileSlices(file, { metadata: true }) };
}

function video(file) {
  const fmts = [['MP4', 'mp4', 0.95, 'Copies the streams: a second or two'], ['MKV', 'mkv', 1], ['WebM', 'webm', 0.39], ['AVI', 'avi', 1.05], ['WMV', 'wmv', 0.56], ['GIF', 'gif', 1.6], ['MP3', 'mp3', 0.017, 'Just the sound, as MP3']];
  const convert = fmts.map(([label, ext, k, detail]) => convertSlice(file, label, ext, file.mb * k, { detail }));
  const c = (suffix, mb) => ({ title: 'MP4', file: out(file.base, suffix, 'mp4'), size: mb });
  const tools = [
    { label: 'Compress', detail: 'H.264 + AAC, up to 1080p', card: c('compressed', file.mb * 0.13), done: '1 file compressed' },
    { label: '50% size', detail: 'Half the file size, guaranteed', card: c('compressed', file.mb * 0.5), done: '1 file at half the size' },
    { label: 'Under 10 MB', detail: 'Fits in 10 MB, for chat apps', card: c('compressed', 9.7), done: '1 file now under 10 MB' },
    { label: 'Under 25 MB', detail: 'Fits in 25 MB, for email', card: c('compressed', 24.6), done: '1 file now under 25 MB' },
    { label: 'Trim', icon: 'trim', detail: 'Keep a part, or cut a part out', done: 'Trim opened' },
    { label: 'Crop', icon: 'crop', detail: 'Exact size, 16:9, 9:16, 1:1, 4:5', done: 'Crop opened' },
    { label: 'Speed', icon: 'speed', detail: '0.25x to 4x, pitch kept or not', done: 'Speed opened' },
    { label: 'Frames', icon: 'frames', detail: 'Save frames as PNG, JPG or WebP', done: 'Frames opened' },
    { label: 'Split', icon: 'split', detail: 'At points you pick, or in equal parts', done: 'Split opened' },
    { label: 'Redact', icon: 'redact', detail: 'Blur or cover areas, each for its own time', done: 'Redact opened' },
    { label: 'Inspect', icon: 'inspect', detail: 'Every metadata field, searchable', done: '31 metadata fields shown' },
  ];
  return { convert, tools, reduce: reduceSlices(file), files: fileSlices(file) };
}

function pdf(file) {
  const convert = [
    convertSlice(file, 'JPG', 'jpg', file.mb * 1.9, { icon: 'image', detail: 'Every page as a JPEG, 300 dpi', done: '4 pages saved as JPG' }),
    convertSlice(file, 'PNG', 'png', file.mb * 5.2, { icon: 'image', detail: 'Every page as a PNG, 300 dpi', done: '4 pages saved as PNG' }),
    convertSlice(file, 'TXT', 'txt', 0.01, { icon: 'document', detail: 'The text; scans read with OCR' }),
    convertSlice(file, 'MD', 'md', 0.012, { icon: 'markdown', detail: 'Text with headings and lists kept' }),
    convertSlice(file, 'DOCX', 'docx', file.mb * 0.4, { icon: 'document', detail: 'Word document, text reflowed' }),
    convertSlice(file, 'EPUB', 'epub', file.mb * 0.35, { icon: 'epub', detail: 'E-book for any reader app' }),
    convertSlice(file, 'HTML', 'html', file.mb * 0.3, { icon: 'html', detail: 'A web page' }),
  ];
  const tools = [
    { label: 'Pages', icon: 'pages', detail: 'Reorder, turn, copy or remove pages', done: 'Pages opened' },
    { label: 'Edit', icon: 'markup', detail: 'Redact, annotate, fill and sign', done: 'Edit opened' },
    { label: 'Compress', icon: 'shrink', detail: 'Smaller PDF, pictures at 150 dpi', card: { title: 'PDF', file: out(file.base, 'compressed', 'pdf'), size: file.mb * 0.45 }, done: '1 PDF compressed' },
    { label: 'Smallest', icon: 'shrink', detail: 'Smallest PDF, pictures at 96 dpi', card: { title: 'PDF', file: out(file.base, 'compressed', 'pdf'), size: file.mb * 0.28 }, done: '1 PDF compressed' },
    { label: 'Copy text', icon: 'text', detail: 'Read the text and copy it', done: 'Copied 112 lines of text' },
    { label: 'OCR PDF', icon: 'ocr', detail: 'PDF with selectable, searchable text', card: { title: 'PDF', file: out(file.base, 'searchable', 'pdf'), size: file.mb * 1.02 }, done: '1 PDF made searchable' },
    { label: 'Inspect', icon: 'inspect', detail: 'Every metadata field, searchable', done: '14 metadata fields shown' },
  ];
  return { convert, tools, reduce: reduceSlices(file), files: fileSlices(file) };
}

function audio(file) {
  const fmts = [['MP3', 'mp3', 0.09], ['M4A', 'm4a', 0.08], ['FLAC', 'flac', 0.55], ['OGG', 'ogg', 0.07], ['Opus', 'opus', 0.05], ['AIFF', 'aiff', 1], ['WMA', 'wma', 0.08]];
  const convert = fmts.map(([label, ext, k]) => convertSlice(file, label, ext, file.mb * k));
  const c = (suffix, mb) => ({ title: 'M4A', file: out(file.base, suffix, 'm4a'), size: mb });
  const tools = [
    { label: 'Compress', detail: 'AAC at a smaller size', card: c('compressed', file.mb * 0.09), done: '1 file compressed' },
    { label: '50% size', detail: 'Half the file size, guaranteed', card: c('compressed', file.mb * 0.5), done: '1 file at half the size' },
    { label: 'Under 10 MB', detail: 'Fits in 10 MB, for chat apps', card: c('compressed', 9.6), done: '1 file now under 10 MB' },
    { label: 'Under 25 MB', detail: 'Fits in 25 MB, for email', card: c('compressed', 24.1), done: '1 file now under 25 MB' },
    { label: 'Normalize', icon: 'normalize', detail: 'Speech, music or broadcast loudness', card: { title: 'WAV', file: out(file.base, 'normalized', 'wav'), size: file.mb }, done: '1 file normalized' },
    { label: 'Trim', icon: 'trim', detail: 'To exact times, and silence at the ends', done: 'Trim opened' },
    { label: 'Channel', icon: 'channel', detail: 'Mono, stereo, left and right, swap', done: 'Channels opened' },
    { label: 'Bleep', icon: 'bleep', detail: 'A tone or silence over the parts you mark', done: 'Bleep opened' },
    { label: 'Visuals', icon: 'visuals', detail: 'An MP4 with moving bars or a wave', done: 'Visualizer opened' },
    { label: 'Inspect', icon: 'inspect', detail: 'Every metadata field, searchable', done: '12 metadata fields shown' },
  ];
  return { convert, tools, reduce: reduceSlices(file), files: fileSlices(file) };
}

function document_(file) {
  const convert = [
    convertSlice(file, 'PDF', 'pdf', file.mb * 1.6, { icon: 'pdf', detail: 'PDF, laid out for A4 or Letter' }),
    convertSlice(file, 'MD', 'md', file.mb * 0.1, { icon: 'markdown', detail: 'Markdown, pictures in a folder' }),
    convertSlice(file, 'HTML', 'html', file.mb * 0.4, { icon: 'html', detail: 'A web page' }),
    convertSlice(file, 'EPUB', 'epub', file.mb * 0.8, { icon: 'epub', detail: 'E-book for any reader app' }),
    convertSlice(file, 'TXT', 'txt', file.mb * 0.05, { icon: 'document', detail: 'Plain text' }),
  ];
  const tools = [
    { label: 'Edit', icon: 'markup', detail: 'Redact, annotate, fill and sign', card: { title: 'PDF', file: out(file.base, 'edited', 'pdf'), size: file.mb * 1.6 }, done: 'Edit opened' },
    { label: 'Inspect', icon: 'inspect', detail: 'Every metadata field, searchable', done: '9 metadata fields shown' },
  ];
  return { convert, tools, files: fileSlices(file) };
}

function archive(file) {
  const convert = [
    { label: 'Extract', icon: 'extract', detail: 'Unpack into a folder next to it', card: { title: 'Folder', file: `${file.base} (extracted)`, size: file.mb * 2.6 }, done: '214 files extracted' },
    convertSlice(file, 'TAR.GZ', 'tar.gz', file.mb * 0.98, { icon: 'zip', detail: 'Repack as TAR.GZ' }),
    convertSlice(file, '7z', '7z', file.mb * 0.81, { icon: 'zip', detail: 'Repack as 7z (LZMA2)' }),
  ];
  return { convert, files: fileSlices(file).filter((s) => s.label !== 'Zip') };
}

const BUILD = { photo, video, pdf, audio, document: document_, archive };

// The sections the wheel shows for a file, in order, each only if it has slices.
export function wheelFor(file) {
  const sets = BUILD[file.kind](file);
  return SECTIONS.filter((s) => sets[s.key]?.length).map((s) => ({ ...s, slices: sets[s.key] }));
}

// Several files at once (a multi-file drag, or a selection and Ctrl+Shift+Space). Files of one kind
// get that kind's wheel, run on each of them; two or more PDFs also get Merge. A mix of kinds gets
// File actions only. Cards show the first file's result; `single` slices make one file for all.
export function wheelForMany(list) {
  if (list.length === 1) return wheelFor(list[0]);
  const n = list.length;
  const total = list.reduce((sum, f) => sum + f.mb, 0);
  const first = list[0];
  const same = list.every((f) => f.kind === first.kind);
  const plural = (w) => (/s$/.test(w) ? w : `${w}s`);
  const files = [
    { label: 'Copy', icon: 'copy', detail: 'Copy them to the clipboard', done: `${n} files copied` },
    { label: 'Move to folder', icon: 'move', detail: 'Move them to a folder you pick', done: 'Pick a folder for them' },
    { label: 'Copy path', icon: 'link', detail: 'Copy every full path', done: `Copied ${n} paths` },
    { label: 'Rename', icon: 'rename', detail: 'Rename with a pattern: {name}, {n}, {date}', done: `Pick a name pattern for ${n} files` },
    { label: 'Float', icon: 'float', detail: 'Keep them on the Float shelf', done: `${n} files on the Float shelf` },
    { label: 'Zip', icon: 'zip', single: true, detail: 'One ZIP of everything', card: { title: 'ZIP', file: 'files.zip', size: total * 0.7 }, done: `${n} files saved in files.zip` },
    { label: 'Share', icon: 'share', detail: 'Nearby sharing, Phone Link, mail, apps', done: 'Windows share opened' },
  ];
  if (!same) return SECTIONS.filter((s) => s.key === 'files').map((s) => ({ ...s, slices: files }));
  const sets = BUILD[first.kind]({ ...first, mb: total });
  const many = (s) => ({
    ...s,
    detail: s.detail?.replace(/^Save an? (\S+) copy next to it$/, 'Save $1 copies next to them'),
    done: s.card ? s.done.replace(/^1 (\w+)/, (m, w) => `${n} ${plural(w)}`) : s.done,
  });
  for (const key of ['convert', 'tools', 'reduce']) if (sets[key]) sets[key] = sets[key].map(many);
  if (first.kind === 'pdf') {
    sets.tools.splice(1, 0, { label: 'Merge', icon: 'merge', single: true, detail: 'One PDF, in the order you choose', card: { title: 'PDF', file: out(first.base, 'merged', 'pdf'), size: total * 0.97 }, done: `${n} PDFs merged into one` });
  }
  sets.files = files.filter((s) => s.label !== 'Zip' || first.kind !== 'archive');
  return SECTIONS.filter((s) => sets[s.key]?.length).map((s) => ({ ...s, slices: sets[s.key] }));
}

export const SAMPLE_FILES = [
  { base: 'receipt-0412', ext: 'jpg', kind: 'photo', mb: 1.8, icon: 'image' },
  { base: 'photo', ext: 'jpg', kind: 'photo', mb: 3.4, icon: 'image' },
  { base: 'contract-v3', ext: 'pdf', kind: 'pdf', mb: 1.6, icon: 'pdf' },
  { base: 'podcast-ep12', ext: 'wav', kind: 'audio', mb: 58, icon: 'audio' },
  { base: 'demo-cut', ext: 'mov', kind: 'video', mb: 248, icon: 'video' },
  { base: 'notes', ext: 'docx', kind: 'document', mb: 0.24, icon: 'document' },
  { base: 'assets', ext: 'zip', kind: 'archive', mb: 12, icon: 'archive' },
  { base: 'invoice-0918', ext: 'pdf', kind: 'pdf', mb: 0.9, icon: 'pdf' },
];
