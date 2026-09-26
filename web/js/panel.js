// Builds the panel: flat faders, the six operator blocks (with envelope
// graphs), the algorithm chart and the preset buttons. It only renders state
// and reports edits; app.js owns the state.
import { OPS, OP_SPEC, paramId, specFor, frequencyLabel, sweepSeconds } from './patch.js';
import { ALGORITHMS, algorithmSVG, roleOf } from './algorithms.js';

const SVG_NS = 'http://www.w3.org/2000/svg';
const MINUS = '−';

function el(tag, className, text) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  if (text !== undefined) e.textContent = text;
  return e;
}

// ---- faders ----------------------------------------------------------------

export function makeFader({ id, label, ariaLabel, min, max, step = 1, format, onInput }) {
  const wrap = el('div', 'fader');
  const lab = el('label', 'fader-label', label);
  lab.htmlFor = id;
  const out = el('output', 'fader-value');
  out.htmlFor = id;
  const input = el('input', 'fader-input');
  input.type = 'range';
  input.id = id;
  input.min = String(min);
  input.max = String(max);
  input.step = String(step);
  if (ariaLabel) input.setAttribute('aria-label', ariaLabel);
  input.addEventListener('input', () => onInput(Number(input.value)));
  wrap.append(lab, out, input);
  const api = {
    el: wrap,
    input,
    set(v) {
      input.value = String(v);
      const text = String(format(v));
      out.textContent = text;
      input.setAttribute('aria-valuetext', text.replace(MINUS, '-'));
      input.style.setProperty('--fill', String((v - min) / (max - min)));
    },
    refresh() { api.set(Number(input.value)); },
  };
  return api;
}

// ---- operator blocks -------------------------------------------------------

const signed = (v) => (v > 0 ? `+${v}` : v < 0 ? `${MINUS}${-v}` : '0');

function coarseText(values, op) {
  const c = values[paramId(op, 'coarse')];
  if (values[paramId(op, 'mode')] === 1) return ['1 Hz', '10 Hz', '100 Hz', '1 kHz'][c & 3];
  return c === 0 ? '×0.5' : `×${c}`;
}

function egPath(values, op, w, h) {
  const g = (k) => values[paramId(op, k)];
  const L = [g('l4'), g('l1'), g('l2'), g('l3'), g('l4')];
  const R = [g('r1'), g('r2'), g('r3'), g('r4')];
  // Falls are straight in level (dB); rises ease in (see RISE_TOP in opsix.c).
  const seconds = R.map((r, i) => (L[i + 1] > L[i]
    ? (Math.log((119 - L[i]) / (119 - L[i + 1])) / Math.log(119 / 20)) * sweepSeconds(r)
    : ((L[i] - L[i + 1]) / 99) * sweepSeconds(r)));
  // Log-ish time axis so a 2 ms click and a 20 s swell both show.
  const widths = seconds.map((t) => Math.log1p(t / 0.004));
  const hold = w * 0.16;
  const scale = (w - hold - 2) / Math.max(1e-6, widths.reduce((a, b) => a + b, 0));
  const y = (level) => h - 3 - (level / 99) * (h - 8);
  const points = [[1, y(L[0])]];
  let x = 1;
  let releaseX = 0;
  for (let i = 0; i < 4; i++) {
    if (i === 3) {
      x += hold;
      points.push([x, y(L[3])]);
      releaseX = x;
    }
    const dx = widths[i] * scale;
    if (L[i + 1] > L[i]) {
      // Rising segments ease in towards the top, as the engine does.
      for (let s = 1; s <= 8; s++) {
        const p = s / 8;
        const gap0 = 119 - L[i], gapEnd = 119 - L[i + 1];
        const level = 119 - gap0 * (gapEnd / gap0) ** p;
        points.push([x + dx * p, y(level)]);
      }
    } else {
      points.push([x + dx, y(L[i + 1])]);
    }
    x += dx;
  }
  const d = points.map(([px, py], i) => `${i ? 'L' : 'M'}${px.toFixed(1)},${py.toFixed(1)}`).join(' ');
  return { d, releaseX, sustainY: y(L[3]) };
}

