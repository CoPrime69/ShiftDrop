// Line icons on a 24 px grid: round caps and joins, stroke from CSS (currentColor). One set for the
// wheel, the top panel and the page, drawn to the brand's icon rules.

const P = {
  // sections
  convert: '<path d="M4 8h13l-3-3"/><path d="M20 16H7l3 3"/>',
  tools: '<path d="M4 7h10"/><path d="M18 7h2"/><circle cx="16" cy="7" r="2"/><path d="M4 17h2"/><path d="M10 17h10"/><circle cx="8" cy="17" r="2"/>',
  reduce: '<path d="M4 4l6 6"/><path d="M10 5v5H5"/><path d="M20 20l-6-6"/><path d="M14 19v-5h5"/>',
  files: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  // kinds of file
  image: '<rect x="3" y="5" width="18" height="14" rx="2"/><circle cx="8.5" cy="10" r="1.5"/><path d="M21 15l-5-5-9 9"/>',
  video: '<rect x="3" y="6" width="13" height="12" rx="2"/><path d="M16 10l5-3v10l-5-3"/>',
  audio: '<path d="M9 18V6l11-2v12"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="17.5" cy="16" r="2.5"/>',
  pdf: '<path d="M7 3h7l5 5v11a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M14 3v5h5"/><path d="M9 13h6"/><path d="M9 17h4"/>',
  document: '<path d="M7 3h7l5 5v11a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M14 3v5h5"/><path d="M9 12h6"/><path d="M9 15h6"/><path d="M9 18h4"/>',
  archive: '<rect x="4" y="3" width="16" height="18" rx="2"/><path d="M12 3v2"/><path d="M12 7v2"/><path d="M12 11v2"/><rect x="10" y="14" width="4" height="4" rx="1"/>',
  // actions
  globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18"/><path d="M12 3a14 14 0 0 1 0 18"/><path d="M12 3a14 14 0 0 0 0 18"/>',
  rotate: '<path d="M20 12a8 8 0 1 1-3-6.2"/><path d="M20 4v5h-5"/>',
  rotateLeft: '<path d="M4 12a8 8 0 1 0 3-6.2"/><path d="M4 4v5h5"/>',
  flip: '<path d="M12 3v18"/><path d="M8 7L3 12l5 5z"/><path d="M16 7l5 5-5 5z"/>',
  fit: '<path d="M4 9V5a1 1 0 0 1 1-1h4"/><path d="M15 4h4a1 1 0 0 1 1 1v4"/><path d="M20 15v4a1 1 0 0 1-1 1h-4"/><path d="M9 20H5a1 1 0 0 1-1-1v-4"/>',
  shrink: '<path d="M9 4v4a1 1 0 0 1-1 1H4"/><path d="M20 9h-4a1 1 0 0 1-1-1V4"/><path d="M15 20v-4a1 1 0 0 1 1-1h4"/><path d="M4 15h4a1 1 0 0 1 1 1v4"/>',
  crop: '<path d="M6 2v14a2 2 0 0 0 2 2h14"/><path d="M2 6h14a2 2 0 0 1 2 2v14"/>',
  redact: '<rect x="3" y="5" width="18" height="14" rx="2"/><rect x="6" y="10" width="12" height="4" rx="1" fill="currentColor" stroke="none"/>',
  private: '<path d="M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6z"/><path d="M9 12l2 2 4-4"/>',
  tag: '<path d="M3 12V5a2 2 0 0 1 2-2h7l9 9-9 9z"/><circle cx="8" cy="8" r="1.5"/><path d="M14 15l4-4"/>',
  edit: '<path d="M12 3v3"/><path d="M12 18v3"/><path d="M3 12h3"/><path d="M18 12h3"/><circle cx="12" cy="12" r="4"/><path d="M5.6 5.6l2 2"/><path d="M16.4 16.4l2 2"/><path d="M5.6 18.4l2-2"/><path d="M16.4 7.6l2-2"/>',
  markup: '<path d="M4 20l1-4L16 5l3 3L8 19z"/><path d="M14 7l3 3"/>',
  frame: '<rect x="3" y="3" width="18" height="18" rx="2"/><rect x="7" y="7" width="10" height="10" rx="1"/>',
  text: '<path d="M4 7V5h16v2"/><path d="M12 5v14"/><path d="M9 19h6"/>',
  ocr: '<path d="M4 8V5a1 1 0 0 1 1-1h3"/><path d="M16 4h3a1 1 0 0 1 1 1v3"/><path d="M20 16v3a1 1 0 0 1-1 1h-3"/><path d="M8 20H5a1 1 0 0 1-1-1v-3"/><path d="M8 9h8"/><path d="M12 9v7"/>',
  inspect: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6"/><path d="M12 7.5v.5"/>',
  copy: '<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>',
  move: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M10 13h6"/><path d="M13 10l3 3-3 3"/>',
  link: '<path d="M10 14a4 4 0 0 0 6 0l3-3a4 4 0 0 0-6-6l-1 1"/><path d="M14 10a4 4 0 0 0-6 0l-3 3a4 4 0 0 0 6 6l1-1"/>',
  terminal: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3"/><path d="M12 15h5"/>',
  duplicate: '<path d="M7 3h7l5 5v11a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M14 3v5h5"/><path d="M12 11v6"/><path d="M9 14h6"/>',
  rename: '<path d="M4 7h10"/><path d="M4 12h7"/><path d="M4 17h5"/><path d="M13 20l1-3 6-6 2 2-6 6z"/>',
  float: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M15 4v16"/><path d="M17.5 9h1"/><path d="M17.5 12h1"/>',
  zip: '<rect x="4" y="3" width="16" height="18" rx="2"/><path d="M12 3v2"/><path d="M12 7v2"/><path d="M12 11v2"/><rect x="10" y="14" width="4" height="4" rx="1"/>',
  share: '<circle cx="18" cy="5" r="2.5"/><circle cx="6" cy="12" r="2.5"/><circle cx="18" cy="19" r="2.5"/><path d="M8.2 10.8l7.6-4.6"/><path d="M8.2 13.2l7.6 4.6"/>',
  trim: '<circle cx="6" cy="7" r="2.5"/><circle cx="6" cy="17" r="2.5"/><path d="M8 8.5L20 18"/><path d="M8 15.5L20 6"/>',
  speed: '<path d="M4 18a8 8 0 1 1 16 0"/><path d="M12 18l4-6"/>',
  join: '<rect x="3" y="7" width="8" height="10" rx="1.5"/><rect x="13" y="7" width="8" height="10" rx="1.5"/><path d="M11 12h2"/>',
  frames: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 4v16"/><path d="M17 4v16"/><path d="M3 9h4"/><path d="M3 15h4"/><path d="M17 9h4"/><path d="M17 15h4"/>',
  split: '<rect x="3" y="7" width="18" height="10" rx="1.5"/><path d="M12 4v16"/>',
  normalize: '<path d="M3 12h2"/><path d="M7 8v8"/><path d="M11 5v14"/><path d="M15 8v8"/><path d="M19 10v4"/>',
  channel: '<path d="M4 10h3l4-4v12l-4-4H4z"/><path d="M15 9a4 4 0 0 1 0 6"/><path d="M18 6a8 8 0 0 1 0 12"/>',
  bleep: '<path d="M3 12h3l2-5 3 10 3-10 2 5h5"/>',
  visuals: '<path d="M5 20v-6"/><path d="M10 20V8"/><path d="M15 20v-9"/><path d="M20 20V4"/>',
  merge: '<rect x="4" y="3" width="10" height="13" rx="1.5"/><rect x="10" y="8" width="10" height="13" rx="1.5"/>',
  pages: '<rect x="3" y="4" width="8" height="7" rx="1"/><rect x="13" y="4" width="8" height="7" rx="1"/><rect x="3" y="13" width="8" height="7" rx="1"/><rect x="13" y="13" width="8" height="7" rx="1"/>',
  extract: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M12 10v6"/><path d="M9 13l3 3 3-3"/>',
  epub: '<path d="M4 5a2 2 0 0 1 2-2h12v16H6a2 2 0 0 0-2 2z"/><path d="M4 21V5"/><path d="M18 19v2H6"/>',
  html: '<path d="M8 8l-4 4 4 4"/><path d="M16 8l4 4-4 4"/><path d="M13 6l-2 12"/>',
  markdown: '<rect x="2" y="6" width="20" height="12" rx="2"/><path d="M6 15V9l2.5 3L11 9v6"/><path d="M16 9v6"/><path d="M14 13l2 2 2-2"/>',
  // page
  shift: '<path d="M12 4l8 8h-4v7H8v-7H4z"/>',
  download: '<path d="M12 4v11"/><path d="M7 10l5 5 5-5"/><path d="M5 20h14"/>',
  code: '<path d="M8 8l-4 4 4 4"/><path d="M16 8l4 4-4 4"/>',
  github: '<path d="M9 19c-4 1.3-4-2-6-2.5"/><path d="M15 21v-3.5c0-1 .1-1.4-.5-2 2.8-.3 5.5-1.4 5.5-6a4.6 4.6 0 0 0-1.3-3.2 4.2 4.2 0 0 0-.1-3.2s-1.1-.3-3.5 1.3a12 12 0 0 0-6.2 0C6.5 2.8 5.4 3.1 5.4 3.1a4.2 4.2 0 0 0-.1 3.2A4.6 4.6 0 0 0 4 9.5c0 4.6 2.7 5.7 5.5 6-.6.6-.6 1.2-.5 2V21"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  offline: '<path d="M2 8.5a15 15 0 0 1 20 0"/><path d="M5.5 12a10 10 0 0 1 13 0"/><path d="M9 15.5a5 5 0 0 1 6 0"/><path d="M12 19h.01"/><path d="M3 3l18 18"/>',
  account: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/><path d="M3 3l18 18"/>',
  telemetry: '<path d="M3 12h4l3-8 4 16 3-8h4"/><path d="M3 3l18 18"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  branch: '<circle cx="6" cy="5" r="2"/><circle cx="6" cy="19" r="2"/><circle cx="18" cy="7" r="2"/><path d="M6 7v10"/><path d="M18 9a6 6 0 0 1-6 6H6"/>',
  arrow: '<path d="M5 12h14"/><path d="M13 6l6 6-6 6"/>',
  sign: '<path d="M4 20l1-4L16 5l3 3L8 19z"/><path d="M14 20h6"/>',
  cursor: '<path d="M5 3l14 8-6 2-3 6z"/>',
  more: '<circle cx="5" cy="12" r="1.2"/><circle cx="12" cy="12" r="1.2"/><circle cx="19" cy="12" r="1.2"/>',
};

export function icon(name) {
  return P[name] ?? P.image;
}

// A standalone inline SVG, for the page's HTML.
export function iconSvg(name, cls = 'icon') {
  return `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true" focusable="false">${icon(name)}</svg>`;
}
