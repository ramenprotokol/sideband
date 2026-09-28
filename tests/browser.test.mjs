// End-to-end checks of the built dist/ in headless Chrome (audio muted):
// no console errors at desktop and true 400 px phone width, in both themes and
// with reduced motion; Start really starts the AudioWorklet (the worklet's own
// meter messages prove the C engine is rendering); keys play; links load,
// clamp and fail politely; MIDI denial is handled. window.__sideband is exposed
// only with ?test=1 or under automation.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { serve } from '../scripts/serve.mjs';
import { browserPlan, findChrome, launchChrome } from './cdp.mjs';
import { FONT_FILES, dist } from './helpers.mjs';
import { PARAM_COUNT, bytesToB64url } from '../web/js/patch.js';

const chromePath = findChrome();
const plan = browserPlan(chromePath);
let server;
let chrome;
let base;

before(async () => {
  if (!plan.run) return;
  server = await serve(dist, 0);
  base = `http://127.0.0.1:${server.address().port}/`;
  chrome = await launchChrome(chromePath);
});

after(async () => {
  await chrome?.close();
  await new Promise((r) => (server ? server.close(r) : r()));
});

async function open(opts, hash = '') {
  const page = await chrome.openPage(opts);
  await page.navigate(`${base}?test=1${hash}`);
  await page.waitFor('window.__sideband && document.querySelectorAll(".op").length === 6', 20000);
  return page;
}

function noProblems(page, label) {
  assert.deepEqual(page.problems, [], `${label}: ${JSON.stringify(page.problems)}`);
}

const layout = `JSON.stringify({
  scrollW: document.documentElement.scrollWidth,
  clientW: document.documentElement.clientWidth,
  theme: document.documentElement.dataset.theme,
  ops: document.querySelectorAll('.op').length,
  cards: document.querySelectorAll('.algo-card[role=radio]').length,
  faders: document.querySelectorAll('input[type=range]').length,
  keys: document.querySelectorAll('.key').length,
  checked: document.querySelector('.algo-card[aria-checked=true]')?.dataset.algo,
  preset: document.querySelector('.preset[aria-pressed=true] .preset-name')?.textContent,
  volume: document.querySelector('#volume').value,
  volumeText: document.querySelector('.volume .fader-value').textContent,
  idle: document.querySelector('#scope-idle').hidden,
  hash: location.hash,
})`;

