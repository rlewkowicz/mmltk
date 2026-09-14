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
  const microtasks = [], frames = [], events = [], keys = [], reports = [], lines = [];
  const allocations = {maps: 0, sets: 0, scratch: 0, reads: 0, copies: 0, listeners: 0, clock: 0};
  const css = {x: 0, y: 0, left: 0, top: 0, width: 640, height: 480};
  const canvas = {
    width: 640, height: 480,
    style: {
      values: new Map(),
      getPropertyValue(name) { return this.values.get(name)?.[0] ?? ''; },
      getPropertyPriority(name) { return this.values.get(name)?.[1] ?? ''; },
      setProperty(name, value, priority = '') { this.values.set(name, [value, priority]); },
      removeProperty(name) { this.values.delete(name); },
    },
    getBoundingClientRect: () => css,
    dispatchEvent: event => {
      events.push(event.type);
      if (event.key) keys.push([event.type, event.key, event.ctrlKey]);
      return true;
    },
  };
  const sampling = {pixel: () => [64, 64, 64, 255], error: undefined};
  const NativeMap = Map, NativeSet = Set;
  install('Map', class extends NativeMap { constructor(...args) { super(...args); allocations.maps++; } });
  install('Set', class extends NativeSet { constructor(...args) { super(...args); allocations.sets++; } });
  install('document', {querySelector: () => canvas, activeElement: canvas, hasFocus: () => true, visibilityState: 'visible'});
  install('window', {
    devicePixelRatio: 1,
    addEventListener: () => { allocations.listeners++; },
    removeEventListener: () => { allocations.listeners--; },
  });
  install('performance', {now: () => { allocations.clock++; return 0; }});
  install('dump', line => { lines.push(line); reports.push(JSON.parse(line)); });
  install('PointerEvent', class { constructor(type, fields) { this.type = type; Object.assign(this, fields); } });
  install('KeyboardEvent', class { constructor(type, fields) { this.type = type; Object.assign(this, fields); } });
  install('queueMicrotask', callback => microtasks.push(callback));
  install('requestAnimationFrame', callback => frames.push(callback));
  install('OffscreenCanvas', class {
    constructor(width, height) { this.width = width; this.height = height; allocations.scratch++; }
    getContext() {
      let sourceX = 0, sourceY = 0;
      return {
        clearRect() {}, drawImage(source, x, y) {
          if (source === canvas) allocations.copies++;
          sourceX = x; sourceY = y;
        },
        getImageData(x, y, width, height) {
          allocations.reads++;
          sampling.lastRead = [x, y, width, height];
          if (sampling.error) throw sampling.error;
          if (sampling.data) return {data: sampling.data};
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
  return {canvas, css, sampling, allocations, events, keys, reports, lines, frames, microtasks,
    flushMicrotasks: () => drain(microtasks), flushFrames: () => drain(frames)};
}

function assertQuiet(fixture) {
  assert.deepEqual(fixture.allocations, {maps: 0, sets: 0, scratch: 0, reads: 0, copies: 0, listeners: 0, clock: 0});
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

test('quiet pressure gesture preserves its ordered terminal edge without diagnostic collection', t => {
  const f = canvasFixture(t);
  for (const steps of [0, 257, Infinity, 1.5]) {
    assert.equal(browser.mmltkIntegrationAnnotationPointer(0, 0, 100, 100, .1, .1, .9, .9, false, steps), 0);
  }
  assert.equal(browser.mmltkIntegrationAnnotationPointer(0, 0, 100, 100, .1, .1, .9, .9, false, 160), 1);
  f.flushMicrotasks();
  assert.equal(f.events.length, 163);
  assert.deepEqual(f.events.slice(0, 2), ['pointermove', 'pointerdown']);
  assert.ok(f.events.slice(2, -1).every(event => event === 'pointermove'));
  assert.equal(f.events.at(-1), 'pointerup');
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
  browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery), [0, 0, 8, 8], [0], '{}', 7, 11, completed);
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
  const pixels = () => browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery), [0, 0, 8, 8], [0], '{}', 7, 11, (...values) => results.push(values));
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
  [gallery, callback => browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery), [0, 0, 8, 8], [0], '{}', 7, 11, callback)],
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
      // Independently specified raster strips: left/top outer edges and the first interior
      // edge. Rust's actual sample builder is checked against these positions in
      // the existing native browser-app fixtures; this is not a CPU shader model.
      const strips = [
        [0, false, [black, white, black, clean]],
        [cell - 2, false, [clean, black, white, black, clean]],
        [0, true, [black, white, black, clean]],
      ];
      const raster = new Map(), points = [];
      for (const [start, horizontal, colors] of strips) {
        for (const [offset, color] of colors.entries()) {
          const screenX = horizontal ? cell * 1.5 : start + offset + 0.5;
          const screenY = horizontal ? start + offset + 0.5 : y;
          raster.set(`${Math.floor(screenX)},${Math.floor(screenY)}`, color);
          points.push(screenX, screenY, screenX, screenY, ...color, 4, 0);
        }
      }
      f.sampling.pixel = (x, y) => raster.get(`${x},${y}`) ?? clean;
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
      assert.equal(f.allocations.scratch, 1, 'one reusable canvas snapshot');
      assert.equal(f.allocations.copies, 2, 'one WebGPU readback per complete probe batch');
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

test('held placeholder hover uses the exact draw without selecting it', t => {
  const f = canvasFixture(t);
  assert.equal(browser.mmltkIntegrationHoverAfterSurfaceDraw(10, 20, gallery, 7), 1);
  browser.mmltkIntegrationDriverDraw(gallery, 6, 10);
  f.flushMicrotasks();
  assert.deepEqual(f.events, []);
  browser.mmltkIntegrationDriverDraw(gallery, 7, 11);
  f.flushMicrotasks();
  assert.deepEqual(f.events, ['pointermove']);
  assertQuiet(f);
});

test('partial atlas sampling reports each exact ready card and reuses canvas storage', t => {
  const f = canvasFixture(t, true);
  const results = [];
  browser.mmltkIntegrationReceipt(gallery, 'partial', 7, 11);
  browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery),
    [0, 0, 8, 8, 10, 10, 4, 4], [3, 9],
    '{"presentation_revision":11,"dataset_identity":10848950138688527399}', 7, 11,
    (...values) => results.push(values));
  f.flushFrames();
  assert.deepEqual(results, [['observed', 2, 2]]);
  const cells = f.reports.filter(record => record.event === 'integration.atlas_ready_cell');
  assert.deepEqual(cells.map(record => record.compiled_index), [3, 9]);
  assert.ok(cells.every(record => record.matched && record.presentation_revision === 11));
  const cellLines = f.lines.filter(line => line.includes('"event":"integration.atlas_ready_cell"'));
  assert.equal(cellLines.length, 2);
  assert.ok(cellLines.every(line => line.includes('"dataset_identity":10848950138688527399')));
  assert.equal(f.allocations.scratch, 1);
  assert.equal(f.allocations.reads, 2);
  assert.deepEqual(f.events, []);
});

