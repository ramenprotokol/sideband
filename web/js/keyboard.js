// Two octaves on the computer keyboard (by physical key position, so it works
// on any layout) plus the on-screen keys for mouse and touch.

// event.code -> semitone above the base note.
export const KEYMAP = {
  KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyV: 5, KeyG: 6, KeyB: 7, KeyH: 8, KeyN: 9, KeyJ: 10, KeyM: 11,
  KeyQ: 12, Digit2: 13, KeyW: 14, Digit3: 15, KeyE: 16, KeyR: 17, Digit5: 18, KeyT: 19, Digit6: 20, KeyY: 21, Digit7: 22, KeyU: 23,
};

const LABELS = Object.fromEntries(Object.entries(KEYMAP).map(([code, i]) => [i, code.replace(/^Key|^Digit/, '')]));
const NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
const BLACK = new Set([1, 3, 6, 8, 10]);

export const MIN_BASE = 24; // C1
export const MAX_BASE = 84; // C6 (top key = B7)
export const KEYBOARD_VELOCITY = 100;

export function noteName(note) {
  return `${NAMES[note % 12]}${Math.floor(note / 12) - 1}`;
}

function isTextEntry(el) {
  if (!el) return false;
  if (el.isContentEditable) return true;
  if (el.tagName === 'TEXTAREA' || el.tagName === 'SELECT') return true;
  return el.tagName === 'INPUT' && !['range', 'button', 'checkbox', 'radio'].includes(el.type);
}

export class Keyboard {
  constructor(container, { onNoteOn, onNoteOff, onOctave }) {
    this.container = container;
    this.onNoteOn = onNoteOn;
    this.onNoteOff = onNoteOff;
    this.onOctave = onOctave;
    this.base = 48; // C3
    this.held = new Map(); // key code or pointer id -> note
    this.heldKey = new Map(); // key code or pointer id -> on-screen key index
    this.keys = [];
    this.build();
    window.addEventListener('keydown', (e) => this.keydown(e));
    window.addEventListener('keyup', (e) => this.keyup(e));
    window.addEventListener('blur', () => this.releaseAll());
    document.addEventListener('visibilitychange', () => { if (document.hidden) this.releaseAll(); });
  }

  build() {
    const whites = document.createElement('div');
    whites.className = 'keys-inner';
    let white = 0;
    for (let i = 0; i < 24; i++) {
      const key = document.createElement('button');
      key.type = 'button';
      key.tabIndex = -1;
      key.className = BLACK.has(i % 12) ? 'key black' : 'key white';
      key.dataset.index = String(i);
      const label = document.createElement('span');
      label.className = 'key-label';
      label.textContent = LABELS[i];
      key.append(label);
      if (i % 12 === 0) {
        const octave = document.createElement('span');
        octave.className = 'key-octave';
        key.append(octave);
      }
      if (BLACK.has(i % 12)) {
        key.style.setProperty('--at', String(white));
      } else {
        white++;
      }
      key.addEventListener('pointerdown', (e) => this.pointerdown(e, i));
      key.addEventListener('pointerup', (e) => this.pointerup(e));
      key.addEventListener('pointercancel', (e) => this.pointerup(e));
      key.addEventListener('lostpointercapture', (e) => this.pointerup(e));
      key.addEventListener('contextmenu', (e) => e.preventDefault());
      this.keys.push(key);
      whites.append(key);
    }
    this.container.append(whites);
    this.relabel();
  }

  relabel() {
    this.keys.forEach((key, i) => {
      const note = this.base + i;
      key.setAttribute('aria-label', `${noteName(note)} (computer key ${LABELS[i]})`);
      const oct = key.querySelector('.key-octave');
      if (oct) oct.textContent = noteName(note);
    });
    this.onOctave?.(this.base);
  }

  shift(octaves) {
    const next = Math.min(MAX_BASE, Math.max(MIN_BASE, this.base + 12 * octaves));
    if (next === this.base) return;
    this.base = next;
    this.relabel();
  }

  press(id, index) {
    if (this.held.has(id)) return;
    const note = this.base + index;
    this.held.set(id, note);
    this.heldKey.set(id, index); // the octave may shift before release
    this.keys[index]?.classList.add('down');
    this.onNoteOn(note, KEYBOARD_VELOCITY);
  }

  release(id) {
    const note = this.held.get(id);
    if (note === undefined) return;
    const index = this.heldKey.get(id);
    this.held.delete(id);
    this.heldKey.delete(id);
    if (![...this.heldKey.values()].includes(index)) this.keys[index]?.classList.remove('down');
    // Only stop the note if no other finger or key still holds it.
    if (![...this.held.values()].includes(note)) this.onNoteOff(note);
  }

  releaseAll() {
    for (const id of [...this.held.keys()]) this.release(id);
    this.keys.forEach((k) => k.classList.remove('down'));
  }

  keydown(e) {
    if (e.metaKey || e.ctrlKey || e.altKey || isTextEntry(e.target)) return;
    if (e.code === 'Minus' || e.code === 'Equal') {
      e.preventDefault();
      if (!e.repeat) this.shift(e.code === 'Minus' ? -1 : 1);
      return;
    }
    const index = KEYMAP[e.code];
    if (index === undefined) return;
    e.preventDefault();
    if (e.repeat) return;
    this.press(e.code, index);
  }

  keyup(e) {
    if (KEYMAP[e.code] === undefined) return;
    this.release(e.code);
  }

  pointerdown(e, index) {
    if (e.button !== 0) return;
    e.preventDefault();
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch { /* not capturable */ }
    this.press(`p${e.pointerId}`, index);
  }

  pointerup(e) {
    this.release(`p${e.pointerId}`);
  }
}
