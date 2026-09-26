// The patch model and the share-link codec: round trips, clamping, and
// hostile links (every one must decode to a safe patch or a clear message,
// quickly, without throwing).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PARAM_COUNT, ENCODED_LENGTH, MAX_HASH_LENGTH, NAME_MAX, specFor, clampValue, defaultValues,
  encodePatch, decodePatch, patchToHash, patchFromHash, sanitizeName, bytesToB64url, b64urlToBytes,
  frequencyLabel, paramId, sweepSeconds,
} from '../web/js/patch.js';
import { PRESETS } from '../web/js/presets.js';
import { parseMidi } from '../web/js/midi.js';

function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

function randomPatch(r) {
  return Array.from({ length: PARAM_COUNT }, (_, id) => {
    const s = specFor(id);
    return s.min + Math.floor(r() * (s.max - s.min + 1));
  });
}

const inRange = (values) => values.length === PARAM_COUNT &&
  values.every((v, id) => Number.isInteger(v) && v >= specFor(id).min && v <= specFor(id).max);

test('the layout is 80 parameters and the link body is 107 characters', () => {
  assert.equal(PARAM_COUNT, 80);
  assert.equal(ENCODED_LENGTH, 107);
  assert.equal(encodePatch(defaultValues()).length, 2 + 107);
});

test('base64url round-trips every byte value at every length', () => {
  for (let len = 0; len < 12; len++) {
    for (let start = 0; start < 256; start += 17) {
      const bytes = Array.from({ length: len }, (_, i) => (start + i * 31) & 255);
      assert.deepEqual(b64urlToBytes(bytesToB64url(bytes)), bytes);
    }
  }
  assert.equal(b64urlToBytes('A'), null, 'a single leftover character is not valid');
  assert.equal(b64urlToBytes('AA=A'), null, 'padding and other characters are refused');
  assert.equal(b64urlToBytes('AA+/'), null, 'standard base64 characters are refused');
});

test('patches round-trip through the URL exactly', () => {
  const r = rng(1234);
  for (let i = 0; i < 2000; i++) {
    const values = randomPatch(r);
    const name = ['', 'E.PIANO', 'MY BELL 2', 'A/B+C-D'][i % 4];
    const back = patchFromHash(patchToHash(values, name));
    assert.equal(back.status, 'ok');
    assert.deepEqual(back.values, values);
    assert.equal(back.clamped, 0);
    assert.equal(back.name, name);
  }
});

test('every preset is valid, distinct and survives a round trip', () => {
  assert.equal(PRESETS.length, 6);
  assert.deepEqual(PRESETS.map((p) => p.name), ['E.PIANO', 'BELL', 'BASS', 'BRASS', 'PAD', 'MALLET']);
  const seen = new Set();
  for (const p of PRESETS) {
    assert.ok(inRange(p.values), p.name);
    assert.deepEqual(decodePatch(encodePatch(p.values)).values, p.values);
    const key = p.values.join(',');
    assert.ok(!seen.has(key), `${p.name} duplicates another preset`);
    seen.add(key);
    assert.ok(p.note.length > 20, `${p.name} needs a description`);
  }
});

test('out-of-range bytes in a link are clamped and counted', () => {
  const values = defaultValues();
  const good = encodePatch(values);
  // Every byte 255: far above every maximum.
  const huge = `1.${bytesToB64url(new Array(PARAM_COUNT).fill(255))}`;
  const r = decodePatch(huge);
  assert.ok(inRange(r.values));
  assert.equal(r.clamped, PARAM_COUNT);
  r.values.forEach((v, id) => assert.equal(v, specFor(id).max));
  // One bad byte: algorithm 200.
  const bytes = b64urlToBytes(good.slice(2));
  bytes[0] = 200;
  const one = decodePatch(`1.${bytesToB64url(bytes)}`);
  assert.equal(one.clamped, 1);
  assert.equal(one.values[0], 7);
});

test('clampValue mirrors the engine: rounds half away from zero, NaN falls back to default', () => {
  const det = paramId(0, 'detune');
  assert.equal(clampValue(det, -2.5), -3);
  assert.equal(clampValue(det, 2.5), 3);
  assert.equal(clampValue(det, -99), -20);
  assert.equal(clampValue(0, 1e300), 7);
  assert.equal(clampValue(0, Number.NaN), specFor(0).def);
  assert.equal(clampValue(0, Infinity), specFor(0).def);
  assert.equal(clampValue(0, '3'), 3);
  assert.equal(clampValue(0, 'x'), specFor(0).def);
});

