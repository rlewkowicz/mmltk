let integrationState;
let integrationDriver;

export function mmltkIntegrationDriver(enabled) {
  if (!enabled) { integrationDriver = undefined; return; }
  integrationDriver ??= {integrationFullscreenSettled:false, integrationAnnotationPointerEnd:null,
    pendingSurfaceClick:undefined, readySurface:undefined};
}

export function mmltkIntegrationInitialize(enabled) {
  if (!enabled) {
    if (integrationState) {
      for (const type of integrationState.inputTypes) {
        window.removeEventListener(type, integrationState.onInput, true);
      }
      const retired = integrationState;
      integrationState = undefined;
      for (const complete of retired.completions) complete('invalidated', 0, 0);
    }
    return;
  }
  if (integrationState) return;
  integrationState = {
    integrationSurfaceDraws: new Map(),
    completions: new Set(),
    initialAtlasWithoutInput: false,
    initialAtlasInputCount: 0,
    initialAtlasCompleted: false,
    boundaryPending: false,
    boundaryLatest: undefined,
    compositionPending: false,
    canvasScratch: undefined,
    canvasContext: undefined,
    integrationRenderKey: 0,
    inputTypes: ['pointermove', 'pointerdown', 'pointerup', 'wheel', 'focus', 'keydown'],
    onInput: undefined,
  };
  const owner = integrationState;
  integrationState.onInput = (event) => {
    if (integrationState !== owner) return;
    if (!integrationState.initialAtlasWithoutInput) return;
    integrationState.initialAtlasInputCount++;
    report({event: 'integration.failure', control: 'explore.gallery.workspace',
      detail: `input during initial atlas completion: ${event.type}`,
      a: String(integrationState.initialAtlasInputCount), b: '0', c: '0', d: '0'});
  };
  for (const type of integrationState.inputTypes) {
    window.addEventListener(type, integrationState.onInput, true);
  }
}

// Every asynchronous entry captures its scheduling owner. Reset settles all
// once callbacks while their bindgen storage is still alive; queued work then
// becomes inert, including nested frames and finally blocks.
function integrationFrame(callback) {
  const owner = integrationDriver;
  if (owner) requestAnimationFrame(() => {
    if (integrationDriver === owner) callback();
  });
}
function integrationMicrotask(callback) {
  const owner = integrationDriver;
  if (owner) queueMicrotask(() => {
    if (integrationDriver === owner) callback();
  });
}
function integrationCompletion(callback) {
  const owner = integrationState;
  let pending = true;
  const complete = (outcome, first = 0, second = 0) => {
    if (!pending) return;
    pending = false;
    owner?.completions.delete(complete);
    callback(outcome, first, second);
  };
  if (owner) owner.completions.add(complete);
  else complete('invalidated', 0, 0);
  return complete;
}
export function mmltkIntegrationResetScenario() {
  const enabled = !!integrationState, driver = !!integrationDriver;
  mmltkIntegrationInitialize(false);
  mmltkIntegrationDriver(false);
  mmltkIntegrationDriver(driver);
  mmltkIntegrationInitialize(enabled);
}
function canvasGeometry() {
  const canvas = document.querySelector('canvas'), css = canvas?.getBoundingClientRect();
  return {canvas, width: canvas?.width, height: canvas?.height,
    css: css && [css.x, css.y, css.width, css.height]};
}
function sameGeometry(left, right) {
  return !!left.canvas && left.canvas === right.canvas && left.width === right.width &&
    left.height === right.height && left.css.every((value, index) => value === right.css[index]);
}
export function mmltkIntegrationReceipt(control, key, sourceRevision, presentationRevision) {
  if (!integrationState) return;
  const previous = integrationState.integrationSurfaceDraws.get(control), geometry = canvasGeometry();
  if (previous?.key !== key || !sameGeometry(previous.geometry, geometry)) {
    integrationState.integrationSurfaceDraws.set(control, {key, sourceRevision, presentationRevision, geometry});
  }
}
export function mmltkIntegrationProbe(control) {
  if (!integrationState) return undefined;
  return {owner: integrationState, control, drawn: integrationState?.integrationSurfaceDraws.get(control)};
}
function probeCurrent(receipt) {
  return !!receipt?.owner && !!receipt.drawn && integrationState === receipt.owner &&
    integrationState.integrationSurfaceDraws.get(receipt.control) === receipt.drawn &&
    sameGeometry(receipt.drawn.geometry, canvasGeometry());
}

function integrationPointer(rect, x, y, type, buttons) {
  const event = new PointerEvent(type, {
    bubbles: true,
    cancelable: true,
    composed: true,
    clientX: rect.left + x,
    clientY: rect.top + y,
    pointerId: 1,
    pointerType: 'mouse',
    isPrimary: true,
    button: type === 'pointerdown' || type === 'pointerup' ? 0 : -1,
    buttons,
  });
  Object.defineProperties(event, {
    offsetX: {value: x},
    offsetY: {value: y},
    getCoalescedEvents: {value: () => [event]},
  });
  return event;
}

