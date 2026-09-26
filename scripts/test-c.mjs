#!/usr/bin/env node
// Compiles the C unit tests natively with `zig cc` and runs them.
// Two builds: one with UndefinedBehaviorSanitizer (trapping, so any undefined
// float-to-int cast or overflow in the engine aborts the run), and one
// optimised build, which is what the timing-sensitive code really runs as.
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = join(root, 'build');
mkdirSync(out, { recursive: true });

const common = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off'];
const variants = [
  { name: 'ubsan', flags: ['-O1', '-g', '-fsanitize=undefined', '-fsanitize-trap=undefined'] },
  { name: 'O2', flags: ['-O2'] },
];

let failed = false;
for (const v of variants) {
  const exe = join(out, `test_opsix_${v.name}`);
  const compile = spawnSync('zig', [...common, ...v.flags, '-o', exe, join(root, 'tests', 'c', 'test_opsix.c'), '-lm'], { stdio: 'inherit' });
  if (compile.error) {
    console.error(`could not run zig: ${compile.error.message}. op-six needs Zig 0.16 on PATH.`);
    process.exit(1);
  }
  if (compile.status !== 0) process.exit(compile.status ?? 1);
  console.log(`\n== C unit tests (${v.name}) ==`);
  const r = spawnSync(exe, [], { stdio: 'inherit' });
  if (r.status !== 0) failed = true;
}
process.exit(failed ? 1 : 0);
