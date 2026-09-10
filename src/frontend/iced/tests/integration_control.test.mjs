import assert from 'node:assert/strict';
import test from 'node:test';
import * as browser from '../src/integration_control/browser.mjs';

const gallery = 'explore.gallery.workspace';
const detail = 'explore.detail.workspace';
const workspace = 'workflow.visual.workspace';

// Exercise the runtime module with bounded browser effects. No probe or input
// algorithm is reproduced here; frames and microtasks are delivered explicitly.
function canvasFixture(t, diagnostics = false, driver = true) {
  const original = [];
  const install = (name, value) => {
    original.push([name, Object.getOwnPropertyDescriptor(globalThis, name)]);
    Object.defineProperty(globalThis, name, {value, configurable: true, writable: true});
  };
  const microtasks = [], frames = [], events = [], reports = [];
  const allocations = {maps: 0, sets: 0, scratch: 0, reads: 0, listeners: 0, clock: 0};
  const css = {x: 0, y: 0, left: 0, top: 0, width: 640, height: 480};
  const canvas = {
    width: 640, height: 480,
    getBoundingClientRect: () => css,
    dispatchEvent: event => { events.push(event.type); return true; },
  };
  const NativeMap = Map, NativeSet = Set;
  install('Map', class extends NativeMap { constructor(...args) { super(...args); allocations.maps++; } });
  install('Set', class extends NativeSet { constructor(...args) { super(...args); allocations.sets++; } });
  install('document', {querySelector: () => canvas, activeElement: canvas});
  install('window', {
    devicePixelRatio: 1,
    addEventListener: () => { allocations.listeners++; },
    removeEventListener: () => { allocations.listeners--; },
  });
  install('performance', {now: () => { allocations.clock++; return 0; }});
  install('dump', line => reports.push(JSON.parse(line)));
  install('PointerEvent', class { constructor(type, fields) { this.type = type; Object.assign(this, fields); } });
  install('queueMicrotask', callback => microtasks.push(callback));
  install('requestAnimationFrame', callback => frames.push(callback));
  install('OffscreenCanvas', class {
    constructor(width, height) { this.width = width; this.height = height; allocations.scratch++; }
    getContext() {
      return {
        clearRect() {}, drawImage() {},
        getImageData(_x, _y, width, height) {
          allocations.reads++;
          return {data: Uint8ClampedArray.from({length: width * height * 4}, (_, i) => i % 4 === 3 ? 255 : 64)};
        },
      };
    }
  });
  browser.mmltkIntegrationDriver(driver);
  browser.mmltkIntegrationInitialize(diagnostics);
  t.after(() => {
    browser.mmltkIntegrationInitialize(false);
    browser.mmltkIntegrationDriver(false);
    for (const [name, descriptor] of original.reverse()) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else delete globalThis[name];
    }
  });
  const drain = queue => {
    let remaining = 32;
    while (queue.length) {
      assert.ok(remaining-- > 0, 'bounded asynchronous handoff');
      queue.shift()();
    }
  };
  return {canvas, css, allocations, events, reports, frames, microtasks,
    flushMicrotasks: () => drain(microtasks), flushFrames: () => drain(frames)};
}

function assertQuiet(fixture) {
  assert.deepEqual(fixture.allocations, {maps: 0, sets: 0, scratch: 0, reads: 0, listeners: 0, clock: 0});
  assert.deepEqual(fixture.reports, []);
}

test('ordinary execution constructs neither owner and schedules no input or probe work', t => {
  const f = canvasFixture(t, false, false);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  browser.mmltkIntegrationReceipt(gallery, 'unused', 7, 11);
  assert.equal(browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 7, false), 0);
  browser.mmltkIntegrationRenderedStyle('unused', 'unused', 0, 0, 0, 1, 10, 10);
  assert.deepEqual(f.microtasks, []);
  assert.deepEqual(f.frames, []);
  assert.deepEqual(f.events, []);
  assertQuiet(f);
});

test('quiet draw-conditioned input uses direct readiness, once, independently of diagnostics', t => {
  const f = canvasFixture(t);
  assert.equal(browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 7, false), 1);
  browser.mmltkIntegrationDriverDraw(detail, 7, 11);
  browser.mmltkIntegrationDriverDraw(gallery, 8, 12);
  f.flushMicrotasks();
  assert.deepEqual(f.events, []);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  f.flushMicrotasks();
  assert.deepEqual(f.events, ['pointermove', 'pointerdown', 'pointerup']);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  f.flushMicrotasks();
  assert.equal(f.events.length, 3);
  // An already completed matching draw can also release a newly armed request.
  assert.equal(browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 6, true), 1);
  f.flushMicrotasks();
  assert.equal(f.events.length, 6);
  assertQuiet(f);
});

test('quiet reset retires both armed and queued input without touching replacement input', t => {
  const f = canvasFixture(t);
  browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 7, false);
  browser.mmltkIntegrationResetScenario();
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  f.flushMicrotasks();
  assert.deepEqual(f.events, []);
  browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 7, false);
  assert.equal(f.microtasks.length, 1);
  browser.mmltkIntegrationResetScenario();
  browser.mmltkIntegrationClickAfterSurfaceDraw(30, 40, gallery, 8, false);
  browser.mmltkIntegrationDriverDraw(gallery, 8, 12);
  f.flushMicrotasks();
  assert.deepEqual(f.events, ['pointermove', 'pointerdown', 'pointerup']);
  assertQuiet(f);
});

