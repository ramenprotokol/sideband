// The audio thread. This AudioWorkletProcessor owns the C engine (compiled to
// WebAssembly) and does nothing else: it applies messages from the page
// between blocks and copies each rendered block into Web Audio's buffers.
//
// Output 0 goes to the speakers. Output 1 is the monitor tap (after the
// limiter, before the volume) that feeds the scope and spectrum, so the
// displays stay readable at any listening volume.

const BLOCK = 128;
const PARAMS = 80;

class OpSixProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ok = false;
    this.pos = BLOCK;
    this.blocks = 0;
    this.minGain = 1;
    this.meterEvery = Math.max(1, Math.round(sampleRate / 20 / BLOCK)); // ~20 meter messages a second
    try {
      const bytes = options && options.processorOptions && options.processorOptions.wasm;
      const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
      this.x = instance.exports;
      this.x.opsix_init(sampleRate);
      const mem = this.x.memory.buffer; // fixed size: the module can never grow it
      this.out = new Float32Array(mem, this.x.opsix_render(), BLOCK);
      this.mon = new Float32Array(mem, this.x.opsix_monitor(), BLOCK);
      this.ok = true;
      this.port.postMessage({ type: 'ready', sampleRate: this.x.opsix_sample_rate() });
    } catch (e) {
      this.port.postMessage({ type: 'error', message: String((e && e.message) || e) });
    }
    this.port.onmessage = (e) => this.handle(e.data);
  }

  handle(m) {
    if (!this.ok || !m || typeof m !== 'object') return;
    const x = this.x;
    switch (m.type) {
      case 'patch':
        if (Array.isArray(m.values) && m.values.length === PARAMS) {
          for (let id = 0; id < PARAMS; id++) x.opsix_set_param(id, Number(m.values[id]));
        }
        break;
      case 'param':
        x.opsix_set_param(m.id | 0, Number(m.value));
        break;
      case 'on':
        x.opsix_note_on(m.note | 0, m.velocity | 0);
        break;
      case 'off':
        x.opsix_note_off(m.note | 0);
        break;
      case 'allOff':
        x.opsix_all_notes_off();
        break;
      case 'panic':
        x.opsix_panic();
        break;
      case 'volume':
        x.opsix_set_volume(Number(m.gain));
        break;
      default:
        break;
    }
  }

  process(_inputs, outputs) {
    if (!this.ok) return true; // outputs stay zero-filled: silence
    const main = outputs[0];
    const mon = outputs[1];
    const n = main[0].length;
    let written = 0;
    while (written < n) {
      if (this.pos >= BLOCK) {
        this.x.opsix_render();
        const g = this.x.opsix_gain_reduction();
        if (g < this.minGain) this.minGain = g;
        this.pos = 0;
        this.blocks++;
      }
      const take = Math.min(BLOCK - this.pos, n - written);
      const src = this.out.subarray(this.pos, this.pos + take);
      for (let c = 0; c < main.length; c++) main[c].set(src, written);
      if (mon && mon[0]) mon[0].set(this.mon.subarray(this.pos, this.pos + take), written);
      this.pos += take;
      written += take;
    }
    if (this.blocks >= this.meterEvery) {
      this.blocks = 0;
      this.port.postMessage({
        type: 'meter',
        peak: this.x.opsix_take_peak(),
        gain: this.minGain,
        voices: this.x.opsix_active_voices(),
      });
      this.minGain = 1;
    }
    return true;
  }
}

registerProcessor('op-six', OpSixProcessor);
