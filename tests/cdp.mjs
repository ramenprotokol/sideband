// A small Chrome DevTools Protocol driver for the browser tests and the
// screenshots. It launches headless Chrome on a random debugging port with
// audio muted, opens pages with exact device emulation (headless Chrome will
// not shrink a real window below 500 px), sends trusted mouse and key events
// (so they count as user gestures for Web Audio), and records console errors,
// uncaught exceptions and failed loads.
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

export function findChrome(env = process.env) {
  if (env.CHROME_PATH) return existsSync(env.CHROME_PATH) ? env.CHROME_PATH : null;
  return [
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
  ].find((p) => existsSync(p)) ?? null;
}

// Locally a missing Chrome skips the browser tests; REQUIRE_BROWSER=1 (for CI)
// turns that into a failure.
export function browserPlan(chromePath, env = process.env) {
  if (chromePath) return { run: true };
  if (env.REQUIRE_BROWSER === '1') return { fail: 'REQUIRE_BROWSER=1 but Chrome was not found (set CHROME_PATH).' };
  return { skip: 'Chrome not found; set CHROME_PATH to run the browser tests' };
}

export async function launchChrome(chromePath) {
  const profile = mkdtempSync(join(tmpdir(), 'op-six-chrome-'));
  const proc = spawn(chromePath, [
    '--headless=new',
    '--remote-debugging-port=0',
    `--user-data-dir=${profile}`,
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-extensions',
    '--hide-scrollbars',
    '--mute-audio',
    '--force-color-profile=srgb',
    'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });

  const wsUrl = await new Promise((resolve, reject) => {
    let buf = '';
    const timer = setTimeout(() => reject(new Error('Chrome did not start within 20 s')), 20000);
    proc.stderr.on('data', (d) => {
      buf += d;
      const m = /DevTools listening on (ws:\/\/\S+)/.exec(buf);
      if (m) { clearTimeout(timer); resolve(m[1]); }
    });
    proc.once('exit', (code) => { clearTimeout(timer); reject(new Error(`Chrome exited early (${code})`)); });
  });

  const ws = new WebSocket(wsUrl);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
  let nextId = 0;
  const pending = new Map();
  const listeners = new Set();
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      const p = pending.get(msg.id);
      pending.delete(msg.id);
      if (msg.error) p.reject(new Error(msg.error.message)); else p.resolve(msg.result);
    } else {
      for (const l of listeners) l(msg);
    }
  };
  const send = (method, params = {}, sessionId) => {
    const id = ++nextId;
    ws.send(JSON.stringify({ id, method, params, sessionId }));
    return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
  };

  async function openPage({ width, height, mobile = false, scale = 1, scheme, reducedMotion = false }) {
    const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
    const s = (method, params) => send(method, params, sessionId);
    const problems = [];
    const onEvent = (msg) => {
      if (msg.sessionId !== sessionId) return;
      if (msg.method === 'Runtime.exceptionThrown') {
        const d = msg.params.exceptionDetails;
        problems.push({ kind: 'exception', text: d.exception?.description ?? d.text });
      } else if (msg.method === 'Runtime.consoleAPICalled' && (msg.params.type === 'error' || msg.params.type === 'warning')) {
        problems.push({ kind: `console.${msg.params.type}`, text: msg.params.args.map((a) => a.value ?? a.description).join(' ') });
      } else if (msg.method === 'Log.entryAdded' && msg.params.entry.level === 'error') {
        problems.push({ kind: 'log', text: msg.params.entry.text, url: msg.params.entry.url ?? '' });
      }
    };
    listeners.add(onEvent);
    await s('Page.enable');
    await s('Runtime.enable');
    await s('Log.enable');
    await s('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: scale, mobile });
    const features = [];
    if (scheme) features.push({ name: 'prefers-color-scheme', value: scheme });
    features.push({ name: 'prefers-reduced-motion', value: reducedMotion ? 'reduce' : 'no-preference' });
    await s('Emulation.setEmulatedMedia', { features });

    const evaluate = async (expression) => {
      const r = await s('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
      return r.result.value;
    };
    const waitFor = async (expression, timeout = 15000) => {
      const end = Date.now() + timeout;
      while (Date.now() < end) {
        if (await evaluate(expression).catch(() => false)) return true;
        await new Promise((r) => setTimeout(r, 80));
      }
      throw new Error(`timed out waiting for: ${expression}`);
    };
    const navigate = async (url) => {
      const loaded = new Promise((resolve) => {
        const on = (msg) => {
          if (msg.sessionId === sessionId && msg.method === 'Page.loadEventFired') { listeners.delete(on); resolve(); }
        };
        listeners.add(on);
      });
      await s('Page.navigate', { url });
      await loaded;
    };
    // A trusted click in the middle of the element matched by `selector`.
    const click = async (selector) => {
      const box = await evaluate(`(() => { const e = document.querySelector(${JSON.stringify(selector)}); e.scrollIntoView({ block: 'center' }); const r = e.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; })()`);
      for (const type of ['mousePressed', 'mouseReleased']) {
        await s('Input.dispatchMouseEvent', { type, x: box.x, y: box.y, button: 'left', clickCount: 1 });
      }
    };
    // key is the DOM key value ('z', 'ArrowRight'); printable keys also send text.
    const key = async (type, code, keyValue) => {
      const printable = typeof keyValue === 'string' && keyValue.length === 1;
      await s('Input.dispatchKeyEvent', { type, code, key: keyValue, text: type === 'keyDown' && printable ? keyValue : undefined });
    };
    return {
      problems,
      evaluate,
      waitFor,
      navigate,
      click,
      key,
      send: s,
      screenshot: async (full = false) => {
        const params = { format: 'png' };
        if (full) {
          const { cssContentSize } = await s('Page.getLayoutMetrics');
          params.captureBeyondViewport = true;
          params.clip = { x: 0, y: 0, width, height: Math.ceil(cssContentSize.height), scale: 1 };
        }
        return Buffer.from((await s('Page.captureScreenshot', params)).data, 'base64');
      },
      close: async () => {
        listeners.delete(onEvent);
        await send('Target.closeTarget', { targetId }).catch(() => {});
      },
    };
  }

  async function close() {
    try { ws.close(); } catch { /* closed */ }
    proc.kill();
    await new Promise((r) => (proc.exitCode !== null ? r() : proc.once('exit', r)));
    rmSync(profile, { recursive: true, force: true });
  }

  return { openPage, close, send };
}
