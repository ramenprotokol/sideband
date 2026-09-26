// The CRT displays: an oscilloscope with phosphor persistence and a spectrum
// analyser, both reading the monitor tap through an AnalyserNode. Each display
// is two canvases: a static graticule, and a trace layer that is faded a
// little each frame instead of cleared, so earlier sweeps glow and decay.

const PHOSPHOR = '124, 255, 160';
const GRID = 'rgba(124, 255, 160, 0.16)';
const GRID_MAJOR = 'rgba(124, 255, 160, 0.3)';
const LABEL = 'rgba(160, 255, 190, 0.75)';

const SCOPE_DIVS_X = 10;
const SCOPE_DIVS_Y = 8;
const SPEC_MIN_HZ = 20;
const SPEC_MAX_HZ = 20000;
const SPEC_MIN_DB = -110;
const SPEC_MAX_DB = -10;

function fit(canvas) {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
  const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    return true;
  }
  return false;
}

export class Monitor {
  constructor({ scope, scopeGrid, spectrum, spectrumGrid, idle, reducedMotion }) {
    this.scope = scope;
    this.scopeGrid = scopeGrid;
    this.spectrum = spectrum;
    this.spectrumGrid = spectrumGrid;
    this.idle = idle;
    this.analyser = null;
    this.sampleRate = 48000;
    this.frame = this.frame.bind(this);
    this.raf = 0;
    this.last = 0;
    this.frames = 0;
    this.setReducedMotion(reducedMotion);
    const ro = new ResizeObserver(() => this.layout());
    [scope, spectrum].forEach((c) => ro.observe(c));
    document.addEventListener('visibilitychange', () => this.kick());
    this.layout();
  }

  // Reduced motion: redraw about 6 times a second with a slower fade, so the
  // trace changes gently. The sound is unaffected.
  setReducedMotion(reduced) {
    this.reduced = reduced;
    this.interval = reduced ? 160 : 0;
    this.fadeScope = reduced ? 0.5 : 0.22;
    this.fadeSpectrum = reduced ? 0.6 : 0.35;
  }

  attach(analyser, sampleRate) {
    this.analyser = analyser;
    this.sampleRate = sampleRate;
    this.time = new Float32Array(analyser.fftSize);
    this.freq = new Float32Array(analyser.frequencyBinCount);
    this.idle.hidden = true;
    this.layout();
    this.kick();
  }

  // Milliseconds per horizontal division, from the real sample rate.
  get msPerDiv() {
    return (this.span() / this.sampleRate) * 1000 / SCOPE_DIVS_X;
  }

  span() {
    return 1024;
  }

  kick() {
    if (!this.raf && this.analyser && !document.hidden) this.raf = requestAnimationFrame(this.frame);
  }

  layout() {
    fit(this.scope);
    fit(this.spectrum);
    fit(this.scopeGrid);
    fit(this.spectrumGrid);
    this.drawScopeGrid();
    this.drawSpectrumGrid();
  }

  drawScopeGrid() {
    const c = this.scopeGrid;
    const g = c.getContext('2d');
    const { width: w, height: h } = c;
    g.clearRect(0, 0, w, h);
    g.lineWidth = 1;
    for (let i = 0; i <= SCOPE_DIVS_X; i++) {
      const x = Math.round((i / SCOPE_DIVS_X) * (w - 1)) + 0.5;
      g.strokeStyle = i === SCOPE_DIVS_X / 2 ? GRID_MAJOR : GRID;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
    }
    for (let j = 0; j <= SCOPE_DIVS_Y; j++) {
      const y = Math.round((j / SCOPE_DIVS_Y) * (h - 1)) + 0.5;
      g.strokeStyle = j === SCOPE_DIVS_Y / 2 ? GRID_MAJOR : GRID;
      g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
    }
    // Minor ticks on the centre lines, as on a real graticule.
    g.strokeStyle = GRID_MAJOR;
    const tick = Math.max(3, h / 60);
    for (let i = 0; i <= SCOPE_DIVS_X * 5; i++) {
      const x = Math.round((i / (SCOPE_DIVS_X * 5)) * (w - 1)) + 0.5;
      g.beginPath(); g.moveTo(x, h / 2 - tick); g.lineTo(x, h / 2 + tick); g.stroke();
    }
    for (let j = 0; j <= SCOPE_DIVS_Y * 5; j++) {
      const y = Math.round((j / (SCOPE_DIVS_Y * 5)) * (h - 1)) + 0.5;
      g.beginPath(); g.moveTo(w / 2 - tick, y); g.lineTo(w / 2 + tick, y); g.stroke();
    }
    if (this.analyser) {
      const dpr = w / Math.max(1, c.clientWidth);
      g.fillStyle = LABEL;
      g.font = `${Math.round(10 * dpr)}px "IBM Plex Mono", ui-monospace, monospace`;
      g.fillText(`${this.msPerDiv.toFixed(2)} ms/div`, 6 * dpr, h - 6 * dpr);
    }
  }

  xForHz(hz, w) {
    return (Math.log(hz / SPEC_MIN_HZ) / Math.log(SPEC_MAX_HZ / SPEC_MIN_HZ)) * w;
  }

