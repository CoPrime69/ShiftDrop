// The ShiftDrop wheel, drawn in SVG with the app's own geometry and motion.
//
// Everything here mirrors the Windows app: src/keyboard/WheelLayout.* (rings, slots, hit-testing),
// src/keyboard/WheelLabels.* (one label style, one or two lines, never cut), and src/Overlay.cpp
// (tile shape, lift on hover, the open spring, the section switch). Lengths are DIPs; the SVG's
// viewBox is in DIPs and CSS scales it.

import { icon } from './icons.js?v=23';

const SVG = 'http://www.w3.org/2000/svg';

// ---- Layout (WheelLayout.h) -----------------------------------------------------------------------

export const RING_CAPACITY = 8;
export const CENTRE = 52;          // centre disc = dead zone: dropping here cancels
const INNER_IN = 58, INNER_OUT = 128;
const OUTER_IN = 134, OUTER_OUT = 192;

const GAP = 4;                     // straight gap between tiles
const CORNER = 6;                  // tile corner rounding
const LIFT = 7, PUSH = 3;          // a hovered tile grows outward and moves out along its centre line
const INNER_LIFT = 2, INNER_PUSH = 1;
const PLATE = 4;                   // the plate around the tiles

const OPEN_MS = 420, FADE_IN_MS = 120, SWITCH_MS = 260, CLOSE_MS = 200, HOT_TAU = 42;

export const LABEL_PX = 13, ICON_PX = 20, ICON_GAP = 3, LINE_HEIGHT = 1.05, LABEL_PAD = 1.5;
const MAIN_PX = 20, DETAIL_PX = 12;

export function arrange(total) {
  const slots = [];
  if (total <= RING_CAPACITY) {
    for (let i = 0; i < total; i++) slots.push({ ring: 0, index: i, count: total });
    return { slots, rings: 1 };
  }
  const inner = Math.ceil(total / 2), outer = total - inner;
  for (let i = 0; i < inner; i++) slots.push({ ring: 0, index: i, count: inner });
  for (let i = 0; i < outer; i++) slots.push({ ring: 1, index: i, count: outer });
  return { slots, rings: 2 };
}

const band = (ring) => (ring === 0 ? [INNER_IN, INNER_OUT] : [OUTER_IN, OUTER_OUT]);
const span = (slot) => 360 / slot.count;
const mid = (slot) => slot.index * span(slot);
export const outerEdge = (layout) => band(layout.rings - 1)[1];

export const OUTSIDE = -1, ON_CENTRE = -2;

// The slot under a point `d` DIPs from the centre at `deg` (0 = 12 o'clock, clockwise).
export function hitSlot(layout, d, deg, reach) {
  if (d > outerEdge(layout) + reach) return OUTSIDE;
  if (d < CENTRE || layout.slots.length === 0) return ON_CENTRE;
  const ring = layout.rings === 2 && d > (INNER_OUT + OUTER_IN) / 2 ? 1 : 0;
  let base = 0, count = 0;
  for (const s of layout.slots) {
    if (s.ring < ring) base++;
    else if (s.ring === ring) count++;
  }
  deg = ((deg % 360) + 360) % 360;
  const sp = 360 / count;
  return base + (Math.floor((deg + sp / 2) / sp) % count);
}

// ---- Geometry (Overlay.cpp: TileGeometry) ---------------------------------------------------------

const RAD = Math.PI / 180;
const polar = (r, deg) => [r * Math.sin(deg * RAD), -r * Math.cos(deg * RAD)];
const pt = (p) => `${p[0].toFixed(2)} ${p[1].toFixed(2)}`;