test('desktop: loads clean, Start runs the C engine in the AudioWorklet, keys play', { skip: plan.skip }, async () => {
  if (plan.fail) assert.fail(plan.fail);
  const page = await open({ width: 1280, height: 800, scheme: 'dark' });
  try {
    const l = JSON.parse(await page.evaluate(layout));
    assert.equal(l.scrollW, l.clientW, 'no horizontal scroll');
    assert.equal(l.theme, 'dark');
    assert.equal(l.ops, 6);
    assert.equal(l.cards, 8);
    assert.equal(l.faders, 6 * 12 + 2, '72 operator faders + feedback + volume');
    assert.equal(l.keys, 24);
    assert.equal(l.checked, '0');
    assert.equal(l.preset, 'E.PIANO');
    assert.equal(l.volume, '50');
    assert.equal(l.volumeText, '−12.0 dB');
    assert.equal(l.hash, '', 'no link is written until the voice changes');
    assert.equal(await page.evaluate('window.__sideband.engine.ready'), false, 'no audio before a gesture');

    await page.click('#start');
    await page.waitFor('window.__sideband.engine.ready', 15000);
    await page.waitFor('window.__sideband.engine.ctx.state === "running"', 10000);
    assert.match(await page.evaluate('document.querySelector("#start").textContent'), /AUDIO ON/);
    assert.equal(await page.evaluate('document.querySelector("#scope-idle").hidden'), true);

    await page.key('keyDown', 'KeyZ', 'z');
    await page.key('keyDown', 'KeyC', 'c');
    // The meter comes from the audio thread: proof the WASM engine renders.
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("2 /")', 10000);
    await page.waitFor(`(() => { const a = window.__sideband.engine.analyser; const b = new Float32Array(a.fftSize); a.getFloatTimeDomainData(b); return b.some((v) => Math.abs(v) > 0.01); })()`, 10000);
    await page.waitFor('/dBFS/.test(document.querySelector("#meter-peak").textContent)', 10000);
    const peakText = await page.evaluate('document.querySelector("#meter-peak").textContent');
    const peakDb = -Number(peakText.replace(/[^\d.]/g, ''));
    assert.ok(peakDb <= -1, `measured output peak ${peakText} must stay below the -1 dBFS ceiling`);
    assert.ok(await page.evaluate('window.__sideband.monitor.frames') > 0, 'the scope is drawing');

    await page.key('keyUp', 'KeyZ', 'z');
    await page.key('keyUp', 'KeyC', 'c');
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("0 /")', 10000);

    // Editing a fader updates the link in the address bar.
    await page.evaluate(`(() => { const f = document.querySelector('#op2-level'); f.value = '40'; f.dispatchEvent(new Event('input', { bubbles: true })); })()`);
    await page.waitFor('location.hash.startsWith("#p=1.")', 3000);
    assert.equal(await page.evaluate('window.__sideband.state.values[2 + 13 + 4]'), 40);
    assert.equal(await page.evaluate('document.querySelector(".preset[aria-pressed=true]")'), null);

    // The algorithm chart is a keyboard radio group.
    await page.evaluate('document.querySelector(".algo-card[aria-checked=true]").focus()');
    await page.key('keyDown', 'ArrowRight', 'ArrowRight');
    await page.key('keyUp', 'ArrowRight', 'ArrowRight');
    assert.equal(await page.evaluate('window.__sideband.state.values[0]'), 1);
    assert.equal(await page.evaluate('document.activeElement.dataset.algo'), '1');
    assert.match(await page.evaluate('document.querySelector("#op1 .pin-in").textContent'), /OP2/);

    // Volume: the fader sets the engine's gain; the default is restored on reload.
    await page.evaluate(`(() => { const f = document.querySelector('#volume'); f.value = '0'; f.dispatchEvent(new Event('input', { bubbles: true })); })()`);
    assert.equal(await page.evaluate('window.__sideband.engine.volume'), 0);
    assert.equal(await page.evaluate('document.querySelector(".volume .fader-value").textContent'), 'OFF');

    // All notes off and pause work without errors.
    await page.click('#panic');
    await page.click('#start');
    await page.waitFor('window.__sideband.engine.ctx.state === "suspended"', 5000);
    await page.click('#start');
    await page.waitFor('window.__sideband.engine.ctx.state === "running"', 5000);
    noProblems(page, 'desktop');
  } finally {
    await page.close();
  }
});

test('every request stays on this site, every font face loads from it, and the console stays clean', { skip: plan.skip }, async () => {
  if (plan.fail) assert.fail(plan.fail);
  const page = await open({ width: 1280, height: 800, scheme: 'dark' });
  try {
    const faces = [
      '400 16px "IBM Plex Mono"',
      '500 16px "IBM Plex Mono"',
      '600 16px "IBM Plex Mono"',
      '400 16px "IBM Plex Sans Condensed"',
      '500 16px "IBM Plex Sans Condensed"',
    ];
    await page.evaluate(`Promise.all(${JSON.stringify(faces)}.map((f) => document.fonts.load(f, 'sideband'))).then(() => document.fonts.ready).then(() => true)`);
    const urls = await page.evaluate('performance.getEntriesByType("resource").map((e) => e.name)');
    assert.deepEqual(urls.filter((u) => !u.startsWith(base)), [], 'no request left this site');
    for (const f of faces) assert.equal(await page.evaluate(`document.fonts.check(${JSON.stringify(f)}, 'sideband')`), true, `document.fonts.check(${f})`);
    const declared = await page.evaluate('[...document.fonts].map((f) => `${f.family.replace(/"/g, "")} ${f.weight} ${f.status}`)');
    assert.equal(declared.length, FONT_FILES.length, `one face per font file: ${declared.join(', ')}`);
    assert.deepEqual(declared.filter((f) => !f.endsWith(' loaded')), [], 'every declared face loaded');
    const fontUrls = urls.filter((u) => u.endsWith('.woff2')).map((u) => u.slice(base.length)).sort();
    assert.deepEqual(fontUrls, FONT_FILES.map((f) => `fonts/${f}`), 'each font file fetched from this site');
    noProblems(page, 'fonts');
  } finally {
    await page.close();
  }
});

