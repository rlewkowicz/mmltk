import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';
import test from 'node:test';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

function page(episode = new Map()) {
  const elements = new Map(['mmltk-recovery', 'mmltk-recovery-detail', 'mmltk-recovery-button']
    .map(id => [id, {hidden: true, textContent: ''}]));
  const events = new Map(), reports = [], timers = [];
  let reloads = 0;
  const window = {
    location: {reload() { reloads++; }},
    addEventListener(name, callback) { events.set(name, callback); },
    mmltkIntegrationRecovery(message, manual) { reports.push({message, manual}); },
  };
  runInNewContext(script, {
    window,
    document: {getElementById: id => elements.get(id)},
    sessionStorage: {
      getItem: key => episode.get(key), setItem: (key, value) => episode.set(key, value),
      removeItem: key => episode.delete(key),
    },
    setTimeout: callback => timers.push(callback),
  });
  return {window, reports, timers, events, reloads: () => reloads,
    overlay: elements.get('mmltk-recovery'), detail: elements.get('mmltk-recovery-detail'),
    button: elements.get('mmltk-recovery-button')};
}

test('graphics recovery keeps GPU exhaustion visible without an allocation reload loop', () => {
  const p = page();
  p.window.mmltkRecoverWebGpu('WebGPU out of memory: resized canvas allocation failed');
  assert.equal(p.reloads(), 0);
  assert.equal(p.overlay.hidden, false);
  assert.equal(p.button.hidden, false);
  assert.match(p.detail.textContent, /native work continues/);
  const first = p.detail.textContent;
  p.window.mmltkRecoverWebGpu('Buffer is invalid');
  p.timers[0]();
  p.window.mmltkRecoverWebGpu('WebGPU device lost');
  assert.equal(p.detail.textContent, first);
  assert.equal(p.reloads(), 0);
  assert.equal(p.reports.length, 1);
  p.window.mmltkManualWebGpuReload();
  assert.equal(p.reloads(), 1);
});

test('graphics recovery retains the bounded device-loss automatic retry', () => {
  const episode = new Map();
  const first = page(episode);
  first.window.mmltkRecoverWebGpu('WebGPU device lost');
  first.window.mmltkRecoverWebGpu('WebGPU device lost');
  assert.equal(first.reloads(), 1);
  const retry = page(episode);
  retry.window.mmltkRecoverWebGpu('WebGPU device lost');
  assert.equal(retry.reloads(), 0);
  assert.equal(retry.button.hidden, false);
  assert.match(retry.detail.textContent, /Automatic reload did not recover/);
});

test('graphics recovery handles browser memory errors while leaving unrelated errors alone', () => {
  const p = page();
  p.events.get('error')({message: 'unrelated input error'});
  assert.equal(p.overlay.hidden, true);
  p.events.get('unhandledrejection')({reason: 'WebGPU: Not enough memory left.'});
  assert.equal(p.overlay.hidden, false);
  assert.equal(p.reloads(), 0);
  assert.equal(p.button.hidden, false);
});
