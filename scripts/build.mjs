#!/usr/bin/env node
// Builds dist/ from a clean clone. Needs Zig 0.16 (for `zig cc`) and Node 20+.
//   1. compiles src/sideband.c to dist/sideband.wasm with zig cc (wasm32-freestanding,
//      -nostdlib: no libc and no compiler runtime are linked in)
//   2. copies web/ into dist/
//   3. writes dist/_headers (CSP, no-cache revalidation for unhashed files)
import { spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync, readdirSync } from 'node:fs';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');

export const EXPORTS = [
  'sideband_init', 'sideband_sample_rate',
  'sideband_set_param', 'sideband_get_param', 'sideband_param_min', 'sideband_param_max', 'sideband_param_default',
  'sideband_note_on', 'sideband_note_off', 'sideband_all_notes_off', 'sideband_panic', 'sideband_set_volume',
  'sideband_render', 'sideband_monitor', 'sideband_take_peak', 'sideband_gain_reduction', 'sideband_active_voices',
  'sideband_algo_mod_mask', 'sideband_algo_carrier_mask', 'sideband_algo_feedback_op',
];

// Fixed 256 KiB of linear memory (4 pages): room for the static buffers and
// the stack, and it can never grow, so JS views of it never go stale.
const MEMORY = 4 * 65536;

export const ZIG_ARGS = [
  'cc', '-target', 'wasm32-freestanding',
  '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
  '-nostdlib', '-mbulk-memory', '-ffp-contract=off',
  '-Wl,--no-entry', '-Wl,--strip-all', '-Wl,-z,stack-size=65536',
  `-Wl,--initial-memory=${MEMORY}`, `-Wl,--max-memory=${MEMORY}`,
  ...EXPORTS.map((name) => `-Wl,--export=${name}`),
];

function run(cmd, args) {
  const r = spawnSync(cmd, args, { cwd: root, stdio: 'inherit' });
  if (r.error) {
    console.error(`could not run ${cmd}: ${r.error.message}`);
    if (cmd === 'zig') console.error('sideband needs Zig 0.16 on PATH (it uses `zig cc` to compile C to WebAssembly).');
    process.exit(1);
  }
  if (r.status !== 0) process.exit(r.status ?? 1);
}

function listFiles(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? listFiles(join(dir, e.name)) : [join(dir, e.name)],
  );
}

const HEADERS = `/*
  Content-Security-Policy: default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src 'self' data:; connect-src 'self'; media-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'
  X-Content-Type-Options: nosniff
  Referrer-Policy: no-referrer
  Permissions-Policy: camera=(), microphone=(), geolocation=(), midi=(self)
  Cache-Control: no-cache
`;

export function build() {
  rmSync(dist, { recursive: true, force: true });
  mkdirSync(dist, { recursive: true });
  run('zig', [...ZIG_ARGS, '-o', join(dist, 'sideband.wasm'), join(root, 'src', 'sideband.c')]);
  cpSync(join(root, 'web'), dist, { recursive: true });
  writeFileSync(join(dist, '_headers'), HEADERS);

  let total = 0;
  for (const file of listFiles(dist).sort()) {
    const size = statSync(file).size;
    total += size;
    console.log(`${String(size).padStart(8)}  ${relative(dist, file)}`);
  }
  console.log(`${String(total).padStart(8)}  bytes in dist/`);
  const wasm = readFileSync(join(dist, 'sideband.wasm'));
  const mod = new WebAssembly.Module(wasm);
  const imports = WebAssembly.Module.imports(mod);
  if (imports.length) {
    console.error(`sideband.wasm must be self-contained but imports: ${JSON.stringify(imports)}`);
    process.exit(1);
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) build();