// One tile: an annular sector with a constant-width gap to its neighbours and rounded corners.
export function tilePath(count, index, inner, outer) {
  if (count === 1) {
    const ring = (r) => `M${pt(polar(r, 0))}A${r} ${r} 0 0 1 ${pt(polar(r, 180))}A${r} ${r} 0 0 1 ${pt(polar(r, 360))}Z`;
    return ring(outer) + ring(inner);
  }
  const sp = 360 / count;
  const a0 = index * sp - sp / 2, a1 = a0 + sp;
  const g = GAP / 2;
  const c = Math.min(CORNER, (outer - inner) / 2 - 0.5);
  // A point `along` the radial line at angle a, `side` across it (positive = toward larger angles).
  const at = (a, along, side) => [along * Math.sin(a * RAD) + side * Math.cos(a * RAD), -along * Math.cos(a * RAD) + side * Math.sin(a * RAD)];
  const outerC = outer - c, innerC = inner + c;
  const dOut = Math.asin((g + c) / outerC) / RAD, dIn = Math.asin((g + c) / innerC) / RAD;
  const lOut = Math.sqrt(outerC * outerC - (g + c) * (g + c));
  const lIn = Math.sqrt(innerC * innerC - (g + c) * (g + c));
  const outerLarge = sp - 2 * dOut > 180 ? 1 : 0;
  const innerLarge = sp - 2 * dIn > 180 ? 1 : 0;
  return [
    `M${pt(polar(outer, a0 + dOut))}`,
    `A${outer} ${outer} 0 ${outerLarge} 1 ${pt(polar(outer, a1 - dOut))}`,
    `A${c} ${c} 0 0 1 ${pt(at(a1, lOut, -g))}`,
    `L${pt(at(a1, lIn, -g))}`,
    `A${c} ${c} 0 0 1 ${pt(polar(inner, a1 - dIn))}`,
    `A${inner} ${inner} 0 ${innerLarge} 0 ${pt(polar(inner, a0 + dIn))}`,
    `A${c} ${c} 0 0 1 ${pt(at(a0, lIn, g))}`,
    `L${pt(at(a0, lOut, g))}`,
    `A${c} ${c} 0 0 1 ${pt(polar(outer, a0 + dOut))}Z`,
  ].join('');
}

// ---- Labels (WheelLabels.cpp) ---------------------------------------------------------------------

const LABEL_FONT = `600 ${LABEL_PX}px "Segoe UI Variable Text", "Segoe UI Variable", "Segoe UI", "IBM Plex Sans", system-ui, sans-serif`;
const DETAIL_FONT = `400 ${DETAIL_PX}px "Segoe UI Variable Text", "Segoe UI Variable", "Segoe UI", "IBM Plex Sans", system-ui, sans-serif`;
let measureCtx = null;
function measure(text, font = LABEL_FONT) {
  measureCtx ??= document.createElement('canvas').getContext('2d');
  measureCtx.font = font;
  return measureCtx.measureText(text).width;
}

// Whether rows (stacked, each centred on the block) stay `pad` inside the slot's tile.
function rowsFit(slot, rows, radius, pad = LABEL_PAD) {
  const [inner, outer] = band(slot.ring);
  const m = mid(slot), sp = span(slot);
  const [cx, cy] = polar(radius, m);
  const total = rows.reduce((s, r) => s + r.h, 0);
  const g = GAP / 2 + pad;
  const n0 = [Math.cos((m - sp / 2) * RAD), Math.sin((m - sp / 2) * RAD)];
  const n1 = [Math.cos((m + sp / 2) * RAD), Math.sin((m + sp / 2) * RAD)];
  let y = cy - total / 2;
  for (const r of rows) {
    if (r.w > 0) {
      for (const [x, yy] of [[cx - r.w / 2, y], [cx + r.w / 2, y], [cx - r.w / 2, y + r.h], [cx + r.w / 2, y + r.h]]) {
        const d = Math.hypot(x, yy);
        if (d < inner + pad || d > outer - pad) return false;
        if (slot.count > 1 && (x * n0[0] + yy * n0[1] < g || x * n1[0] + yy * n1[1] > -g)) return false;
      }
    }
    y += r.h;
  }
  return true;
}

function rowsOf(lines, widths, hasIcon) {
  const rows = [];
  if (hasIcon) rows.push({ w: ICON_PX, h: ICON_PX }, { w: 0, h: ICON_GAP });
  lines.forEach((_, i) => rows.push({ w: widths[i], h: LABEL_PX * LINE_HEIGHT }));
  return rows;
}

