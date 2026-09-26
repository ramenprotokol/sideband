#!/usr/bin/env node
// Times dist/sideband.wasm in Node on this machine: milliseconds per 128-sample
// block with 1 and 8 voices, against the real-time budget. Run `npm run build`
// first. The figures are for this machine only; the page makes no speed claims.
import { PRESETS } from '../web/js/presets.js';
import { loadEngine } from '../tests/helpers.mjs';

const SR = 48000;
const budget = (128 / SR) * 1000;
for (const voices of [1, 8]) {
  for (const preset of PRESETS) {
    const { x, setPatch } = loadEngine();
    x.sideband_init(SR);
    setPatch(preset.values);
    for (let n = 0; n < voices; n++) x.sideband_note_on(48 + n * 3, 100);
    for (let i = 0; i < 200; i++) x.sideband_render(); // warm up
    const blocks = 3000;
    const t0 = performance.now();
    for (let i = 0; i < blocks; i++) x.sideband_render();
    const ms = (performance.now() - t0) / blocks;
    console.log(`${preset.name.padEnd(8)} ${voices} voice${voices > 1 ? 's' : ' '}: ${ms.toFixed(4)} ms per block (${((ms / budget) * 100).toFixed(1)}% of the ${budget.toFixed(2)} ms budget at 48 kHz)`);
  }
}