test('phone, 400 px, light theme, reduced motion: no horizontal scroll and a slowed scope', { skip: plan.skip }, async () => {
  if (plan.fail) assert.fail(plan.fail);
  const page = await open({ width: 400, height: 860, mobile: true, scale: 2, scheme: 'light', reducedMotion: true });
  try {
    const l = JSON.parse(await page.evaluate(layout));
    assert.equal(l.clientW, 400);
    assert.equal(l.scrollW, l.clientW, 'no horizontal scroll at 400 px');
    assert.equal(l.theme, 'light');
    assert.ok(await page.evaluate('window.__sideband.monitor.interval') > 0, 'reduced motion throttles the scope');
    // A key press is enough to start audio (it is a user gesture).
    await page.key('keyDown', 'KeyQ', 'q');
    await page.waitFor('window.__sideband.engine.ready', 15000);
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("1 /")', 10000);
    await page.key('keyUp', 'KeyQ', 'q');
    // Every control stays inside the viewport.
    const overflow = await page.evaluate(`[...document.querySelectorAll('button, input, .op, .algo-card, .crt')].filter((e) => { const r = e.getBoundingClientRect(); return r.width && (r.left < -1 || r.right > 401); }).map((e) => e.id || e.className)`);
    assert.deepEqual(overflow, []);
    // Theme toggle switches to the teal panel.
    await page.click('#theme-toggle');
    assert.equal(await page.evaluate('document.documentElement.dataset.theme'), 'dark');
    noProblems(page, 'phone');
  } finally {
    await page.close();
  }
});

// The dock is fixed to the bottom of the window. Focusing a control scrolls it
// into view, and without scroll padding the browser parks it right behind the
// dock, so a keyboard user loses track of where focus is.
const hiddenFocus = `(() => {
  const dock = document.getElementById('dock');
  const stops = [...document.querySelectorAll('a[href], button, input, [tabindex]')]
    .filter((e) => e.tabIndex >= 0 && !e.disabled && !dock.contains(e) && e.getClientRects().length);
  const hidden = [];
  for (const e of stops) {
    e.focus();
    const r = e.getBoundingClientRect();
    if (Math.min(r.bottom, dock.getBoundingClientRect().top) - Math.max(r.top, 0) <= 0) hidden.push(e.id || e.className);
  }
  return JSON.stringify({ stops: stops.length, hidden });
})()`;

for (const [label, opts] of [['1280×800', { width: 1280, height: 800 }], ['400 px phone', { width: 400, height: 860, mobile: true, scale: 2 }]]) {
  test(`keyboard focus is never hidden behind the dock (${label})`, { skip: plan.skip }, async () => {
    if (plan.fail) assert.fail(plan.fail);
    const page = await open(opts);
    try {
      const r = JSON.parse(await page.evaluate(hiddenFocus));
      assert.ok(r.stops > 80, `found ${r.stops} focus stops`);
      assert.deepEqual(r.hidden, [], `${r.hidden.length} of ${r.stops} focused controls sat behind the dock`);
      noProblems(page, label);
    } finally {
      await page.close();
    }
  });
}

test('phone, strict autoplay: the first tap on a key unlocks audio and sounds that note', { skip: plan.skip }, async () => {
  if (plan.fail) assert.fail(plan.fail);
  const page = await chrome.openPage({ width: 400, height: 860, mobile: true, scale: 2, scheme: 'dark' });
  try {
    await page.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
    await page.navigate(`${base}?test=1`);
    await page.waitFor('window.__sideband && document.querySelectorAll(".op").length === 6', 20000);
    // The policy is really enforced: a context made without a gesture stays suspended.
    assert.equal(await page.evaluate('(async () => { const c = new AudioContext(); await new Promise((r) => setTimeout(r, 300)); const s = c.state; await c.close(); return s; })()'), 'suspended');
    // Record what the audio thread reports, so a short note cannot slip between polls.
    await page.evaluate(`(() => { window.__heard = 0; const e = window.__sideband.engine; const on = e.onMeter; e.onMeter = (m) => { window.__heard = Math.max(window.__heard, m.voices | 0); on(m); }; })()`);
    // One tap: touchstart creates the context (still suspended), touchend unlocks it.
    await page.tap('.key.white:nth-child(5)', 120);
    await page.waitFor('window.__sideband.engine.ctx && window.__sideband.engine.ctx.state === "running"', 10000);
    await page.waitFor('window.__heard >= 1', 10000);
    assert.equal(await page.evaluate('window.__sideband.engine.ready'), true);
    // ...and the voice ends on its own after the short tap.
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("0 /")', 10000);
    // A second tap plays straight away.
    await page.evaluate('window.__heard = 0');
    await page.tap('.key.white:nth-child(8)', 300);
    await page.waitFor('window.__heard >= 1', 5000);
    noProblems(page, 'touch');
  } finally {
    await page.close();
  }
});