// One line if it fits, else two broken at a space (never inside "10 MB"), the most even split first.
export function fitLabel(slot, text, hasIcon) {
  const [inner, outer] = band(slot.ring);
  const centre = (inner + outer) / 2;
  const tryLines = (lines) => {
    const widths = lines.map((l) => measure(l));
    const rows = rowsOf(lines, widths, hasIcon);
    // The block sits on the tile's middle line; nudge it in or out if that is what makes it fit.
    for (const dr of [0, -2, 2, -4, 4, -6, 6, -8, 8]) {
      if (rowsFit(slot, rows, centre + dr)) return { lines, widths, radius: centre + dr, fits: true };
    }
    return null;
  };
  const one = tryLines([text]);
  if (one) return finish(one, hasIcon);
  const words = text.split(' ');
  const splits = [];
  for (let i = 1; i < words.length; i++) {
    const a = words.slice(0, i).join(' '), b = words.slice(i).join(' ');
    if (/\d$/.test(a) && /^(MB|KB|GB|px)\b/.test(b)) continue; // keep a number with its unit
    splits.push([a, b]);
  }
  splits.sort((x, y) => Math.abs(measure(x[0]) - measure(x[1])) - Math.abs(measure(y[0]) - measure(y[1])));
  for (const s of splits) {
    const two = tryLines(s);
    if (two) return finish(two, hasIcon);
  }
  // Too long even on two lines: the app's tests stop such a label; draw it on two anyway.
  const fallback = splits[0] ?? [text];
  return finish({ lines: fallback, widths: fallback.map((l) => measure(l)), radius: centre, fits: false }, hasIcon);
}

function finish(fit, hasIcon) {
  const rows = rowsOf(fit.lines, fit.widths, hasIcon);
  const total = rows.reduce((s, r) => s + r.h, 0);
  let y = -total / 2;
  fit.icon = hasIcon;
  fit.lineDy = [];
  if (hasIcon) {
    fit.iconDy = y + ICON_PX / 2;
    y += ICON_PX + ICON_GAP;
  }
  for (let i = 0; i < fit.lines.length; i++) {
    fit.lineDy.push(y + (LABEL_PX * LINE_HEIGHT) / 2);
    y += LABEL_PX * LINE_HEIGHT;
  }
  return fit;
}

function wrap(text, width, font = DETAIL_FONT) {
  const words = text.split(' ');
  const lines = [];
  let line = '';
  for (const w of words) {
    const next = line ? `${line} ${w}` : w;
    if (line && measure(next, font) > width) {
      lines.push(line);
      line = w;
    } else line = next;
  }
  if (line) lines.push(line);
  return lines;
}

// ---- Motion (Theme.cpp) ---------------------------------------------------------------------------

export const easeOutCubic = (t) => 1 - Math.pow(1 - Math.min(Math.max(t, 0), 1), 3);
// Damped oscillation, normalised so it starts at 0 and has settled by t = 1.
export const spring = (t) => (t >= 1 ? 1 : 1 - Math.exp(-6 * t) * Math.cos(7.5 * t));
const approach = (v, target, dt, tau) => {
  const next = target + (v - target) * Math.exp(-dt / tau);
  return Math.abs(next - target) < 0.004 ? target : next;
};

function el(name, attrs = {}, parent) {
  const e = document.createElementNS(SVG, name);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}

// ---- The wheel ------------------------------------------------------------------------------------

let uid = 0;

export class Wheel {
  // options.reducedMotion: no spring, no turns, no lifts animating (states still change).
  constructor(options = {}) {
    this.id = `w${++uid}`;
    this.reducedMotion = options.reducedMotion ?? false;
    this.svg = el('svg', { class: 'wheel-svg', 'aria-hidden': 'true', focusable: 'false' });
    const defs = el('defs', {}, this.svg);
    // Soft shadow under the ring, slightly lower than the wheel, as if lit from above (Overlay.cpp).
    this.shadowGrad = el('radialGradient', { id: `${this.id}-shadow`, gradientUnits: 'userSpaceOnUse', cx: 0, cy: 5 }, defs);
    this.defs = defs;
    this.root = el('g', { class: 'wheel-root' }, this.svg);
    this.under = el('g', { class: 'wheel-under' }, this.root);
    this.shadow = el('circle', { cx: 0, cy: 5, fill: `url(#${this.id}-shadow)` }, this.under);
    this.plate = el('circle', { class: 'wheel-plate', cx: 0, cy: 0 }, this.under);
    this.prevLayer = el('g', { class: 'wheel-tiles prev' }, this.root);
    this.layer = el('g', { class: 'wheel-tiles' }, this.root);
    this.centre = el('g', { class: 'wheel-centre' }, this.root);
    el('circle', { class: 'wheel-centre-disc', cx: 0, cy: 0, r: CENTRE }, this.centre);
    el('circle', { class: 'wheel-centre-line', cx: 0, cy: 0, r: CENTRE - 0.5 }, this.centre);
    this.mainA = el('text', { class: 'wheel-main', 'text-anchor': 'middle' }, this.centre);
    this.mainB = el('text', { class: 'wheel-main', 'text-anchor': 'middle' }, this.centre);
    this.detailA = el('text', { class: 'wheel-detail', 'text-anchor': 'middle' }, this.centre);
    this.detailB = el('text', { class: 'wheel-detail', 'text-anchor': 'middle' }, this.centre);
    this.dots = el('g', { class: 'wheel-dots' }, this.centre);

    this.slices = [];
    this.tiles = [];
    this.hot = -1;
    this.hotT = [];
    this.openT = 1;
    this.closeT = 0;
    this.closing = false;
    this.dropped = false;
    this.switchT = 1;
    this.switchDir = 1;
    this.sections = 1;
    this.section = 0;
    this.centreText = '';
    this.title = '';
    this.frame = null;
    this.last = 0;
    this.builds = 0;
    this.prevEdge = 0;
    // The rings the drawing area is sized for. A wheel whose sections differ (one ring here, two
    // there) sets this to the most, so switching never resizes or moves it.
    this.frameRings = 1;
    this.onClosed = null;
  }