const fpsValues = [20, 6, 74, 22, 0, 0, 100, 60, 30, .5, 0];
function fpsCapture(results, values = fpsValues, scale = 1) {
  browser.mmltkIntegrationFpsDraw(gallery, values);
  browser.mmltkIntegrationFpsPixels(browser.mmltkIntegrationProbe(gallery), values, scale,
    (...result) => results.push(result));
}

test('FPS product activation without diagnostics collects no geometry or canvas pixels', t => {
  const f = canvasFixture(t, false, false);
  browser.mmltkIntegrationFpsDraw(gallery, fpsValues);
  assert.deepEqual(f.frames, []);
  assertQuiet(f);
});

for (const [dpi, uiScale] of [[1, 1], [1.25, 1.5], [1.5, 1.25], [2, 1.5]]) {
  test(`FPS capture uses actual canvas backing/CSS dimensions at DPI ${dpi} and UI scale ${uiScale}`, t => {
    const f = canvasFixture(t, true);
    f.canvas.width = 640 * dpi; f.canvas.height = 480 * dpi;
    // Deliberately disagree: actual canvas dimensions, not ambient devicePixelRatio, own conversion.
    window.devicePixelRatio = 3;
    browser.mmltkIntegrationReceipt(gallery, 'fps', 7, 11);
    const result = [];
    fpsCapture(result, fpsValues, uiScale);
    assert.equal(f.allocations.reads, 0);
    assert.equal(f.frames.length, 1);
    f.flushFrames();
    const scale = dpi * uiScale;
    const left = Math.ceil(20 * scale), top = Math.ceil(6 * scale);
    const width = Math.floor(94 * scale) - left, height = Math.floor(28 * scale) - top;
    assert.deepEqual(f.sampling.lastRead, [left, top, width, height]);
    assert.equal(result.length, 1);
    assert.equal(result[0][0], 'observed');
    assert.deepEqual(result[0][1], [width, height]);
    assert.ok(result[0][2] instanceof Uint8ClampedArray);
    assert.equal(result[0][2].length, width * height * 4);
    assert.equal(f.allocations.copies, 1);
    assert.equal(f.allocations.reads, 1);
    assert.equal(f.allocations.scratch, 1);
    fpsCapture([], fpsValues, uiScale);
    f.flushFrames();
    assert.equal(f.allocations.scratch, 1, 'scenario scratch storage is reused');
  });
}

