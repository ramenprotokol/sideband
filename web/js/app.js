// Wires the page together. The main thread is UI only: it keeps the patch,
// draws, and sends messages to the audio thread (see audio.js / worklet.js).
import { PARAM_COUNT, clampValue, patchFromHash, patchToHash, sanitizeName } from './patch.js';
import { PRESETS } from './presets.js';
import { AudioEngine } from './audio.js';
import { Monitor } from './scope.js';
import { Keyboard, noteName } from './keyboard.js';
import { connectMidi } from './midi.js';
import { buildAlgorithms, buildOperators, buildPresets, feedbackFader, makeFader } from './panel.js';

const $ = (id) => document.getElementById(id);
const MINUS = '−';

// ---- theme (per-viewer convenience; storage may be unavailable) -------------

const THEME_KEY = 'op-six-theme';
function storedTheme() {
  try { return localStorage.getItem(THEME_KEY); } catch { return null; }
}
function applyTheme(theme, save) {
  document.documentElement.dataset.theme = theme;
  $('theme-name').textContent = theme === 'light' ? 'PRINTED MANUAL' : 'TEAL PANEL';
  $('theme-toggle').setAttribute('aria-pressed', String(theme === 'light'));
  $('theme-toggle').setAttribute('aria-label', `Page style: ${theme === 'light' ? 'printed manual (light)' : 'teal panel (dark)'}. Press to switch.`);
  if (save) {
    try { localStorage.setItem(THEME_KEY, theme); } catch { /* private mode: fine */ }
  }
}
const prefersLight = window.matchMedia('(prefers-color-scheme: light)');
applyTheme(storedTheme() ?? (prefersLight.matches ? 'light' : 'dark'), false);
$('theme-toggle').addEventListener('click', () => {
  applyTheme(document.documentElement.dataset.theme === 'light' ? 'dark' : 'light', true);
});
prefersLight.addEventListener('change', (e) => { if (!storedTheme()) applyTheme(e.matches ? 'light' : 'dark', false); });

// ---- state ----------------------------------------------------------------

const state = { values: PRESETS[0].values.slice(), name: PRESETS[0].name };
let messageTimer = 0;
function message(text, { sticky = false } = {}) {
  const m = $('message');
  m.textContent = text;
  clearTimeout(messageTimer);
  if (!sticky && text) messageTimer = setTimeout(() => { m.textContent = ''; }, 8000);
}
function audioStatus(text) {
  $('audio-status').textContent = text;
}

// ---- audio ----------------------------------------------------------------

const formatDb = (gain) => {
  if (!(gain > 0)) return 'OFF';
  const db = 20 * Math.log10(gain);
  return `${db < -0.05 ? MINUS : ''}${Math.abs(db).toFixed(1)} dB`;
};

const engine = new AudioEngine({
  onMeter(m) {
    const peak = Number(m.peak);
    $('meter-peak').textContent = peak > 1e-5 ? `${MINUS}${Math.abs(20 * Math.log10(peak)).toFixed(1)} dBFS` : 'SILENT';
    const gr = 20 * Math.log10(Math.max(1e-6, Number(m.gain)));
    const limit = $('meter-limit');
    limit.textContent = gr < -0.05 ? `${MINUS}${Math.abs(gr).toFixed(1)} dB` : '0.0 dB';
    limit.classList.toggle('active', gr < -0.05);
    $('meter-voices').textContent = `${m.voices | 0} / 8`;
  },
  onState(s) {
    const start = $('start');
    if (s === 'running') {
      start.textContent = 'AUDIO ON';
      start.setAttribute('aria-pressed', 'true');
      start.classList.add('on');
      audioStatus(`Audio is on at ${(engine.sampleRate / 1000).toFixed(1)} kHz. Play the keys.`);
    } else if (s === 'suspended') {
      start.textContent = 'AUDIO PAUSED';
      start.setAttribute('aria-pressed', 'false');
      start.classList.remove('on');
      audioStatus('Audio is paused. Press the button again to resume.');
    } else if (s === 'error') {
      start.textContent = 'START AUDIO';
      start.classList.remove('on');
      audioStatus('The audio thread stopped with an error. Reload the page to start again.');
    }
  },
});
engine.setPatch(state.values);

