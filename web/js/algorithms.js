// The eight algorithms, as drawn in the manual. The routing mirrors the
// ALGOS table in src/opsix.c; tests/render.test.mjs checks the two agree by
// reading the table out of the compiled WASM.
//
// pos: [column, row] per operator, row 0 = carriers on the output bus.

export const ALGORITHMS = [
  { name: 'PAIRS', edges: [[2, 1], [4, 3], [6, 5]], carriers: [1, 3, 5],
    pos: { 1: [0, 0], 2: [0, 1], 3: [1, 0], 4: [1, 1], 5: [2, 0], 6: [2, 1] } },
  { name: 'TWIN STACKS', edges: [[3, 2], [2, 1], [6, 5], [5, 4]], carriers: [1, 4],
    pos: { 1: [0, 0], 2: [0, 1], 3: [0, 2], 4: [1, 0], 5: [1, 1], 6: [1, 2] } },
  { name: 'PAIR + FOUR', edges: [[2, 1], [6, 5], [5, 4], [4, 3]], carriers: [1, 3],
    pos: { 1: [0, 0], 2: [0, 1], 3: [1, 0], 4: [1, 1], 5: [1, 2], 6: [1, 3] } },
  { name: 'TOWER', edges: [[6, 5], [5, 4], [4, 3], [3, 2], [2, 1]], carriers: [1],
    pos: { 1: [0, 0], 2: [0, 1], 3: [0, 2], 4: [0, 3], 5: [0, 4], 6: [0, 5] } },
  { name: 'FAN', edges: [[2, 1], [6, 3], [6, 4], [6, 5]], carriers: [1, 3, 4, 5],
    pos: { 1: [0, 0], 2: [0, 1], 3: [1, 0], 4: [2, 0], 5: [3, 0], 6: [2, 1] } },
  { name: 'BRANCH', edges: [[2, 1], [3, 1], [5, 1], [4, 3], [6, 5]], carriers: [1],
    pos: { 1: [1, 0], 2: [0, 1], 3: [1, 1], 4: [1, 2], 5: [2, 1], 6: [2, 2] } },
  { name: 'STACK + SINES', edges: [[6, 5], [5, 4]], carriers: [1, 2, 3, 4],
    pos: { 1: [0, 0], 2: [1, 0], 3: [2, 0], 4: [3, 0], 5: [3, 1], 6: [3, 2] } },
  { name: 'ORGAN', edges: [], carriers: [1, 2, 3, 4, 5, 6],
    pos: { 1: [0, 0], 2: [1, 0], 3: [2, 0], 4: [3, 0], 5: [4, 0], 6: [5, 0] } },
];

export const FEEDBACK_OP = 6;

// Same bit layout as opsix_algo_mod_mask(): bit j set = OP(j+1) feeds OP(op+1).
export function modMask(algo, op) {
  return ALGORITHMS[algo].edges.reduce((m, [from, to]) => (to === op + 1 ? m | (1 << (from - 1)) : m), 0);
}

export function carrierMask(algo) {
  return ALGORITHMS[algo].carriers.reduce((m, c) => m | (1 << (c - 1)), 0);
}

// Where an operator's output goes, and what feeds it, for the op blocks.
export function roleOf(algo, op) {
  const a = ALGORITHMS[algo];
  const n = op + 1;
  const to = a.edges.filter(([f]) => f === n).map(([, t]) => t);
  const from = a.edges.filter(([, t]) => t === n).map(([f]) => f);
  return { carrier: a.carriers.includes(n), to, from, feedback: n === FEEDBACK_OP };
}

const PITCH_X = 27;
const PITCH_Y = 24;
const BOX = 18;

// An SVG line drawing of one algorithm. Pure string building (no DOM), so the
// Node tests can check it too. Colours come from CSS (currentColor).
export function algorithmSVG(algo, { title } = {}) {
  const a = ALGORITHMS[algo];
  const cols = Math.max(...Object.values(a.pos).map(([c]) => c)) + 1;
  const rows = Math.max(...Object.values(a.pos).map(([, r]) => r)) + 1;
  const padX = 18;
  const top = 16;
  const width = Math.max(cols * PITCH_X + padX * 2, 3 * PITCH_X + padX * 2);
  const offsetX = (width - cols * PITCH_X) / 2;
  const busY = top + rows * PITCH_Y + 6;
  const height = busY + 18;
  const cx = (op) => offsetX + a.pos[op][0] * PITCH_X + PITCH_X / 2;
  const cy = (op) => top + (rows - 1 - a.pos[op][1]) * PITCH_Y + PITCH_Y / 2;
  const parts = [];

  // Modulation wires: from the bottom of the source to the top of the target.
  // Sources beside their target drop to a joint above it first.
  for (const [from, to] of a.edges) {
    const x1 = cx(from), y1 = cy(from) + BOX / 2;
    const x2 = cx(to), y2 = cy(to) - BOX / 2;
    if (x1 === x2) {
      parts.push(`<line class="wire" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"/>`);
    } else {
      const jy = y2 - 3;
      parts.push(`<polyline class="wire" points="${x1},${y1} ${x1},${jy} ${x2},${jy} ${x2},${y2}"/>`);
    }
  }
  // Carriers drop onto the output bus.
  const carrierXs = a.carriers.map(cx);
  for (const c of a.carriers) parts.push(`<line class="wire" x1="${cx(c)}" y1="${cy(c) + BOX / 2}" x2="${cx(c)}" y2="${busY}"/>`);
  const busL = Math.min(...carrierXs), busR = Math.max(...carrierXs);
  const outX = (busL + busR) / 2;
  parts.push(`<line class="bus" x1="${busL - 4}" y1="${busY}" x2="${busR + 4}" y2="${busY}"/>`);
  parts.push(`<line class="wire" x1="${outX}" y1="${busY}" x2="${outX}" y2="${busY + 7}"/>`);
  parts.push(`<path class="arrow" d="M${outX - 3.5},${busY + 6} L${outX + 3.5},${busY + 6} L${outX},${busY + 11} Z"/>`);

  // Feedback loop on OP6: out of the right side, up and back into the top.
  {
    const x = cx(FEEDBACK_OP), y = cy(FEEDBACK_OP);
    const r = x + BOX / 2 + 6, t = y - BOX / 2 - 6;
    parts.push(`<polyline class="wire fb" points="${x + BOX / 2},${y} ${r},${y} ${r},${t} ${x},${t} ${x},${y - BOX / 2}"/>`);
  }
  // Operator boxes, drawn last so wires tuck under them.
  for (let op = 1; op <= 6; op++) {
    const x = cx(op) - BOX / 2, y = cy(op) - BOX / 2;
    const carrier = a.carriers.includes(op);
    parts.push(`<g class="opbox${carrier ? ' carrier' : ''}"><rect x="${x}" y="${y}" width="${BOX}" height="${BOX}"/>` +
      `<text x="${cx(op)}" y="${cy(op) + 4}">${op}</text></g>`);
  }
  const label = title ? `<title>${title}</title>` : '';
  // data-w lets the page draw every diagram at one scale (CSS --algo-scale),
  // so boxes are the same size across the chart, as on a printed page.
  return `<svg class="algo-svg" viewBox="0 0 ${width} ${height}" data-w="${width}" aria-hidden="true" focusable="false">${label}${parts.join('')}</svg>`;
}