for (const change of ['css', 'backing', 'canvas', 'receipt', 'counter', 'sample', 'scenario']) {
  test(`FPS ${change} invalidation discards old capture and rearms current displayed evidence`, t => {
    const f = canvasFixture(t, true);
    browser.mmltkIntegrationReceipt(gallery, 'fps', 7, 11);
    const old = [], current = [];
    fpsCapture(old);
    const values = fpsValues.slice();
    if (change === 'css') f.css.width += 1;
    if (change === 'backing') f.canvas.width += 1;
    if (change === 'canvas') {
      const replacement = {...f.canvas};
      document.querySelector = () => replacement;
    }
    if (change === 'counter') values[0] += .5;
    if (change === 'sample') values[8] += 1;
    if (change === 'scenario') browser.mmltkIntegrationResetScenario();
    browser.mmltkIntegrationReceipt(gallery, change === 'receipt' ? 'new' : 'fps', 7, 11);
    browser.mmltkIntegrationFpsDraw(gallery, values);
    f.flushFrames();
    assert.deepEqual(old, [['invalidated', 0, 0]]);
    assert.equal(f.allocations.reads, 0);
    fpsCapture(current, values);
    f.flushFrames();
    assert.equal(current.length, 1);
    assert.equal(current[0][0], 'observed');
  });
}

test('FPS replacement on the same image has distinct ownership and obsolete finally work is inert', t => {
  const f = canvasFixture(t, true);
  browser.mmltkIntegrationReceipt(gallery, 'same', 7, 11);
  const old = [], current = [];
  fpsCapture(old);
  fpsCapture(current);
  assert.deepEqual(old, [['invalidated', 0, 0]]);
  f.frames.shift()();
  assert.deepEqual(old, [['invalidated', 0, 0]]);
  f.flushFrames();
  assert.equal(current.length, 1);
  assert.equal(current[0][0], 'observed');
  assert.equal(f.allocations.reads, 1);
});

for (const stop of ['reset', 'diagnostics', 'driver']) {
  test(`FPS ${stop} settles a queued once callback and leaves subsequent work inert`, t => {
    const f = canvasFixture(t, true);
    browser.mmltkIntegrationReceipt(gallery, 'fps', 7, 11);
    const result = [];
    fpsCapture(result);
    if (stop === 'reset') browser.mmltkIntegrationResetScenario();
    if (stop === 'diagnostics') browser.mmltkIntegrationInitialize(false);
    if (stop === 'driver') browser.mmltkIntegrationDriver(false);
    assert.deepEqual(result, [[stop === 'driver' ? 'failed' : 'invalidated', 0, 0]]);
    f.flushFrames();
    assert.equal(result.length, 1);
    assert.equal(f.allocations.reads, 0);
  });
}

