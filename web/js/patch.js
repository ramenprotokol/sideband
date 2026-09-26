// The patch model, shared by the page and the Node tests.
//
// A patch is 80 small integers (2 global + 6 operators x 13). The layout and
// the limits mirror src/sideband.c exactly; tests/render.test.mjs loads the real
// WASM and checks every min/max/default against this file.
//
// Share links carry the patch in the URL fragment:  #p=1.<107 chars>&n=<name>
// The fragment is decoded with hard limits (length, alphabet, byte count) and
// every value is clamped, so a crafted link can only ever produce a patch the
// synth could have made itself. Volume is never part of a link.

export const OPS = 6;
export const GLOBALS = 2;
export const OP_PARAMS = 13;
export const PARAM_COUNT = GLOBALS + OPS * OP_PARAMS;
export const FORMAT_VERSION = 1;
export const MAX_HASH_LENGTH = 300;
export const NAME_MAX = 16;

export const GLOBAL_SPEC = [
  { key: 'algo', label: 'ALGORITHM', min: 0, max: 7, def: 0 },
  { key: 'feedback', label: 'FEEDBACK', min: 0, max: 7, def: 0 },
];

export const OP_SPEC = [
  { key: 'mode', label: 'MODE', min: 0, max: 1, def: 0 },
  { key: 'coarse', label: 'COARSE', min: 0, max: 31, def: 1 },
  { key: 'fine', label: 'FINE', min: 0, max: 99, def: 0 },
  { key: 'detune', label: 'DETUNE', min: -20, max: 20, def: 0 },
  { key: 'level', label: 'LEVEL', min: 0, max: 99, def: 0 },
  { key: 'r1', label: 'R1', min: 0, max: 99, def: 99 },
  { key: 'r2', label: 'R2', min: 0, max: 99, def: 99 },
  { key: 'r3', label: 'R3', min: 0, max: 99, def: 99 },
  { key: 'r4', label: 'R4', min: 0, max: 99, def: 60 },
  { key: 'l1', label: 'L1', min: 0, max: 99, def: 99 },
  { key: 'l2', label: 'L2', min: 0, max: 99, def: 99 },
  { key: 'l3', label: 'L3', min: 0, max: 99, def: 99 },
  { key: 'l4', label: 'L4', min: 0, max: 99, def: 0 },
];

const OP_INDEX = Object.fromEntries(OP_SPEC.map((s, i) => [s.key, i]));

export function paramId(op, key) {
  if (key === 'algo') return 0;
  if (key === 'feedback') return 1;
  return GLOBALS + op * OP_PARAMS + OP_INDEX[key];
}

export function specFor(id) {
  if (id < GLOBALS) return GLOBAL_SPEC[id];
  const op = Math.floor((id - GLOBALS) / OP_PARAMS);
  const s = OP_SPEC[(id - GLOBALS) % OP_PARAMS];
  if (s.key === 'level' && op === 0) return { ...s, def: 99 };
  return s;
}

export function clampValue(id, value) {
  const s = specFor(id);
  const n = typeof value === 'number' ? value : Number(value);
  if (!Number.isFinite(n)) return s.def;
  const r = Math.sign(n) * Math.round(Math.abs(n)); // half away from zero, like the C side
  return Math.min(s.max, Math.max(s.min, r));
}

export function defaultValues() {
  return Array.from({ length: PARAM_COUNT }, (_, id) => specFor(id).def);
}

// Allowed name characters: what a 1983 LCD could show, give or take.
export function sanitizeName(raw) {
  return String(raw ?? '')
    .slice(0, 64)
    .toUpperCase()
    .replace(/[^A-Z0-9 .\-+/]/g, '')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, NAME_MAX);
}

// ---- base64url, strict ----------------------------------------------------

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
const B64_INDEX = new Map([...B64].map((c, i) => [c, i]));

export function bytesToB64url(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | ((bytes[i + 1] ?? 0) << 8) | (bytes[i + 2] ?? 0);
    const chars = i + 2 < bytes.length ? 4 : i + 1 < bytes.length ? 3 : 2;
    for (let k = 0; k < chars; k++) out += B64[(n >> (18 - 6 * k)) & 63];
  }
  return out;
}

export function b64urlToBytes(text) {
  if (text.length % 4 === 1) return null;
  const bytes = [];
  for (let i = 0; i < text.length; i += 4) {
    const group = text.slice(i, i + 4);
    let n = 0;
    for (let k = 0; k < 4; k++) {
      const v = k < group.length ? B64_INDEX.get(group[k]) : 0;
      if (v === undefined) return null;
      n = (n << 6) | v;
    }
    bytes.push((n >> 16) & 255);
    if (group.length > 2) bytes.push((n >> 8) & 255);
    if (group.length > 3) bytes.push(n & 255);
  }
  return bytes;
}

// ---- patch <-> link -------------------------------------------------------

export const ENCODED_LENGTH = Math.ceil((PARAM_COUNT * 4) / 3); // 107 characters