test('a hostile link is clamped, a broken link fails politely, and neither makes noise or errors', { skip: plan.skip }, async () => {
  if (plan.fail) assert.fail(plan.fail);
  const hostile = `#p=1.${bytesToB64url(new Array(PARAM_COUNT).fill(255))}&n=%3Cimg%20src%3Dx%20onerror%3Dalert(1)%3E`;
  let page = await open({ width: 1024, height: 768, scheme: 'dark' }, hostile);
  try {
    const message = await page.evaluate('document.querySelector("#message").textContent');
    assert.match(message, /80 values were out of range/);
    const inRange = await page.evaluate(`window.__sideband.state.values.every((v, id) => {
      const f = id === 0 ? 7 : id === 1 ? 7 : [1, 31, 99, 20, 99, 99, 99, 99, 99, 99, 99, 99, 99][(id - 2) % 13];
      return v === f; })`);
    assert.ok(inRange, 'every value clamped to its maximum');
    assert.equal(await page.evaluate('document.querySelector("#patch-name").value'), 'IMG SRCX ONERROR');
    assert.equal(await page.evaluate('document.querySelectorAll("img").length'), 0);
    // Even this patch, at full volume with a chord, is measured under the ceiling.
    await page.click('#start');
    await page.waitFor('window.__sideband.engine.ready', 15000);
    await page.evaluate(`(() => { const f = document.querySelector('#volume'); f.value = '100'; f.dispatchEvent(new Event('input', { bubbles: true })); })()`);
    for (const [c, t] of [['KeyZ', 'z'], ['KeyX', 'x'], ['KeyC', 'c'], ['KeyV', 'v'], ['KeyB', 'b'], ['KeyN', 'n'], ['KeyM', 'm'], ['KeyQ', 'q'], ['KeyW', 'w']]) {
      await page.key('keyDown', c, t);
    }
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("8 /")', 10000);
    await new Promise((r) => setTimeout(r, 400));
    const peakText = await page.evaluate('document.querySelector("#meter-peak").textContent');
    assert.ok(-Number(peakText.replace(/[^\d.]/g, '')) <= -1, `peak ${peakText}`);
    // The compressor pulls a patch this dense down (the limiter may or may not
    // still be catching peaks when the meter is read).
    assert.match(await page.evaluate('document.querySelector("#meter-comp").textContent'), /−\d/, 'the compressor is working');
    noProblems(page, 'hostile link');
  } finally {
    await page.close();
  }

  page = await open({ width: 1024, height: 768, scheme: 'dark' }, '#p=1.not-a-patch');
  try {
    const message = await page.evaluate('document.querySelector("#message").textContent');
    assert.match(message, /107/);
    assert.match(message, /Loaded E\.PIANO instead/);
    assert.equal(await page.evaluate('location.hash'), '');
    noProblems(page, 'broken link');
  } finally {
    await page.close();
  }

  page = await open({ width: 1024, height: 768, scheme: 'dark' }, `#p=${'1'.repeat(400)}`);
  try {
    assert.match(await page.evaluate('document.querySelector("#message").textContent'), /too long/);
    noProblems(page, 'long link');
  } finally {
    await page.close();
  }
});

test('MIDI: a declined permission gives a clear message and the keyboard still works', { skip: plan.skip }, async () => {
  if (plan.fail) assert.fail(plan.fail);
  const page = await open({ width: 1280, height: 800, scheme: 'dark' });
  try {
    const origin = new URL(base).origin;
    await chrome.send('Browser.setPermission', { permission: { name: 'midi' }, setting: 'denied', origin });
    await page.click('#midi');
    await page.waitFor('/MIDI/.test(document.querySelector("#message").textContent)', 10000);
    const message = await page.evaluate('document.querySelector("#message").textContent');
    assert.match(message, /declined|does not offer|could not start/);
    await page.waitFor('window.__sideband.engine.ready', 15000);
    await page.key('keyDown', 'KeyN', 'n');
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("1 /")', 10000);
    // Shifting the octave while a key is held still releases the right note.
    await page.key('keyDown', 'Equal', '=');
    await page.key('keyUp', 'Equal', '=');
    assert.equal(await page.evaluate('document.querySelector("#oct-readout").textContent'), 'C4');
    await page.key('keyUp', 'KeyN', 'n');
    await page.waitFor('document.querySelector("#meter-voices").textContent.startsWith("0 /")', 10000);
    assert.equal(await page.evaluate('document.querySelectorAll(".key.down").length'), 0);
    noProblems(page, 'midi');
  } finally {
    await page.close();
  }
});