function matchesIntegrationSurfaceClick(pending, drawn) {
  return pending && drawn && pending.control === drawn.control &&
    (pending.allowNewer ? drawn.sourceRevision >= pending.sourceRevision :
      drawn.sourceRevision === pending.sourceRevision);
}

function dispatchIntegrationSurfaceClick() {
  const owner = integrationDriver;
  const pending = owner?.pendingSurfaceClick, drawn = owner?.readySurface;
  if (!matchesIntegrationSurfaceClick(pending, drawn) || pending.queuedDraw === drawn) return;
  pending.queuedDraw = drawn;
  integrationMicrotask(() => {
    if (owner.pendingSurfaceClick !== pending || owner.readySurface !== drawn) return;
    owner.pendingSurfaceClick = undefined;
    const canvas = document.querySelector('canvas');
    if (!canvas) return;
    if (integrationState) report({
      event: pending.hover ? 'integration.surface_hover_dispatched' : 'integration.surface_click_dispatched',
      control: pending.control,
      detail: 'real-canvas-pointer',
      a: String(drawn.sourceRevision),
      b: String(drawn.presentationRevision),
      c: String(pending.x),
      d: String(pending.y),
    });
    if (pending.hover) canvas.dispatchEvent(integrationPointer(canvas.getBoundingClientRect(), pending.x, pending.y, 'pointermove', 0));
    else integrationClick(canvas, canvas.getBoundingClientRect(), pending.x, pending.y);
  });
}

export function mmltkIntegrationDriverDraw(control, sourceRevision, presentationRevision) {
  const owner = integrationDriver;
  if (!owner) return;
  const previous = owner.readySurface;
  if (previous?.control !== control || previous.sourceRevision !== sourceRevision ||
      previous.presentationRevision !== presentationRevision) {
    owner.readySurface = {control, sourceRevision, presentationRevision};
  }
  dispatchIntegrationSurfaceClick();
}

function report(record) {
  if (!integrationState) return;
  record.elapsed_ms = performance.now();
  if (!integrationState.initialAtlasCompleted && record.event === 'integration.explore_open_submission' && record.detail === 'submitted') {
    integrationState.initialAtlasWithoutInput = true;
    integrationState.initialAtlasInputCount = 0;
  } else if (record.event === 'integration.initial_atlas_complete') {
    integrationState.initialAtlasWithoutInput = false;
    integrationState.initialAtlasCompleted = true;
  }
  const line = JSON.stringify(record);
  if (typeof globalThis.dump === 'function') globalThis.dump(`${line}\n`);
  else console.error(line);
}

export function mmltkIntegrationReport(event, control, detail, a, b, c, d) {
  if (!integrationState) return;
  report({event, control, detail, a: String(a), b: String(b), c: String(c), d: String(d)});
}

export function mmltkIntegrationAtlasPixels(receipt, rectangles, cards, fields, sourceRevision, presentationRevision, completed) {
  completed = integrationCompletion(completed);
  if (!integrationState) return;
  if (!probeCurrent(receipt)) { completed('invalidated'); return; }
  rectangles = rectangles.slice();
  cards = Array.from(cards);
  // The draw report is emitted while encoding Iced's current submission.
  // Sample the actual canvas at its next presentation opportunity, without
  // dispatching input, scheduling an Iced redraw, or introducing a timer.
  integrationFrame(() => {
    try {
      if (!probeCurrent(receipt)) { completed('invalidated'); return; }
      const canvas = document.querySelector('canvas');
      if (!canvas) throw new Error('missing WebGPU canvas');
      const drawn = integrationState.integrationSurfaceDraws.get('explore.gallery.workspace');
      if (!drawn || drawn.sourceRevision !== sourceRevision ||
          drawn.presentationRevision !== presentationRevision) {
        completed('invalidated');
        return;
      }
      if (integrationState.initialAtlasInputCount !== 0) { completed('failed'); return; }
      const context = canvasSnapshot(canvas);
      const identity = JSON.parse(fields);
      if (cards.length * 4 !== rectangles.length) throw new Error('ready-cell identity count');
      let nonblack = 0;
      for (let i = 0; i < rectangles.length; i += 4) {
        const width = Math.min(8, Math.floor(rectangles[i + 2]));
        const height = Math.min(8, Math.floor(rectangles[i + 3]));
        const x = Math.floor(rectangles[i] + (rectangles[i + 2] - width) / 2);
        const y = Math.floor(rectangles[i + 1] + (rectangles[i + 3] - height) / 2);
        const pixels = context.getImageData(x, y, width, height).data;
        let colored = 0;
        for (let p = 0; p < pixels.length; p += 4) {
          if (pixels[p + 3] > 0 && Math.max(pixels[p], pixels[p + 1], pixels[p + 2]) > 8) {
            colored++;
          }
        }
        const matched = colored * 2 >= width * height;
        nonblack += Number(matched);
        const center = (Math.floor(height / 2) * width + Math.floor(width / 2)) * 4;
        const rgba = (pixels[center] | pixels[center + 1] << 8 | pixels[center + 2] << 16 | pixels[center + 3] << 24) >>> 0;
        const side = identity.image?.[2] / identity.columns;
        const sample = Number.isFinite(side) && side > 0 ? {
          cell_sample_x: Math.round(((x + Math.floor(width / 2) + .5 - identity.image[0]) % side) / side * identity.card_extent * 1000),
          cell_sample_y: Math.round(((y + Math.floor(height / 2) + .5 - identity.image[1]) % side) / side * identity.card_extent * 1000),
          cell_sample_rgba: rgba,
        } : {};
        report({event: 'integration.atlas_ready_cell', ...identity, ...sample,
          compiled_index: cards[i / 4], canvas_x: x, canvas_y: y,
          sampled_pixels: width * height, colored_pixels: colored, matched});
      }
      report({event: 'integration.atlas_canvas_sample', control: 'explore.gallery.workspace',
        detail: 'javascript-pixel-counts', a: String(sourceRevision), b: String(presentationRevision),
        c: String(rectangles.length / 4), d: String(nonblack)});
      completed('observed', rectangles.length / 4, nonblack);
    } catch (error) {
      report({event: 'integration.failure', control: 'explore.gallery.workspace',
        detail: `initial atlas canvas read: ${error}`, a: '0', b: '0', c: '0', d: '0'});
      completed('failed');
    }
  });
}

