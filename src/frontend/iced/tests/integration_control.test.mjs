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
  const listeners = new Map();
  const sampling = {pixel: () => [64, 64, 64, 255], error: undefined};
  const NativeMap = Map, NativeSet = Set;
  install('Map', class extends NativeMap { constructor(...args) { super(...args); allocations.maps++; } });
  install('Set', class extends NativeSet { constructor(...args) { super(...args); allocations.sets++; } });
  install('document', {querySelector: () => canvas, activeElement: canvas, hasFocus: () => true, visibilityState: 'visible'});
  install('window', {
    devicePixelRatio: 1,
    addEventListener: (type, callback) => { allocations.listeners++; listeners.set(type, callback); },
    removeEventListener: (type) => { allocations.listeners--; listeners.delete(type); },
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
          if (sampling.data) return {data: sampling.data, width, height};
          if (sampling.raster) {
            const data = new Uint8ClampedArray(width * height * 4);
            for (let row = 0; row < height; ++row) for (let column = 0; column < width; ++column) {
              data.set(sampling.raster(x + column, y + row), (row * width + column) * 4);
            }
            return {data, width, height};
          }
          const pixel = sampling.pixel(sourceX + x, sourceY + y);
          return {data: Uint8ClampedArray.from({length: width * height * 4}, (_, i) => pixel[i % 4]), width, height};
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
    input: (type, fields = {}) => listeners.get(type)?.({type, ...fields}),
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

test('downsampled annotation outlines retain class hue at the shared viewer width', t => {
  const f = canvasFixture(t, true), results = [];
  browser.mmltkIntegrationReceipt(workspace, 'outline', 7, 11);
  const yellow = [255, 255, 0], orange = [255, 80.52631258964539, 0];
  const mask = [183.60000729560852, 82.62000024318695, 98.56424868106842];
  for (const [expected, pixel] of [
    [yellow, [156, 171, 53]], [yellow, [48, 80, 112]],
    [yellow, [156, 53, 171]], [yellow, yellow],
    [orange, [131, 80, 67]], [orange, [90, 80, 89]], [orange, [131, 67, 80]],
    [mask, [110, 81, 106]], [mask, [48, 80, 112]], [[48, 80, 112], [48, 80, 112]],
  ]) {
    f.sampling.pixel = () => [...pixel, 255];
    browser.mmltkIntegrationAnnotationPixels(browser.mmltkIntegrationProbe(workspace),
      [0, 0, 80, 40], [180, 90], [90, 45, ...expected, 24, 2], 7, 11,
      (...values) => results.push(values));
  }
  assert.deepEqual(results.map(result => result[2]), [1, 0, 0, 1, 1, 0, 0, 1, 0, 1]);
  assert.equal(f.allocations.copies, 10);
  assert.deepEqual(f.events, []);
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

test('atlas sampling finds visible image content beyond padding and rejects black tiles', t => {
  const f = canvasFixture(t, true);
  const reads = [];
  f.sampling.pixel = (x, y) => {
    reads.push([x, y]);
    assert.ok(x >= 11 && x + 8 <= 70 && y >= 21 && y + 8 <= 80);
    return y > 60 ? [48, 80, 112, 255] : [0, 0, 0, 255];
  };
  const results = [];
  const sample = () => browser.mmltkIntegrationAtlasPixels(browser.mmltkIntegrationProbe(gallery),
    [10.25, 20.25, 60, 60], [4], '{"image":[0,0,100,100],"columns":1,"card_extent":100}', 7, 11,
    (...values) => results.push(values));
  browser.mmltkIntegrationReceipt(gallery, 'clipped-padding', 7, 11);
  sample();
  f.flushFrames();
  assert.deepEqual(results, [['observed', 1, 1]]);
  const cell = f.reports.find(record => record.event === 'integration.atlas_ready_cell');
  assert.equal(cell.canvas_y, reads.at(-1)[1]);
  assert.equal(cell.cell_sample_y, (cell.canvas_y + 4.5) * 1000);
  assert.equal(cell.cell_sample_rgba, 0xff705030);
  assert.equal(f.allocations.copies, 1);
  assert.ok(reads.length > 1 && reads.length <= 9);
  f.sampling.pixel = () => [0, 0, 0, 255];
  const before = f.allocations.reads;
  sample();
  f.flushFrames();
  assert.deepEqual(results.at(-1), ['observed', 1, 0]);
  assert.equal(f.allocations.reads - before, 9);
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

test('numeric keyboard entry allows a render frame between characters and stops on cancellation', t => {
  const f = canvasFixture(t, true);
  browser.mmltkIntegrationReplaceNumber(20, 30, '9999', 20);
  f.flushMicrotasks();
  for (let i = 0; i < 3; i++) f.frames.shift()();
  const characters = () => f.keys.filter(([type, key]) => type === 'keydown' && key === '9');
  for (let count = 1; count <= 2; count++) {
    f.frames.shift()();
    assert.equal(characters().length, count);
  }
  browser.mmltkIntegrationCancelNumberEdit();
  f.flushFrames();
  assert.equal(characters().length, 2);
  assert.equal(f.reports.some(record => record.detail === 'keyboard'), false);
  browser.mmltkIntegrationReplaceNumber(20, 30, '9999', 20);
  f.flushMicrotasks();
  f.flushFrames();
  assert.equal(characters().length, 6);
  assert.equal(f.reports.filter(record => record.detail === 'keyboard').length, 1);
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

// Local draw coordinates deliberately remain below the canvas. Only the
// measured outer frame is permitted to place this action's pixel probe.
function primaryFacts(phase = 0, scale = 1) {
  return [451, 801, 200, 46, 300, 600, 640, 480, scale, phase, 0, 0.4, 1, 1];
}
function primaryBounds(x = 10, y = 100) {
  return [x, y, 202, 48, 0, 52, 640, 428, 0, 0, 640, 480];
}
function primaryCapture(f, control, label, active, facts, bounds = primaryBounds()) {
  const token = browser.mmltkIntegrationPrimaryAction(control, label, active, false, facts);
  assert.ok(token > 0);
  const outcomes = [];
  const complete = (outcome, executing) => outcomes.push([outcome, executing]);
  browser.mmltkIntegrationPrimaryActionMeasured(control, token, bounds, complete);
  f.flushFrames();
  assert.equal(outcomes[0]?.[0], 'measure');
  browser.mmltkIntegrationPrimaryActionMeasured(control, token, bounds, complete);
  return {token, outcomes};
}

// Paint a synthetic stroked rounded path at screen coordinates. This fixture
// supplies pixels, independently of the adapter's 400 perimeter sample sites.
function primaryRaster(f, phase, active = true, scale = 1, save = false) {
  const points = [], pixels = new Map(), radius = 8.5;
  for (const [cx, cy, start] of [[190,10,-Math.PI/2], [190,36,0], [10,36,Math.PI/2], [10,10,Math.PI]]) {
    for (let index = 0; index <= 64; ++index) {
      const angle = start + index * Math.PI / 128;
      points.push([cx + radius * Math.cos(angle), cy + radius * Math.sin(angle)]);
    }
  }
  const lengths = points.map((point, index) => Math.hypot(point[0]-points[(index+1)%points.length][0], point[1]-points[(index+1)%points.length][1]));
  const perimeter = lengths.reduce((total, length) => total+length, 0);
  // The closed path starts at the top-right corner, 180px after the action's
  // top-left tangent; this offset aligns the supplied component phase.
  let travelled = 180;
  for (let index = 0; index < points.length; ++index) {
    const [x,y] = points[index], [nextX,nextY] = points[(index+1)%points.length];
    const length = lengths[index], steps = Math.ceil(length * scale * 4);
    for (let step = 0; step < steps; ++step) {
      const fraction = step / steps, distance = travelled + fraction * length;
      const blue = active && ((distance/perimeter-phase)%0.1+0.1)%0.1 < 0.05;
      if (!blue) continue;
      const px = (11+x+(nextX-x)*fraction)*scale, py = (101+y+(nextY-y)*fraction)*scale;
      for (let row=Math.floor(py-1.4*scale); row<=Math.ceil(py+1.4*scale); ++row) {
        for (let column=Math.floor(px-1.4*scale); column<=Math.ceil(px+1.4*scale); ++column) {
          if (Math.hypot(column+0.5-px,row+0.5-py)<=1.4*scale) pixels.set(`${column},${row}`, [0,102,255,255]);
        }
      }
    }
    travelled += length;
  }
  const core = active && !save ? [210,30,30,255] : [20,180,40,255];
  f.sampling.raster = (x,y) => pixels.get(`${x},${y}`) ?? core;
}

test('primary action evidence measures the revealed inner frame and bounds all six idle receipts', t => {
  const f = canvasFixture(t, true);
  f.sampling.pixel = () => [20,180,40,255];
  for (const [control,label] of [
    ['train.primary','Start Training'], ['validate.primary','Start Validation'],
    ['predict.primary','Run Predict'], ['export.primary','Run Export'],
    ['live.primary','Start Live'], ['annotation.save','Save Annotations'],
  ]) {
    const {token,outcomes} = primaryCapture(f,control,label,false,primaryFacts());
    assert.equal(outcomes.at(-1)[0], 'observed');
    assert.equal(browser.mmltkIntegrationPrimaryActionCurrent(control,token), true);
    assert.deepEqual(f.sampling.lastRead, [11,101,200,46]);
    for (let index=0;index<64;++index) assert.equal(browser.mmltkIntegrationPrimaryAction(control,label,false,false,primaryFacts()),0);
  }
  assert.equal(browser.mmltkIntegrationPrimaryAction('seventh','unused',false,false,primaryFacts()),0);
  assert.equal(f.allocations.reads,6);
  assert.equal(f.allocations.scratch,1);
  // Each of the six accepted opt-in pixel records gets one report timestamp.
  assert.equal(f.allocations.clock,6);
  assert.deepEqual(f.reports.filter(report => report.event === 'integration.primary_action.pixels').map(report => report.height),[46,46,46,46,46,46]);
});

test('primary active evidence retains actual theme, ten segments, moving phases and two-capture limit', t => {
  const f = canvasFixture(t, true);
  for (const phase of [0,0.05]) {
    primaryRaster(f,phase);
    const {token,outcomes} = primaryCapture(f,'export.primary','Stop Export',true,primaryFacts(phase));
    assert.equal(outcomes.at(-1)[0], phase === 0 ? 'retry' : 'observed');
    if (phase !== 0) assert.equal(browser.mmltkIntegrationPrimaryActionCurrent('export.primary',token),true);
  }
  assert.equal(browser.mmltkIntegrationPrimaryAction('export.primary','Stop Export',true,false,primaryFacts(0.1)),0);
  assert.equal(f.allocations.reads,2);
  const reports = f.reports.filter(report => report.event === 'integration.primary_action.pixels');
  assert.equal(reports.length,2);
  assert.ok(reports.every(report => report.segments === 10 && report.band_leaks === 0));
  assert.deepEqual(reports.map(report => report.phase),[0,0.05]);
});

test('primary measurements reject clipping and changed final geometry then retry after reveal', t => {
  const f = canvasFixture(t,true);
  f.sampling.pixel = () => [20,180,40,255];
  let facts = primaryFacts();
  for (const bounds of [primaryBounds(10,470),primaryBounds(10,700),primaryBounds(-100,100)]) {
    const token = browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,facts);
    let outcome;
    browser.mmltkIntegrationPrimaryActionMeasured('train.primary',token,bounds,value => { outcome=value; });
    assert.equal(outcome,'invalidated');
  }
  const token = browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,facts);
  browser.mmltkIntegrationPrimaryActionMeasured('train.primary',token,primaryBounds(),()=>{});
  f.flushFrames();
  browser.mmltkIntegrationPrimaryActionMeasured('train.primary',token,primaryBounds(20,110),outcome => assert.equal(outcome,'invalidated'));
  assert.equal(f.allocations.reads,0);
  facts = primaryFacts(); facts[4] += 20; facts[5] += 30;
  const result = primaryCapture(f,'train.primary','Start Training',false,facts,primaryBounds(20,110));
  assert.equal(result.outcomes.at(-1)[0],'observed');
  assert.deepEqual(f.sampling.lastRead,[21,111,200,46]);
});

test('primary capture rejects stale callbacks after scroll, navigation, layout, resize and scale changes', t => {
  const f = canvasFixture(t,true);
  f.sampling.pixel = () => [20,180,40,255];
  for (const invalidate of [
    () => f.input('wheel'),
    () => browser.mmltkIntegrationPrimaryPage('navigation.explore',1),
    () => { const facts=primaryFacts(); facts[1]+=10; browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,facts); },
    () => { f.canvas.width+=10; },
    () => { f.css.width+=10; },
    () => browser.mmltkIntegrationPrimaryPage('navigation.train',1.5),
    () => { const facts=primaryFacts(0,2); browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,facts); },
    () => browser.mmltkIntegrationResetScenario(),
    () => browser.mmltkIntegrationInitialize(false),
  ]) {
    browser.mmltkIntegrationResetScenario();
    browser.mmltkIntegrationInitialize(true);
    const token=browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,primaryFacts());
    const outcomes=[];
    browser.mmltkIntegrationPrimaryActionMeasured('train.primary',token,primaryBounds(),outcome=>outcomes.push(outcome));
    invalidate();
    f.flushFrames();
    assert.deepEqual(outcomes,['invalidated']);
    assert.equal(browser.mmltkIntegrationPrimaryActionCurrent('train.primary',token),false);
  }
  assert.equal(f.allocations.reads,0);
});

test('primary observations scale measured logical coordinates exactly once and reject stale receipts', t => {
  const f=canvasFixture(t,true);
  f.canvas.width=1280; f.canvas.height=960;
  f.sampling.pixel=()=>[20,180,40,255];
  const {token,outcomes}=primaryCapture(f,'train.primary','Start Training',false,primaryFacts(0,2));
  assert.equal(outcomes.at(-1)[0],'observed');
  assert.deepEqual(f.sampling.lastRead,[22,202,400,92]);
  f.input('wheel');
  assert.equal(browser.mmltkIntegrationPrimaryActionCurrent('train.primary',token),false);
  const result=primaryCapture(f,'train.primary','Start Training',false,primaryFacts(0,2));
  assert.equal(result.outcomes.at(-1)[0],'observed');
  assert.equal(browser.mmltkIntegrationPrimaryActionCurrent('train.primary',result.token),true);
});

for (const [dpi, uiScale] of [[1,1.5], [1.25,1.5], [2,1.5]]) {
  test(`primary capture uses applied UI scale ${uiScale} and backing scale ${dpi} despite a stale renderer hint`, t => {
    const f=canvasFixture(t,true);
    f.canvas.width=640*dpi; f.canvas.height=480*dpi;
    f.sampling.pixel=()=>[20,180,40,255];
    browser.mmltkIntegrationPrimaryPage('navigation.train',uiScale);
    const {outcomes}=primaryCapture(f,'train.primary','Start Training',false,primaryFacts(0,1));
    assert.equal(outcomes.at(-1)[0],'observed');
    const scale=dpi*uiScale, left=Math.floor(11*scale), top=Math.floor(101*scale);
    assert.deepEqual(f.sampling.lastRead,[left,top,Math.ceil(211*scale)-left,Math.ceil(147*scale)-top]);
  });
}

test('primary mismatches have bounded retries and quiet reporting has no observation work', t => {
  const f=canvasFixture(t,true);
  for (let attempt=0;attempt<32;++attempt) {
    const {outcomes}=primaryCapture(f,'train.primary','Start Training',false,primaryFacts());
    assert.equal(outcomes.at(-1)[0],'retry');
  }
  assert.equal(browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,primaryFacts()),0);
  assert.equal(f.allocations.reads,32);
  browser.mmltkIntegrationInitialize(false);
  const before={...f.allocations};
  for (let index=0;index<64;++index) {
    assert.equal(browser.mmltkIntegrationPrimaryAction('train.primary','Start Training',false,false,primaryFacts()),0);
    browser.mmltkIntegrationPrimaryPage('navigation.train',1);
    assert.equal(browser.mmltkIntegrationPrimaryActionCurrent('train.primary',1),false);
  }
  assert.deepEqual(f.allocations,before);
  assert.deepEqual(f.frames,[]);
});

test('Save Annotations active perimeter evidence keeps its green save-only core', t => {
  const f=canvasFixture(t,true);
  for (const phase of [0,0.05]) {
    primaryRaster(f,phase,true,1,true);
    const result=primaryCapture(f,'annotation.save','Save Annotations',true,primaryFacts(phase));
    assert.equal(result.outcomes.at(-1)[0],phase === 0 ? 'retry' : 'observed');
  }
  const reports=f.reports.filter(report=>report.event === 'integration.primary_action.pixels');
  assert.equal(reports.length,2);
  assert.ok(reports.every(report=>report.label === 'Save Annotations' && report.core[1]>report.core[0]+60));
});

const validationWorkspace = 'validate.detail.image';
const captionPatch = [20, 20, 24, 12, 0, 255, 255, 255, 0, 0];
function captionRaster(stage, broken = false) {
  return (x, y) => {
    if (x < 20 || x >= 44 || y < 20 || y >= 32) return [48, 80, 112, 255];
    const mode = stage % 4;
    if (mode === 2) return [48, 80, 112, 255];
    const gtGlyph = x >= 24 && x < 28 && y >= 23 && y < 29;
    if (mode === 3 || (broken && mode === 0 && gtGlyph)) {
      return gtGlyph ? [0, 0, 0, 255] : [0, 255, 255, 255];
    }
    return x >= 34 && x < 37 && y >= 23 && y < 29 ? [255, 255, 255, 255] : [255, 0, 0, 255];
  };
}
function workflowCaption(fixture, stage, results, patches = captionPatch, caseIndex = 6) {
  browser.mmltkIntegrationReceipt(validationWorkspace, `caption-geometry-${stage}`, 7 + stage, 11 + stage);
  browser.mmltkIntegrationWorkflowPixels(validationWorkspace, [0, 0, 100, 80], false, false,
    7 + stage, 11 + stage, stage, caseIndex, patches, (...values) => results.push(values));
  fixture.flushFrames();
}

test('workflow caption patches compare completed layers including GT glyphs under Det backgrounds', t => {
  const f = canvasFixture(t, true), results = [];
  for (let stage = 0; stage <= 8; ++stage) {
    f.sampling.raster = captionRaster(stage);
    workflowCaption(f, stage, results);
    assert.equal(results.at(-1)[0], 'observed');
  }
  const evidence = f.reports.filter(record => record.event === 'integration.workflow.caption_pixels');
  assert.equal(evidence.length, 9);
  assert.ok(evidence[3].glyphs > 0);
  assert.equal(evidence[4].compared, 24 * 12);
  assert.equal(evidence[8].verified, 1);
  assert.equal(f.allocations.copies, 9, 'caption and ordinary workflow pixels share one canvas snapshot');
  assert.equal(f.reports.some(record => record.event === 'integration.annotation_swatch'), false);
});

test('workflow caption oracle rejects GT glyphs surviving the Det background', t => {
  const f = canvasFixture(t, true), results = [];
  for (let stage = 0; stage <= 4; ++stage) {
    f.sampling.raster = captionRaster(stage, stage === 4);
    workflowCaption(f, stage, results);
  }
  assert.equal(results.at(-1)[0], 'failed');
  assert.ok(f.reports.some(record => record.detail?.includes('completely cover ground-truth')));
});

test('workflow caption probes keep invalidation and disabled execution independent of evidence', t => {
  const f = canvasFixture(t, true), results = [];
  browser.mmltkIntegrationReceipt(validationWorkspace, 'before', 7, 11);
  browser.mmltkIntegrationWorkflowPixels(validationWorkspace, [0, 0, 100, 80], false, false,
    7, 11, 1, 6, captionPatch, (...values) => results.push(values));
  browser.mmltkIntegrationReceipt(validationWorkspace, 'after', 8, 12);
  f.flushFrames();
  assert.deepEqual(results, [['invalidated', 0, 0]]);
  assert.equal(f.allocations.reads, 0);
  browser.mmltkIntegrationInitialize(false);
  browser.mmltkIntegrationWorkflowPixels(validationWorkspace, [0, 0, 100, 80], false, false,
    8, 12, 1, 6, { [Symbol.iterator]() { throw new Error('disabled caption collection'); } },
    (...values) => results.push(values));
  assert.equal(f.allocations.reads, 0);
  assert.equal(f.frames.length, 0);
});

test('workflow caption probes reject malformed bounds, absent RGB, and missing overlap proof', t => {
  const f = canvasFixture(t, true), results = [];
  for (const patches of [captionPatch.slice(1), [...captionPatch, ...new Array(80).fill(0)],
    [20, 20, 513, 12, 0, 255, 255, 255, 0, 0],
    [20, 20, 24, 12, 0, 256, 255, 255, 0, 0],
    [-10, 20, 24, 12, 0, 255, 255, 255, 0, 0]]) {
    workflowCaption(f, 1, results, patches);
    assert.equal(results.at(-1)[0], 'failed');
  }
  f.sampling.pixel = () => [64, 64, 64, 255];
  workflowCaption(f, 1, results);
  assert.equal(results.at(-1)[0], 'failed', 'explicit RGB must be observed');
  workflowCaption(f, 8, results, []);
  assert.equal(results.at(-1)[0], 'failed', 'a compiled oracle without overlap is not acceptance');
});


test('workflow caption baselines cannot prove a replacement geometry or survive canvas failure', t => {
  const f = canvasFixture(t, true), results = [];
  for (let stage = 1; stage <= 3; ++stage) {
    f.sampling.raster = captionRaster(stage);
    workflowCaption(f, stage, results);
  }
  f.sampling.raster = captionRaster(8);
  workflowCaption(f, 8, results, [21, ...captionPatch.slice(1)]);
  assert.equal(results.at(-1)[0], 'failed', 'new geometry cannot inherit a layer comparison');
  f.sampling.error = new Error('canvas unavailable');
  const before = results.length;
  workflowCaption(f, 1, results);
  assert.deepEqual(results.at(-1), ['failed', 0, 0]);
  browser.mmltkIntegrationResetScenario();
  f.flushFrames();
  assert.equal(results.length, before + 1, 'failure completes its exact request once');
});


test('workflow captions sample explicit backing pixels without a second CSS scale', t => {
  const f = canvasFixture(t, true), results = [];
  f.css.width = 320;
  f.css.height = 240;
  const reads = [];
  f.sampling.raster = (x, y) => { reads.push([x, y]); return captionRaster(1)(x, y); };
  workflowCaption(f, 1, results);
  assert.equal(results.at(-1)[0], 'observed');
  assert.deepEqual(reads[0], [20, 20]);
  assert.deepEqual(reads[24 * 12 - 1], [43, 31]);
  assert.deepEqual(reads[24 * 12], [30, 20], 'ordinary workflow bounds still use CSS conversion');
  assert.equal(f.allocations.copies, 1);
});

test('workflow overlap proof cannot borrow Det background from another patch', t => {
  const f = canvasFixture(t, true), results = [];
  const patches = [...captionPatch, 60, ...captionPatch.slice(1)];
  for (let stage = 1; stage <= 4; ++stage) {
    f.sampling.raster = (x, y) => {
      if (x >= 20 && x < 44 && y >= 20 && y < 32) {
        // A has the Det background and a GT background, but no GT glyph.
        return stage === 2 ? [48, 80, 112, 255] :
          (stage === 3 ? [0, 255, 255, 255] : [255, 0, 0, 255]);
      }
      if (x >= 60 && x < 84 && y >= 20 && y < 32) {
        // B has GT glyphs, but neither Det-only nor combined contains Det.
        return stage === 3 ? captionRaster(3)(x - 40, y) : [48, 80, 112, 255];
      }
      return [48, 80, 112, 255];
    };
    workflowCaption(f, stage, results, patches);
    if (results.at(-1)[0] === 'failed') break;
  }
  assert.equal(results.at(-1)[0], 'failed');
  assert.ok(f.reports.some(record => record.detail?.includes('expected colors')));
});

for (const leak of ['gt-background', 'det-background', 'gt-glyph', 'det-glyph']) {
  test(`workflow hidden state rejects partial ${leak} leakage in its own patch`, t => {
    const f = canvasFixture(t, true), results = [];
    for (let stage = 1; stage <= 3; ++stage) {
      f.sampling.raster = (x, y) => {
        if (stage === 2) {
          if (leak === 'gt-background' && x === 22 && y === 22) return [0, 255, 255, 255];
          if (leak === 'det-background' && x === 22 && y === 22) return [255, 0, 0, 255];
          if (leak === 'gt-glyph' && x === 24 && y === 23) return [0, 0, 0, 255];
          if (leak === 'det-glyph' && x === 34 && y === 23) return [255, 255, 255, 255];
        }
        return captionRaster(stage)(x, y);
      };
      workflowCaption(f, stage, results);
      if (results.at(-1)[0] === 'failed') break;
    }
    assert.equal(results.at(-1)[0], 'failed');
    assert.ok(f.reports.some(record => record.detail?.includes('hidden validation captions retained')));
  });
}

for (const stage of [1, 3]) {
  test(`workflow single layer ${stage} rejects the other layer's partial caption`, t => {
    const f = canvasFixture(t, true), results = [];
    f.sampling.raster = (x, y) => x === 22 && y === 22 ?
      (stage === 1 ? [0, 255, 255, 255] : [255, 0, 0, 255]) : captionRaster(stage)(x, y);
    workflowCaption(f, stage, results);
    assert.equal(results.at(-1)[0], 'failed');
  });
}
