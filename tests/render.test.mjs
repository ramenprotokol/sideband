// Offline render: the real dist/sideband.wasm, run in Node. Checks that the JS
// patch model and algorithm drawings match the compiled C, that WASM output is
// bit-identical to the native build (golden hash shared with the C tests),
// that notes land on the right pitch, and that nothing a link can express
// gets past the output ceiling.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { PARAM_COUNT, specFor, decodePatch, bytesToB64url, defaultValues, paramId } from '../web/js/patch.js';
import { PRESETS } from '../web/js/presets.js';
import { ALGORITHMS, modMask, carrierMask, FEEDBACK_OP } from '../web/js/algorithms.js';
import { dist, loadEngine, fnv1a, peak, spectrum, peakHz } from './helpers.mjs';

const SR = 48000;
const CEILING = 0.89125094; // -1 dBFS, SIDEBAND_CEILING
const THRESHOLD = 0.70794578; // -3 dBFS, SIDEBAND_LIMIT_THRESHOLD
const GOLDEN_HASH = 0x900e4f46; // same constant as tests/c/test_sideband.c

test('the WASM module is self-contained and exports the engine API', () => {
  const mod = new WebAssembly.Module(readFileSync(join(dist, 'sideband.wasm')));
  assert.deepEqual(WebAssembly.Module.imports(mod), [], 'no imports: no libc, no JS callbacks');
  const names = WebAssembly.Module.exports(mod).map((e) => e.name);
  for (const n of ['sideband_init', 'sideband_render', 'sideband_monitor', 'sideband_set_param', 'sideband_note_on', 'sideband_note_off', 'sideband_set_volume', 'sideband_comp_gain', 'sideband_gain_reduction', 'memory']) {
    assert.ok(names.includes(n), `missing export ${n}`);
  }
});

test('the JS parameter spec matches the C engine for all 80 parameters', () => {
  const { x } = loadEngine();
  x.sideband_init(SR);
  for (let id = 0; id < PARAM_COUNT; id++) {
    const s = specFor(id);
    assert.equal(x.sideband_param_min(id), s.min, `min of ${id}`);
    assert.equal(x.sideband_param_max(id), s.max, `max of ${id}`);
    assert.equal(x.sideband_param_default(id), s.def, `default of ${id}`);
    assert.equal(x.sideband_get_param(id), s.def, `initial value of ${id}`);
  }
  assert.equal(x.sideband_param_max(PARAM_COUNT), 0, 'ids past the end are rejected');
});

test('the algorithm drawings match the routing compiled into the engine', () => {
  const { x } = loadEngine();
  assert.equal(ALGORITHMS.length, 8);
  for (let a = 0; a < 8; a++) {
    assert.equal(x.sideband_algo_carrier_mask(a), carrierMask(a), `carriers of algorithm ${a + 1}`);
    assert.equal(x.sideband_algo_feedback_op(a), FEEDBACK_OP - 1);
    for (let op = 0; op < 6; op++) {
      assert.equal(x.sideband_algo_mod_mask(a, op), modMask(a, op), `algorithm ${a + 1}, inputs of OP${op + 1}`);
    }
    // Every operator has a place in the drawing.
    assert.deepEqual(Object.keys(ALGORITHMS[a].pos).map(Number).sort(), [1, 2, 3, 4, 5, 6]);
  }
});

