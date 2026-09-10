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
  const sampling = {pixel: () => [64, 64, 64, 255], error: undefined};
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
      let sourceX = 0, sourceY = 0;
      return {
        clearRect() {}, drawImage(_canvas, x, y) { sourceX = x; sourceY = y; },
        getImageData(x, y, width, height) {
          allocations.reads++;
          if (sampling.error) throw sampling.error;
          const pixel = sampling.pixel(sourceX + x, sourceY + y);
          return {data: Uint8ClampedArray.from({length: width * height * 4}, (_, i) => pixel[i % 4])};
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
  return {canvas, css, sampling, allocations, events, reports, frames, microtasks,
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
  assert.equal(browser.mmltkIntegrationProbe(gallery), undefined);
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
  browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery), [0, 0, 8, 8], 7, 11, completed);
  browser.mmltkIntegrationAtlasComposition(browser.mmltkIntegrationProbe(gallery), [0, 0, 1, 1, 64, 64, 64, 255, 4, 0], [], '{}', 7, 11, 1, completed);
  browser.mmltkIntegrationUpscalePixels(browser.mmltkIntegrationProbe(detail), [0, 0, 8, 8], [0, 0, 8, 8], 7, 11, completed);
  browser.mmltkIntegrationAnnotationSwatch(browser.mmltkIntegrationProbe(workspace), [0, 0, 8, 8], [64, 64, 64], 'tool', '', false, completed);
  browser.mmltkIntegrationAnnotationPixels(browser.mmltkIntegrationProbe(workspace), [0, 0, 8, 8], [8, 8], [4, 4, 64, 64, 64, 0, 1], 7, 11, completed);
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  assert.deepEqual(results, [['invalidated', 0, 0], ['invalidated', 0, 0], ['invalidated', 0, 0], ['invalidated', 0, 0], ['invalidated', 0, 0]]);
  assert.equal(f.allocations.scratch, 0);
  assert.equal(f.allocations.reads, 0);
  assert.deepEqual(f.frames, []);
  assert.deepEqual(f.events, []);
  browser.mmltkIntegrationResetScenario();
  assert.equal(results.length, 5, 'all once callbacks have already retired');
});

test('same-frame geometry change rejects old pixel work and admits the replacement', t => {
  const f = canvasFixture(t, true);
  const results = [];
  const pixels = () => browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery), [0, 0, 8, 8], 7, 11, (...values) => results.push(values));
  browser.mmltkIntegrationReceipt(gallery, 'geometry-A', 7, 11);
  pixels();
  browser.mmltkIntegrationReceipt(gallery, 'geometry-B', 7, 11);
  pixels();
  f.flushFrames();
  assert.deepEqual(results, [['invalidated', 0, 0], ['observed', 1, 1]]);
  assert.equal(f.allocations.reads, 1);
  pixels();
  f.css.x = 17;
  f.flushFrames();
  assert.deepEqual(results.at(-1), ['invalidated', 0, 0]);
  assert.equal(f.allocations.reads, 1, 'canvas geometry is also part of the captured receipt');
});