  drawSpectrumGrid() {
    const c = this.spectrumGrid;
    const g = c.getContext('2d');
    const { width: w, height: h } = c;
    const dpr = w / Math.max(1, c.clientWidth);
    g.clearRect(0, 0, w, h);
    g.lineWidth = 1;
    for (let decade = 10; decade <= 10000; decade *= 10) {
      for (let k = 1; k < 10; k++) {
        const hz = decade * k;
        if (hz < SPEC_MIN_HZ || hz > SPEC_MAX_HZ) continue;
        const x = Math.round(this.xForHz(hz, w)) + 0.5;
        g.strokeStyle = k === 1 ? GRID_MAJOR : GRID;
        g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
      }
    }
    for (let db = SPEC_MAX_DB; db >= SPEC_MIN_DB; db -= 20) {
      const y = Math.round(((SPEC_MAX_DB - db) / (SPEC_MAX_DB - SPEC_MIN_DB)) * (h - 1)) + 0.5;
      g.strokeStyle = GRID;
      g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
    }
    g.fillStyle = LABEL;
    g.font = `${Math.round(10 * dpr)}px "IBM Plex Mono", ui-monospace, monospace`;
    for (const [hz, text] of [[100, '100'], [1000, '1k'], [10000, '10k']]) {
      g.fillText(text, this.xForHz(hz, w) + 3 * dpr, h - 6 * dpr);
    }
    for (const db of [-30, -70]) {
      const y = ((SPEC_MAX_DB - db) / (SPEC_MAX_DB - SPEC_MIN_DB)) * h;
      g.fillText(`${db}`, 4 * dpr, y - 3 * dpr);
    }
  }

  frame(now) {
    this.raf = 0;
    if (!this.analyser || document.hidden) return;
    if (!this.interval || now - this.last >= this.interval) {
      this.last = now;
      this.drawScope();
      this.drawSpectrum();
      this.frames++;
    }
    this.raf = requestAnimationFrame(this.frame);
  }

  fade(canvas, amount) {
    const g = canvas.getContext('2d');
    g.globalCompositeOperation = 'destination-out';
    g.fillStyle = `rgba(0, 0, 0, ${amount})`;
    g.fillRect(0, 0, canvas.width, canvas.height);
    g.globalCompositeOperation = 'source-over';
    return g;
  }

  // A glow pass and a sharp core pass: the phosphor look without a blur filter.
  stroke(g, path, dpr) {
    g.lineJoin = 'round';
    g.strokeStyle = `rgba(${PHOSPHOR}, 0.22)`;
    g.lineWidth = 4 * dpr;
    g.stroke(path);
    g.strokeStyle = `rgba(${PHOSPHOR}, 0.95)`;
    g.lineWidth = 1.3 * dpr;
    g.stroke(path);
  }

  drawScope() {
    const c = this.scope;
    const { width: w, height: h } = c;
    const dpr = w / Math.max(1, c.clientWidth);
    const g = this.fade(c, this.fadeScope);
    const buf = this.time;
    this.analyser.getFloatTimeDomainData(buf);
    const span = this.span();
    // Trigger on a rising zero crossing so a steady tone stands still.
    let start = 0;
    for (let i = 1; i < buf.length - span; i++) {
      if (buf[i - 1] < 0 && buf[i] >= 0) { start = i; break; }
    }
    const path = new Path2D();
    const yScale = h; // 0.5 FS = 4 divisions = half the height: 0.125 FS/div
    for (let i = 0; i < span; i++) {
      const x = (i / (span - 1)) * w;
      const v = buf[start + i];
      const y = h / 2 - v * yScale;
      if (i === 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    this.stroke(g, path, dpr);
  }

  drawSpectrum() {
    const c = this.spectrum;
    const { width: w, height: h } = c;
    const dpr = w / Math.max(1, c.clientWidth);
    const g = this.fade(c, this.fadeSpectrum);
    const bins = this.freq;
    this.analyser.getFloatFrequencyData(bins);
    const binHz = this.sampleRate / (2 * bins.length);
    const path = new Path2D();
    let started = false;
    // One point per pixel column, taking the loudest bin that falls in it.
    const cols = Math.max(2, Math.floor(w / dpr));
    for (let col = 0; col < cols; col++) {
      const lo = SPEC_MIN_HZ * (SPEC_MAX_HZ / SPEC_MIN_HZ) ** (col / cols);
      const hi = SPEC_MIN_HZ * (SPEC_MAX_HZ / SPEC_MIN_HZ) ** ((col + 1) / cols);
      const a = Math.floor(lo / binHz), b = Math.min(Math.floor(hi / binHz), bins.length - 1);
      if (a >= bins.length - 1) break;
      let db = -Infinity;
      if (b > a) {
        // Several bins in this column: show the loudest.
        for (let k = a + 1; k <= b; k++) if (bins[k] > db) db = bins[k];
      } else {
        // Less than one bin per column (low frequencies): interpolate between
        // neighbouring bins instead of drawing flat steps.
        const f = Math.sqrt(lo * hi) / binHz;
        const k = Math.floor(f);
        db = bins[k] + (bins[k + 1] - bins[k]) * (f - k);
      }
      if (!Number.isFinite(db)) db = SPEC_MIN_DB;
      db = Math.max(SPEC_MIN_DB, Math.min(SPEC_MAX_DB, db));
      const x = (col / (cols - 1)) * w;
      const y = ((SPEC_MAX_DB - db) / (SPEC_MAX_DB - SPEC_MIN_DB)) * h;
      if (!started) { path.moveTo(x, y); started = true; } else path.lineTo(x, y);
    }
    this.stroke(g, path, dpr);
  }
}
