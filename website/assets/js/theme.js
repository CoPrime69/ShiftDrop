// Light or dark, before the page paints: the visitor's choice if they made one, else the system's.
// Loaded as a plain script in <head> so there is no flash of the other theme.
(() => {
  const root = document.documentElement;
  const system = matchMedia('(prefers-color-scheme: dark)');
  let chosen = null;
  try { chosen = localStorage.getItem('theme'); } catch {}
  const apply = (theme) => {
    root.dataset.theme = theme;
    for (const m of document.querySelectorAll('meta[name="theme-color"]')) m.content = theme === 'dark' ? '#0E1014' : '#F6F7F9';
  };
  apply(chosen === 'light' || chosen === 'dark' ? chosen : system.matches ? 'dark' : 'light');
  system.addEventListener('change', (e) => {
    let saved = null;
    try { saved = localStorage.getItem('theme'); } catch {}
    if (!saved) apply(e.matches ? 'dark' : 'light');
  });
  // The header's sun/moon button.
  window.toggleTheme = () => {
    const next = root.dataset.theme === 'dark' ? 'light' : 'dark';
    apply(next);
    try { localStorage.setItem('theme', next); } catch {}
    return next;
  };
})();
