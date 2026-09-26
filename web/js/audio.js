// Main-thread side of the audio engine: creates the AudioContext on a user
// gesture, loads the worklet and the WASM, and forwards messages. No DSP here.

const READY_TIMEOUT_MS = 8000;

// Fetch the WASM early (it is small) so pressing Start is quick.
let wasmBytes = null;
function loadWasm() {
  if (!wasmBytes) {
    wasmBytes = fetch(new URL('../opsix.wasm', import.meta.url)).then((r) => {
      if (!r.ok) throw new Error(`the synth engine (opsix.wasm) failed to load: HTTP ${r.status}`);
      return r.arrayBuffer();
    });
    wasmBytes.catch(() => { wasmBytes = null; }); // allow a retry
  }
  return wasmBytes;
}
loadWasm().catch(() => {});

export class AudioEngine {
  constructor({ onMeter = () => {}, onState = () => {} } = {}) {
    this.onMeter = onMeter;
    this.onState = onState;
    this.ctx = null;
    this.node = null;
    this.analyser = null;
    this.ready = false;
    this.starting = null;
    this.sampleRate = 0;
    this.patch = null;
    this.volume = 0.25;
  }

  get running() {
    return this.ready && this.ctx && this.ctx.state === 'running';
  }

  // Must be called from a user gesture (click, key or touch).
  start() {
    if (this.ready) {
      if (this.ctx.state !== 'running') return this.ctx.resume().then(() => this.emitState());
      return Promise.resolve();
    }
    if (!this.starting) {
      this.starting = this.boot().catch((e) => {
        this.starting = null;
        this.teardown();
        throw e;
      });
    }
    return this.starting;
  }

  async boot() {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) throw new Error('this browser has no Web Audio support.');
    const ctx = new AC({ latencyHint: 'interactive' });
    this.ctx = ctx;
    // Resume inside the gesture; some browsers only allow it there.
    const resumed = ctx.resume().catch(() => {});
    if (!ctx.audioWorklet) throw new Error('this browser has no AudioWorklet, which op-six needs for its audio thread.');
    const [bytes] = await Promise.all([loadWasm(), ctx.audioWorklet.addModule(new URL('./worklet.js', import.meta.url))]);
    const node = new AudioWorkletNode(ctx, 'op-six', {
      numberOfInputs: 0,
      numberOfOutputs: 2,
      outputChannelCount: [2, 1],
      processorOptions: { wasm: bytes },
    });
    this.node = node;
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('the audio thread did not answer in time.')), READY_TIMEOUT_MS);
      node.port.onmessage = (e) => {
        const m = e.data;
        if (m && m.type === 'ready') {
          clearTimeout(timer);
          this.sampleRate = m.sampleRate;
          resolve();
        } else if (m && m.type === 'error') {
          clearTimeout(timer);
          reject(new Error(`the synth engine could not start: ${m.message}`));
        }
      };
      node.onprocessorerror = () => {
        clearTimeout(timer);
        reject(new Error('the audio thread stopped with an error.'));
      };
    });
    node.port.onmessage = (e) => {
      const m = e.data;
      if (m && m.type === 'meter') this.onMeter(m);
    };
    node.onprocessorerror = () => {
      this.ready = false;
      this.emitState('error');
    };

    const analyser = ctx.createAnalyser();
    analyser.fftSize = 4096;
    analyser.smoothingTimeConstant = 0.5;
    analyser.minDecibels = -110;
    analyser.maxDecibels = -10;
    node.connect(analyser, 1, 0);
    node.connect(ctx.destination, 0, 0);
    this.analyser = analyser;
    this.ready = true;
    if (this.patch) this.post({ type: 'patch', values: this.patch });
    this.post({ type: 'volume', gain: this.volume });
    ctx.onstatechange = () => this.emitState();
    await resumed;
    this.emitState();
  }

  teardown() {
    try { this.node?.disconnect(); } catch { /* already gone */ }
    try { this.ctx?.close(); } catch { /* already closed */ }
    this.ctx = null;
    this.node = null;
    this.analyser = null;
    this.ready = false;
  }

  emitState(force) {
    this.onState(force ?? (this.ready ? this.ctx.state : 'off'));
  }

  post(message) {
    if (this.ready) this.node.port.postMessage(message);
  }

  setPatch(values) {
    this.patch = values.slice();
    this.post({ type: 'patch', values: this.patch });
  }

  setParam(id, value) {
    if (this.patch) this.patch[id] = value;
    this.post({ type: 'param', id, value });
  }

  setVolume(gain) {
    this.volume = gain;
    this.post({ type: 'volume', gain });
  }

  noteOn(note, velocity) { this.post({ type: 'on', note, velocity }); }
  noteOff(note) { this.post({ type: 'off', note }); }
  allNotesOff() { this.post({ type: 'allOff' }); }
  panic() { this.post({ type: 'panic' }); }
}