function canvasSnapshot(canvas) {
  if (!integrationState.canvasScratch || integrationState.canvasScratch.width !== canvas.width || integrationState.canvasScratch.height !== canvas.height) {
    integrationState.canvasScratch = new OffscreenCanvas(canvas.width, canvas.height);
    integrationState.canvasContext = integrationState.canvasScratch.getContext('2d', {willReadFrequently:true});
  }
  if (!integrationState.canvasContext) throw new Error('missing diagnostic canvas pixel reader');
  // Read the WebGPU canvas once per probe. Sampling that retained snapshot
  // avoids another GPU readback for every individual pixel in the batch.
  integrationState.canvasContext.clearRect(0, 0, canvas.width, canvas.height);
  integrationState.canvasContext.drawImage(canvas, 0, 0);
  return integrationState.canvasContext;
}

export function mmltkIntegrationAtlasComposition(receipt, points, cards, fields, source, presentation, columns, completed) {
  completed = integrationCompletion(completed);
  if (!integrationState) return;
  if (!probeCurrent(receipt)) { completed('invalidated'); return; }
  if (integrationState.compositionPending?.drawn === receipt.drawn) { completed('invalidated'); return; }
  integrationState.compositionPending = receipt;
  points = points.slice();
  cards = Array.from(cards);
  integrationFrame(() => {
    let matched = 0;
    let emitted = 0;
    try {
      if (!probeCurrent(receipt)) { completed('invalidated'); return; }
      const drawn = integrationState.integrationSurfaceDraws.get('explore.gallery.workspace');
      if (!drawn || drawn.sourceRevision !== source || drawn.presentationRevision !== presentation) { completed('invalidated'); return; }
      const canvas = document.querySelector('canvas');
      if (!canvas) throw new Error('missing composition canvas');
      const context = canvasSnapshot(canvas);
      const identity = JSON.parse(fields);
      for (let i = 0; i < points.length; i += 10) {
        const [x,y,screenX,screenY,r,g,b,a,kind,card] = points.slice(i,i+10);
        const observed = Array.from(context.getImageData(Math.floor(screenX),Math.floor(screenY),1,1).data);
        const expected = [r,g,b,a];
        const valid = observed.every((value,index)=>Math.abs(value-expected[index])<=4);
        matched += Number(valid);
        emitted += Number(kind !== 4);
        report({event:kind === 4 ? 'integration.atlas_grid_pixel' : 'integration.atlas_composition',...identity,columns,card,kind,
          sample_x:x,sample_y:y,canvas_x:screenX,canvas_y:screenY,expected,observed,
          matched:valid});
      }
      report({event:'integration.atlas_composition_complete',...identity,columns,cards,emitted});
      completed('observed', points.length/10,matched);
    } catch (error) {
      report({event:'integration.failure', detail:`atlas composition read: ${error}`});
      completed('failed');
    } finally {
      if (integrationState === receipt.owner && integrationState.compositionPending === receipt) integrationState.compositionPending = undefined;
    }
  });
}
export function mmltkIntegrationBoundaryPixels(points, fields, control, source, presentation) {
  if (!integrationState) return;
  const receipt = mmltkIntegrationProbe(control);
  if (!probeCurrent(receipt)) return;
  const request = {points:points.slice(),fields,control,source,presentation,receipt};
  if (integrationState.boundaryPending) { integrationState.boundaryLatest = request; return; }
  runBoundaryPixels(request);
}
function runBoundaryPixels({points,fields,control,source,presentation,receipt}) {
  integrationState.boundaryPending = true;
  integrationFrame(() => {
    try {
      if (!probeCurrent(receipt)) return;
      const drawn = integrationState.integrationSurfaceDraws.get(control);
      if (!drawn || drawn.sourceRevision !== source || drawn.presentationRevision !== presentation) return;
      const canvas = document.querySelector('canvas');
      if (!canvas) return;
      const context = canvasSnapshot(canvas);
      const identity = JSON.parse(fields);
      for (let i = 0; i < points.length; i += 5) {
        const [x,y,screenX,screenY,sample_index] = points.slice(i,i+5);
        const rgba = context.getImageData(Math.floor(screenX),Math.floor(screenY),1,1).data;
        report({event:'iced.surface.canvas_pixel',...identity,control,sample_index,sample_x:x,sample_y:y,
          canvas_x:screenX,canvas_y:screenY,
          sample_rgba:(rgba[0]|rgba[1]<<8|rgba[2]<<16|rgba[3]<<24)>>>0});
      }
    } finally {
      if (integrationState === receipt.owner) {
        integrationState.boundaryPending = false;
        const next = integrationState.boundaryLatest;
        integrationState.boundaryLatest = undefined;
        if (next) runBoundaryPixels(next);
      }
    }
  });
}

