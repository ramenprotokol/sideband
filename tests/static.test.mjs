// Checks on the built dist/ and the repo that do not need a browser:
// required files, headers (CSP, caching), third-party notices, no inline code
// the CSP would block, no bare deploy scripts, no private paths.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { dist, root } from './helpers.mjs';
import { globalHeaders } from '../scripts/serve.mjs';

const read = (...p) => readFileSync(join(...p), 'utf8');

function walk(dir, skip = new Set()) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    if (skip.has(e.name)) return [];
    const p = join(dir, e.name);
    return e.isDirectory() ? walk(p, skip) : [p];
  });
}

test('dist/ holds the page, the engine and the worklet', () => {
  for (const f of ['index.html', 'styles.css', 'opsix.wasm', 'favicon.svg', '_headers', 'js/app.js', 'js/worklet.js', 'js/patch.js']) {
    assert.ok(existsSync(join(dist, f)), `missing dist/${f}`);
  }
  const files = walk(dist);
  assert.ok(files.length < 20000, 'Cloudflare Pages allows 20,000 files');
  for (const f of files) assert.ok(statSync(f).size < 25 * 1024 * 1024, `${relative(dist, f)} is over 25 MiB`);
});

test('THIRD-PARTY-NOTICES.txt ships in dist/ and the page links to it', () => {
  const notices = read(dist, 'THIRD-PARTY-NOTICES.txt');
  assert.match(notices, /IBM Plex Mono/);
  assert.match(notices, /SIL Open Font License, Version 1\.1/);
  assert.match(notices, /-nostdlib/);
  const html = read(dist, 'index.html');
  assert.match(html, /href="THIRD-PARTY-NOTICES\.txt"/);
  assert.match(read(root, 'README.md'), /THIRD-PARTY-NOTICES/);
});

test('_headers: a strict CSP, and no long cache on unhashed files', async () => {
  const h = await globalHeaders(dist);
  const csp = h['content-security-policy'];
  assert.ok(csp, 'CSP present');
  const directives = Object.fromEntries(csp.split(';').map((d) => d.trim().split(/\s+/)).map(([k, ...v]) => [k, v]));
  // WebAssembly compilation only: no inline script, no JS eval.
  assert.deepEqual(directives['script-src'], ["'self'", "'wasm-unsafe-eval'"]);
  assert.doesNotMatch(csp, /unsafe-inline/);
  assert.deepEqual(directives['default-src'], ["'self'"]);
  assert.match(csp, /frame-ancestors 'none'/);
  assert.equal(h['cache-control'], 'no-cache', 'files are not content-hashed, so they must revalidate');
  assert.doesNotMatch(read(dist, '_headers'), /max-age=[1-9]/);
  assert.match(h['permissions-policy'], /midi=\(self\)/);
});

test('the page has no inline scripts, inline styles or style attributes (the CSP would block them)', () => {
  const html = read(dist, 'index.html');
  assert.doesNotMatch(html, /<script(?![^>]*\bsrc=)[^>]*>/i, 'inline <script>');
  assert.doesNotMatch(html, /<style/i, 'inline <style>');
  assert.doesNotMatch(html, /\sstyle=/i, 'style attribute');
  assert.doesNotMatch(html, /\son[a-z]+=/i, 'inline event handler');
  for (const f of walk(join(dist, 'js'))) {
    const src = read(f);
    assert.doesNotMatch(src, /style="/, `${relative(dist, f)} writes a style attribute into markup`);
    assert.doesNotMatch(src, /\beval\(|new Function\(/, `${relative(dist, f)} uses eval`);
  }
});

test('no bare deploy command in scripts; wrangler.toml carries no account id', () => {
  const pkg = JSON.parse(read(root, 'package.json'));
  for (const [name, cmd] of Object.entries(pkg.scripts)) {
    assert.doesNotMatch(cmd, /wrangler\s+(pages\s+)?deploy/, `npm script "${name}" deploys directly`);
  }
  const toml = read(root, 'wrangler.toml');
  assert.match(toml, /pages_build_output_dir\s*=\s*"dist"/);
  assert.doesNotMatch(toml, /^\s*account_id\s*=/m);
});

test('no local absolute paths or build output in tracked sources', () => {
  const files = walk(root, new Set(['.git', 'node_modules', 'dist', 'build', 'docs']));
  const home = /\/(Users|home)\/[a-z]/i;
  for (const f of files) {
    if (/\.(png|wasm)$/.test(f)) continue;
    assert.doesNotMatch(read(f), home, `${relative(root, f)} contains a local path`);
  }
  const ignore = read(root, '.gitignore');
  assert.match(ignore, /^dist\/$/m);
  assert.match(ignore, /^build\/$/m);
});

test('LICENSE is MIT for ramenprotokol', () => {
  const lic = read(root, 'LICENSE');
  assert.match(lic, /MIT License/);
  assert.match(lic, /Copyright \(c\) 2026 ramenprotokol/);
});