  get reach() { return outerEdge(this.layout) + LIFT; }
  get extent() { return band(Math.max(this.layout.rings, this.frameRings) - 1)[1] + PLATE + 26; }

  // Show a section: its slices, the centre's main line and the section's title.
  set(content, { animate = false, dir = 1 } = {}) {
    if (animate && !this.reducedMotion && this.tiles.length) {
      this.prevLayer.replaceChildren(...this.layer.childNodes);
      this.switchT = 0;
      this.switchDir = dir;
      this.prevEdge = outerEdge(this.layout);
    } else {
      this.prevLayer.replaceChildren();
      this.switchT = 1;
    }
    this.slices = content.slices;
    this.centreText = content.centre;
    this.title = content.title;
    this.sections = content.sections ?? 1;
    this.section = content.section ?? 0;
    this.layout = arrange(this.slices.length);
    this.hot = -1;
    this.hotT = this.slices.map(() => 0);
    this.build();
    this.kick();
  }

  build() {
    const edge = outerEdge(this.layout);
    const ext = this.extent;
    this.svg.setAttribute('viewBox', `${-ext} ${-ext} ${ext * 2} ${ext * 2}`);
    this.svg.style.setProperty('--wheel-dips', ext * 2);
    this.plate.setAttribute('r', edge + PLATE);
    const shadowR = edge + 26;
    this.shadow.setAttribute('r', shadowR);
    this.shadowGrad.setAttribute('r', shadowR);
    const stops = [
      [0, 0], [(INNER_IN - 4) / shadowR, 0], [(INNER_IN + 10) / shadowR, 0.55], [edge / shadowR, 1], [(edge + 9) / shadowR, 0.35], [1, 0],
    ];
    this.shadowGrad.replaceChildren(...stops.map(([o, a]) => el('stop', { offset: o, class: 'wheel-shadow-stop', 'stop-opacity': a })));

    this.layer.replaceChildren();
    // Each build has its own gradients; the turning-away tiles keep theirs until the switch ends.
    const build = ++this.builds;
    this.defs.querySelectorAll('.tile-grad').forEach((g) => { if (Number(g.dataset.build) < build - 1) g.remove(); });
    this.tiles = this.slices.map((slice, i) => {
      const slot = this.layout.slots[i];
      const fit = fitLabel(slot, slice.label, Boolean(slice.icon));
      const g = el('g', { class: 'tile' + (slice.enabled === false ? ' is-dim' : '') }, this.layer);
      const gid = `${this.id}-b${build}-g${i}`;
      const grad = el('linearGradient', { id: gid, class: 'tile-grad', 'data-build': build, gradientUnits: 'userSpaceOnUse' }, this.defs);
      el('stop', { offset: 0, class: 'tile-accent-hi' }, grad);
      el('stop', { offset: 1, class: 'tile-accent' }, grad);
      const base = el('path', { class: 'tile-base' }, g);
      const hotFill = el('path', { class: 'tile-hot', fill: `url(#${gid})`, opacity: 0 }, g);
      const line = el('path', { class: 'tile-line' }, g);
      const label = el('g', { class: 'tile-label' }, g);
      if (slice.icon) {
        const ic = el('g', { class: 'tile-icon' }, label);
        ic.innerHTML = icon(slice.icon);
      }
      fit.lines.forEach((text) => {
        const t = el('text', { 'text-anchor': 'middle', 'dominant-baseline': 'central' }, label);
        t.textContent = text;
      });
      return { g, base, hotFill, line, label, grad, fit, slot };
    });
    this.paintTiles(true);
    this.paintCentre();
  }