for (const invalid of ['missing', 'nonfinite', 'scale', 'clipped', 'outside', 'bytes', 'exception']) {
  test(`FPS ${invalid} capture fails once without declaring pixel evidence`, t => {
    const f = canvasFixture(t, true);
    browser.mmltkIntegrationReceipt(gallery, 'fps', 7, 11);
    const values = fpsValues.slice(), result = [];
    if (invalid === 'missing') values.pop();
    if (invalid === 'nonfinite') values[0] = Infinity;
    if (invalid === 'clipped') values[6] = 80;
    if (invalid === 'outside') { values[0] = 700; values[6] = 800; }
    if (invalid === 'bytes') f.sampling.data = new Uint8ClampedArray(3);
    if (invalid === 'exception') f.sampling.error = new Error('canvas read unavailable');
    fpsCapture(result, values, invalid === 'scale' ? NaN : 1);
    f.flushFrames();
    assert.deepEqual(result, [['failed', 0, 0]]);
    browser.mmltkIntegrationResetScenario();
    assert.equal(result.length, 1);
  });
}


test('FPS capture freezes caller data and later canvas changes invalidate the completed receipt', t => {
  const f = canvasFixture(t, true);
  browser.mmltkIntegrationReceipt(gallery, 'fps', 7, 11);
  const values = fpsValues.slice(), results = [];
  browser.mmltkIntegrationFpsDraw(gallery, values);
  const receipt = browser.mmltkIntegrationProbe(gallery);
  browser.mmltkIntegrationFpsPixels(receipt, values, 1, (...result) => results.push(result));
  values[0] = 500;
  f.flushFrames();
  assert.equal(results[0][0], 'observed');
  assert.deepEqual(f.sampling.lastRead, [20, 6, 74, 22]);
  assert.equal(browser.mmltkIntegrationFpsCurrent(receipt, fpsValues), true);
  f.css.width += 1;
  assert.equal(browser.mmltkIntegrationFpsCurrent(receipt, fpsValues), false);
});

for (const finish of ['completion', 'reset', 'disable']) {
  test(`temporary canvas layout restores responsive CSS on ${finish}`, t => {
    const f = canvasFixture(t);
    f.canvas.style.setProperty('width', '100%', 'important');
    f.canvas.style.setProperty('height', '100vh');
    assert.equal(browser.mmltkIntegrationCanvasSize(1500, 600), true);
    assert.equal(browser.mmltkIntegrationCanvasSize(1000, 1020), true);
    assert.equal(f.canvas.style.getPropertyValue('width'), '1000px');
    assert.equal(browser.mmltkIntegrationCanvasSizeSettled(1000, 1020), false);
    f.css.width = f.canvas.width = 1000;
    f.css.height = f.canvas.height = 1020;
    assert.equal(browser.mmltkIntegrationCanvasSizeSettled(1000, 1020), true);
    if (finish === 'reset') browser.mmltkIntegrationResetScenario();
    else if (finish === 'disable') browser.mmltkIntegrationDriver(false);
    else browser.mmltkIntegrationRestoreCanvasSize();
    assert.equal(f.canvas.style.getPropertyValue('width'), '100%');
    assert.equal(f.canvas.style.getPropertyPriority('width'), 'important');
    assert.equal(f.canvas.style.getPropertyValue('height'), '100vh');
    assert.equal(f.canvas.style.getPropertyPriority('height'), '');
  });
}

test('inactive canvas sizing has no layout or scheduling effects', t => {
  const f = canvasFixture(t, false, false);
  assert.equal(browser.mmltkIntegrationCanvasSize(1500, 600), false);
  assert.equal(browser.mmltkIntegrationCanvasSizeSettled(1500, 600), false);
  browser.mmltkIntegrationRestoreCanvasSize();
  assert.equal(f.canvas.style.values.size, 0);
  assert.deepEqual(f.frames, []);
  assert.deepEqual(f.events, []);
  assertQuiet(f);
});

