import { startDemo } from './demo.js';
import { Wheel } from './wheel.js';
import { SAMPLE_FILES, wheelFor } from './content.js';
import { iconSvg } from './icons.js';

const REPO = 'CoPrime69/ShiftDrop';

// Icons named in the HTML (data-icon="download").
for (const el of document.querySelectorAll('[data-icon]')) {
  el.insertAdjacentHTML('afterbegin', iconSvg(el.dataset.icon, el.dataset.iconClass || 'icon'));
}

// The sun/moon button in the header.
for (const b of document.querySelectorAll('[data-theme-toggle]')) {
  const label = () => b.setAttribute('aria-label', document.documentElement.dataset.theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode');
  label();
  b.addEventListener('click', () => { window.toggleTheme?.(); label(); });
}

// Labels are measured to fit their tiles, so the wheels wait for the fonts.
await Promise.all([document.fonts.load('600 13px "IBM Plex Sans"'), document.fonts.load('400 12px "IBM Plex Sans"')]).catch(() => {});
await document.fonts.ready;

// Still wheels drawn by the same engine (How it works, Private by design, the 404 page). They show a
// photo on Convert with WebP hovered, as the app does.
for (const host of document.querySelectorAll('[data-still-wheel]')) {
  const photo = SAMPLE_FILES.find((f) => f.base === 'photo');
  const [convert] = wheelFor(photo);
  const wheel = new Wheel({ reducedMotion: true });
  wheel.set({ slices: convert.slices, centre: '1 file', title: convert.title, sections: 4, section: 0 });
  const hot = host.dataset.stillWheel;
  if (hot !== '') wheel.setHot(Number(hot));
  host.appendChild(wheel.svg);
}

const stage = document.querySelector('[data-stage]');
if (stage) startDemo(stage);

// The header gains a line once the page scrolls under it.
const header = document.querySelector('.site-header');
if (header) {
  const onScroll = () => header.classList.toggle('is-scrolled', window.scrollY > 8);
  onScroll();
  window.addEventListener('scroll', onScroll, { passive: true });
}

// Point the download buttons straight at the latest installer. Without script, or if GitHub doesn't
// answer, they open the latest release page, which has the same file.
(async () => {
  const buttons = document.querySelectorAll('[data-download]');
  if (!buttons.length) return;
  try {
    const res = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, { headers: { Accept: 'application/vnd.github+json' } });
    if (!res.ok) return;
    const release = await res.json();
    const asset = release.assets?.find((a) => /^ShiftDrop-Setup-.*\.exe$/i.test(a.name));
    if (!asset) return;
    const version = String(release.tag_name || '').replace(/^v/i, '');
    for (const b of buttons) b.href = asset.browser_download_url;
    for (const v of document.querySelectorAll('[data-version]')) v.textContent = version;
    for (const s of document.querySelectorAll('[data-size]')) s.textContent = `${Math.round(asset.size / 1048576)} MB`;
  } catch {
    // Offline or rate-limited: the release page links stay.
  }
})();