const monitor = new Monitor({
  scope: $('scope'),
  scopeGrid: $('scope-grid'),
  spectrum: $('spectrum'),
  spectrumGrid: $('spectrum-grid'),
  idle: $('scope-idle'),
  reducedMotion: window.matchMedia('(prefers-reduced-motion: reduce)').matches,
});
window.matchMedia('(prefers-reduced-motion: reduce)').addEventListener('change', (e) => monitor.setReducedMotion(e.matches));

let keyboard = null;

async function startAudio() {
  if (engine.ready) return;
  audioStatus('Starting audio…');
  try {
    await engine.start();
    monitor.attach(engine.analyser, engine.sampleRate);
    $('meter-rate').textContent = `${(engine.sampleRate / 1000).toFixed(1)} kHz`;
    // Keys pressed while the audio was starting sound now.
    for (const note of new Set(keyboard.held.values())) engine.noteOn(note, 100);
  } catch (e) {
    audioStatus(`Audio could not start: ${e.message}`);
  }
}

$('start').addEventListener('click', async () => {
  if (!engine.ready) return startAudio();
  if (engine.ctx.state === 'running') {
    engine.allNotesOff();
    await engine.ctx.suspend();
  } else {
    await engine.ctx.resume();
  }
  engine.emitState();
});

$('panic').addEventListener('click', () => {
  keyboard.releaseAll();
  engine.panic();
  message('All notes stopped.');
});

// Volume: a quadratic taper, 0..100. 50 = -12 dB, the default. Never saved
// and never in a link, so every visit starts at a safe level.
const volume = makeFader({
  id: 'volume',
  label: 'VOLUME',
  ariaLabel: 'Master volume',
  min: 0,
  max: 100,
  format: (v) => formatDb((v / 100) ** 2),
  onInput: (v) => {
    volume.set(v);
    engine.setVolume((v / 100) ** 2);
  },
});
$('volume-fader').replaceWith(volume.el);
volume.el.classList.add('volume');
volume.set(50);

// ---- panel ----------------------------------------------------------------

let hashTimer = 0;
function writeHashSoon() {
  clearTimeout(hashTimer);
  hashTimer = setTimeout(() => {
    history.replaceState(null, '', `${location.pathname}${location.search}${patchToHash(state.values, state.name)}`);
  }, 250);
}

const ops = buildOperators($('op-grid'), (id, v) => setParam(id, v));
const algos = buildAlgorithms($('algo-grid'), (a) => setParam(0, a));
const feedback = feedbackFader($('feedback-fader'), (id, v) => setParam(id, v));
const presets = buildPresets($('presets'), PRESETS, (i) => {
  loadPatch(PRESETS[i].values, PRESETS[i].name);
  message(`Loaded ${PRESETS[i].name}. ${PRESETS[i].note}`);
  writeHashSoon();
});

function syncAll() {
  const algo = state.values[0];
  algos.sync(algo);
  feedback.set(state.values[1]);
  ops.forEach((o) => o.sync(state.values, algo));
  syncVoice();
}

// Only redraw what an edit touched: one operator, or everything for the
// algorithm and feedback (they change every block's pins).
function syncParam(id) {
  if (id < 2) {
    syncAll();
    return;
  }
  ops[Math.floor((id - 2) / 13)].sync(state.values, state.values[0]);
  syncVoice();
}

function syncVoice() {
  const match = presets.sync(state.values);
  $('preset-note').textContent = match >= 0 ? PRESETS[match].note : 'Edited voice: no factory preset matches it exactly.';
  const nameInput = $('patch-name');
  if (document.activeElement !== nameInput) nameInput.value = state.name;
}