test('canvas sizing restores stylesheet-owned dimensions and rejects invalid requests', t => {
  const f = canvasFixture(t);
  assert.equal(browser.mmltkIntegrationCanvasSize(1500, 600), true);
  assert.equal(browser.mmltkIntegrationCanvasSize(NaN, 1000), false);
  assert.equal(f.canvas.style.getPropertyValue('width'), '1500px');
  browser.mmltkIntegrationRestoreCanvasSize();
  assert.equal(f.canvas.style.values.size, 0);
});

test('rendered numeric paste focuses and selects before one ordinary paste shortcut', t => {
  const f = canvasFixture(t, true), completed = [];
  assert.equal(browser.mmltkIntegrationPasteNumber(20, 30, result => completed.push(result)), 1);
  f.flushMicrotasks();
  f.flushFrames();
  assert.deepEqual(completed, [true]);
  assert.deepEqual(f.events.slice(0, 3), ['pointermove', 'pointerdown', 'pointerup']);
  assert.deepEqual(f.keys, [
    ['keydown', 'Control', true], ['keydown', 'a', true], ['keyup', 'a', true], ['keyup', 'Control', false],
    ['keydown', 'Control', true], ['keydown', 'v', true], ['keyup', 'v', true], ['keyup', 'Control', false],
  ]);
  assert.deepEqual(f.reports.filter(record => record.event === 'integration.number_paste').map(record => record.detail),
    ['scheduled', 'focused', 'delivered']);
  browser.mmltkIntegrationCancelNumberEdit();
  f.flushFrames();
  assert.deepEqual(completed, [true]);
  assert.equal(f.canvas.width, 640);
  assert.equal(f.canvas.height, 480);
  assert.equal(f.allocations.reads + f.allocations.copies, 0);
});

for (const finish of ['cancel', 'reset', 'disable']) {
  for (const begun of [false, true]) {
    test(`numeric paste settles once on ${finish} with begun=${begun}`, t => {
      const f = canvasFixture(t), completed = [];
      browser.mmltkIntegrationPasteNumber(20, 30, result => completed.push(result));
      if (begun) { f.flushMicrotasks(); f.frames.shift()(); }
      if (finish === 'reset') browser.mmltkIntegrationResetScenario();
      else if (finish === 'disable') browser.mmltkIntegrationDriver(false);
      else browser.mmltkIntegrationCancelNumberEdit();
      const count = f.keys.length;
      f.flushMicrotasks();
      f.flushFrames();
      assert.deepEqual(completed, [false]);
      assert.equal(f.keys.length, count, 'late callbacks dispatch no paste or characters');
    });
  }
}

test('inactive and invalid numeric paste complete without scheduling input', t => {
  const f = canvasFixture(t, false, false), completed = [];
  assert.equal(browser.mmltkIntegrationPasteNumber(20, 30, result => completed.push(result)), 0);
  browser.mmltkIntegrationDriver(true);
  assert.equal(browser.mmltkIntegrationPasteNumber(NaN, 30, result => completed.push(result)), 0);
  assert.deepEqual(completed, [false, false]);
  assert.deepEqual(f.frames, []);
  assert.deepEqual(f.microtasks, []);
  assert.deepEqual(f.events, []);
});

test('superseded numeric delivery settles the old request and dispatch failure settles the current request', t => {
  const f = canvasFixture(t), old = [], current = [];
  browser.mmltkIntegrationPasteNumber(20, 30, result => old.push(result));
  browser.mmltkIntegrationPasteNumber(20, 30, result => current.push(result));
  assert.deepEqual(old, [false]);
  f.canvas.dispatchEvent = () => { throw new Error('detached canvas'); };
  f.flushMicrotasks();
  f.flushFrames();
  assert.deepEqual(current, [false]);
  browser.mmltkIntegrationCancelNumberEdit();
  assert.deepEqual(current, [false]);
});