// The same scenario as test_determinism() in the C tests.
function epLikeScenario(x, render) {
  const setp = (op, off, v) => x.sideband_set_param(2 + op * 13 + off, v);
  const organ = (op, level, coarse) => [0, coarse, 0, 0, level, 99, 99, 99, 80, 99, 99, 99, 0].forEach((v, i) => setp(op, i, v));
  x.sideband_init(SR);
  x.sideband_set_param(0, 0);
  x.sideband_set_param(1, 5);
  for (let k = 0; k < 6; k++) organ(k, 0, 1);
  organ(0, 99, 1); setp(0, 6, 30); setp(0, 11, 0); setp(0, 10, 80);
  organ(1, 70, 14); setp(1, 6, 60); setp(1, 10, 0); setp(1, 11, 0);
  organ(2, 95, 1); setp(2, 3, 5);
  organ(3, 75, 1);
  organ(4, 90, 1); setp(4, 3, -5);
  organ(5, 70, 1);
  const n = SR;
  const out = new Float32Array(n);
  const quarter = () => render(n / 4).out.subarray(0, n / 4); // n/4 is not a whole number of blocks
  x.sideband_note_on(60, 100); out.set(quarter(), 0);
  x.sideband_note_on(64, 90); x.sideband_note_on(67, 80); out.set(quarter(), n / 4);
  x.sideband_note_off(60); out.set(quarter(), n / 2);
  x.sideband_all_notes_off(); out.set(quarter(), (3 * n) / 4);
  return out;
}

test('WASM output is deterministic and bit-identical to the native C build', () => {
  const a = loadEngine();
  const first = epLikeScenario(a.x, a.render);
  const second = epLikeScenario(a.x, a.render);
  assert.deepEqual(first, second, 'same patch + same notes = same samples');
  const fresh = loadEngine();
  assert.equal(fnv1a(epLikeScenario(fresh.x, fresh.render)), fnv1a(first));
  assert.equal(fnv1a(first), GOLDEN_HASH, `hash 0x${fnv1a(first).toString(16)} vs golden 0x${GOLDEN_HASH.toString(16)}`);
});

test('a known note lands on its pitch: A4 is 440 Hz, and ratio 2 is 880 Hz', () => {
  const { x, render, setPatch } = loadEngine();
  x.sideband_init(SR);
  x.sideband_note_on(69, 100);
  const { mon } = render(SR / 2);
  const hz = peakHz(spectrum(mon, 8192, 8192), SR);
  assert.ok(Math.abs(hz - 440) < 0.5, `init voice A4 peak at ${hz.toFixed(2)} Hz`);

  const v = defaultValues();
  v[paramId(0, 'coarse')] = 2;
  x.sideband_init(SR);
  setPatch(v);
  x.sideband_note_on(69, 100);
  const two = peakHz(spectrum(render(SR / 2).mon, 8192, 8192), SR);
  assert.ok(Math.abs(two - 880) < 0.5, `ratio 2 peak at ${two.toFixed(2)} Hz`);
});

test('every preset: the strongest peak is on the key, output is finite, audible and within the ceiling', () => {
  for (const p of PRESETS) {
    const { x, render, setPatch } = loadEngine();
    x.sideband_init(SR);
    setPatch(p.values);
    x.sideband_set_volume(1);
    // BASS is played in its own register: key C3 sounds C2 (x0.5 carriers).
    const bass = p.name === 'BASS';
    const note = bass ? 48 : 69;
    const f0 = bass ? 440 * 2 ** ((48 - 69) / 12) * 0.5 : 440;
    x.sideband_note_on(note, 100);
    const held = render(SR * 1.5);
    x.sideband_note_off(note);
    const released = render(SR * 1.5);
    const all = [...held.out, ...released.out];
    assert.ok(all.every(Number.isFinite), `${p.name}: non-finite sample`);
    assert.ok(peak(held.out) <= CEILING, `${p.name}: over the ceiling`);
    assert.ok(peak(held.mon) > 0.05, `${p.name}: too quiet (${peak(held.mon)})`);
    const start = p.name === 'PAD' ? SR : 2048; // the pad takes its time
    const mag = spectrum(held.mon, start, 8192);
    const hz = peakHz(mag, SR);
    // BELL is inharmonic by design; PAD's detuned pair beats, so its loudest
    // partial may be the octave. Every other preset peaks on the fundamental.
    const harmonic = Math.max(1, Math.round(hz / f0));
    if (p.name !== 'BELL') assert.ok(Math.abs(hz - f0 * harmonic) < 2, `${p.name}: strongest peak at ${hz.toFixed(1)} Hz`);
    if (!['BELL', 'PAD'].includes(p.name)) assert.equal(harmonic, 1, `${p.name}: strongest peak at ${hz.toFixed(1)} Hz`);
    if (bass) {
      // A bass needs harmonics a small speaker can reproduce: in the sustain,
      // harmonics 2 to 5 sit within 12 dB of the fundamental.
      const sus = spectrum(held.mon, Math.round(SR * 0.6), 16384);
      const at = (h) => { const i = Math.round((f0 * h * 16384) / SR); return Math.max(sus[i - 1], sus[i], sus[i + 1]); };
      for (const h of [2, 3, 4, 5]) {
        const rel = 20 * Math.log10(at(h) / at(1));
        assert.ok(rel > -12 && rel < 0, `BASS harmonic ${h} is ${rel.toFixed(1)} dB re the fundamental`);
      }
    }
    assert.ok(peak(released.out.subarray(SR)) < 0.02, `${p.name}: still loud a second after release`);
    assert.equal(x.sideband_active_voices() <= 1, true);
  }
});