function canvasPixelBounds(canvas, cssBounds, control) {
  const css = canvas.getBoundingClientRect();
  if (![canvas.width, canvas.height, css.width, css.height].every(value => Number.isFinite(value) && value > 0) ||
      cssBounds.length !== 4 || !cssBounds.every(Number.isFinite) || cssBounds[2] <= 0 || cssBounds[3] <= 0) {
    throw new Error('invalid canvas or CSS probe dimensions');
  }
  const scaleX = canvas.width / css.width, scaleY = canvas.height / css.height;
  const pixels = [cssBounds[0] * scaleX, cssBounds[1] * scaleY, cssBounds[2] * scaleX, cssBounds[3] * scaleY];
  report({event: 'integration.canvas_probe_geometry', control, detail: 'css-to-backing-pixels',
    canvas: [canvas.width, canvas.height], css: [css.width, css.height], css_bounds: cssBounds, pixel_bounds: pixels});
  return pixels;
}

export function mmltkIntegrationAnnotationSwatch(receipt, cssBounds,color,control,detail,button,completed){
  completed = integrationCompletion(completed);
  if (!integrationState) return;
  if (!probeCurrent(receipt)) { completed('invalidated'); return; }
  cssBounds = Array.from(cssBounds);
  color = Array.from(color);
  const canvas=document.querySelector('canvas');
  if(canvas)canvas.dispatchEvent(integrationPointer(canvas.getBoundingClientRect(),0,0,'pointermove',0));
  integrationFrame(()=>integrationFrame(()=>{
    try{
      if (!probeCurrent(receipt)) { completed('invalidated'); return; }
      const canvas=document.querySelector('canvas');
      if (!canvas) throw new Error('missing annotation canvas');
      const bounds = canvasPixelBounds(canvas, cssBounds, control);
      const context=canvasSnapshot(canvas);
      const pixel=context.getImageData(Math.floor(bounds[0]+bounds[2]*(button?0.9:0.5)),Math.floor(bounds[1]+bounds[3]/2),1,1).data;
      const matched=color.every((channel,index)=>Math.abs(channel-pixel[index])<=3)&&pixel[3]>0;
      report({event:button?'integration.annotation_capability':'integration.annotation_swatch',control,detail,expected:color,observed:Array.from(pixel),matched});
      completed('observed', 1,Number(matched));
    }catch(error){report({event:'integration.failure',detail:`annotation swatch read: ${error}`});completed('failed');}
  }));
}