function setParam(id, raw) {
  const v = clampValue(id, raw);
  if (state.values[id] !== v) {
    state.values[id] = v;
    engine.setParam(id, v);
    writeHashSoon();
  }
  syncParam(id);
}

function loadPatch(values, name) {
  if (!Array.isArray(values) || values.length !== PARAM_COUNT) return;
  state.values = values.map((v, id) => clampValue(id, v));
  state.name = sanitizeName(name);
  engine.setPatch(state.values);
  syncAll();
}

$('patch-name').addEventListener('input', (e) => {
  const clean = sanitizeName(e.target.value);
  state.name = clean;
  writeHashSoon();
});
$('patch-name').addEventListener('change', (e) => { e.target.value = state.name; });

$('copy-link').addEventListener('click', async () => {
  clearTimeout(hashTimer);
  const url = `${location.origin}${location.pathname}${patchToHash(state.values, state.name)}`;
  history.replaceState(null, '', url);
  const fallback = $('link-fallback');
  try {
    await navigator.clipboard.writeText(url);
    fallback.hidden = true;
    message('Link copied. Anyone who opens it hears this exact voice.');
  } catch {
    fallback.hidden = false;
    fallback.value = url;
    fallback.focus();
    fallback.select();
    message('Copy the link from the box below (the clipboard was not available).');
  }
});

// ---- links ----------------------------------------------------------------

function loadFromHash({ initial }) {
  const r = patchFromHash(location.hash);
  if (r.status === 'ok') {
    loadPatch(r.values, r.name);
    if (r.clamped > 0) {
      message(`Loaded the voice from this link. ${r.clamped} value${r.clamped === 1 ? ' was' : 's were'} out of range and ${r.clamped === 1 ? 'has' : 'have'} been clamped to safe limits.`, { sticky: true });
    } else if (!initial) {
      message('Loaded the voice from the link.');
    }
    return true;
  }
  if (r.status === 'error') {
    message(`${r.message} Loaded ${PRESETS[0].name} instead.`, { sticky: true });
    loadPatch(PRESETS[0].values, PRESETS[0].name);
    history.replaceState(null, '', `${location.pathname}${location.search}`);
  }
  return false;
}

window.addEventListener('hashchange', () => loadFromHash({ initial: false }));
if (!loadFromHash({ initial: true })) syncAll();

// ---- keyboard and MIDI ----------------------------------------------------

keyboard = new Keyboard($('keys'), {
  onNoteOn(note, velocity) {
    if (!engine.ready) {
      startAudio();
      return; // startAudio() plays held keys once the engine is up
    }
    if (engine.ctx.state !== 'running') engine.ctx.resume().then(() => engine.emitState());
    engine.noteOn(note, velocity);
  },
  onNoteOff(note) { engine.noteOff(note); },
  onOctave(base) { $('oct-readout').textContent = noteName(base); },
});
$('oct-down').addEventListener('click', () => keyboard.shift(-1));
$('oct-up').addEventListener('click', () => keyboard.shift(1));

$('midi').addEventListener('click', async () => {
  startAudio(); // the click is a gesture: a good moment to start audio too
  const access = await connectMidi({
    onMessage(m) {
      if (m.type === 'on') engine.noteOn(m.note, m.velocity);
      else if (m.type === 'off') engine.noteOff(m.note);
      else if (m.type === 'allOff') engine.allNotesOff();
    },
    onStatus: (text) => message(text, { sticky: true }),
  });
  if (access) $('midi').textContent = 'MIDI ON';
});

// Keep the page's bottom padding equal to the dock, whatever it wraps to.
new ResizeObserver(([entry]) => {
  const h = Math.ceil(entry.borderBoxSize?.[0]?.blockSize ?? entry.target.offsetHeight);
  document.documentElement.style.setProperty('--dock-h', `${h}px`);
}).observe($('dock'));

// ---- test hook (automation only) --------------------------------------------

if (navigator.webdriver || new URLSearchParams(location.search).has('test')) {
  window.__opsix = { state, engine, monitor, keyboard, presets: PRESETS };
}