test('nothing a link can express exceeds the ceiling: hostile patches at full volume, dense chords', () => {
  const r = (() => { let s = 7; return () => ((s = (s * 1103515245 + 12345) >>> 0) / 4294967296); })();
  let worst = 0;
  let worstMon = 0;
  for (let trial = 0; trial < 60; trial++) {
    const bytes = Array.from({ length: PARAM_COUNT }, () => (trial % 3 === 0 ? 255 : Math.floor(r() * 256)));
    const { values } = decodePatch(`1.${bytesToB64url(bytes)}`);
    const { x, render, setPatch } = loadEngine();
    x.sideband_init([44100, 48000, 96000][trial % 3]);
    setPatch(values);
    x.sideband_set_volume(1);
    for (let n = 0; n < 12; n++) x.sideband_note_on(24 + Math.floor(r() * 90), 127);
    const { out, mon } = render(8192);
    assert.ok(out.every(Number.isFinite) && mon.every(Number.isFinite));
    worst = Math.max(worst, peak(out));
    worstMon = Math.max(worstMon, peak(mon));
  }
  assert.ok(worst <= CEILING && worst < 1, `worst output ${worst}`);
  assert.ok(worstMon <= THRESHOLD * 1.000001, `worst monitor ${worstMon}`);
});

// Momentary loudness: the loudest 100 ms RMS of the audible band (a 40 Hz
// high-pass removes sub-audio content, which a crafted link can be full of).
function momentaryDb(x, sr = SR) {
  const a = Math.exp((-2 * Math.PI * 40) / sr);
  const y = new Float32Array(x.length);
  let px = 0, py = 0;
  for (let i = 0; i < x.length; i++) { py = a * (py + x[i] - px); px = x[i]; y[i] = py; }
  const w = sr / 10;
  let best = 0;
  for (let i = 0; i + w <= y.length; i += w / 2) {
    let e = 0;
    for (let j = i; j < i + w; j++) e += y[j] * y[j];
    best = Math.max(best, Math.sqrt(e / w));
  }
  return 20 * Math.log10(best);
}