function buildOp(op, onParam) {
  const n = op + 1;
  const block = el('article', 'op');
  block.id = `op${n}`;
  block.setAttribute('aria-labelledby', `op${n}-title`);

  const pinIn = el('div', 'pin pin-in');
  const head = el('header', 'op-head');
  const title = el('h3', 'op-title', `OP ${n}`);
  title.id = `op${n}-title`;
  const role = el('span', 'op-role');
  const mode = el('button', 'btn small mode');
  mode.type = 'button';
  mode.addEventListener('click', () => {
    const id = paramId(op, 'mode');
    onParam(id, mode.getAttribute('aria-pressed') === 'true' ? 0 : 1);
  });
  head.append(title, role, mode);

  const freq = el('div', 'op-freq');
  freq.setAttribute('aria-live', 'off');
  const freqLabel = el('span', 'op-freq-label', 'FREQ');
  const freqValue = el('span', 'op-freq-value');
  freq.append(freqLabel, freqValue);

  const faders = {};
  const osc = el('div', 'op-osc');
  const egFaders = el('div', 'op-eg-faders');
  for (const spec of OP_SPEC) {
    if (spec.key === 'mode') continue;
    const id = paramId(op, spec.key);
    const long = { coarse: 'coarse frequency', fine: 'fine frequency', detune: 'detune', level: 'output level' }[spec.key] ??
      (spec.key[0] === 'r' ? `envelope rate ${spec.key[1]}` : `envelope level ${spec.key[1]}`);
    const f = makeFader({
      id: `op${n}-${spec.key}`,
      label: spec.label,
      ariaLabel: `Operator ${n} ${long}`,
      min: spec.min,
      max: spec.max,
      format: (v) => String(v),
      onInput: (v) => onParam(id, v),
    });
    faders[spec.key] = f;
    (['coarse', 'fine', 'detune', 'level'].includes(spec.key) ? osc : egFaders).append(f.el);
  }

  const eg = el('div', 'op-eg');
  const egHead = el('div', 'op-eg-head', 'EG');
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('class', 'eg-graph');
  svg.setAttribute('viewBox', '0 0 240 56');
  svg.setAttribute('preserveAspectRatio', 'none');
  svg.setAttribute('aria-hidden', 'true');
  const base = document.createElementNS(SVG_NS, 'line');
  base.setAttribute('class', 'eg-axis');
  base.setAttribute('x1', '0'); base.setAttribute('x2', '240'); base.setAttribute('y1', '53'); base.setAttribute('y2', '53');
  const keyOff = document.createElementNS(SVG_NS, 'line');
  keyOff.setAttribute('class', 'eg-keyoff');
  keyOff.setAttribute('y1', '0'); keyOff.setAttribute('y2', '56');
  const path = document.createElementNS(SVG_NS, 'path');
  path.setAttribute('class', 'eg-line');
  svg.append(base, keyOff, path);
  const egLegend = el('div', 'eg-legend');
  egLegend.append(el('span', '', 'KEY ON'), el('span', 'eg-legend-off', 'KEY OFF'));
  eg.append(egHead, svg, egLegend, egFaders);

  const pinOut = el('div', 'pin pin-out');
  block.append(pinIn, head, freq, osc, eg, pinOut);

  function sync(values, algo) {
    const fixed = values[paramId(op, 'mode')] === 1;
    mode.textContent = fixed ? 'FIXED Hz' : 'RATIO';
    mode.setAttribute('aria-pressed', String(fixed));
    mode.setAttribute('aria-label', `Operator ${n} frequency mode: ${fixed ? 'fixed' : 'ratio to the key'}. Press to switch.`);
    freqValue.textContent = frequencyLabel(values, op);
    for (const spec of OP_SPEC) {
      if (spec.key === 'mode') continue;
      const f = faders[spec.key];
      const v = values[paramId(op, spec.key)];
      f.set(v);
      const out = f.el.querySelector('output');
      if (spec.key === 'coarse') out.textContent = coarseText(values, op);
      else if (spec.key === 'fine') out.textContent = fixed ? `×10^.${String(v).padStart(2, '0')}` : `+.${String(v).padStart(2, '0')}`;
      else if (spec.key === 'detune') out.textContent = `${signed(v)}c`;
      f.input.setAttribute('aria-valuetext', out.textContent.replace(MINUS, '-'));
    }
    const r = roleOf(algo, op);
    block.classList.toggle('carrier', r.carrier);
    block.classList.toggle('silent', values[paramId(op, 'level')] === 0);
    role.textContent = r.carrier ? 'CARRIER' : 'MODULATOR';
    pinIn.textContent = [
      r.from.length ? `IN ◂ OP${r.from.join(', OP')}` : 'IN ◂ —',
      r.feedback ? `FB ↺ ${values[1]}` : '',
    ].filter(Boolean).join('   ');
    pinOut.textContent = r.carrier ? 'OUT ▸ MIX' : `OUT ▸ OP${r.to.join(', OP')}`;
    const g = egPath(values, op, 240, 56);
    path.setAttribute('d', g.d);
    keyOff.setAttribute('x1', g.releaseX.toFixed(1));
    keyOff.setAttribute('x2', g.releaseX.toFixed(1));
  }

  return { el: block, sync };
}