export function encodePatch(values) {
  const bytes = new Array(PARAM_COUNT);
  for (let id = 0; id < PARAM_COUNT; id++) bytes[id] = clampValue(id, values[id]) - specFor(id).min;
  return `${FORMAT_VERSION}.${bytesToB64url(bytes)}`;
}

export class PatchLinkError extends Error {}

// Returns { values, clamped } where clamped counts values pulled into range.
export function decodePatch(text) {
  if (typeof text !== 'string') throw new PatchLinkError('The patch in this link is missing.');
  const m = /^(\d{1,3})\.([A-Za-z0-9_-]*)$/.exec(text);
  if (!m) throw new PatchLinkError('The patch in this link is not in a format sideband knows.');
  const version = Number(m[1]);
  if (version !== FORMAT_VERSION) {
    throw new PatchLinkError(`This link uses patch format ${version}; this sideband reads format ${FORMAT_VERSION}.`);
  }
  if (m[2].length !== ENCODED_LENGTH) {
    throw new PatchLinkError(`The patch in this link is ${m[2].length} characters long; it should be ${ENCODED_LENGTH}.`);
  }
  const bytes = b64urlToBytes(m[2]);
  if (!bytes || bytes.length !== PARAM_COUNT) throw new PatchLinkError('The patch in this link is damaged.');
  let clamped = 0;
  const values = bytes.map((b, id) => {
    const s = specFor(id);
    const raw = b + s.min;
    const v = clampValue(id, raw);
    if (v !== raw) clamped++;
    return v;
  });
  return { values, clamped };
}

export function patchToHash(values, name) {
  const clean = sanitizeName(name);
  const n = clean ? `&n=${encodeURIComponent(clean)}` : '';
  return `#p=${encodePatch(values)}${n}`;
}

// Parses a location.hash. Never throws: returns { status: 'none' } when there
// is no patch, { status: 'error', message } for a bad link, or
// { status: 'ok', values, name, clamped }.
export function patchFromHash(hash) {
  if (typeof hash !== 'string' || hash.length <= 1) return { status: 'none' };
  if (hash.length > MAX_HASH_LENGTH) {
    return { status: 'error', message: `This link is ${hash.length} characters long, which is too long to be a sideband patch (the limit is ${MAX_HASH_LENGTH}).` };
  }
  const fields = {};
  for (const part of hash.replace(/^#/, '').split('&').slice(0, 4)) {
    const eq = part.indexOf('=');
    if (eq < 1) continue;
    const key = part.slice(0, eq);
    if (key !== 'p' && key !== 'n') continue;
    let value = part.slice(eq + 1);
    try {
      value = decodeURIComponent(value);
    } catch {
      return { status: 'error', message: 'This link has broken characters in it.' };
    }
    fields[key] = value;
  }
  if (fields.p === undefined) return { status: 'none' };
  try {
    const { values, clamped } = decodePatch(fields.p);
    return { status: 'ok', values, clamped, name: sanitizeName(fields.n) };
  } catch (e) {
    if (e instanceof PatchLinkError) return { status: 'error', message: e.message };
    throw e;
  }
}

// ---- readable patch descriptions (presets) --------------------------------

// ops: [{ c, f, d, lv, r: [4], l: [4], fixed }] for OP1..OP6.
export function valuesFromDescription({ algo, feedback, ops }) {
  const v = defaultValues();
  v[0] = algo;
  v[1] = feedback;
  ops.forEach((o, op) => {
    const set = (key, value) => { v[paramId(op, key)] = value; };
    set('mode', o.fixed ? 1 : 0);
    set('coarse', o.c);
    set('fine', o.f ?? 0);
    set('detune', o.d ?? 0);
    set('level', o.lv);
    ['r1', 'r2', 'r3', 'r4'].forEach((k, i) => set(k, o.r[i]));
    ['l1', 'l2', 'l3', 'l4'].forEach((k, i) => set(k, o.l[i]));
  });
  return v.map((x, id) => clampValue(id, x));
}

// ---- display helpers ------------------------------------------------------

// The same formulas as op_hz() in the C engine, for readouts only.
export function frequencyLabel(values, op) {
  const get = (k) => values[paramId(op, k)];
  const fine = get('fine') / 100;
  const detune = get('detune');
  const det = detune ? ` ${detune > 0 ? '+' : '−'}${Math.abs(detune)}c` : '';
  if (get('mode') === 1) {
    const hz = 10 ** ((get('coarse') & 3) + fine);
    const text = hz >= 1000 ? `${(hz / 1000).toFixed(hz >= 9995 ? 1 : 2)} kHz` : `${hz.toFixed(hz >= 100 ? 0 : hz >= 10 ? 1 : 2)} Hz`;
    return `${text}${det}`;
  }
  const ratio = (get('coarse') === 0 ? 0.5 : get('coarse')) * (1 + fine);
  return `×${ratio.toFixed(2)}${det}`;
}

// Seconds for an envelope rate to sweep the full 0..99 range (C: g_rate_step).
export function sweepSeconds(rate) {
  return 40 * (0.0015 / 40) ** (rate / 99);
}