export function mmltkIntegrationAnnotationPixels(receipt, cssBounds, extent, probes, sourceRevision, presentationRevision, completed) {
  completed = integrationCompletion(completed);
  if (!integrationState) return;
  if (!probeCurrent(receipt)) { completed('invalidated'); return; }
  cssBounds = Array.from(cssBounds);
  extent = Array.from(extent);
  probes = Array.from(probes);
  try {
    const canvas=document.querySelector('canvas');
    const drawn=integrationState.integrationSurfaceDraws.get('workflow.visual.workspace');
    if (!canvas || !drawn || drawn.sourceRevision!==sourceRevision || drawn.presentationRevision!==presentationRevision) { completed('invalidated'); return; }
    const bounds = canvasPixelBounds(canvas, cssBounds, 'annotation.workspace.surface');
    const context=canvasSnapshot(canvas);
    const scale=Math.min(bounds[2]/extent[0],bounds[3]/extent[1]);
    const ox=bounds[0]+(bounds[2]-extent[0]*scale)/2,oy=bounds[1]+(bounds[3]-extent[1]*scale)/2;
    const colorError = (pixels, offset, expected, filteredPalette) => {
      let gain = 1;
      let minimum = 0;
      const peak = Math.max(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
      const low = Math.min(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
      // Filtered thin outlines mix with the background. Preserve class hue
      // and require at least half native chroma; solid controls stay exact.
      if (filteredPalette && peak - low >= 127.5) {
        minimum = low;
        gain = 255 / (peak - low);
      }
      return Math.max(Math.abs(expected[0] - (pixels[offset] - minimum) * gain),
        Math.abs(expected[1] - (pixels[offset + 1] - minimum) * gain),
        Math.abs(expected[2] - (pixels[offset + 2] - minimum) * gain));
    };
    let matched=0;
    for(let i=0;i<probes.length;i+=7){
      const expected = probes.slice(i+2,i+5);
      const filteredPalette = scale < 1 && Math.max(...expected) === 255 && Math.min(...expected) === 0;
      // A one-source-pixel outline clipped by the image boundary can retain
      // strong class hue while downsampling mixes more of the underlying
      // image than an interior two-sided outline.
      const tolerance = filteredPalette ? Math.max(probes[i+5], 48) : probes[i+5];
      const x=Math.round(ox+probes[i]*scale),y=Math.round(oy+probes[i+1]*scale);
      const radius=Math.max(1,Math.ceil(probes[i+6]*scale));
      const left=Math.max(0,x-radius),top=Math.max(0,y-radius),width=Math.min(canvas.width-left,2*radius+1),height=Math.min(canvas.height-top,2*radius+1);
      let hit=false,best=[0,0,0],distance=Infinity;
      if(width>0&&height>0){
        const pixels=context.getImageData(left,top,width,height).data;
        for(let p=0;p<pixels.length;p+=4){const error=colorError(pixels,p,expected,filteredPalette);
          if(error<distance){distance=error;best=[pixels[p],pixels[p+1],pixels[p+2]];}
          if(error<=tolerance&&pixels[p+3]>0)hit=true;
        }
      }
      matched+=Number(hit);
      report({event:'integration.annotation_pixel',control:'annotation.workspace.surface',detail:'completed-canvas',a:String(sourceRevision),b:String(presentationRevision),c:String(probes[i]),d:String(probes[i+1]),expected,observed:best,error:distance,source_to_screen:scale,matched:hit});
    }
    completed('observed', probes.length/7,matched);
  } catch(error){report({event:'integration.failure',detail:`annotation canvas read: ${error}`});completed('failed');}
}

export function mmltkIntegrationUpscalePixels(receipt, imagePixels, buttonCss, source, presentation, completed) {
  completed = integrationCompletion(completed);
  if (!integrationState) return;
  if (!probeCurrent(receipt)) { completed('invalidated'); return; }
  imagePixels = Array.from(imagePixels);
  buttonCss = Array.from(buttonCss);
  integrationFrame(() => integrationFrame(() => {
    try {
      if (!probeCurrent(receipt)) { completed('invalidated'); return; }
      const canvas = document.querySelector('canvas');
      const drawn = integrationState.integrationSurfaceDraws.get('explore.detail.workspace');
      if (!canvas || !drawn || drawn.sourceRevision !== source || drawn.presentationRevision !== presentation) {
        completed('invalidated'); return;
      }
      const buttonPixels = canvasPixelBounds(canvas, buttonCss, 'explore.detail.upscale');
      const probe = new OffscreenCanvas(16, 16);
      const context = probe.getContext('2d', {willReadFrequently: true});
      if (!context) throw new Error('missing Upscale diagnostic pixel reader');
      let checksum = 2166136261;
      let nonblack = 0;
      let blue = 0;
      for (let region = 0; region < 2; ++region) {
        context.clearRect(0, 0, 16, 16);
        context.drawImage(canvas, ...(region === 0 ? imagePixels : buttonPixels), 0, 0, 16, 16);
        const pixels = context.getImageData(0, 0, 16, 16).data;
        for (let i = 0; i < pixels.length; i += 4) {
          if (region === 0) {
            for (let c = 0; c < 4; ++c) checksum = Math.imul(checksum ^ pixels[i+c], 16777619) >>> 0;
            nonblack += Number(pixels[i+3] > 0 && Math.max(pixels[i], pixels[i+1], pixels[i+2]) > 8);
          } else {
            blue += Number(pixels[i+3] > 0 && pixels[i+2] > pixels[i] + 20 && pixels[i+2] > pixels[i+1] + 10);
          }
        }
      }
      completed('observed', nonblack > 0 ? checksum : 0, blue);
    } catch (error) {
      report({event: 'integration.failure', control: 'explore.detail.workspace', detail: `Upscale canvas read: ${error}`, a:'0', b:'0', c:'0', d:'0'});
      completed('failed');
    }
  }));
}

export function mmltkIntegrationRenderedStyle(control, semantic, red, green, blue, alpha, width, height) {
  const owner = integrationState;
  if (!owner) return;
  integrationFrame(() => {
    if (integrationState !== owner) return;
    const renderKey = ++integrationState.integrationRenderKey;
    report({
      event: 'integration.rendered_style',
      control,
      detail: semantic,
      a: String(red),
      b: String(green),
      c: String(blue),
      d: String(alpha),
      render_key: String(renderKey),
    });
    report({
      event: 'integration.rendered_control',
      control,
      detail: semantic,
      a: String(renderKey),
      b: String(width),
      c: String(height),
      d: String(window.devicePixelRatio),
      render_key: String(renderKey),
    });
  });
}

function integrationClick(canvas, rect, x, y) {
    const moved = integrationPointer(rect, x, y, 'pointermove', 0);
    const pressed = integrationPointer(rect, x, y, 'pointerdown', 1);
    const released = integrationPointer(rect, x, y, 'pointerup', 0);
    const moveAccepted = canvas.dispatchEvent(moved);
    const pressAccepted = canvas.dispatchEvent(pressed);
    const releaseAccepted = canvas.dispatchEvent(released);
    if (integrationState) report({
      event: 'integration.pointer_delivered',
      control: '',
      detail: document.activeElement === canvas ? 'canvas-active' : 'canvas-inactive',
      a: String(x),
      b: String(y),
      c: String(Number(moveAccepted) + Number(pressAccepted) + Number(releaseAccepted)),
      d: String(Number(moved.defaultPrevented) + Number(pressed.defaultPrevented) +
                Number(released.defaultPrevented)),
    });
}

export function mmltkIntegrationFullscreen(enabled) {
  integrationDriver.integrationFullscreenSettled = false;
  const request = enabled ? document.documentElement.requestFullscreen() : document.exitFullscreen();
  const owner = integrationDriver;
  request.then(() => {
    if (integrationDriver !== owner) return;
    integrationDriver.integrationFullscreenSettled = true;
    window.dispatchEvent(new Event('resize'));
    const canvas = document.querySelector('canvas');
    if (canvas) {
      const rect = canvas.getBoundingClientRect();
      canvas.dispatchEvent(new WheelEvent('wheel', {
        bubbles: true,
        cancelable: true,
        clientX: rect.left + rect.width * 0.5,
        clientY: rect.top + rect.height * 0.5,
        deltaY: 0,
        deltaMode: WheelEvent.DOM_DELTA_PIXEL,
      }));
    }
    if (integrationState) report({event: 'integration.atlas_window', control: 'explore.gallery.workspace',
      detail: enabled ? 'fullscreen' : 'restored', a: String(window.innerWidth),
      b: String(window.innerHeight), c: String(window.devicePixelRatio), d: String(Number(!!document.fullscreenElement))});
  }).catch(error => { if (integrationDriver !== owner || !integrationState) return; report({event: 'integration.failure', control: 'explore.gallery.workspace',
    detail: String(error) + '; visibility=' + document.visibilityState +
      '; focused=' + document.hasFocus() + '; enabled=' + document.fullscreenEnabled +
      '; activation=' + (navigator.userActivation?.isActive ?? 'unavailable'),
    a: '0', b: '0', c: '0', d: '0'}); });
}
export function mmltkIntegrationFullscreenSettled(enabled) {
  return integrationDriver.integrationFullscreenSettled && !!document.fullscreenElement === enabled;
}

export function mmltkIntegrationClick(x, y) {
  const canvas = document.querySelector('canvas');
  if (!canvas || !Number.isFinite(x) || !Number.isFinite(y)) return 0;
  const rect = canvas.getBoundingClientRect();
  integrationMicrotask(() => integrationClick(canvas, rect, x, y));
  return 1;
}

export function mmltkIntegrationClickAfterSurfaceDraw(x, y, control, sourceRevision, allowNewer) {
  if (!integrationDriver || !document.querySelector('canvas') || !Number.isFinite(x) || !Number.isFinite(y) ||
      typeof control !== 'string' || control.length === 0 ||
      !Number.isSafeInteger(sourceRevision) || sourceRevision <= 0) return 0;
  integrationDriver.pendingSurfaceClick = {x, y, control, sourceRevision, allowNewer};
  dispatchIntegrationSurfaceClick();
  return 1;
}

export function mmltkIntegrationHoverAfterSurfaceDraw(x, y, control, sourceRevision) {
  if (!mmltkIntegrationClickAfterSurfaceDraw(x, y, control, sourceRevision, false)) return 0;
  integrationDriver.pendingSurfaceClick.hover = true;
  return 1;
}

export function mmltkIntegrationSweep(x, y, width, height) {
  const canvas = document.querySelector('canvas');
  if (!canvas || ![x, y, width, height].every(Number.isFinite) || width <= 0 || height <= 0) return 0;
  const rect = canvas.getBoundingClientRect();
  integrationMicrotask(() => {
    if (integrationState) report({
      event: 'integration.explore_sweep_dispatch',
      control: 'explore.gallery.workspace',
      detail: 'begin',
      a: '64',
      b: '8',
      c: String(width),
      d: String(height),
    });
    for (let sample = 0; sample < 64; ++sample) {
      const ratio = sample / 63;
      const localX = x + width * (0.25 + 0.5 * (sample & 1));
      const localY = y + height * ratio;
      canvas.dispatchEvent(integrationPointer(rect, localX, localY, 'pointermove', 0));
      if ((sample & 7) === 7) {
        canvas.dispatchEvent(new WheelEvent('wheel', {
          bubbles: true,
          cancelable: true,
          clientX: rect.left + localX,
          clientY: rect.top + localY,
          deltaY: 96,
          deltaMode: WheelEvent.DOM_DELTA_PIXEL,
        }));
      }
    }
    if (integrationState) report({
      event: 'integration.explore_sweep_dispatch',
      control: 'explore.gallery.workspace',
      detail: 'complete',
      a: '64',
      b: '8',
      c: String(width),
      d: String(height),
    });
  });
  return 1;
}

export function mmltkIntegrationWheel(x, y) {
  const canvas = document.querySelector('canvas');
  if (!canvas || !Number.isFinite(x) || !Number.isFinite(y)) return 0;
  const rect = canvas.getBoundingClientRect();
  integrationMicrotask(() => {
    canvas.dispatchEvent(new WheelEvent('wheel', {
      bubbles: true,
      cancelable: true,
      clientX: rect.left + x,
      clientY: rect.top + y,
      deltaY: -96,
      deltaMode: WheelEvent.DOM_DELTA_PIXEL,
    }));
  });
  return 1;
}

export function mmltkIntegrationSliderDrag(x, y, width, height) {
  const canvas = document.querySelector('canvas');
  if (!canvas || ![x, y, width, height].every(Number.isFinite) || width <= 0 || height <= 0) return 0;
  const rect = canvas.getBoundingClientRect();
  const localY = y + height - Math.min(10, height * 0.2);
  const positions = [0.35, 0.5, 0.62];
  integrationMicrotask(() => {
    canvas.dispatchEvent(integrationPointer(rect, x + width * positions[0], localY, 'pointermove', 0));
    canvas.dispatchEvent(integrationPointer(rect, x + width * positions[0], localY, 'pointerdown', 1));
    if (integrationState) report({
      event: 'integration.ui_scale_pointer',
      control: 'settings.ui_scale',
      detail: 'pressed',
      a: String(positions[0]),
      b: '1',
      c: String(canvas.width),
      d: String(rect.width),
    });
    integrationFrame(() => {
      canvas.dispatchEvent(integrationPointer(rect, x + width * positions[1], localY, 'pointermove', 1));
      if (integrationState) report({
        event: 'integration.ui_scale_pointer',
        control: 'settings.ui_scale',
        detail: 'moved-1',
        a: String(positions[1]),
        b: '1',
        c: String(canvas.width),
        d: String(rect.width),
      });
      integrationFrame(() => {
        canvas.dispatchEvent(integrationPointer(rect, x + width * positions[2], localY, 'pointermove', 1));
        if (integrationState) report({
          event: 'integration.ui_scale_pointer',
          control: 'settings.ui_scale',
          detail: 'moved-2',
          a: String(positions[2]),
          b: '1',
          c: String(canvas.width),
          d: String(rect.width),
        });
        integrationFrame(() => {
          canvas.dispatchEvent(integrationPointer(rect, x + width * positions[2], localY, 'pointerup', 0));
          if (integrationState) report({
            event: 'integration.ui_scale_pointer',
            control: 'settings.ui_scale',
            detail: 'released',
            a: String(positions[2]),
            b: '0',
            c: String(canvas.width),
            d: String(rect.width),
          });
        });
      });
    });
  });
  return 1;
}

function integrationKey(canvas, type, key, code, control, shift) {
  const event = new KeyboardEvent(type, {
    bubbles: true,
    cancelable: true,
    composed: true,
    key,
    code,
    ctrlKey: control,
    shiftKey: shift,
  });
  const accepted = canvas.dispatchEvent(event);
  return Number(accepted) + Number(event.defaultPrevented);
}

export function mmltkIntegrationReplaceNumber(x, y, value, selectionLength) {
  const canvas = document.querySelector('canvas');
  if (!canvas || !Number.isFinite(x) || !Number.isFinite(y) ||
      typeof value !== 'string' || !Number.isInteger(selectionLength) || selectionLength <= 0) return 0;
  const rect = canvas.getBoundingClientRect();
  const observeKeyStage = (stage, result) => { if (integrationState) report({
    event: 'integration.number_key_stage',
    control: stage,
    detail: document.visibilityState,
    a: String(result),
    b: String(document.hasFocus()),
    c: String(document.activeElement === canvas),
    d: value,
  }); };
  if (integrationState) report({
    event: 'integration.number_replace',
    control: '',
    detail: 'scheduled',
    a: String(x),
    b: String(y),
    c: value,
    d: String(document.activeElement === canvas),
  });
  integrationMicrotask(() => {
    const pointerEvents = [
      integrationPointer(rect, x, y, 'pointermove', 0),
      integrationPointer(rect, x, y, 'pointerdown', 1),
      integrationPointer(rect, x, y, 'pointerup', 0),
    ];
    const pointerResult = pointerEvents.reduce(
      (total, event) => total + Number(canvas.dispatchEvent(event)) + Number(event.defaultPrevented),
      0,
    );
    if (integrationState) report({
      event: 'integration.number_replace',
      control: '',
      detail: 'focused',
      a: String(pointerResult),
      b: String(document.activeElement === canvas),
      c: '0',
      d: '0',
    });
    observeKeyStage('awaiting-control-down', pointerResult);
    integrationFrame(() => {
      let keyResult = integrationKey(canvas, 'keydown', 'Control', 'ControlLeft', true, false);
      observeKeyStage('control-down', keyResult);
      integrationFrame(() => {
        keyResult += integrationKey(canvas, 'keydown', 'a', 'KeyA', true, false);
        keyResult += integrationKey(canvas, 'keyup', 'a', 'KeyA', true, false);
        observeKeyStage('select-all', keyResult);
        integrationFrame(() => {
          keyResult += integrationKey(canvas, 'keyup', 'Control', 'ControlLeft', false, false);
          observeKeyStage('control-up', keyResult);
          integrationFrame(() => {
            for (const character of value) {
              const code = character === '.' ? 'Period' :
                character === '-' ? 'Minus' :
                character === '+' ? 'Equal' :
                character === 'e' || character === 'E' ? 'KeyE' : `Digit${character}`;
              keyResult += integrationKey(canvas, 'keydown', character, code, false, false);
              keyResult += integrationKey(canvas, 'keyup', character, code, false, false);
            }
            if (integrationState) report({
              event: 'integration.number_replace',
              control: '',
              detail: 'keyboard',
              a: String(keyResult),
              b: String(document.activeElement === canvas),
              c: value,
              d: String(selectionLength),
            });
          });
        });
      });
    });
  });
  return 1;
}

export function mmltkIntegrationAnnotationRelease() {
  const end=integrationDriver.integrationAnnotationPointerEnd;
  if(end){end.canvas.dispatchEvent(integrationPointer(end.rect,end.x,end.y,'pointerup',0));integrationDriver.integrationAnnotationPointerEnd=null;}
}
export function mmltkIntegrationAnnotationPointer(x, y, width, height, startX, startY, endX, endY, hold, steps = 1) {
  const canvas = document.querySelector('canvas');
  if (!canvas || ![x, y, width, height, startX, startY, endX, endY].every(Number.isFinite)
      || width <= 0 || height <= 0 || !Number.isInteger(steps) || steps < 1 || steps > 256) return 0;
  const rect = canvas.getBoundingClientRect();
  const x0 = x + width * startX;
  const y0 = y + height * startY;
  const x1 = x + width * endX;
  const y1 = y + height * endY;
  integrationMicrotask(() => {
    canvas.dispatchEvent(integrationPointer(rect, x0, y0, 'pointermove', 0));
    canvas.dispatchEvent(integrationPointer(rect, x0, y0, 'pointerdown', 1));
    for (let step = 1; step <= steps; ++step) {
      const fraction = step / steps;
      canvas.dispatchEvent(integrationPointer(rect, x0 + (x1 - x0) * fraction, y0 + (y1 - y0) * fraction, 'pointermove', 1));
    }
    if(hold) integrationDriver.integrationAnnotationPointerEnd={canvas,rect,x:x1,y:y1};
    else canvas.dispatchEvent(integrationPointer(rect, x1, y1, 'pointerup', 0));
  });
  return 1;
}

export function mmltkIntegrationWindowClose() {
  integrationMicrotask(() => integrationFrame(() => integrationFrame(() => window.close())));
  return 1;
}