test('loudness: presets are not whisper-quiet, and dense patches land close to them (default volume)', () => {
  const level = (values, notes, velocity, seconds) => {
    const { x, render, setPatch } = loadEngine();
    x.sideband_init(SR);
    setPatch(values);
    render(SR / 2); // the volume glides in from silence first
    notes.forEach((n) => x.sideband_note_on(n, velocity));
    return momentaryDb(render(SR * seconds).out);
  };
  const presets = PRESETS.map((p) => level(p.values, [p.name === 'BASS' ? 48 : 60], 100, 1.5));
  presets.forEach((db, i) => assert.ok(db > -32, `${PRESETS[i].name}: ${db.toFixed(1)} dBFS momentary`));
  const median = presets.slice().sort((a, b) => a - b)[3];
  const r = (() => { let s = 99; return () => ((s = (s * 1103515245 + 12345) >>> 0) / 4294967296); })();
  let worst = -Infinity;
  for (let trial = 0; trial < 60; trial++) {
    const bytes = Array.from({ length: PARAM_COUNT }, () => (trial % 2 ? Math.floor(r() * 256) : r() < 0.5 ? 0 : 255));
    const { values } = decodePatch(`1.${bytesToB64url(bytes)}`);
    worst = Math.max(worst, level(values, Array.from({ length: 8 }, () => 36 + Math.floor(r() * 60)), 127, 1));
  }
  // The previous build (no compressor) measured a 14 dB gap here.
  assert.ok(worst - median < 9, `dense patches ${worst.toFixed(1)} dBFS vs preset median ${median.toFixed(1)} dBFS`);
});

test('a fader drag makes no zipper noise: level edits are smoothed every sample', () => {
  // OP1 level dragged from 99 to 40 in 4-step jumps, one per 60 Hz UI frame.
  const { x, render, setPatch } = loadEngine();
  x.sideband_init(SR);
  const v = defaultValues();
  v[paramId(0, 'r4')] = 99;
  setPatch(v);
  x.sideband_set_volume(1);
  x.sideband_note_on(69, 127);
  render(SR / 2);
  const mon = new Float32Array(128 * 187); // about half a second
  const every = Math.round((0.0167 * SR) / 128);
  let lvl = 99;
  for (let b = 0; b < mon.length / 128; b++) {
    if (b % every === 0 && lvl > 40) { lvl = Math.max(40, lvl - 4); x.sideband_set_param(paramId(0, 'level'), lvl); }
    mon.set(render(128).mon, b * 128);
  }
  const mag = spectrum(mon, 0, 16384);
  let on = 0, off = 0;
  for (let i = 1; i < mag.length; i++) {
    const p = mag[i] ** 2;
    if (Math.abs((i * SR) / 16384 - 440) < 30) on += p; else off += p;
  }
  const db = 10 * Math.log10(off / on);
  // Stepping the level once per 128-sample block measured -21.6 dB here.
  assert.ok(db < -40, `energy outside 440 +- 30 Hz is ${db.toFixed(1)} dB re the carrier`);
});

test('the default volume is -12 dB and fades in from silence', () => {
  const { x, render } = loadEngine();
  x.sideband_init(SR);
  x.sideband_note_on(69, 127);
  const { out, mon } = render(SR / 2);
  assert.ok(Math.abs(out[1]) < 0.01, 'no jump on the first samples');
  const ratio = peak(out.subarray(SR / 4)) / peak(mon.subarray(SR / 4));
  assert.ok(Math.abs(ratio - 0.25) < 0.001, `steady-state gain ${ratio}`);
  assert.equal(x.sideband_set_volume(5), 1);
  assert.equal(x.sideband_set_volume(Number.NaN), 1);
});

test('rendering is cheap enough for the audio thread (measured, not asserted tightly)', () => {
  const { x, setPatch } = loadEngine();
  x.sideband_init(SR);
  setPatch(PRESETS[3].values);
  for (let n = 0; n < 8; n++) x.sideband_note_on(48 + n * 3, 100);
  const blocks = 2000;
  const t0 = performance.now();
  for (let i = 0; i < blocks; i++) x.sideband_render();
  const perBlock = (performance.now() - t0) / blocks;
  const budget = (128 / SR) * 1000;
  // A loose bound so slow CI machines pass; the measured figure is printed.
  assert.ok(perBlock < budget, `${perBlock.toFixed(4)} ms per block, budget ${budget.toFixed(2)} ms`);
  console.log(`# 8 voices: ${perBlock.toFixed(4)} ms per 128-sample block in Node (real-time budget at 48 kHz: ${budget.toFixed(2)} ms)`);
});
