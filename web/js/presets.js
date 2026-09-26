// Six starting sounds. Algorithms are 0-based here (0 = algorithm 1).
// Operator fields: c coarse, f fine, d detune (cents), lv level,
// r rates R1..R4, l levels L1..L4, fixed = fixed-frequency mode.
import { valuesFromDescription } from './patch.js';

const DESCRIPTIONS = [
  {
    name: 'E.PIANO',
    note: 'Three pairs: a bell-like tine (OP2 at x14) that dies fast over two warm, slightly detuned bodies.',
    algo: 0,
    feedback: 4,
    ops: [
      { c: 1, d: 3, lv: 99, r: [96, 30, 18, 56], l: [99, 88, 0, 0] },
      { c: 14, lv: 72, r: [99, 50, 35, 60], l: [99, 0, 0, 0] },
      { c: 1, d: -3, lv: 95, r: [95, 32, 19, 56], l: [99, 85, 0, 0] },
      { c: 1, lv: 80, r: [95, 40, 22, 56], l: [99, 75, 0, 0] },
      { c: 1, lv: 84, r: [95, 36, 20, 56], l: [99, 80, 0, 0] },
      { c: 1, lv: 72, r: [95, 44, 26, 56], l: [99, 65, 0, 0] },
    ],
  },
  {
    name: 'BELL',
    note: 'Inharmonic pairs (x3.5, x4.4, x1.41 modulators) ring out for seconds, with a low hum at x0.5.',
    algo: 0,
    feedback: 0,
    ops: [
      { c: 1, lv: 99, r: [99, 24, 18, 30], l: [99, 72, 0, 0] },
      { c: 3, f: 50, lv: 80, r: [99, 26, 20, 30], l: [99, 62, 0, 0] },
      { c: 2, f: 76, lv: 86, r: [99, 30, 22, 30], l: [99, 60, 0, 0] },
      { c: 4, f: 40, lv: 70, r: [99, 36, 25, 30], l: [99, 40, 0, 0] },
      { c: 0, lv: 80, r: [99, 20, 16, 26], l: [99, 80, 0, 0] },
      { c: 1, f: 41, lv: 60, r: [99, 40, 30, 30], l: [99, 30, 0, 0] },
    ],
  },
  {
    name: 'BASS',
    note: 'Two three-operator stacks an octave down (x0.5 carriers); the top of each stack plucks and fades.',
    algo: 1,
    feedback: 4,
    ops: [
      { c: 0, lv: 99, r: [99, 50, 35, 70], l: [99, 90, 80, 0] },
      { c: 0, lv: 84, r: [99, 55, 38, 70], l: [99, 86, 76, 0] },
      { c: 1, lv: 72, r: [99, 62, 40, 70], l: [99, 70, 60, 0] },
      { c: 0, d: 2, lv: 90, r: [99, 45, 35, 70], l: [99, 86, 72, 0] },
      { c: 1, lv: 78, r: [99, 60, 42, 70], l: [99, 78, 66, 0] },
      { c: 3, lv: 66, r: [99, 72, 50, 70], l: [99, 40, 0, 0] },
    ],
  },
  {
    name: 'BRASS',
    note: 'Two stacks whose modulators open a little slower than the carriers, so the tone brightens as it swells.',
    algo: 1,
    feedback: 6,
    ops: [
      { c: 1, lv: 99, r: [66, 50, 40, 60], l: [99, 94, 92, 0] },
      { c: 1, lv: 78, r: [60, 45, 40, 60], l: [99, 88, 86, 0] },
      { c: 1, lv: 66, r: [58, 45, 40, 60], l: [99, 85, 80, 0] },
      { c: 1, d: 4, lv: 95, r: [66, 50, 40, 60], l: [99, 94, 92, 0] },
      { c: 1, lv: 76, r: [60, 45, 40, 60], l: [99, 88, 85, 0] },
      { c: 1, lv: 70, r: [58, 45, 40, 60], l: [99, 85, 80, 0] },
    ],
  },
  {
    name: 'PAD',
    note: 'Three detuned sines plus a slowly opening stack; long attack and release.',
    algo: 6,
    feedback: 3,
    ops: [
      { c: 1, d: -6, lv: 90, r: [40, 30, 30, 32], l: [99, 95, 95, 0] },
      { c: 1, d: 6, lv: 90, r: [40, 30, 30, 32], l: [99, 95, 95, 0] },
      { c: 2, d: 2, lv: 78, r: [38, 30, 30, 30], l: [99, 92, 92, 0] },
      { c: 1, lv: 92, r: [40, 30, 30, 32], l: [99, 95, 95, 0] },
      { c: 1, lv: 78, r: [36, 28, 30, 32], l: [99, 92, 90, 0] },
      { c: 2, lv: 68, r: [34, 28, 30, 32], l: [99, 90, 88, 0] },
    ],
  },
  {
    name: 'MALLET',
    note: 'A struck bar: the fundamental and a x4 partial decay quickly, with a very short x10 click on top.',
    algo: 0,
    feedback: 0,
    ops: [
      { c: 1, lv: 99, r: [99, 42, 36, 50], l: [99, 0, 0, 0] },
      { c: 4, lv: 72, r: [99, 60, 50, 50], l: [99, 0, 0, 0] },
      { c: 4, lv: 80, r: [99, 50, 45, 50], l: [99, 0, 0, 0] },
      { c: 1, lv: 60, r: [99, 62, 50, 50], l: [99, 0, 0, 0] },
      { c: 10, lv: 70, r: [99, 72, 60, 50], l: [99, 0, 0, 0] },
      { c: 1, lv: 50, r: [99, 72, 60, 50], l: [99, 0, 0, 0] },
    ],
  },
];

export const PRESETS = DESCRIPTIONS.map((d) => ({
  name: d.name,
  note: d.note,
  values: valuesFromDescription(d),
}));