test('hostile links: every one gives a clear message or a safe patch, and never throws', () => {
  const body = encodePatch(defaultValues()).slice(2);
  const cases = [
    ['#p=', 'error'],
    ['#p=1.', 'error'],
    [`#p=2.${body}`, 'error'],
    [`#p=999999.${body}`, 'error'],
    [`#p=1.${body}A`, 'error'],
    [`#p=1.${body.slice(1)}`, 'error'],
    [`#p=1.${body.slice(0, -1)}*`, 'error'],
    [`#p=1.${body.slice(0, -1)}%`, 'error'],
    [`#p=${'9'.repeat(5000)}`, 'error'],
    ['#p=%E0%A4%A', 'error'],
    ['#' + 'x'.repeat(MAX_HASH_LENGTH + 1), 'error'],
    ['#n=HELLO', 'none'],
    ['#', 'none'],
    ['', 'none'],
    ['#&&&&&&', 'none'],
    ['#constructor=1&__proto__=2', 'none'],
    [`#p=1.${body}&n=${'%3Cscript%3E'.repeat(3)}`, 'ok'],
  ];
  for (const [hash, want] of cases) {
    const r = patchFromHash(hash);
    assert.equal(r.status, want, `${hash.slice(0, 40)} -> ${JSON.stringify(r).slice(0, 120)}`);
    if (r.status === 'error') assert.ok(r.message.length > 10 && r.message.length < 200);
    if (r.status === 'ok') {
      assert.ok(inRange(r.values));
      assert.match(r.name, /^[A-Z0-9 .\-+/]*$/);
    }
  }
  assert.equal(patchFromHash(123).status, 'none');
  assert.equal(patchFromHash(null).status, 'none');
});

test('random garbage links never throw, never hang and never produce out-of-range values', () => {
  const r = rng(99);
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.=&%#p1n';
  const t0 = performance.now();
  let ok = 0;
  for (let i = 0; i < 20000; i++) {
    const len = Math.floor(r() * 400);
    let s = i % 3 === 0 ? '#p=1.' : '#';
    for (let k = 0; k < len; k++) s += alphabet[Math.floor(r() * alphabet.length)];
    const out = patchFromHash(s);
    assert.ok(['ok', 'error', 'none'].includes(out.status));
    if (out.status === 'ok') {
      ok++;
      assert.ok(inRange(out.values));
    }
  }
  // Well-formed random bodies of the right length always decode to a safe patch.
  for (let i = 0; i < 2000; i++) {
    let body = '';
    for (let k = 0; k < ENCODED_LENGTH; k++) body += alphabet[Math.floor(r() * 64)];
    const out = patchFromHash(`#p=1.${body}`);
    assert.equal(out.status, 'ok');
    assert.ok(inRange(out.values));
  }
  const ms = performance.now() - t0;
  assert.ok(ms < 5000, `decoding 22,000 links took ${ms.toFixed(0)} ms`);
  assert.ok(ok >= 0);
});

test('names are cleaned to the display alphabet and length', () => {
  assert.equal(sanitizeName('  my  bell!! <b>2</b> '), 'MY BELL B2/B');
  assert.equal(sanitizeName('x'.repeat(100)).length, NAME_MAX);
  assert.equal(sanitizeName(undefined), '');
  assert.equal(sanitizeName('\u0000‮EVIL'), 'EVIL');
});

test('volume is never part of a link', () => {
  const hash = patchToHash(PRESETS[0].values, 'X');
  assert.doesNotMatch(hash, /vol/i);
  assert.deepEqual(Object.keys(patchFromHash(`${hash}&v=1&volume=100`)).sort(), ['clamped', 'name', 'status', 'values']);
});

test('frequency readouts follow the engine formulas', () => {
  const v = defaultValues();
  const set = (k, x) => { v[paramId(0, k)] = x; };
  assert.equal(frequencyLabel(v, 0), '×1.00');
  set('coarse', 0);
  assert.equal(frequencyLabel(v, 0), '×0.50');
  set('coarse', 3); set('fine', 50); set('detune', -4);
  assert.equal(frequencyLabel(v, 0), '×4.50 −4c');
  set('mode', 1); set('coarse', 2); set('fine', 0); set('detune', 0);
  assert.equal(frequencyLabel(v, 0), '100 Hz');
  set('coarse', 3); set('fine', 99);
  assert.equal(frequencyLabel(v, 0), '9.77 kHz');
  assert.ok(Math.abs(sweepSeconds(0) - 40) < 1e-9);
  assert.ok(Math.abs(sweepSeconds(99) - 0.0015) < 1e-9);
});

test('MIDI messages are parsed defensively', () => {
  assert.deepEqual(parseMidi([0x90, 60, 100]), { type: 'on', note: 60, velocity: 100 });
  assert.deepEqual(parseMidi([0x93, 60, 0]), { type: 'off', note: 60 });
  assert.deepEqual(parseMidi([0x80, 61, 5]), { type: 'off', note: 61 });
  assert.deepEqual(parseMidi([0xb0, 123, 0]), { type: 'allOff' });
  assert.deepEqual(parseMidi([0x90, 200, 255]), { type: 'on', note: 72, velocity: 127 });
  assert.equal(parseMidi([0x90, 60]), null);
  assert.equal(parseMidi([0xf8]), null);
  assert.equal(parseMidi(null), null);
  assert.equal(parseMidi([0xe0, 0, 64]), null);
});