  paintTiles(force = false) {
    const ringsTwo = this.layout.rings === 2;
    const order = this.tiles.map((_, i) => i).sort((a, b) => this.hotT[a] - this.hotT[b]);
    order.forEach((i) => {
      const tile = this.tiles[i];
      const t = this.slices[i].enabled === false ? 0 : this.hotT[i];
      if (!force && tile.lastT === t) return;
      tile.lastT = t;
      const e = easeOutCubic(t);
      const slot = tile.slot;
      const [inner, outerBase] = band(slot.ring);
      const innerOfTwo = ringsTwo && slot.ring === 0;
      const push = (innerOfTwo ? INNER_PUSH : PUSH) * e;
      const m = mid(slot);
      const outer = outerBase + (innerOfTwo ? INNER_LIFT : LIFT) * e;
      const d = tilePath(slot.count, slot.index, inner, outer);
      tile.base.setAttribute('d', d);
      tile.hotFill.setAttribute('d', d);
      tile.line.setAttribute('d', d);
      tile.hotFill.setAttribute('opacity', t.toFixed(3));
      tile.line.setAttribute('opacity', (1 - t).toFixed(3));
      const [gx1, gy1] = polar(outer, m), [gx2, gy2] = polar(inner, m);
      tile.grad.setAttribute('x1', gx1); tile.grad.setAttribute('y1', gy1);
      tile.grad.setAttribute('x2', gx2); tile.grad.setAttribute('y2', gy2);
      const [px, py] = polar(push, m);
      tile.g.setAttribute('transform', `translate(${px.toFixed(2)} ${py.toFixed(2)})`);
      tile.g.style.setProperty('--hot', t.toFixed(3));
      tile.g.classList.toggle('is-hot', t > 0.5);
      // Icon above the label, the block centred on the tile's middle line; the hovered one moves out 2.
      const [ax, ay] = polar(tile.fit.radius + 2 * e, m);
      let k = 0;
      for (const child of tile.label.children) {
        if (child.classList.contains('tile-icon')) {
          const s = ICON_PX / 24;
          child.setAttribute('transform', `translate(${(ax - ICON_PX / 2).toFixed(2)} ${(ay + tile.fit.iconDy - ICON_PX / 2).toFixed(2)}) scale(${s})`);
        } else {
          child.setAttribute('x', ax.toFixed(2));
          child.setAttribute('y', (ay + tile.fit.lineDy[k++]).toFixed(2));
        }
      }
      if (t > 0) this.layer.appendChild(tile.g); // draw the hovered tile last so its lift overlaps cleanly
    });
  }

  paintCentre() {
    const hot = this.hot >= 0 ? this.slices[this.hot] : null;
    const hotT = this.hot >= 0 ? this.hotT[this.hot] : 0;
    // Main line: the count (or a size), crossfading to the hovered slice's headline when it has one.
    const headline = hot && hot.headline;
    this.setLines(this.mainA, [this.centreText], -16, MAIN_PX, headline ? 1 - hotT : 1);
    this.setLines(this.mainB, headline ? [headline] : [], -16, MAIN_PX, headline ? hotT : 0);
    // Under it the detail: the section's name, crossfading to what the hovered slice does.
    const width = 2 * CENTRE - 8;
    this.setLines(this.detailA, wrap(this.title, width), 7, DETAIL_PX * 1.25, 1 - hotT, true);
    this.setLines(this.detailB, hot ? wrap(hot.detail ?? hot.label, width) : [], 7, DETAIL_PX * 1.25, hotT, true);
    // Dots: one per section, the current one in the accent colour.
    if (this.dots.dataset.key !== `${this.sections}:${this.section}`) {
      this.dots.dataset.key = `${this.sections}:${this.section}`;
      this.dots.replaceChildren();
      if (this.sections > 1) {
        const gap = 9, y = CENTRE - 9, x0 = (-gap * (this.sections - 1)) / 2;
        for (let i = 0; i < this.sections; i++) {
          const on = i === this.section;
          el('circle', { cx: x0 + gap * i, cy: y, r: on ? 2.6 : 2.1, class: on ? 'dot on' : 'dot' }, this.dots);
        }
      }
    }
  }

