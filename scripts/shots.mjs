#!/usr/bin/env node
// Takes review screenshots of dist/ in headless Chrome (desktop and a true
// 400 px phone width via device emulation), with audio started and a chord
// held so the scope and spectrum show a signal.
//   node scripts/shots.mjs <output-dir> [--theme=light|dark] [--hash=#p=...]
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { serve } from './serve.mjs';
import { findChrome, launchChrome } from '../tests/cdp.mjs';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = process.argv[2];
if (!outDir) {
  console.error('usage: node scripts/shots.mjs <output-dir> [--theme=light|dark]');
  process.exit(2);
}
const theme = (process.argv.find((a) => a.startsWith('--theme=')) ?? '--theme=dark').slice(8);
const preset = Number((process.argv.find((a) => a.startsWith('--preset=')) ?? '--preset=4').slice(9));
mkdirSync(outDir, { recursive: true });

const chromePath = findChrome();
if (!chromePath) {
  console.error('Chrome not found (set CHROME_PATH).');
  process.exit(1);
}
const server = await serve(join(root, 'dist'), 0);
const base = `http://127.0.0.1:${server.address().port}/`;
const chrome = await launchChrome(chromePath);
try {
  for (const [name, width, height, mobile] of [['desktop', 1280, 800, false], ['phone', 400, 860, true]]) {
    const page = await chrome.openPage({ width, height, mobile, scale: 1, scheme: theme });
    await page.navigate(`${base}?test=1`);
    await page.waitFor('window.__sideband && document.fonts.status === "loaded"', 20000);
    await page.click('#start');
    await page.waitFor('window.__sideband.engine.ready', 15000).catch(() => {});
    if (preset > 0) await page.click(`#presets .preset:nth-child(${preset})`);
    for (const [code, text] of [['KeyZ', 'z'], ['KeyC', 'c'], ['KeyB', 'b']]) await page.key('keyDown', code, text);
    await new Promise((r) => setTimeout(r, 900));
    await page.evaluate('window.scrollTo(0, 0)');
    writeFileSync(join(outDir, `${name}-${theme}.png`), await page.screenshot());
    writeFileSync(join(outDir, `${name}-${theme}-full.png`), await page.screenshot(true));
    console.log(`${name}: problems ${JSON.stringify(page.problems)}`);
    await page.close();
  }
} finally {
  await chrome.close();
  server.close();
}
