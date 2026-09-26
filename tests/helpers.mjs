import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = join(dirname(fileURLToPath(import.meta.url)), '..');
export const dist = join(root, 'dist');

// A fresh instance of the built engine (npm test builds dist/ first).
export function loadEngine() {
  const bytes = readFileSync(join(dist, 'sideband.wasm'));
  const x = new WebAssembly.Instance(new WebAssembly.Module(bytes), {}).exports;
  const block = () => new Float32Array(x.memory.buffer, x.sideband_render(), 128);
  const monitor = () => new Float32Array(x.memory.buffer, x.sideband_monitor(), 128);
  // Render n samples (rounded up to whole blocks) of output and monitor.
  const render = (n) => {
    const blocks = Math.ceil(n / 128);
    const out = new Float32Array(blocks * 128);
    const mon = new Float32Array(blocks * 128);
    for (let b = 0; b < blocks; b++) {
      out.set(block(), b * 128);
      mon.set(monitor(), b * 128);
    }
    return { out, mon };
  };
  const setPatch = (values) => values.forEach((v, id) => x.sideband_set_param(id, v));
  return { x, render, setPatch };
}

export function fnv1a(floats) {
  const bytes = new Uint8Array(floats.buffer, floats.byteOffset, floats.byteLength);
  let h = 2166136261 >>> 0;
  for (const b of bytes) {
    h ^= b;
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h;
}

export function peak(x) {
  let p = 0;
  for (const v of x) if (Math.abs(v) > p) p = Math.abs(v);
  return p;
}

// Magnitude spectrum of n samples starting at `start` (Hann window, radix-2).
export function spectrum(x, start, n = 8192) {
  const re = new Float64Array(n);
  const im = new Float64Array(n);
  for (let i = 0; i < n; i++) re[i] = (x[start + i] ?? 0) * (0.5 - 0.5 * Math.cos((2 * Math.PI * i) / n));
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < len / 2; k++) {
        const c = Math.cos(ang * k), s = Math.sin(ang * k);
        const a = i + k, b = a + len / 2;
        const vr = re[b] * c - im[b] * s, vi = re[b] * s + im[b] * c;
        re[b] = re[a] - vr; im[b] = im[a] - vi;
        re[a] += vr; im[a] += vi;
      }
    }
  }
  const mag = new Float64Array(n / 2);
  for (let i = 0; i < n / 2; i++) mag[i] = Math.hypot(re[i], im[i]);
  return mag;
}

// Frequency of the strongest spectral peak, refined by parabolic interpolation.
export function peakHz(mag, sampleRate, n = mag.length * 2) {
  let best = 1;
  for (let i = 2; i < mag.length - 1; i++) if (mag[i] > mag[best]) best = i;
  const a = Math.log(mag[best - 1] + 1e-30), b = Math.log(mag[best] + 1e-30), c = Math.log(mag[best + 1] + 1e-30);
  const shift = (0.5 * (a - c)) / (a - 2 * b + c);
  return ((best + shift) * sampleRate) / n;
}