  setLines(node, lines, top, step, opacity, fromTop = false) {
    const key = lines.join('\n');
    if (node.dataset.key !== key) {
      node.dataset.key = key;
      node.replaceChildren();
      const first = fromTop ? top + step * 0.5 : top - ((lines.length - 1) * step) / 2;
      lines.forEach((line, i) => {
        const span = el('tspan', { x: 0, y: (first + i * step).toFixed(2), 'dominant-baseline': 'central' }, node);
        span.textContent = line;
      });
    }
    node.setAttribute('opacity', Math.max(0, Math.min(1, opacity)).toFixed(3));
  }

  setHot(i) {
    if (i >= 0 && this.slices[i]?.enabled === false) i = -1;
    if (i === this.hot) return;
    this.hot = i;
    this.kick();
  }

  open() {
    this.closing = false;
    this.dropped = false;
    this.closeT = 0;
    this.openT = this.reducedMotion ? 1 : 0;
    this.kick();
  }

  close(dropped) {
    this.closing = true;
    this.dropped = dropped;
    this.closeT = this.reducedMotion ? 1 : 0;
    this.kick();
  }

  kick() {
    if (this.frame) return;
    this.last = performance.now();
    this.frame = requestAnimationFrame((now) => this.tick(now));
  }

  tick(now) {
    this.frame = null;
    const dt = Math.min(64, now - this.last);
    this.last = now;
    let busy = false;
    if (this.openT < 1) { this.openT = Math.min(1, this.openT + dt / OPEN_MS); busy = true; }
    if (this.switchT < 1) { this.switchT = Math.min(1, this.switchT + dt / SWITCH_MS); busy = true; }
    if (this.closing && this.closeT < 1) { this.closeT = Math.min(1, this.closeT + dt / CLOSE_MS); busy = true; }
    for (let i = 0; i < this.hotT.length; i++) {
      const target = i === this.hot ? 1 : 0;
      if (this.hotT[i] !== target) {
        this.hotT[i] = this.reducedMotion ? target : approach(this.hotT[i], target, dt, HOT_TAU);
        busy = true;
      }
    }
    this.render();
    if (busy) this.kick();
    else if (this.closing && this.closeT >= 1 && this.onClosed) {
      const done = this.onClosed;
      this.onClosed = null;
      done();
    }
  }

  render() {
    this.paintTiles();
    this.paintCentre();
    // The open spring and fade-in; the close fades and settles (after a drop) or shrinks away.
    let zoom = 0.86 + 0.14 * spring(this.openT);
    let opacity = easeOutCubic((this.openT * OPEN_MS) / FADE_IN_MS);
    if (this.closing) {
      zoom *= 1 - (this.dropped ? 0.04 : 0.1) * easeOutCubic(this.closeT);
      opacity *= 1 - easeOutCubic(this.closeT);
    }
    this.root.setAttribute('transform', `scale(${zoom.toFixed(4)})`);
    this.root.setAttribute('opacity', opacity.toFixed(3));
    // Switching sections: the old tiles turn away and fade while the new ones turn in.
    if (this.switchT < 1) {
      const e = easeOutCubic(this.switchT), turn = 24 * this.switchDir;
      // The plate and its shadow grow or shrink to the new rings instead of jumping.
      const edge = outerEdge(this.layout);
      const k = (this.prevEdge + (edge - this.prevEdge) * e + PLATE) / (edge + PLATE);
      this.under.setAttribute('transform', `scale(${k.toFixed(4)})`);
      this.prevLayer.setAttribute('transform', `rotate(${(-turn * e).toFixed(2)})`);
      // The old labels are gone before the new ones show, so the two sets never read as one jumble.
      const t = this.switchT;
      this.prevLayer.setAttribute('opacity', Math.max(0, 1 - t / 0.3).toFixed(3));
      this.layer.setAttribute('transform', `rotate(${(turn * (1 - e)).toFixed(2)})`);
      this.layer.setAttribute('opacity', easeOutCubic((t - 0.3) / 0.7).toFixed(3));
    } else {
      if (this.prevLayer.childNodes.length) this.prevLayer.replaceChildren();
      this.under.removeAttribute('transform');
      this.layer.removeAttribute('transform');
      this.layer.setAttribute('opacity', 1);
    }
  }

  // Hit-test a point in DIPs from the wheel's centre.
  slotAt(x, y) {
    const d = Math.hypot(x, y);
    const deg = Math.atan2(x, -y) / RAD;
    return hitSlot(this.layout, d, deg, LIFT);
  }
}
