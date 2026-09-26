// WCAG AA contrast (4.5:1) for text in both themes, computed from the colour
// tokens in web/styles.css, including semi-transparent tokens composited on
// the surfaces they sit on.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { root } from './helpers.mjs';

const css = readFileSync(join(root, 'web', 'styles.css'), 'utf8');

function tokens(selector) {
  const start = css.indexOf(`${selector} {`);
  assert.ok(start >= 0, `no ${selector} block`);
  const body = css.slice(start, css.indexOf('}', start));
  return Object.fromEntries([...body.matchAll(/--([a-z0-9-]+):\s*([^;]+);/g)].map((m) => [m[1], m[2].trim()]));
}

function parse(color) {
  let m = /^#([0-9a-f]{6})$/i.exec(color);
  if (m) return [0, 2, 4].map((i) => parseInt(m[1].slice(i, i + 2), 16)).concat(1);
  m = /^rgba?\(([^)]+)\)$/.exec(color);
  if (m) {
    const [r, g, b, a = '1'] = m[1].split(',').map((s) => s.trim());
    return [Number(r), Number(g), Number(b), Number(a)];
  }
  throw new Error(`cannot parse ${color}`);
}

const over = ([r, g, b, a], [R, G, B]) => [r * a + R * (1 - a), g * a + G * (1 - a), b * a + B * (1 - a), 1];

function luminance([r, g, b]) {
  const f = (c) => {
    const s = c / 255;
    return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b);
}

function ratio(fg, bg) {
  const b = parse(bg);
  const f = over(parse(fg), b);
  const [l1, l2] = [luminance(f), luminance(b)].sort((x, y) => y - x);
  return (l1 + 0.05) / (l2 + 0.05);
}

const THEMES = { dark: tokens('[data-theme="dark"]'), light: tokens('[data-theme="light"]') };

// [text token, background token] pairs that the page actually uses.
const PAIRS = [
  ['ink', 'panel'], ['ink', 'panel-deep'], ['ink', 'panel-raise'],
  ['ink-2', 'panel'], ['ink-2', 'panel-deep'], ['ink-2', 'panel-raise'],
  ['panel', 'ink'], // inverted: selected preset, selected algorithm, carrier badge
  ['spot', 'panel'], ['spot', 'panel-deep'], // NOTE marker, active limiter readout
  ['spot-ink', 'spot'], // START button, pressed keys
  ['key-white-ink', 'key-white'], ['key-black-ink', 'key-black'],
];

for (const [theme, t] of Object.entries(THEMES)) {
  test(`${theme} theme: every text pairing reaches 4.5:1`, () => {
    for (const [fg, bg] of PAIRS) {
      assert.ok(t[fg] && t[bg], `${theme}: missing --${fg} or --${bg}`);
      const r = ratio(t[fg], t[bg]);
      assert.ok(r >= 4.5, `${theme}: --${fg} on --${bg} is ${r.toFixed(2)}:1`);
    }
  });
}

test('phosphor labels on the CRT reach 4.5:1 against the darkest and lightest screen colour', () => {
  // The CRT is dark in both themes; its labels are drawn at 75% alpha.
  for (const bg of ['#020906', '#0a2a1a']) {
    assert.ok(ratio('rgba(160, 255, 190, 0.75)', bg) >= 4.5);
    assert.ok(ratio('#9dffbc', bg) >= 4.5);
  }
});