test('reset settles nested callbacks once and obsolete work cannot clear replacement probes', t => {
  const f = canvasFixture(t, true);
  const old = [], current = [];
  browser.mmltkIntegrationReceipt(gallery, 'old', 7, 11);
  browser.mmltkIntegrationReceipt(detail, 'old', 7, 11);
  browser.mmltkIntegrationReceipt(workspace, 'old', 7, 11);
  const points = [0, 0, 1, 1, 64, 64, 64, 255, 4, 0];
  const composition = completed => browser.mmltkIntegrationAtlasComposition(browser.mmltkIntegrationProbe(gallery), points, [], '{}', 7, 11, 1, completed);
  composition((...values) => old.push(values));
  browser.mmltkIntegrationUpscalePixels(browser.mmltkIntegrationProbe(detail), [0, 0, 8, 8], [0, 0, 8, 8], 7, 11, (...values) => old.push(values));
  browser.mmltkIntegrationAnnotationSwatch(browser.mmltkIntegrationProbe(workspace), [0, 0, 8, 8], [64, 64, 64], 'tool', '', false, (...values) => old.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  browser.mmltkIntegrationResetScenario();
  assert.deepEqual(old, [['invalidated', 0, 0], ['invalidated', 0, 0], ['invalidated', 0, 0]]);
  browser.mmltkIntegrationReceipt(gallery, 'new', 7, 11);
  composition((...values) => current.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  f.flushFrames();
  assert.deepEqual(old, [['invalidated', 0, 0], ['invalidated', 0, 0], ['invalidated', 0, 0]]);
  assert.deepEqual(current, [['observed', 1, 1]]);
  assert.equal(f.allocations.reads, 2);
  assert.equal(f.reports.filter(record => record.event === 'iced.surface.canvas_pixel').length, 1);
});

test('diagnostic replacement preserves pending probes and does not cancel quiet driver input', t => {
  const f = canvasFixture(t, true);
  const old = [], current = [];
  const points = [0, 0, 1, 1, 64, 64, 64, 255, 4, 0];
  const composition = completed => browser.mmltkIntegrationAtlasComposition(browser.mmltkIntegrationProbe(gallery), points, [], '{}', 7, 11, 1, completed);
  browser.mmltkIntegrationReceipt(gallery, 'old', 7, 11);
  composition((...values) => old.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  browser.mmltkIntegrationClickAfterSurfaceDraw(10, 20, gallery, 7, false);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  browser.mmltkIntegrationInitialize(false);
  assert.deepEqual(old, [['invalidated', 0, 0]]);
  f.flushMicrotasks();
  assert.deepEqual(f.events, ['pointermove', 'pointerdown', 'pointerup']);
  assert.deepEqual(f.reports, []);
  browser.mmltkIntegrationInitialize(true);
  browser.mmltkIntegrationReceipt(gallery, 'new', 7, 11);
  composition((...values) => current.push(values));
  browser.mmltkIntegrationBoundaryPixels([0, 0, 1, 1, 0], '{}', gallery, 7, 11);
  f.flushFrames();
  assert.deepEqual(old, [['invalidated', 0, 0]]);
  assert.deepEqual(current, [['observed', 1, 1]]);
  assert.equal(f.allocations.reads, 2);
  assert.equal(f.reports.filter(record => record.event === 'iced.surface.canvas_pixel').length, 1);
});

const probeCalls = [
  [gallery, callback => browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery), [0, 0, 8, 8], 7, 11, callback)],
  [gallery, callback => browser.mmltkIntegrationAtlasComposition(browser.mmltkIntegrationProbe(gallery), [0, 0, 1, 1, 64, 64, 64, 255, 4, 0], [], '{}', 7, 11, 4, callback)],
  [workspace, callback => browser.mmltkIntegrationAnnotationSwatch(browser.mmltkIntegrationProbe(workspace), [0, 0, 8, 8], [64, 64, 64], 'tool', '', false, callback)],
  [workspace, callback => browser.mmltkIntegrationAnnotationPixels(browser.mmltkIntegrationProbe(workspace), [0, 0, 8, 8], [8, 8], [4, 4, 64, 64, 64, 0, 1], 7, 11, callback)],
  [detail, callback => browser.mmltkIntegrationUpscalePixels(browser.mmltkIntegrationProbe(detail), [0, 0, 8, 8], [0, 0, 8, 8], 7, 11, callback)],
];

for (const [index, [control, sample]] of probeCalls.entries()) {
  for (const geometry of ['css', 'backing', 'canvas']) {
    test(`probe ${index}: ${geometry} replacement before a new Rust draw invalidates, then resamples`, t => {
      const f = canvasFixture(t, true), results = [];
      browser.mmltkIntegrationReceipt(control, 'physical-A', 7, 11);
      if (geometry === 'css') f.css.width += 20;
      else if (geometry === 'backing') f.canvas.width += 20;
      else document.querySelector = () => ({...f.canvas});
      sample((...values) => results.push(values));
      f.flushFrames();
      assert.deepEqual(results, [['invalidated', 0, 0]]);
      assert.equal(f.allocations.reads, 0);
      if (geometry === 'canvas') {
        const replacement = {...f.canvas};
        document.querySelector = () => replacement;
      }
      // Same Rust frame and geometry key can draw on a replaced CSS/backing canvas.
      browser.mmltkIntegrationReceipt(control, 'physical-A', 7, 11);
      sample((...values) => results.push(values));
      f.flushFrames();
      assert.equal(results[1][0], 'observed');
      assert.ok(f.allocations.reads > 0);
      browser.mmltkIntegrationResetScenario();
      f.flushFrames();
      assert.equal(results.length, 2);
    });
  }

  if (index !== 3) {
    test(`probe ${index}: geometry replacement between queued frames settles only its own work`, t => {
      const f = canvasFixture(t, true), old = [], current = [];
      browser.mmltkIntegrationReceipt(control, 'current-frame', 7, 11);
      sample((...values) => old.push(values));
      f.css.width += 20;
      browser.mmltkIntegrationReceipt(control, 'current-frame', 7, 11);
      sample((...values) => current.push(values));
      f.flushFrames();
      assert.deepEqual(old, [['invalidated', 0, 0]]);
      assert.equal(current.length, 1);
      assert.equal(current[0][0], 'observed');
      browser.mmltkIntegrationResetScenario();
      f.flushFrames();
      assert.equal(old.length, 1);
      assert.equal(current.length, 1);
    });
  }

  test(`probe ${index}: pixel reader exceptions settle as failures once`, t => {
    const f = canvasFixture(t, true), results = [];
    browser.mmltkIntegrationReceipt(control, 'current', 7, 11);
    f.sampling.error = new Error('reader unavailable');
    sample((...values) => results.push(values));
    f.flushFrames();
    assert.deepEqual(results, [['failed', 0, 0]]);
    assert.equal(f.reports.filter(record => record.event === 'integration.failure').length, 1);
    browser.mmltkIntegrationResetScenario();
    assert.equal(results.length, 1);
  });

  test(`probe ${index}: current black pixels remain measured failure evidence`, t => {
    const f = canvasFixture(t, true), results = [];
    browser.mmltkIntegrationReceipt(control, 'current', 7, 11);
    f.sampling.pixel = () => [0, 0, 0, 255];
    sample((...values) => results.push(values));
    f.flushFrames();
    assert.equal(results.length, 1);
    assert.equal(results[0][0], 'observed');
    assert.equal(results[0][2], 0);
    if (control === detail) assert.equal(results[0][1], 0, 'black Upscale image has a failed checksum');
  });
}

test('composition adapter failure is not invalidation or partial observation', t => {
  const f = canvasFixture(t, true), results = [];
  browser.mmltkIntegrationReceipt(gallery, 'current', 7, 11);
  browser.mmltkIntegrationAtlasComposition(browser.mmltkIntegrationProbe(gallery), [0, 0, 1, 1, 64, 64, 64, 255, 4, 0], [], 'invalid-json', 7, 11, 4,
    (...values) => results.push(values));
  f.flushFrames();
  assert.deepEqual(results, [['failed', 0, 0]]);
});

for (const columns of [4, 10]) {
  for (const dpi of [1, 1.5]) {
    test(`composition bounds the grid with clean neighbors: ${columns} columns at DPI ${dpi}`, t => {
      const f = canvasFixture(t, true), results = [];
      const width = 800 * dpi, cell = width / columns, y = cell / 2;
      f.canvas.width = width;
      f.canvas.height = width;
      browser.mmltkIntegrationReceipt(gallery, 'grid', 7, 11);
      const clean = [48, 80, 112, 255], black = [0, 0, 0, 255], white = [255, 255, 255, 255];
      // Independently specified raster strips: outer edges and the first interior
      // edge. Rust's actual sample builder is checked against these positions in
      // the existing native browser-app fixtures; this is not a CPU shader model.
      const strips = [
        [0, [black, white, black, clean]],
        [cell - 2, [clean, black, white, black, clean]],
        [width - 4, [clean, black, white, black]],
      ];
      const raster = new Map(), points = [];
      for (const [start, colors] of strips) {
        for (const [offset, color] of colors.entries()) {
          const x = start + offset;
          raster.set(x, color);
          points.push(x + 0.5, y, x + 0.5, y, ...color, 4, 0);
        }
      }
      f.sampling.pixel = x => raster.get(x) ?? clean;
      const sample = () => browser.mmltkIntegrationAtlasComposition(browser.mmltkIntegrationProbe(gallery), points, [], '{}', 7, 11, columns,
        (...values) => results.push(values));
      sample();
      f.flushFrames();
      assert.deepEqual(results, [['observed', 13, 13]]);
      // A wider black border preserves all nine old samples but destroys every
      // clean neighbor. This independent defect must fail the complete oracle.
      for (const [x, color] of raster) if (color === clean) raster.set(x, black);
      sample();
      f.flushFrames();
      assert.deepEqual(results[1], ['observed', 13, 9]);
      assert.equal(f.allocations.scratch, 1, 'one reusable single-pixel reader');
    });
  }
}

test('pre-location canvas requests retain their original geometry across another draw', t => {
  const f = canvasFixture(t, true), results = [];
  browser.mmltkIntegrationReceipt(workspace, 'same-rust-receipt', 7, 11);
  const original = browser.mmltkIntegrationProbe(workspace);
  f.css.x = 17;
  browser.mmltkIntegrationReceipt(workspace, 'same-rust-receipt', 7, 11);
  const current = browser.mmltkIntegrationProbe(workspace);
  const pixels = request => browser.mmltkIntegrationAnnotationPixels(request, [0, 0, 8, 8], [8, 8],
    [4, 4, 64, 64, 64, 0, 1], 7, 11, (...values) => results.push(values));
  const swatch = request => browser.mmltkIntegrationAnnotationSwatch(request, [0, 0, 8, 8], [64, 64, 64],
    'tool', '', true, (...values) => results.push(values));
  pixels(original);
  swatch(original);
  assert.deepEqual(results, [['invalidated', 0, 0], ['invalidated', 0, 0]]);
  assert.equal(f.allocations.reads, 0);
  pixels(current);
  swatch(current);
  f.flushFrames();
  assert.deepEqual(results.slice(2), [['observed', 1, 1], ['observed', 1, 1]]);
  browser.mmltkIntegrationResetScenario();
  f.flushFrames();
  assert.equal(results.length, 4);
});
