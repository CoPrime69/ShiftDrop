# Website

The ShiftDrop website: plain HTML, CSS and JavaScript, with nothing to build. `.github/workflows/pages.yml`
publishes this folder to GitHub Pages when it changes on main.

To preview it, serve the folder (ES modules don't load from `file://`):

```sh
cd website
python -m http.server 8000
```

then open http://localhost:8000.

- `assets/js/wheel.js` draws the wheel with the app's own geometry and motion (see the comments for the
  C++ each part mirrors). Change it when the app's wheel changes.
- `assets/js/content.js` lists what the demo's wheel offers each sample file, in the app's wording.
- `assets/fonts/` holds Instrument Sans, IBM Plex Sans and JetBrains Mono (SIL Open Font License, see
  `OFL.txt`), served from the site itself so no font service sees visitors.

After changing any CSS or JavaScript, bump the `?v=` number on every reference to it (the `<link>` and
`<script>` tags in both HTML files, and the `import` lines in `assets/js/`), so that browsers fetch the
new files instead of reusing cached ones.
