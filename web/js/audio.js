// Main-thread side of the audio engine: creates the AudioContext on a user
// gesture, loads the worklet and the WASM, and forwards messages. No DSP here.

const READY_TIMEOUT_MS = 8000;
const PAUSE_TAIL_MS = 1000; // how long a pause waits for release tails to finish
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Fetch the WASM early (it is small) so pressing Start is quick.
let wasmBytes = null;
function loadWasm() {
  if (!wasmBytes) {
    wasmBytes = fetch(new URL('../sideband.wasm', import.meta.url)).then((r) => {
      if (!r.ok) throw new Error(`the synth engine (sideband.wasm) failed to load: HTTP ${r.status}`);
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
    this.voices = 0;       // from the audio thread's meter messages
    this.meterSeq = 0;     // counts meter messages, to wait for a fresh one
    this.userPaused = false;
    this.pauseToken = 0;   // a newer pause or resume cancels an older pause
  }

  get running() {
    return this.ready && this.ctx && this.ctx.state === 'running';
  }

  // Must be called from a user gesture (click, key or touch).
  start() {
    if (this.ready) {
      if (this.ctx.state !== 'running') return this.resume();
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
    if (!ctx.audioWorklet) throw new Error('this browser has no AudioWorklet, which sideband needs for its audio thread.');
    const [bytes] = await Promise.all([loadWasm(), ctx.audioWorklet.addModule(new URL('./worklet.js', import.meta.url))]);
    const node = new AudioWorkletNode(ctx, 'sideband', {
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
      if (m && m.type === 'meter') {
        this.voices = m.voices | 0;
        this.meterSeq++;
        this.onMeter(m);
      }
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

  // Touch browsers only count pointerup/touchend (and keys and clicks) as
  // user activation, not pointerdown. A context created on pointerdown stays
  // suspended; the page calls this on those later events to let it run.
  // It never undoes a pause the player chose.
  unlock() {
    if (!this.ctx || this.userPaused || this.ctx.state !== 'suspended') return;
    this.ctx.resume().then(() => this.emitState(), () => {});
  }

  // The player resumes (START again, or a key while paused).
  resume() {
    this.userPaused = false;
    this.pauseToken++;
    if (!this.ctx) return Promise.resolve();
    return this.ctx.resume().then(() => this.emitState(), () => {});
  }

  // Pausing lets notes finish their release (up to a second), fades out
  // anything still sounding over 5 ms, and only then suspends the context,
  // so a pause never cuts a sound mid-wave.
  async pause() {
    if (!this.ready || this.ctx.state !== 'running') return;
    const token = ++this.pauseToken;
    this.userPaused = true;
    this.emitState('pausing');
    this.allNotesOff();
    const seq = this.meterSeq;
    const end = performance.now() + PAUSE_TAIL_MS;
    while (performance.now() < end && (this.meterSeq <= seq + 1 || this.voices > 0)) {
      await sleep(40);
      if (token !== this.pauseToken) return;
    }
    if (this.voices > 0) {
      this.panic();
      await sleep(30);
      if (token !== this.pauseToken) return;
    }
    await this.ctx.suspend();
    if (token === this.pauseToken) this.emitState();
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