export function buildOperators(container, onParam) {
  const ops = [];
  for (let op = 0; op < OPS; op++) {
    const o = buildOp(op, onParam);
    container.append(o.el);
    ops.push(o);
  }
  return ops;
}

// ---- algorithm chart -------------------------------------------------------

export function buildAlgorithms(container, onSelect) {
  const cards = ALGORITHMS.map((a, i) => {
    const card = el('button', 'algo-card');
    card.type = 'button';
    card.setAttribute('role', 'radio');
    card.dataset.algo = String(i);
    const carriers = a.carriers.length;
    card.setAttribute('aria-label', `Algorithm ${i + 1}, ${a.name.toLowerCase()}: ${carriers} carrier${carriers > 1 ? 's' : ''}`);
    const figure = el('span', 'algo-figure');
    figure.innerHTML = algorithmSVG(i); // generated from constants, no user input
    const svg = figure.querySelector('svg');
    svg.style.width = `calc(${svg.dataset.w}px * var(--algo-scale))`;
    const cap = el('span', 'algo-cap');
    cap.append(el('span', 'algo-num', `ALG ${i + 1}`), el('span', 'algo-name', a.name));
    const sub = el('span', 'algo-sub', `${carriers} CARRIER${carriers > 1 ? 'S' : ''}`);
    card.append(figure, cap, sub);
    card.addEventListener('click', () => onSelect(i));
    card.addEventListener('keydown', (e) => {
      const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
      let next = null;
      if (step) next = (i + step + ALGORITHMS.length) % ALGORITHMS.length;
      if (e.key === 'Home') next = 0;
      if (e.key === 'End') next = ALGORITHMS.length - 1;
      if (next === null) return;
      e.preventDefault();
      onSelect(next);
      cards[next].focus();
    });
    container.append(card);
    return card;
  });
  return {
    sync(algo) {
      cards.forEach((c, i) => {
        c.setAttribute('aria-checked', String(i === algo));
        c.tabIndex = i === algo ? 0 : -1;
      });
    },
  };
}

// ---- presets ---------------------------------------------------------------

export function buildPresets(container, presets, onPick) {
  const buttons = presets.map((p, i) => {
    const b = el('button', 'btn preset');
    b.type = 'button';
    b.append(el('span', 'preset-num', String(i + 1).padStart(2, '0')), el('span', 'preset-name', p.name));
    b.addEventListener('click', () => onPick(i));
    container.append(b);
    return b;
  });
  return {
    sync(values) {
      let match = -1;
      buttons.forEach((b, i) => {
        const same = presets[i].values.every((v, id) => v === values[id]);
        if (same && match < 0) match = i;
        b.setAttribute('aria-pressed', String(same));
      });
      return match;
    },
  };
}

export function feedbackFader(container, onParam) {
  const spec = specFor(1);
  const f = makeFader({
    id: 'feedback',
    label: 'OP6 FEEDBACK',
    ariaLabel: 'Operator 6 feedback',
    min: spec.min,
    max: spec.max,
    format: (v) => String(v),
    onInput: (v) => onParam(1, v),
  });
  container.replaceWith(f.el);
  f.el.id = 'feedback-fader';
  f.el.classList.add('wide');
  return f;
}