test('missing physical receipts fail closed before scheduling or sampling', t => {
  const f = canvasFixture(t, true);
  const results = [];
  const completed = (...values) => results.push(values);
  browser.mmltkIntegrationAtlasPixels([0, 0, 8, 8], 7, 11, completed);
  browser.mmltkIntegrationAtlasComposition([0, 0, 1, 1, 64, 64, 64, 255, 4, 0], [], '{}', 7, 11, 1, completed);
  browser.mmltkIntegrationUpscalePixels([0, 0, 8, 8], [0, 0, 8, 8], 7, 11, completed);
  browser.mmltkIntegrationAnnotationSwatch([0, 0, 8, 8], [64, 64, 64], 'tool', '', false, completed);
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  assert.deepEqual(results, [[0, 0], [0, 0], [0, 0], [0, 0]]);
  assert.equal(f.allocations.scratch, 0);
  assert.equal(f.allocations.reads, 0);
  assert.deepEqual(f.frames, []);
  assert.deepEqual(f.events, []);
  browser.mmltkIntegrationResetScenario();
  assert.equal(results.length, 4, 'all once callbacks have already retired');
});

test('same-frame geometry change rejects old pixel work and admits the replacement', t => {
  const f = canvasFixture(t, true);
  const results = [];
  const pixels = () => browser.mmltkIntegrationAtlasPixels([0, 0, 8, 8], 7, 11, (...values) => results.push(values));
  browser.mmltkIntegrationReceipt(gallery, 'geometry-A', 7, 11);
  pixels();
  browser.mmltkIntegrationReceipt(gallery, 'geometry-B', 7, 11);
  pixels();
  f.flushFrames();
  assert.deepEqual(results, [[0, 0], [1, 1]]);
  assert.equal(f.allocations.reads, 1);
  pixels();
  f.css.x = 17;
  f.flushFrames();
  assert.deepEqual(results.at(-1), [0, 0]);
  assert.equal(f.allocations.reads, 1, 'canvas geometry is also part of the captured receipt');
});

test('reset settles nested callbacks once and obsolete work cannot clear replacement probes', t => {
  const f = canvasFixture(t, true);
  const old = [], current = [];
  browser.mmltkIntegrationReceipt(gallery, 'old', 7, 11);
  browser.mmltkIntegrationReceipt(detail, 'old', 7, 11);
  browser.mmltkIntegrationReceipt(workspace, 'old', 7, 11);
  const points = [0, 0, 1, 1, 64, 64, 64, 255, 4, 0];
  const composition = completed => browser.mmltkIntegrationAtlasComposition(points, [], '{}', 7, 11, 1, completed);
  composition((...values) => old.push(values));
  browser.mmltkIntegrationUpscalePixels([0, 0, 8, 8], [0, 0, 8, 8], 7, 11, (...values) => old.push(values));
  browser.mmltkIntegrationAnnotationSwatch([0, 0, 8, 8], [64, 64, 64], 'tool', '', false, (...values) => old.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  browser.mmltkIntegrationResetScenario();
  assert.deepEqual(old, [[0, 0], [0, 0], [0, 0]]);
  browser.mmltkIntegrationReceipt(gallery, 'new', 7, 11);
  composition((...values) => current.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  f.flushFrames();
  assert.deepEqual(old, [[0, 0], [0, 0], [0, 0]]);
  assert.deepEqual(current, [[1, 1]]);
  assert.equal(f.allocations.reads, 2);
  assert.equal(f.reports.filter(record => record.event === 'iced.surface.canvas_pixel').length, 1);
});

test('diagnostic replacement preserves pending probes and does not cancel quiet driver input', t => {
  const f = canvasFixture(t, true);
  const old = [], current = [];
  const points = [0, 0, 1, 1, 64, 64, 64, 255, 4, 0];
  const composition = completed => browser.mmltkIntegrationAtlasComposition(points, [], '{}', 7, 11, 1, completed);
  browser.mmltkIntegrationReceipt(gallery, 'old', 7, 11);
  composition((...values) => old.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 7, false);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  browser.mmltkIntegrationInitialize(false);
  assert.deepEqual(old, [[0, 0]]);
  f.flushMicrotasks();
  assert.deepEqual(f.events, ['pointermove', 'pointerdown', 'pointerup']);
  assert.deepEqual(f.reports, []);
  browser.mmltkIntegrationInitialize(true);
  browser.mmltkIntegrationReceipt(gallery, 'new', 7, 11);
  composition((...values) => current.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  f.flushFrames();
  assert.deepEqual(old, [[0, 0]]);
  assert.deepEqual(current, [[1, 1]]);
  assert.equal(f.allocations.reads, 2);
  assert.equal(f.reports.filter(record => record.event === 'iced.surface.canvas_pixel').length, 1);
});
