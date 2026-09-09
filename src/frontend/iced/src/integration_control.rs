mod annotation_checks;
mod annotation_product;
use crate::generated::FeatureId;
use crate::message::Message as RootMessage;
use crate::view::{annotation, explore, train};
use crate::view_model::{ApplicationModel, ConnectionState};
use iced::advanced::widget::operation::Outcome;
use iced::advanced::widget::{self, Id, Operation};
use iced::widget::operation::{AbsoluteOffset, RelativeOffset};
use iced::{Rectangle, Task, Vector};

thread_local! {
    static COMPLETION_WITHOUT_INPUT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(crate) fn repeated_redraws_enabled() -> bool {
    !COMPLETION_WITHOUT_INPUT.with(std::cell::Cell::get)
}

const TRAIN_CARD: &str = train::DATASET_CARD_ID;
const COMPILE_DATASET: &str = train::COMPILE_DATASET_ID;
const DATASET_STATUS: &str = train::DATASET_STATUS_ID;
const DATASET_SOURCE: &str = train::DATASET_SOURCE_ID;
const COMPILED_DIRECTORY: &str = train::COMPILED_DIRECTORY_ID;
const COMPILE_DIMENSIONS: &str = train::COMPILE_DIMENSIONS_ID;
const COMPILE_RESOLUTION: &str = train::COMPILE_RESOLUTION_ID;
const COMPILE_PROGRESS: &str = train::COMPILE_PROGRESS_ID;
const DATASET_BROWSE: &str = train::DATASET_BROWSE_ID;
const TRAIN_MODEL_CARD: &str =
    crate::view::workflow::model_card::stable_id(crate::generated::FeatureId::Train);
const TRAIN_MODEL_PROGRESS: &str =
    crate::view::workflow::model_card::progress_id(crate::generated::FeatureId::Train);
const TRAIN_MODEL_PARTS: [&str; 6] = [
    crate::view::workflow::model_card::TRAIN_SELECTOR_ID,
    crate::view::workflow::model_card::TRAIN_PRESETS_ID,
    crate::view::workflow::model_card::TRAIN_DIVIDER_ID,
    crate::view::workflow::model_card::TRAIN_CUSTOM_ID,
    crate::view::workflow::model_card::TRAIN_STATUS_ID,
    crate::view::workflow::model_card::TRAIN_ACTION_ID,
];
const BENCHMARK_OVERRIDE: &str = train::BENCHMARK_OVERRIDE_ID;
pub const EXPLORE_DATASET_PANE: &str = explore::DATASET_PANE_ID;
pub const EXPLORE_DETAILS_PANE: &str = explore::DETAILS_PANE_ID;
pub const EXPLORE_DETAIL_CLOSE: &str = explore::DETAIL_CLOSE_ID;
const EXPLORE_OPEN: &str = explore::OPEN_ID;
const EXPLORE_CARD: &str = explore::STATUS_CARD_ID;
const EXPLORE_GALLERY: &str = explore::GALLERY_WORKSPACE_ID;
const EXPLORE_LATER: &str = explore::GALLERY_LATER_ID;
const EXPLORE_NEXT: &str = explore::DETAIL_NEXT_ID;
const EXPLORE_PREVIOUS: &str = explore::DETAIL_PREVIOUS_ID;
const EXPLORE_ANNOTATE: &str = explore::DETAIL_ANNOTATE_ID;
fn overlay_control(index: usize, detail: bool) -> &'static str {
    let controls = if detail {
        [
            explore::DETAIL_BOXES_ID,
            explore::DETAIL_MASKS_ID,
            explore::DETAIL_BOXES_ID,
            explore::DETAIL_LABELS_ID,
        ]
    } else {
        [
            explore::GALLERY_BOXES_ID,
            explore::GALLERY_MASKS_ID,
            explore::GALLERY_BOXES_ID,
            explore::GALLERY_LABELS_ID,
        ]
    };
    controls[index % 4]
}
const VIEWER_SAVE: &str = crate::view::workflow::Composition::new(FeatureId::Annotate, 0.0)
    .stable_id(crate::view::workflow::Region::PrimaryAction);

fn annotation_message(message: annotation::Message) -> Task<RootMessage> {
    Task::done(RootMessage::Workspace(
        crate::view::router::Message::Annotation(message),
    ))
}

fn explore_message(message: explore::Message) -> Task<RootMessage> {
    Task::done(RootMessage::Workspace(
        crate::view::router::Message::Explore(message),
    ))
}

pub(crate) fn report_viewer_label(
    category: u16,
    color: iced::Color,
    catalog_count: usize,
    overlay: &crate::generated::ExploreOverlay,
    frame: crate::presentation_surface::FrameReady,
    detail: bool,
) {
    report(
        "integration.viewer_label_rgb",
        explore::DETAIL_LABELS_ID,
        "actual-iced-label",
        [
            f64::from(category),
            f64::from(color.r),
            f64::from(color.g),
            f64::from(color.b),
        ],
    );
    report(
        "integration.viewer_label_catalog",
        explore::DETAIL_LABELS_ID,
        "full-native-catalog",
        [
            f64::from(category),
            catalog_count as f64,
            f64::from(u8::from(overlay.showboxes)),
            f64::from(u8::from(overlay.showmasks)),
        ],
    );
    report(
        "integration.viewer_label_frame",
        explore::DETAIL_LABELS_ID,
        if detail {
            "exact-scene-product"
        } else {
            "gallery-atlas"
        },
        [
            frame.presentation_revision as f64,
            frame.content_sequence as f64,
            frame.content_session as f64,
            f64::from(u8::from(overlay.showboxes)),
        ],
    );
}
pub const EXPLORE_AUGMENTATION_TOGGLE: &str = explore::AUGMENTATION_TOGGLE_ID;
pub const EXPLORE_AUGMENTATION_REROLL: &str = explore::AUGMENTATION_REROLL_ID;
pub const EXPLORE_RESHUFFLE: &str = explore::RESHUFFLE_ID;
pub const EXPLORE_DETAIL_ORIGINAL: &str = explore::DETAIL_ORIGINAL_ID;
pub const EXPLORE_UPSCALE_ACTIONS: [&str; 3] = [
    explore::DETAIL_UPSCALE_BASIC_ID,
    explore::DETAIL_UPSCALE_FAST_ID,
    explore::DETAIL_UPSCALE_NEURAL_ID,
];

fn upscale_acceptance_label(kernel: crate::generated::UpscaleKernel) -> &'static str {
    match kernel {
        crate::generated::UpscaleKernel::Default => "basic-four-times",
        crate::generated::UpscaleKernel::ShiftLut => "fast-four-times",
        crate::generated::UpscaleKernel::RealPlksr => "neural-four-times",
    }
}

fn explore_order_signature(indices: &[u32]) -> u64 {
    indices
        .iter()
        .fold(14_695_981_039_346_656_037_u64, |hash, index| {
            (hash ^ u64::from(*index)).wrapping_mul(1_099_511_628_211)
        })
}

const ANNOTATION_SURFACE: &str = annotation::WORKSPACE_ID;
const ANNOTATION_SIDEBAR: &str = annotation::SIDEBAR_ID;
const ANNOTATION_TIMELINE: &str = annotation::TIMELINE_ID;
const ANNOTATION_OPERATION: &str = "annotation.operation";
const ANNOTATION_STOP: &str = "annotation.stop";
const ANNOTATION_BRUSH_RADIUS: &str = "annotation.brush_radius";
const SETTINGS_GROUPS: [&str; 3] = [
    "settings.group.appearance",
    "settings.group.typography",
    "settings.group.environment",
];
const SETTINGS_SHOW_FPS: &str = "settings.show_fps";
const SETTINGS_NUMERIC_CONTROLS: [&str; 5] = [
    "settings.ui_scale",
    "settings.font_size",
    "settings.secondary_font_size",
    "settings.mono_font_size",
    "settings.text_input_font_size",
];
const SETTINGS_FOOTER: &str = "settings.footer";
const SETTINGS_MODAL: &str = "settings.modal";
const SETTINGS_RESET: &str = "settings.reset";
const SETTINGS_CLOSE: &str = "settings.close";
const ERROR_MODAL: &str = "error.modal";
const ERROR_COPY: &str = crate::view::error_modal::COPY_ID;
const ERROR_DISMISS: &str = crate::view::error_modal::DISMISS_ID;
const SIDEBAR_HEADER_HEIGHT: f32 = 48.0;
const SIDEBAR_REVEAL_INSET: f32 = 16.0;
const SIDEBAR_VISIBLE_INSET: f32 = 8.0;

#[derive(Debug, Clone)]
pub enum Message {
    Advance,
    NumberWheelDelivered,
    #[cfg(target_arch = "wasm32")]
    UpscalePixels {
        source: u64,
        presentation: u64,
        checksum: u32,
        blue: u32,
    },
    Located {
        control: String,
        bounds: Rectangle,
    },
    SurfaceDrawn {
        presentation_revision: u64,
        source_revision: u64,
        viewer: Option<ViewerDraw>,
    },
    AtlasDrawn {
        source_revision: u64,
        visibility: u8,
        clipped_top: f32,
        clipped_bottom: f32,
        row_extent: f32,
        receipt: AtlasDraw,
    },
    GalleryDrawn {
        presentation_revision: u64,
        source_revision: u64,
    },
    #[cfg(target_arch = "wasm32")]
    AnnotationPixels {
        revision: u64,
        expected: u32,
        matched: u32,
    },
    #[cfg(target_arch = "wasm32")]
    AnnotationControlPixels {
        expected: u32,
        matched: u32,
    },
    AtlasPixels {
        receipt: AtlasDraw,
        visible: u32,
        nonblack: u32,
    },
    AtlasComposition {
        receipt: AtlasDraw,
        expected: u32,
        matched: u32,
    },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ViewerDraw {
    pub crop: [u32; 4],
    pub container: Rectangle,
    pub image: Rectangle,
    pub fit_revision: u64,
}

#[derive(Default)]
struct SurfaceDrawObserver {
    output: Option<iced::futures::channel::mpsc::Sender<Message>>,
    identity: (u64, u64),
    viewer: Option<(u64, u64, ViewerDraw)>,
    gallery: Option<(u64, u64)>,
    atlas: Option<AtlasDraw>,
}

thread_local! {
    static SURFACE_DRAW_OBSERVER: std::cell::RefCell<SurfaceDrawObserver> =
        std::cell::RefCell::new(SurfaceDrawObserver::default());
}

struct SurfaceDrawSubscription;

impl Drop for SurfaceDrawSubscription {
    fn drop(&mut self) {
        SURFACE_DRAW_OBSERVER
            .with(|observer| *observer.borrow_mut() = SurfaceDrawObserver::default());
    }
}

fn surface_draw_stream() -> impl iced::futures::Stream<Item = Message> {
    iced::stream::channel(1, async move |output| {
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = Some(output));
        let _subscription = SurfaceDrawSubscription;
        std::future::pending::<()>().await;
    })
}

struct FindControl {
    target: Id,
    translation: Vector,
    pending_translation: Vector,
    bounds: Option<Rectangle>,
}

impl FindControl {
    fn capture(&mut self, id: Option<&Id>, bounds: Rectangle) {
        if id == Some(&self.target) {
            self.bounds = Some(Rectangle {
                x: bounds.x - self.translation.x,
                y: bounds.y - self.translation.y,
                ..bounds
            });
        }
    }
}

impl Operation<Rectangle> for FindControl {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<Rectangle>)) {
        let parent_translation = self.translation;
        self.translation += self.pending_translation;
        self.pending_translation = Vector::ZERO;
        operate(self);
        self.translation = parent_translation;
    }

    fn container(&mut self, id: Option<&Id>, bounds: Rectangle) {
        self.capture(id, bounds);
    }

    fn scrollable(
        &mut self,
        id: Option<&Id>,
        bounds: Rectangle,
        _content_bounds: Rectangle,
        translation: Vector,
        _state: &mut dyn iced::advanced::widget::operation::Scrollable,
    ) {
        self.capture(id, bounds);
        self.pending_translation += translation;
    }

    fn text_input(
        &mut self,
        id: Option<&Id>,
        bounds: Rectangle,
        _state: &mut dyn iced::advanced::widget::operation::TextInput,
    ) {
        self.capture(id, bounds);
    }

    fn finish(&self) -> Outcome<Rectangle> {
        Outcome::Some(self.bounds.unwrap_or_default())
    }
}

fn locate(control: String) -> Task<RootMessage> {
    let target = control.clone();
    widget::operate(FindControl {
        target: Id::from(control),
        translation: Vector::ZERO,
        pending_translation: Vector::ZERO,
        bounds: None,
    })
    .map(move |bounds| {
        RootMessage::Integration(Message::Located {
            control: target.clone(),
            bounds,
        })
    })
}

fn settled_settings_snapshot<'a>(
    model: &'a ApplicationModel,
    settings: &crate::view::settings::SettingsModel,
    after_revision: u64,
) -> Option<&'a crate::generated::SettingsUiState> {
    if settings.has_local_edits() {
        return None;
    }
    model
        .settings_snapshot
        .as_ref()
        .filter(|snapshot| snapshot.revision > after_revision)
}

fn ui_scale_evidence(
    model: &ApplicationModel,
    settings: &crate::view::settings::SettingsModel,
    baseline: f32,
    applied: f32,
) -> [f64; 4] {
    [
        f64::from(baseline),
        f64::from(
            settings
                .draft
                .as_ref()
                .map_or(applied, |draft| draft.ui.uiscale),
        ),
        f64::from(applied),
        model
            .settings_snapshot
            .as_ref()
            .map_or(0.0, |snapshot| snapshot.revision as f64),
    ]
}

fn sidebar_reveal_offset(pane: Rectangle, target: Rectangle) -> Option<AbsoluteOffset> {
    let visible_top = pane.y + SIDEBAR_HEADER_HEIGHT + SIDEBAR_REVEAL_INSET;
    let visible_bottom = pane.y + pane.height - SIDEBAR_REVEAL_INSET;
    let target_bottom = target.y + target.height;
    let y = if target.y < visible_top {
        target.y - visible_top
    } else if target_bottom > visible_bottom {
        target_bottom - visible_bottom
    } else {
        0.0
    };
    (y.abs() > f32::EPSILON).then_some(AbsoluteOffset { x: 0.0, y })
}

fn sidebar_control_visible(pane: Rectangle, target: Rectangle) -> bool {
    let visible_top = pane.y + SIDEBAR_HEADER_HEIGHT + SIDEBAR_VISIBLE_INSET;
    let visible_bottom = pane.y + pane.height - SIDEBAR_VISIBLE_INSET;
    target.x >= pane.x
        && target.x + target.width <= pane.x + pane.width
        && target.y >= visible_top
        && target.y + target.height <= visible_bottom
}

#[cfg(target_arch = "wasm32")]
#[wasm_bindgen::prelude::wasm_bindgen(inline_js = r#"
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

const integrationSurfaceDraws = new Map();
let integrationPendingSurfaceClick;

function matchesIntegrationSurfaceClick(pending, drawn) {
  return drawn && (pending.allowNewer ? drawn.sourceRevision >= pending.sourceRevision :
    drawn.sourceRevision === pending.sourceRevision);
}

function dispatchIntegrationSurfaceClick(pending) {
  queueMicrotask(() => {
    const drawn = integrationSurfaceDraws.get(pending.control);
    if (!matchesIntegrationSurfaceClick(pending, drawn)) {
      integrationPendingSurfaceClick = pending;
      return;
    }
    const canvas = document.querySelector('canvas');
    if (!canvas) return;
    report({
      event: 'integration.surface_click_dispatched',
      control: pending.control,
      detail: 'real-canvas-pointer',
      a: String(drawn.sourceRevision),
      b: String(drawn.presentationRevision),
      c: String(pending.x),
      d: String(pending.y),
    });
    integrationClick(canvas, canvas.getBoundingClientRect(), pending.x, pending.y);
  });
}

function releaseIntegrationSurfaceClick(record) {
  if (record.event !== 'integration.surface_draw') return;
  const sourceRevision = Number(record.b);
  if (!Number.isSafeInteger(sourceRevision) || sourceRevision <= 0) return;
  const drawn = {sourceRevision, presentationRevision: Number(record.a)};
  integrationSurfaceDraws.set(record.control, drawn);
  const pending = integrationPendingSurfaceClick;
  if (!pending || pending.control !== record.control || !matchesIntegrationSurfaceClick(pending, drawn)) return;
  integrationPendingSurfaceClick = undefined;
  dispatchIntegrationSurfaceClick(pending);
}

let initialAtlasWithoutInput = false;
let initialAtlasInputCount = 0;
let initialAtlasCompleted = false;
for (const type of ['pointermove', 'pointerdown', 'pointerup', 'wheel', 'focus', 'keydown']) {
  window.addEventListener(type, () => {
    if (!initialAtlasWithoutInput) return;
    initialAtlasInputCount++;
    report({event: 'integration.failure', control: 'explore.gallery.workspace',
      detail: `input during initial atlas completion: ${type}`,
      a: String(initialAtlasInputCount), b: '0', c: '0', d: '0'});
  }, true);
}

function report(record) {
  record.elapsed_ms = performance.now();
  if (!initialAtlasCompleted && record.event === 'integration.explore_open_submission' && record.detail === 'submitted') {
    initialAtlasWithoutInput = true;
    initialAtlasInputCount = 0;
  } else if (record.event === 'integration.initial_atlas_complete') {
    initialAtlasWithoutInput = false;
    initialAtlasCompleted = true;
  }
  const line = JSON.stringify(record);
  if (typeof globalThis.dump === 'function') globalThis.dump(`${line}\n`);
  console.error(line);
  releaseIntegrationSurfaceClick(record);
}

export function mmltkIntegrationReport(event, control, detail, a, b, c, d) {
  report({event, control, detail, a: String(a), b: String(b), c: String(c), d: String(d)});
}

export function mmltkIntegrationAtlasPixels(rectangles, sourceRevision, presentationRevision, completed) {
  rectangles = rectangles.slice();
  // The draw report is emitted while encoding Iced's current submission.
  // Sample the actual canvas at its next presentation opportunity, without
  // dispatching input, scheduling an Iced redraw, or introducing a timer.
  requestAnimationFrame(() => {
    try {
      const canvas = document.querySelector('canvas');
      if (!canvas) throw new Error('missing WebGPU canvas');
      const drawn = integrationSurfaceDraws.get('explore.gallery.workspace');
      if (!drawn || drawn.sourceRevision !== sourceRevision ||
          drawn.presentationRevision !== presentationRevision || initialAtlasInputCount !== 0) {
        completed(0, 0);
        return;
      }
      const probe = new OffscreenCanvas(8, 8);
      const context = probe.getContext('2d', {willReadFrequently: true});
      if (!context) throw new Error('missing diagnostic pixel reader');
      let nonblack = 0;
      for (let i = 0; i < rectangles.length; i += 4) {
        context.clearRect(0, 0, 8, 8);
        context.drawImage(canvas, rectangles[i], rectangles[i + 1],
          rectangles[i + 2], rectangles[i + 3], 0, 0, 8, 8);
        const pixels = context.getImageData(0, 0, 8, 8).data;
        let colored = 0;
        for (let p = 0; p < pixels.length; p += 4) {
          if (pixels[p + 3] > 0 && Math.max(pixels[p], pixels[p + 1], pixels[p + 2]) > 8) {
            colored++;
          }
        }
        nonblack += Number(colored >= 32);
      }
      report({event: 'integration.atlas_canvas_sample', control: 'explore.gallery.workspace',
        detail: 'javascript-pixel-counts', a: String(sourceRevision), b: String(presentationRevision),
        c: String(rectangles.length / 4), d: String(nonblack)});
      completed(rectangles.length / 4, nonblack);
    } catch (error) {
      report({event: 'integration.failure', control: 'explore.gallery.workspace',
        detail: `initial atlas canvas read: ${error}`, a: '0', b: '0', c: '0', d: '0'});
      completed(0, 0);
    }
  });
}

let boundaryScratch;
let boundaryPending = false;
let boundaryLatest;
let compositionPending = false;
let annotationScratch;
let annotationContext;

function annotationCanvasSnapshot(canvas) {
  if (!annotationScratch || annotationScratch.width !== canvas.width || annotationScratch.height !== canvas.height) {
    annotationScratch = new OffscreenCanvas(canvas.width, canvas.height);
    annotationContext = annotationScratch.getContext('2d', {willReadFrequently:true});
  }
  if (!annotationContext) throw new Error('missing annotation canvas pixel reader');
  annotationContext.clearRect(0, 0, canvas.width, canvas.height);
  annotationContext.drawImage(canvas, 0, 0);
  return annotationContext;
}

export function mmltkIntegrationAtlasComposition(points, cards, fields, source, presentation, columns, completed) {
  if (compositionPending) { completed(0,0); return; }
  compositionPending = true;
  points = points.slice();
  cards = Array.from(cards);
  requestAnimationFrame(() => {
    let matched = 0;
    let emitted = 0;
    try {
      const drawn = integrationSurfaceDraws.get('explore.gallery.workspace');
      if (!drawn || drawn.sourceRevision !== source || drawn.presentationRevision !== presentation) return;
      const canvas = document.querySelector('canvas');
      if (!canvas) return;
      boundaryScratch ??= new OffscreenCanvas(1,1);
      const context = boundaryScratch.getContext('2d', {willReadFrequently:true});
      if (!context) return;
      const identity = JSON.parse(fields);
      for (let i = 0; i < points.length; i += 10) {
        const [x,y,screenX,screenY,r,g,b,a,kind,card] = points.slice(i,i+10);
        context.clearRect(0,0,1,1);
        context.drawImage(canvas,Math.floor(screenX),Math.floor(screenY),1,1,0,0,1,1);
        const observed = Array.from(context.getImageData(0,0,1,1).data);
        const expected = [r,g,b,a];
        const valid = observed.every((value,index)=>Math.abs(value-expected[index])<=4);
        matched += Number(valid);
        ++emitted;
        report({event:'integration.atlas_composition',...identity,columns,card,kind,
          sample_x:x,sample_y:y,canvas_x:screenX,canvas_y:screenY,expected,observed,
          matched:valid});
      }
      report({event:'integration.atlas_composition_complete',...identity,columns,cards,emitted});
    } finally { compositionPending = false; completed(cards.length*4,matched); }
  });
}
export function mmltkIntegrationBoundaryPixels(points, fields, control, source, presentation) {
  const request = {points:points.slice(),fields,control,source,presentation};
  if (boundaryPending) { boundaryLatest = request; return; }
  runBoundaryPixels(request);
}
function runBoundaryPixels({points,fields,control,source,presentation}) {
  boundaryPending = true;
  requestAnimationFrame(() => {
    try {
      const drawn = integrationSurfaceDraws.get(control);
      if (!drawn || drawn.sourceRevision !== source || drawn.presentationRevision !== presentation) return;
      const canvas = document.querySelector('canvas');
      if (!canvas) return;
      boundaryScratch ??= new OffscreenCanvas(1, 1);
      const context = boundaryScratch.getContext('2d', {willReadFrequently:true});
      if (!context) return;
      const identity = JSON.parse(fields);
      for (let i = 0; i < points.length; i += 5) {
        const [x,y,screenX,screenY,sample_index] = points.slice(i,i+5);
        context.clearRect(0,0,1,1);
        context.drawImage(canvas,Math.floor(screenX),Math.floor(screenY),1,1,0,0,1,1);
        const rgba = context.getImageData(0,0,1,1).data;
        report({event:'iced.surface.canvas_pixel',...identity,control,sample_index,sample_x:x,sample_y:y,
          canvas_x:screenX,canvas_y:screenY,
          sample_rgba:(rgba[0]|rgba[1]<<8|rgba[2]<<16|rgba[3]<<24)>>>0});
      }
    } finally {
      boundaryPending = false;
      const next = boundaryLatest;
      boundaryLatest = undefined;
      if (next) runBoundaryPixels(next);
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

export function mmltkIntegrationAnnotationSwatch(cssBounds,color,control,detail,button,completed){
  cssBounds = Array.from(cssBounds);
  color = Array.from(color);
  const canvas=document.querySelector('canvas');
  if(canvas)canvas.dispatchEvent(integrationPointer(canvas.getBoundingClientRect(),0,0,'pointermove',0));
  requestAnimationFrame(()=>requestAnimationFrame(()=>{
    try{
      const canvas=document.querySelector('canvas');
      if (!canvas) throw new Error('missing annotation canvas');
      const bounds = canvasPixelBounds(canvas, cssBounds, control);
      const context=annotationCanvasSnapshot(canvas);
      const pixel=context.getImageData(Math.floor(bounds[0]+bounds[2]*(button?0.9:0.5)),Math.floor(bounds[1]+bounds[3]/2),1,1).data;
      const matched=color.every((channel,index)=>Math.abs(channel-pixel[index])<=3)&&pixel[3]>0;
      report({event:button?'integration.annotation_capability':'integration.annotation_swatch',control,detail,expected:color,observed:Array.from(pixel),matched});
      completed(1,Number(matched));
    }catch(error){report({event:'integration.failure',detail:`annotation swatch read: ${error}`});completed(1,0);}
  }));
}

export function mmltkIntegrationAnnotationPixels(cssBounds, extent, probes, sourceRevision, presentationRevision, completed) {
  cssBounds = Array.from(cssBounds);
  extent = Array.from(extent);
  probes = Array.from(probes);
  try {
    const canvas=document.querySelector('canvas');
    const drawn=integrationSurfaceDraws.get('workflow.visual.workspace');
    if (!canvas || !drawn || drawn.sourceRevision!==sourceRevision || drawn.presentationRevision!==presentationRevision) { completed(probes.length/7,0); return; }
    const bounds = canvasPixelBounds(canvas, cssBounds, 'annotation.workspace.surface');
    const context=annotationCanvasSnapshot(canvas);
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
    completed(probes.length/7,matched);
  } catch(error){report({event:'integration.failure',detail:`annotation canvas read: ${error}`});completed(probes.length/7,0);}
}

let integrationRenderKey = 0;
export function mmltkIntegrationUpscalePixels(imagePixels, buttonCss, source, presentation, completed) {
  imagePixels = Array.from(imagePixels);
  buttonCss = Array.from(buttonCss);
  requestAnimationFrame(() => requestAnimationFrame(() => {
    try {
      const canvas = document.querySelector('canvas');
      const drawn = integrationSurfaceDraws.get('explore.detail.workspace');
      if (!canvas || !drawn || drawn.sourceRevision !== source || drawn.presentationRevision !== presentation) {
        completed(0, 0); return;
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
      completed(nonblack > 0 ? checksum : 0, blue);
    } catch (error) {
      report({event: 'integration.failure', control: 'explore.detail.workspace', detail: `Upscale canvas read: ${error}`, a:'0', b:'0', c:'0', d:'0'});
      completed(0, 0);
    }
  }));
}

export function mmltkIntegrationRenderedStyle(control, semantic, red, green, blue, alpha, width, height) {
  requestAnimationFrame(() => {
    const renderKey = ++integrationRenderKey;
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
    report({
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

let integrationFullscreenSettled = false;
export function mmltkIntegrationFullscreen(enabled) {
  integrationFullscreenSettled = false;
  const request = enabled ? document.documentElement.requestFullscreen() : document.exitFullscreen();
  request.then(() => {
    integrationFullscreenSettled = true;
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
    report({event: 'integration.atlas_window', control: 'explore.gallery.workspace',
      detail: enabled ? 'fullscreen' : 'restored', a: String(window.innerWidth),
      b: String(window.innerHeight), c: String(window.devicePixelRatio), d: String(Number(!!document.fullscreenElement))});
  }).catch(error => report({event: 'integration.failure', control: 'explore.gallery.workspace',
    detail: String(error) + '; visibility=' + document.visibilityState +
      '; focused=' + document.hasFocus() + '; enabled=' + document.fullscreenEnabled +
      '; activation=' + (navigator.userActivation?.isActive ?? 'unavailable'),
    a: '0', b: '0', c: '0', d: '0'}));
}
export function mmltkIntegrationFullscreenSettled(enabled) {
  return integrationFullscreenSettled && !!document.fullscreenElement === enabled;
}

export function mmltkIntegrationClick(x, y) {
  const canvas = document.querySelector('canvas');
  if (!canvas || !Number.isFinite(x) || !Number.isFinite(y)) return 0;
  const rect = canvas.getBoundingClientRect();
  queueMicrotask(() => integrationClick(canvas, rect, x, y));
  return 1;
}

export function mmltkIntegrationClickAfterSurfaceDraw(x, y, control, sourceRevision, allowNewer) {
  if (!document.querySelector('canvas') || !Number.isFinite(x) || !Number.isFinite(y) ||
      typeof control !== 'string' || control.length === 0 ||
      !Number.isSafeInteger(sourceRevision) || sourceRevision <= 0) return 0;
  const pending = {x, y, control, sourceRevision, allowNewer};
  if (matchesIntegrationSurfaceClick(pending, integrationSurfaceDraws.get(control))) {
    dispatchIntegrationSurfaceClick(pending);
  } else {
    integrationPendingSurfaceClick = pending;
  }
  return 1;
}

export function mmltkIntegrationSweep(x, y, width, height) {
  const canvas = document.querySelector('canvas');
  if (!canvas || ![x, y, width, height].every(Number.isFinite) || width <= 0 || height <= 0) return 0;
  const rect = canvas.getBoundingClientRect();
  queueMicrotask(() => {
    report({
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
    report({
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
  queueMicrotask(() => {
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
  queueMicrotask(() => {
    canvas.dispatchEvent(integrationPointer(rect, x + width * positions[0], localY, 'pointermove', 0));
    canvas.dispatchEvent(integrationPointer(rect, x + width * positions[0], localY, 'pointerdown', 1));
    report({
      event: 'integration.ui_scale_pointer',
      control: 'settings.ui_scale',
      detail: 'pressed',
      a: String(positions[0]),
      b: '1',
      c: String(canvas.width),
      d: String(rect.width),
    });
    requestAnimationFrame(() => {
      canvas.dispatchEvent(integrationPointer(rect, x + width * positions[1], localY, 'pointermove', 1));
      report({
        event: 'integration.ui_scale_pointer',
        control: 'settings.ui_scale',
        detail: 'moved-1',
        a: String(positions[1]),
        b: '1',
        c: String(canvas.width),
        d: String(rect.width),
      });
      requestAnimationFrame(() => {
        canvas.dispatchEvent(integrationPointer(rect, x + width * positions[2], localY, 'pointermove', 1));
        report({
          event: 'integration.ui_scale_pointer',
          control: 'settings.ui_scale',
          detail: 'moved-2',
          a: String(positions[2]),
          b: '1',
          c: String(canvas.width),
          d: String(rect.width),
        });
        requestAnimationFrame(() => {
          canvas.dispatchEvent(integrationPointer(rect, x + width * positions[2], localY, 'pointerup', 0));
          report({
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
  const observeKeyStage = (stage, result) => report({
    event: 'integration.number_key_stage',
    control: stage,
    detail: document.visibilityState,
    a: String(result),
    b: String(document.hasFocus()),
    c: String(document.activeElement === canvas),
    d: value,
  });
  report({
    event: 'integration.number_replace',
    control: '',
    detail: 'scheduled',
    a: String(x),
    b: String(y),
    c: value,
    d: String(document.activeElement === canvas),
  });
  queueMicrotask(() => {
    const pointerEvents = [
      integrationPointer(rect, x, y, 'pointermove', 0),
      integrationPointer(rect, x, y, 'pointerdown', 1),
      integrationPointer(rect, x, y, 'pointerup', 0),
    ];
    const pointerResult = pointerEvents.reduce(
      (total, event) => total + Number(canvas.dispatchEvent(event)) + Number(event.defaultPrevented),
      0,
    );
    report({
      event: 'integration.number_replace',
      control: '',
      detail: 'focused',
      a: String(pointerResult),
      b: String(document.activeElement === canvas),
      c: '0',
      d: '0',
    });
    observeKeyStage('awaiting-control-down', pointerResult);
    requestAnimationFrame(() => {
      let keyResult = integrationKey(canvas, 'keydown', 'Control', 'ControlLeft', true, false);
      observeKeyStage('control-down', keyResult);
      requestAnimationFrame(() => {
        keyResult += integrationKey(canvas, 'keydown', 'a', 'KeyA', true, false);
        keyResult += integrationKey(canvas, 'keyup', 'a', 'KeyA', true, false);
        observeKeyStage('select-all', keyResult);
        requestAnimationFrame(() => {
          keyResult += integrationKey(canvas, 'keyup', 'Control', 'ControlLeft', false, false);
          observeKeyStage('control-up', keyResult);
          requestAnimationFrame(() => {
            for (const character of value) {
              const code = character === '.' ? 'Period' :
                character === '-' ? 'Minus' :
                character === '+' ? 'Equal' :
                character === 'e' || character === 'E' ? 'KeyE' : `Digit${character}`;
              keyResult += integrationKey(canvas, 'keydown', character, code, false, false);
              keyResult += integrationKey(canvas, 'keyup', character, code, false, false);
            }
            report({
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

let integrationAnnotationPointerEnd = null;
export function mmltkIntegrationAnnotationRelease() {
  const end=integrationAnnotationPointerEnd;
  if(end){end.canvas.dispatchEvent(integrationPointer(end.rect,end.x,end.y,'pointerup',0));integrationAnnotationPointerEnd=null;}
}
export function mmltkIntegrationAnnotationPointer(x, y, width, height, startX, startY, endX, endY, hold) {
  const canvas = document.querySelector('canvas');
  if (!canvas || ![x, y, width, height].every(Number.isFinite) || width <= 0 || height <= 0) return 0;
  const rect = canvas.getBoundingClientRect();
  const x0 = x + width * startX;
  const y0 = y + height * startY;
  const x1 = x + width * endX;
  const y1 = y + height * endY;
  queueMicrotask(() => {
    canvas.dispatchEvent(integrationPointer(rect, x0, y0, 'pointermove', 0));
    canvas.dispatchEvent(integrationPointer(rect, x0, y0, 'pointerdown', 1));
    canvas.dispatchEvent(integrationPointer(rect, x1, y1, 'pointermove', 1));
    if(hold) integrationAnnotationPointerEnd={canvas,rect,x:x1,y:y1};
    else canvas.dispatchEvent(integrationPointer(rect, x1, y1, 'pointerup', 0));
  });
  return 1;
}

export function mmltkIntegrationWindowClose() {
  queueMicrotask(() => requestAnimationFrame(() => requestAnimationFrame(() => window.close())));
  return 1;
}
"#)]
extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationBoundaryPixels)]
    fn boundary_pixels_js(
        points: &[f32],
        fields: &str,
        control: &str,
        source: f64,
        presentation: f64,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAtlasComposition)]
    fn atlas_composition_js(
        points: &[f32],
        cards: &[u32],
        fields: &str,
        source: f64,
        presentation: f64,
        columns: u32,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAnnotationSwatch)]
    fn annotation_swatch_js(
        css_bounds: &[f64],
        color: &[f64],
        control: &str,
        detail: &str,
        button: bool,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAnnotationPixels)]
    fn annotation_pixels_js(
        css_bounds: &[f64],
        extent: &[f64],
        probes: &[f64],
        source: f64,
        presentation: f64,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAtlasPixels)]
    fn atlas_pixels_js(
        rectangles: &[f32],
        source_revision: f64,
        presentation_revision: f64,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFullscreen)]
    fn fullscreen_js(enabled: bool);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFullscreenSettled)]
    fn fullscreen_settled_js(enabled: bool) -> bool;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationReport)]
    fn report_js(event: &str, control: &str, detail: &str, a: f64, b: f64, c: f64, d: f64);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationUpscalePixels)]
    fn upscale_pixels_js(
        image_pixels: &[f32],
        button_css: &[f32],
        source: f64,
        presentation: f64,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationRenderedStyle)]
    fn rendered_style_js(
        control: &str,
        semantic: &str,
        red: f64,
        green: f64,
        blue: f64,
        alpha: f64,
        width: f64,
        height: f64,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationClick)]
    fn click_js(x: f64, y: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationClickAfterSurfaceDraw)]
    fn click_after_surface_draw_js(
        x: f64,
        y: f64,
        control: &str,
        source_revision: f64,
        allow_newer: bool,
    ) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationSweep)]
    fn sweep_js(x: f64, y: f64, width: f64, height: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationWheel)]
    fn wheel_js(x: f64, y: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationSliderDrag)]
    fn slider_drag_js(x: f64, y: f64, width: f64, height: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationReplaceNumber)]
    fn replace_number_js(x: f64, y: f64, value: &str, selection_length: u32) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAnnotationPointer)]
    fn annotation_pointer_js(
        x: f64,
        y: f64,
        width: f64,
        height: f64,
        start_x: f64,
        start_y: f64,
        end_x: f64,
        end_y: f64,
        hold: bool,
    ) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAnnotationRelease)]
    fn annotation_release_js();
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationWindowClose)]
    fn window_close_js() -> u32;
}

pub(crate) fn sample_boundary_pixels(
    surface: crate::presentation_surface::Surface,
    control: &str,
    image: Rectangle,
    clip: Rectangle,
) {
    #[cfg(target_arch = "wasm32")]
    {
        if !crate::presentation_surface::pixel_trace::enabled() {
            return;
        }
        let Some(frame) = surface.frame else {
            return;
        };
        let [crop_x, crop_y, crop_width, crop_height] = surface.content_region();
        if crop_width == 0 || crop_height == 0 {
            return;
        }
        let coordinate = |index: usize, size: u32| {
            [
                0,
                191.min(size - 1),
                383.min(size - 1),
                (size - 1) / 2,
                size - 1,
            ][index]
        };
        let mut points = [0.0f32; 25 * 5];
        let mut count = 0;
        for index in 0..25 {
            let x = coordinate(index % 5, frame.content_width);
            let y = coordinate(index / 5, frame.content_height);
            if x < crop_x || y < crop_y || x >= crop_x + crop_width || y >= crop_y + crop_height {
                continue;
            }
            let screen = iced::Point::new(
                image.x + (x - crop_x) as f32 * image.width / crop_width as f32,
                image.y + (y - crop_y) as f32 * image.height / crop_height as f32,
            );
            let Some(visible) = clip.intersection(&image) else {
                continue;
            };
            if screen.x < visible.x + 2.0
                || screen.y < visible.y + 2.0
                || screen.x >= visible.x + visible.width - 2.0
                || screen.y >= visible.y + visible.height - 2.0
            {
                continue;
            }
            points[count..count + 5].copy_from_slice(&[
                x as f32,
                y as f32,
                screen.x,
                screen.y,
                index as f32,
            ]);
            count += 5;
        }
        boundary_pixels_js(
            &points[..count],
            &format!(
                "{{{}}}",
                crate::presentation_surface::surface_trace_fields(surface, surface)
            ),
            control,
            frame.content_sequence as f64,
            frame.presentation_revision as f64,
        );
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (surface, control, image, clip);
}

fn sample_atlas_composition(draw: &AtlasDraw) {
    #[cfg(target_arch = "wasm32")]
    {
        if !crate::presentation_surface::pixel_trace::enabled() {
            return;
        }
        let snapshot = &draw.snapshot;
        if !pixel_fixture_enabled()
            || snapshot.augmentation.enabled
            || snapshot.overlay.showlabels
            || !snapshot.overlay.showmasks
            || !snapshot.overlay.showboxes
            || !matches!(snapshot.viewport.columns, 4 | 10)
            || snapshot.gallery.slots.iter().any(|ready| !*ready)
        {
            return;
        }
        let Some(frame) = draw.surface.frame else {
            return;
        };
        // The fixture is a constant clean image and a rectangular mask with a
        // central hole. Four isolated samples exclude all label geometry.
        let mut points = [0.0f32; 256 * 4 * 10];
        let mut count = 0;
        let mut cards = [0u32; 256];
        let mut card_count = 0;
        let columns = snapshot.viewport.columns.max(1);
        let side = frame.content_width as f32 / columns as f32;
        for label in &snapshot.labels {
            let slot = (label.box_.first.y / side) as usize * columns as usize
                + (label.box_.first.x / side) as usize;
            if snapshot.order.visibleindices.get(slot) != Some(&label.compiledindex) {
                continue;
            }
            if !snapshot.gallery.slots.get(slot).copied().unwrap_or(false) {
                continue;
            }
            let card_origin = iced::Point::new(
                draw.image.x
                    + (slot % columns as usize) as f32 * side * draw.image.width
                        / frame.content_width as f32,
                draw.image.y
                    + (slot / columns as usize) as f32 * side * draw.image.height
                        / frame.content_height as f32,
            );
            let card_end = iced::Point::new(
                card_origin.x + side * draw.image.width / frame.content_width as f32 - 1.0,
                card_origin.y + side * draw.image.height / frame.content_height as f32 - 1.0,
            );
            // Eligibility is independent of whether individual probes succeed:
            // all ready annotated cards fully inside the visible clip.
            if !draw.clip.contains(card_origin) || !draw.clip.contains(card_end) {
                continue;
            }
            let Some(color) = snapshot.dataset.palette.get(label.category as usize) else {
                continue;
            };
            let color = crate::presentation_surface::labels::class_color(color);
            let rgb = [color.r, color.g, color.b].map(|value| (value * 255.0).round());
            let width = label.box_.second.x - label.box_.first.x;
            let height = label.box_.second.y - label.box_.first.y;
            if cards[..card_count].contains(&label.compiledindex) {
                continue;
            }
            if card_count == cards.len() {
                return;
            }
            cards[card_count] = label.compiledindex;
            card_count += 1;
            let start = count;
            for (kind, (relative_x, relative_y)) in
                [(0.2, 0.7), (0.5, 0.5), (0.0, 0.7), (-0.25, 0.7)]
                    .into_iter()
                    .enumerate()
            {
                if count + 10 > points.len() {
                    break;
                }
                let x = (label.box_.first.x + width * relative_x).floor()
                    + if kind == 2 { -0.5 } else { 0.5 };
                let y = (label.box_.first.y + height * relative_y).floor() + 0.5;
                let screen = iced::Point::new(
                    draw.image.x + x * draw.image.width / frame.content_width as f32,
                    draw.image.y + y * draw.image.height / frame.content_height as f32,
                );
                if !draw.clip.contains(screen) {
                    continue;
                }
                let base = [48.0, 80.0, 112.0];
                // Canvas pixel centers generally do not coincide with source
                // texel centers. Follow the existing linear sampler, including
                // the one-pixel stroke's clean/mask neighbours.
                let texel_x = (screen.x.floor() + 0.5 - draw.image.x) * frame.content_width as f32
                    / draw.image.width
                    - 0.5;
                let texel_y = (screen.y.floor() + 0.5 - draw.image.y) * frame.content_height as f32
                    / draw.image.height
                    - 0.5;
                let alpha_at = |px: f32, py: f32| {
                    let left = label.box_.first.x.floor() - 1.0;
                    let top = label.box_.first.y.floor() - 1.0;
                    let right = label.box_.second.x.ceil();
                    let bottom = label.box_.second.y.ceil();
                    if ((px == left || px == right) && py >= top && py <= bottom)
                        || ((py == top || py == bottom) && px >= left && px <= right)
                    {
                        return 1.0;
                    }
                    let center_x = px + 0.5;
                    let center_y = py + 0.5;
                    let inside = center_x >= label.box_.first.x
                        && center_x < label.box_.second.x
                        && center_y >= label.box_.first.y
                        && center_y < label.box_.second.y;
                    let hole = center_x >= label.box_.first.x + width * 0.375
                        && center_x < label.box_.first.x + width * 0.625
                        && center_y >= label.box_.first.y + height * 0.375
                        && center_y < label.box_.first.y + height * 0.625;
                    if inside && !hole { 0.36 } else { 0.0 }
                };
                let tx = texel_x.fract();
                let ty = texel_y.fract();
                let expected: [f32; 3] = std::array::from_fn(|channel| {
                    let color_at = |dx: f32, dy: f32| {
                        let alpha = alpha_at(texel_x.floor() + dx, texel_y.floor() + dy);
                        (base[channel] * (1.0 - alpha) + rgb[channel] * alpha).round()
                    };
                    ((color_at(0.0, 0.0) * (1.0 - tx) + color_at(1.0, 0.0) * tx) * (1.0 - ty)
                        + (color_at(0.0, 1.0) * (1.0 - tx) + color_at(1.0, 1.0) * tx) * ty)
                        .round()
                });
                points[count..count + 10].copy_from_slice(&[
                    x,
                    y,
                    screen.x,
                    screen.y,
                    expected[0],
                    expected[1],
                    expected[2],
                    255.0,
                    kind as f32,
                    label.compiledindex as f32,
                ]);
                count += 10;
            }
            if count - start != 40 {
                return;
            }
        }
        let Some(mut output) =
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
        else {
            return;
        };
        let receipt = draw.clone();
        let completed = pixel_result_callback(move |expected, matched| {
            let _ = output.try_send(Message::AtlasComposition {
                receipt,
                expected,
                matched,
            });
        });
        atlas_composition_js(
            &points[..count],
            &cards[..card_count],
            &format!(
                "{{{},\"image_x\":{},\"image_y\":{},\"image_width\":{},\"image_height\":{}}}",
                crate::presentation_surface::surface_trace_fields(draw.surface, draw.surface),
                draw.image.x,
                draw.image.y,
                draw.image.width,
                draw.image.height
            ),
            frame.content_sequence as f64,
            frame.presentation_revision as f64,
            snapshot.viewport.columns,
            &completed,
        );
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = draw;
}

fn pixel_fixture_enabled() -> bool {
    #[cfg(target_arch = "wasm32")]
    {
        thread_local! {
            static FIXTURE: bool = web_sys::window()
                .and_then(|window| window.location().search().ok())
                .and_then(|search| web_sys::UrlSearchParams::new_with_str(&search).ok())
                .and_then(|params| params.get("mmltk_integration_pixel_fixture"))
                .is_some_and(|value| value == "1");
        }
        FIXTURE.with(|enabled| *enabled)
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        false
    }
}

#[cfg(target_arch = "wasm32")]
fn pixel_result_callback(completed: impl FnOnce(u32, u32) + 'static) -> wasm_bindgen::JsValue {
    // Keep the JavaScript boundary explicit: the optimized bindgen adapter can
    // share integer closure shims with externref closures.
    wasm_bindgen::closure::Closure::once_into_js(
        move |first: wasm_bindgen::JsValue, second: wasm_bindgen::JsValue| {
            let counts = [first, second].map(|value| {
                value.as_f64().filter(|number| {
                    number.is_finite()
                        && *number >= 0.0
                        && *number <= f64::from(u32::MAX)
                        && number.fract() == 0.0
                })
            });
            if let [Some(first), Some(second)] = counts {
                completed(first as u32, second as u32);
            } else {
                report(
                    "integration.failure",
                    "",
                    "pixel callback returned invalid counts",
                    [0.0; 4],
                );
                completed(0, 0);
            }
        },
    )
}

#[cfg(target_arch = "wasm32")]
fn sample_upscale_pixels(
    image_pixels: Rectangle,
    button_css: Rectangle,
    source: u64,
    presentation: u64,
) {
    let completed = pixel_result_callback(move |checksum, blue| {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            if let Some(output) = observer.borrow_mut().output.as_mut() {
                let _ = output.try_send(Message::UpscalePixels {
                    source,
                    presentation,
                    checksum,
                    blue,
                });
            }
        });
    });
    upscale_pixels_js(
        &[
            image_pixels.x,
            image_pixels.y,
            image_pixels.width,
            image_pixels.height,
        ],
        &[
            button_css.x + 4.0,
            button_css.y + 4.0,
            (button_css.width - 8.0).max(1.0),
            (button_css.height - 8.0).max(1.0),
        ],
        source as f64,
        presentation as f64,
        &completed,
    );
}

#[cfg(not(target_arch = "wasm32"))]
fn sample_upscale_pixels(_image: Rectangle, _button: Rectangle, _source: u64, _presentation: u64) {}

#[cfg(target_arch = "wasm32")]
fn report(event: &str, control: &str, detail: &str, values: [f64; 4]) {
    report_js(
        event, control, detail, values[0], values[1], values[2], values[3],
    );
}

#[cfg(not(target_arch = "wasm32"))]
fn report(_event: &str, _control: &str, _detail: &str, _values: [f64; 4]) {}

pub(crate) fn report_snapshot_conflict(family: &str, revision: u64, fields: &str) {
    report(
        "integration.snapshot_conflict",
        family,
        fields,
        [revision as f64, 0.0, 0.0, 0.0],
    );
}

#[cfg(target_arch = "wasm32")]
fn report_rendered_control_style(
    control: &str,
    semantic: &str,
    color: [f64; 4],
    bounds: Rectangle,
) {
    rendered_style_js(
        control,
        semantic,
        color[0],
        color[1],
        color[2],
        color[3],
        f64::from(bounds.width),
        f64::from(bounds.height),
    );
}

#[cfg(not(target_arch = "wasm32"))]
fn report_rendered_control_style(
    _control: &str,
    _semantic: &str,
    _color: [f64; 4],
    _bounds: Rectangle,
) {
}

#[derive(Debug, Clone, PartialEq)]
pub struct AtlasDraw {
    pub surface: crate::presentation_surface::Surface,
    pub snapshot: std::sync::Arc<crate::generated::ExploreSnapshot>,
    pub bounds: Rectangle,
    pub image: Rectangle,
    pub clip: Rectangle,
}

pub(crate) fn report_atlas_draw(draw: AtlasDraw, dark: bool, scale: f32) {
    let snapshot = draw.snapshot.clone();
    let frame = draw.surface.frame.expect("encoded atlas frame");
    let image = draw.image;
    let clip = image
        .intersection(&draw.clip)
        .expect("visible encoded atlas");
    let visibility = u8::from(snapshot.overlay.showboxes)
        | (u8::from(snapshot.overlay.showmasks) << 1)
        | (u8::from(snapshot.overlay.showlabels) << 2);
    report(
        "integration.atlas_geometry",
        EXPLORE_GALLERY,
        "uniform-square",
        [
            frame.presentation_revision as f64,
            frame.content_sequence as f64,
            (image.width / snapshot.viewport.columns.max(1) as f32) as f64,
            (image.height / snapshot.viewport.rowcount.max(1) as f32) as f64,
        ],
    );
    report(
        "integration.atlas_scale",
        EXPLORE_GALLERY,
        "source-to-screen",
        [
            frame.presentation_revision as f64,
            frame.content_sequence as f64,
            (image.width / frame.content_width.max(1) as f32) as f64,
            (image.height / frame.content_height.max(1) as f32) as f64,
        ],
    );
    report(
        "integration.atlas_clip",
        EXPLORE_GALLERY,
        "scrollable-clip",
        [
            frame.presentation_revision as f64,
            frame.content_sequence as f64,
            (clip.y - image.y) as f64,
            (image.y + image.height - clip.y - clip.height) as f64,
        ],
    );
    report(
        "integration.atlas_visibility",
        EXPLORE_GALLERY,
        if dark { "dark" } else { "light" },
        [
            frame.content_sequence as f64,
            visibility as f64,
            snapshot
                .gallery
                .slots
                .iter()
                .filter(|ready| **ready)
                .count() as f64,
            snapshot.gallery.slots.len() as f64,
        ],
    );
    let new_draw = SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let same_rendered_draw = observer.atlas.as_ref().is_some_and(|previous| {
            previous.surface == draw.surface
                && previous.bounds == draw.bounds
                && previous.image == draw.image
                && previous.clip == draw.clip
                && previous.snapshot.overlay == draw.snapshot.overlay
        });
        if !same_rendered_draw
            && observer.output.as_mut().is_some_and(|output| {
                output
                    .try_send(Message::AtlasDrawn {
                        source_revision: frame.content_sequence,
                        visibility,
                        clipped_top: clip.y - image.y,
                        clipped_bottom: image.y + image.height - clip.y - clip.height,
                        row_extent: image.width / snapshot.viewport.columns.max(1) as f32 / scale,
                        receipt: draw.clone(),
                    })
                    .is_ok()
            })
        {
            observer.atlas = Some(draw.clone());
            true
        } else {
            false
        }
    });
    if new_draw {
        sample_atlas_composition(&draw);
    }
    if new_draw
        && (COMPLETION_WITHOUT_INPUT.with(std::cell::Cell::get) || pixel_fixture_enabled())
        && !snapshot.gallery.slots.is_empty()
        && snapshot.gallery.slots.iter().all(|ready| *ready)
    {
        sample_atlas_pixels(draw);
    }
}

#[cfg(target_arch = "wasm32")]
fn sample_atlas_pixels(draw: AtlasDraw) {
    let rectangles = atlas_pixel_rectangles(&draw);
    let frame = draw.surface.frame.expect("drawn atlas publication");
    let Some(mut output) = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
    else {
        return;
    };
    let completed = pixel_result_callback(move |visible, nonblack| {
        let _ = output.try_send(Message::AtlasPixels {
            receipt: draw,
            visible,
            nonblack,
        });
    });
    atlas_pixels_js(
        &rectangles,
        frame.content_sequence as f64,
        frame.presentation_revision as f64,
        &completed,
    );
}

#[cfg(not(target_arch = "wasm32"))]
fn sample_atlas_pixels(_draw: AtlasDraw) {}

#[cfg(any(target_arch = "wasm32", test))]
fn atlas_pixel_rectangles(draw: &AtlasDraw) -> Vec<f32> {
    let snapshot = &draw.snapshot;
    let columns = snapshot.viewport.columns.max(1);
    let side = draw.image.width / columns as f32;
    let mut rectangles = Vec::with_capacity(snapshot.gallery.slots.len() * 4);
    for (slot, ready) in snapshot.gallery.slots.iter().enumerate() {
        if !*ready {
            continue;
        }
        // Interior samples exclude the grid and card boundary. Intersect with
        // the real draw clip so an offscreen tile cannot satisfy acceptance.
        let tile = Rectangle {
            x: draw.image.x + (slot as u32 % columns) as f32 * side + side * 0.2,
            y: draw.image.y + (slot as u32 / columns) as f32 * side + side * 0.2,
            width: side * 0.6,
            height: side * 0.6,
        };
        if let Some(visible) = tile.intersection(&draw.clip)
            && visible.width >= 2.0
            && visible.height >= 2.0
        {
            rectangles.extend([visible.x, visible.y, visible.width, visible.height]);
        }
    }
    rectangles
}

pub(crate) fn report_surface_draw(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    redraw: bool,
    content_width: u32,
    content_height: u32,
    count: u64,
    viewer: ViewerDraw,
) {
    report(
        "integration.surface_draw",
        control,
        if redraw { "redraw" } else { "draw" },
        [
            revision as f64,
            source_revision as f64,
            content_width as f64,
            content_height as f64,
        ],
    );
    if control == explore::DETAIL_WORKSPACE_ID {
        report(
            "integration.viewer_sample",
            control,
            "actual-draw",
            [
                viewer.crop[2] as f64,
                viewer.crop[3] as f64,
                viewer.image.width as f64,
                viewer.image.height as f64,
            ],
        );
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let identity = (revision, source_revision, viewer);
            if observer.viewer != Some(identity)
                && let Some(output) = observer.output.as_mut()
            {
                if output
                    .try_send(Message::SurfaceDrawn {
                        presentation_revision: revision,
                        source_revision,
                        viewer: Some(viewer),
                    })
                    .is_ok()
                {
                    observer.viewer = Some(identity);
                }
            }
        });
    }
    report(
        "integration.surface_draw_ordinal",
        control,
        if redraw { "redraw" } else { "draw" },
        [
            revision as f64,
            source_revision as f64,
            count as f64,
            if redraw { 1.0 } else { 0.0 },
        ],
    );
    if control == EXPLORE_GALLERY {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let identity = (revision, source_revision);
            if observer.gallery != Some(identity)
                && observer.output.as_mut().is_some_and(|output| {
                    output
                        .try_send(Message::GalleryDrawn {
                            presentation_revision: revision,
                            source_revision,
                        })
                        .is_ok()
                })
            {
                observer.gallery = Some(identity);
            }
        });
    }
    if control == crate::view::workspace::STABLE_ID {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            let identity = (revision, source_revision);
            if observer.identity != identity
                && observer.output.as_mut().is_some_and(|output| {
                    output
                        .try_send(Message::SurfaceDrawn {
                            presentation_revision: revision,
                            source_revision,
                            viewer: None,
                        })
                        .is_ok()
                })
            {
                observer.identity = identity;
            }
        });
    }
}

pub(crate) fn report_surface_sync(
    current: Option<crate::presentation_surface::Surface>,
    updated: Option<crate::presentation_surface::Surface>,
) {
    let identity = |surface: Option<crate::presentation_surface::Surface>| {
        surface.map_or_else(
            || "none".to_string(),
            |surface| format!("{:016x}{:016x}", surface.high, surface.low),
        )
    };
    report(
        "integration.surface_sync",
        &identity(current),
        &identity(updated),
        [
            current.map_or(0.0, |surface| surface.generation as f64),
            updated.map_or(0.0, |surface| surface.generation as f64),
            updated.map_or(0.0, |surface| surface.width as f64),
            updated.map_or(0.0, |surface| surface.height as f64),
        ],
    );
}

pub(crate) fn report_surface_geometry(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    geometry: Rectangle,
) {
    report(
        "integration.surface_geometry",
        control,
        "shader-viewport",
        [
            revision as f64,
            source_revision as f64,
            f64::from(geometry.width),
            f64::from(geometry.height),
        ],
    );
}

pub(crate) fn report_surface_container(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    bounds: Rectangle,
    content_width: u32,
    content_height: u32,
) {
    report(
        "integration.surface_container",
        control,
        "rendered-contain-container",
        [
            revision as f64,
            source_revision as f64,
            f64::from(bounds.width),
            f64::from(bounds.height),
        ],
    );
    report(
        "integration.surface_content",
        control,
        "exported-native-frame",
        [
            revision as f64,
            source_revision as f64,
            content_width as f64,
            content_height as f64,
        ],
    );
}

pub(crate) fn report_surface_scale(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    scale: f32,
) {
    report(
        "integration.surface_scale",
        control,
        "physical-to-logical",
        [
            revision as f64,
            source_revision as f64,
            f64::from(scale),
            f64::from(scale),
        ],
    );
}

pub(crate) fn report_annotation_gesture(detail: &str, values: [f64; 4]) {
    report(
        "integration.annotation_gesture",
        ANNOTATION_SURFACE,
        detail,
        values,
    );
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct SampleablePresentation {
    source_revision: u64,
    presentation_revision: u64,
    content_width: u32,
    content_height: u32,
    capability_width: u32,
    capability_height: u32,
}

fn sampleable_presentation(
    model: &ApplicationModel,
    frame: Option<crate::presentation_surface::FrameReady>,
    source: crate::generated::PresentationSourceKind,
    source_revision: u64,
) -> Option<SampleablePresentation> {
    let viewed = (source == crate::generated::PresentationSourceKind::Explore
        && model.explore.snapshot.as_ref().is_some_and(|snapshot| {
            snapshot.mode == crate::generated::ExploreMode::Detail
                && snapshot.frame.revision == source_revision
        }))
    .then(|| model.viewed_explore_frame())
    .flatten();
    let (source, source_revision) = viewed.as_ref().map_or((source, source_revision), |viewed| {
        (viewed.source.kind, viewed.revision)
    });
    let presentation = model.presentation.as_ref()?;
    let frame = frame?;
    (presentation.completed.source.kind == source
        && presentation.completed.revision == source_revision
        && frame.content_sequence == source_revision
        && presentation.timelineready != 0
        && presentation.presentationrevision == frame.presentation_revision)
        .then_some(SampleablePresentation {
            source_revision,
            presentation_revision: frame.presentation_revision,
            content_width: frame.content_width,
            content_height: frame.content_height,
            capability_width: presentation.capability.extent.width,
            capability_height: presentation.capability.extent.height,
        })
}

fn fully_drawn_gallery(
    model: &ApplicationModel,
    frame: Option<crate::presentation_surface::FrameReady>,
    snapshot: &crate::generated::ExploreSnapshot,
    drawn: Option<(u64, u64)>,
) -> Option<SampleablePresentation> {
    if !snapshot.ready
        || snapshot.busy
        || snapshot.mode != crate::generated::ExploreMode::Gallery
        || snapshot.gallery.generation == 0
        || snapshot.gallery.slots.is_empty()
        || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
        || snapshot.gallery.slots.iter().any(|ready| !*ready)
    {
        return None;
    }
    let sampleable = sampleable_presentation(
        model,
        frame,
        crate::generated::PresentationSourceKind::Explore,
        snapshot.frame.revision,
    )?;
    (drawn == Some((sampleable.presentation_revision, sampleable.source_revision)))
        .then_some(sampleable)
}

#[cfg(target_arch = "wasm32")]
fn click(bounds: Rectangle) -> bool {
    click_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
    ) == 1
}

#[cfg(target_arch = "wasm32")]
fn click_after_surface_draw(
    bounds: Rectangle,
    control: &str,
    source_revision: u64,
    allow_newer: bool,
) -> bool {
    click_after_surface_draw_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
        control,
        source_revision as f64,
        allow_newer,
    ) == 1
}

fn gallery_slot_bounds(bounds: Rectangle, columns: u32, slot: u32, clipped_top: f32) -> Rectangle {
    let columns = columns.max(1);
    let card_extent = bounds.width / columns as f32;
    Rectangle {
        x: bounds.x + (slot % columns) as f32 * card_extent,
        y: bounds.y - clipped_top + (slot / columns) as f32 * card_extent,
        width: card_extent,
        height: card_extent,
    }
}

#[cfg(target_arch = "wasm32")]
fn click_number_edge(bounds: Rectangle, upper: bool) -> bool {
    click_js(
        f64::from(bounds.x + bounds.width - 2.0),
        f64::from(bounds.y + if upper { 2.0 } else { bounds.height - 2.0 }),
    ) == 1
}

#[cfg(not(target_arch = "wasm32"))]
fn click_number_edge(_bounds: Rectangle, _upper: bool) -> bool {
    false
}

#[cfg(target_arch = "wasm32")]
fn wheel_number_input(bounds: Rectangle) -> bool {
    wheel_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
    ) == 1
}

#[cfg(target_arch = "wasm32")]
fn replace_number_input(bounds: Rectangle, value: &str, selection_length: usize) -> bool {
    let Ok(selection_length) = u32::try_from(selection_length) else {
        return false;
    };
    replace_number_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
        value,
        selection_length,
    ) == 1
}

#[cfg(not(target_arch = "wasm32"))]
fn replace_number_input(_bounds: Rectangle, _value: &str, _selection_length: usize) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
fn wheel_number_input(_bounds: Rectangle) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
fn click(_bounds: Rectangle) -> bool {
    false
}

#[cfg(not(target_arch = "wasm32"))]
fn click_after_surface_draw(
    _bounds: Rectangle,
    _control: &str,
    _source_revision: u64,
    _allow_newer: bool,
) -> bool {
    false
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Phase {
    Disabled,
    AtlasPixelColumns(u32),
    AtlasPixelRestore,
    ViewerConfirmSettings,
    ViewerRestoreSettings,
    ViewerDepart,
    ViewerReenter,
    ViewerAwaitDisconnect,
    ViewerReconnect,
    ViewerSquareBasic,
    AwaitBootstrap,
    SettingsOpen,
    AwaitSettings,
    SettingsModal,
    SettingsGroup(usize),
    SettingsScaleDrag,
    AwaitSettingsScaleDrag,
    AwaitSettingsScaleRelease,
    AwaitSettingsScaleSnapshot,
    AwaitSettingsScaleRestoreDraft,
    AwaitSettingsScaleRestored,
    SettingsShowFps,
    AwaitSettingsShowFps,
    AwaitSettingsShowFpsChangedSnapshot,
    SettingsRestoreShowFps,
    AwaitSettingsShowFpsRestored,
    AwaitSettingsShowFpsSnapshot,
    SettingsNumeric {
        index: usize,
        part: usize,
    },
    SettingsFooter,
    SettingsReset,
    SettingsClose,
    AwaitSettingsClosed,
    TrainNavigation,
    AwaitTrain,
    PageNavigation(FeatureId),
    AwaitPage(FeatureId),
    PageRegion {
        page: FeatureId,
        index: usize,
    },
    TrainModelCard,
    TrainModelPart(usize),
    TrainModelProgress,
    ReturnTrain,
    AwaitReturnTrain,
    AdvancedField(usize),
    AdvancedSpinnerEdge {
        index: usize,
        upper: bool,
    },
    AdvancedSpinnerVerify {
        index: usize,
        upper: bool,
    },
    AdvancedSpinnerWheel(usize),
    AwaitAdvancedSpinnerWheel(usize),
    AdvancedSpinnerWheelVerify(usize),
    AdvancedNumericEdit(usize),
    AwaitAdvancedNumericDraft(usize),
    AwaitAdvancedNumericSnapshot(usize),
    AdvancedAssignment,
    AwaitAdvancedAssignmentDraft,
    AwaitAdvancedAssignmentSnapshot,
    AdvancedMatchFree(usize),
    AdvancedDenoisingToggle,
    AwaitAdvancedDenoisingDraft,
    AwaitAdvancedDenoisingSnapshot,
    AdvancedDenoising(usize),
    TriggerError,
    AwaitErrorModal,
    ErrorModal,
    ErrorCopy,
    AwaitErrorCopy,
    ErrorDismiss,
    AwaitErrorDismissed,
    TrainCard,
    DatasetBrowse,
    BenchmarkOverride,
    AwaitBenchmarkOverride,
    AwaitBenchmarkChangedSnapshot,
    BenchmarkRestore,
    AwaitBenchmarkRestored,
    AwaitBenchmarkSnapshot,
    DatasetSource,
    AwaitDatasetSource,
    CompiledDirectory,
    AwaitCompiledDirectory,
    CompileDimensions,
    AwaitCompileDimensions,
    CompileResolution,
    AwaitCompileResolution,
    AwaitDatasetSettings(u64),
    Compile,
    AwaitCompileProgress,
    CompileProgress,
    CompileActionWithProgress,
    AwaitCompileCompletion,
    DatasetStatus,
    ExploreNavigation,
    AwaitExplore,
    ExploreOpen,
    AwaitExploreReady,
    AwaitExploreInitialPatch {
        revision: u64,
        frame_revision: u64,
    },
    AwaitExploreExactGrid(u64),
    AwaitExploreExactGridPatch {
        revision: u64,
        frame_revision: u64,
    },
    ExploreDatasetPane,
    ExploreDetailsPane,
    ExplorePolicyOrderReady,
    ExplorePolicyOrder(u64),
    AwaitExplorePolicyOrder(u64),
    ExplorePolicyRangeReady,
    ExplorePolicyRange(u64),
    ExplorePolicyRangeVisible(u64),
    AwaitExplorePolicyRange(u64),
    ExplorePolicyOverlayReady,
    ExplorePolicyOverlay(u64),
    ExplorePolicyOverlayVisible(u64),
    AwaitExplorePolicyOverlay(u64),
    AwaitExploreOverlayAll(u64),
    AwaitExploreOverlaySubset(u64),
    AwaitExploreOverlayRestored(u64),
    ExploreAugmentationToggle {
        revision: u64,
        frame_revision: u64,
    },
    AwaitExploreAugmentationToggle {
        revision: u64,
        frame_revision: u64,
    },
    ExploreAugmentationReroll {
        revision: u64,
        frame_revision: u64,
    },
    AwaitExploreAugmentationReroll {
        revision: u64,
        frame_revision: u64,
    },
    ExploreReshuffle {
        revision: u64,
        frame_revision: u64,
        shuffle_seed: u64,
        augmentation_seed: u64,
        order_signature: u64,
    },
    AwaitExploreReshuffle {
        revision: u64,
        frame_revision: u64,
        shuffle_seed: u64,
        augmentation_seed: u64,
        order_signature: u64,
    },
    ExploreCard {
        revision: u64,
        frame_revision: u64,
    },
    AwaitGalleryPatch {
        revision: u64,
        frame_revision: u64,
    },
    GallerySweep,
    AwaitGallerySweep,
    GalleryLaterReady,
    GalleryLater(u32),
    AwaitGalleryScroll(u32),
    GalleryImage(u32),
    AwaitDetail(u32),
    DetailOriginal {
        revision: u64,
        frame_revision: u64,
        padded_width: u32,
        padded_height: u32,
    },
    AwaitDetailOriginal {
        revision: u64,
        frame_revision: u64,
        padded_width: u32,
        padded_height: u32,
    },
    DetailFit,
    AwaitDetailFit,
    ViewerSelect,
    ViewerRapidGallery,
    ViewerRapidSelection(u64),
    AwaitAtlasCapacity,
    AtlasCapacity,
    AtlasRestoreColumns,
    AwaitAtlasColumns,
    AwaitAtlasEmpty,
    AtlasEmpty,
    AtlasRestoreFilter,
    AwaitAtlasRestored,
    AwaitAtlasWindow(bool),
    AwaitAtlasRows {
        rows: u32,
        next_stage: u8,
    },
    AwaitAtlasScroll(u8),
    AtlasOverlay(usize),
    AwaitAtlasOverlay(usize),
    ViewerOverlay(usize),
    AwaitViewerOverlay(usize),
    ViewerNoAspect,
    CopyAwaitObject {
        index: u16,
        mask: bool,
    },
    CopyUndo {
        index: u16,
        mask: bool,
    },
    CopyAwaitUndo {
        index: u16,
        mask: bool,
    },
    CopyRedo {
        index: u16,
        mask: bool,
    },
    CopyAwaitRedo {
        index: u16,
        mask: bool,
    },
    CopyAwaitClass {
        index: u16,
    },
    CopyLayout(u8),
    CopySwatchWait,
    CopyCapability,
    CopyCapabilityWait,
    CopyAwaitScale(bool),
    CopyProductStart,
    CopyProductWait,
    CopyAwaitOutput,
    CopySave,
    CopyAwaitSave,
    StartUpscale {
        kernel: usize,
        source_width: u32,
        source_height: u32,
        upscale_revision: u64,
        upscale_frame_revision: u64,
        presentation_revision: u64,
    },
    AwaitUpscale {
        kernel: usize,
        source_width: u32,
        source_height: u32,
        upscale_revision: u64,
        upscale_frame_revision: u64,
        presentation_revision: u64,
    },
    DetailNext(u32),
    AwaitNext(u32),
    DetailPrevious(u32),
    AwaitPrevious(u32),
    DetailCloseEvidence,
    AwaitDetailClose,
    ExploreDatasetReopen {
        revision: u64,
        frame_revision: u64,
    },
    AwaitExploreDatasetReopen {
        revision: u64,
        frame_revision: u64,
    },
    GalleryReselect,
    AwaitDetailAgain,
    OpenAnnotation,
    AwaitAnnotation,
    AnnotationSidebar {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AnnotationTimeline {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AnnotationOperation {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AnnotationStop {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AnnotationBrush {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AnnotationTool {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AwaitTool {
        revision: u64,
        tool: crate::generated::AnnotationTool,
    },
    AnnotationSurface(u64),
    AwaitAnnotationFrame(u64),
    AnnotationPointer(u64),
    AwaitPointer(u64),
    Complete,
    Failed,
}

impl Phase {
    fn deadline_class(&self) -> &'static str {
        match self {
            Self::AwaitBootstrap => "startup",
            Self::AwaitCompileProgress
            | Self::CompileProgress
            | Self::CompileActionWithProgress
            | Self::AwaitCompileCompletion
            | Self::AwaitExploreReady
            | Self::AwaitExploreInitialPatch { .. }
            | Self::AwaitExploreExactGrid(_)
            | Self::AwaitExploreExactGridPatch { .. }
            | Self::AwaitExplorePolicyOrder(_)
            | Self::AwaitExplorePolicyRange(_)
            | Self::AwaitExplorePolicyOverlay(_)
            | Self::AwaitExploreOverlayAll(_)
            | Self::AwaitExploreOverlaySubset(_)
            | Self::AwaitExploreOverlayRestored(_)
            | Self::AwaitExploreAugmentationToggle { .. }
            | Self::AwaitExploreAugmentationReroll { .. }
            | Self::AwaitExploreReshuffle { .. }
            | Self::AwaitGalleryPatch { .. }
            | Self::GallerySweep
            | Self::AwaitGallerySweep
            | Self::GalleryLaterReady
            | Self::AwaitGalleryScroll(_)
            | Self::AwaitDetail(_)
            | Self::AwaitDetailOriginal { .. }
            | Self::AwaitDetailFit
            | Self::AwaitAtlasColumns
            | Self::AwaitAtlasEmpty
            | Self::AwaitAtlasRestored
            | Self::AwaitAtlasWindow(_)
            | Self::AwaitAtlasRows { .. }
            | Self::AwaitAtlasScroll(_)
            | Self::AwaitAtlasOverlay(_)
            | Self::AwaitViewerOverlay(_)
            | Self::AwaitNext(_)
            | Self::AwaitPrevious(_)
            | Self::AwaitDetailClose
            | Self::AwaitExploreDatasetReopen { .. }
            | Self::AwaitDetailAgain
            | Self::AwaitAnnotation
            | Self::AwaitAnnotationFrame(_)
            | Self::AwaitPointer(_)
            | Self::CopyCapabilityWait
            | Self::CopyProductWait
            | Self::CopyAwaitOutput
            | Self::ViewerAwaitDisconnect
            | Self::ViewerReconnect
            | Self::StartUpscale { .. }
            | Self::AwaitUpscale { .. }
            | Self::Complete => "work",
            _ => "interaction",
        }
    }
}

fn region_id(page: FeatureId, index: usize) -> &'static str {
    let composition = crate::view::workflow::Composition::new(page, 0.0);
    composition.stable_id(composition.audit_regions()[index])
}

fn settings_numeric_id(index: usize, part: usize) -> String {
    let control = SETTINGS_NUMERIC_CONTROLS[index];
    match part {
        0 => control.to_owned(),
        1 => format!("{control}.label"),
        2 => format!("{control}.value"),
        _ => unreachable!("Settings numeric audit has three rendered identities"),
    }
}

fn advanced_field_id(index: usize) -> String {
    let constraint = match index {
        0 => crate::generated::constraint_workflowstrainrequestbatchsize(),
        1 => crate::generated::constraint_workflowstrainrequestvalbatchsize(),
        2 => crate::generated::constraint_workflowstrainrequestepochs(),
        3 => crate::generated::constraint_workflowstrainrequestgradaccumsteps(),
        4 => crate::generated::constraint_workflowstrainrequestlr(),
        5 => crate::generated::constraint_workflowstrainrequestlrencoder(),
        6 => crate::generated::constraint_workflowstrainrequestweightdecay(),
        7 => crate::generated::constraint_workflowstrainrequestmomentum(),
        _ => unreachable!("Advanced has eight fixed numeric controls"),
    };
    constraint.stable_field_id.to_string()
}

fn match_free_field_id(index: usize) -> String {
    let constraint = match index {
        0 => crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreerho(),
        1 => crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreecorrespondenceweight(),
        2 => crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreequeryweight(),
        _ => unreachable!("Match-Free has three numeric controls"),
    };
    constraint.stable_field_id.to_string()
}

fn denoising_field_id(index: usize) -> String {
    let constraint = match index {
        0 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinggroups(),
        1 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinglabelnoiseratio(),
        2 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingcenternoisescale(),
        3 => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingsizenoisescale(),
        _ => unreachable!("DN has four numeric controls"),
    };
    constraint.stable_field_id.to_string()
}

fn advanced_control_value(
    settings: &crate::view::settings::SettingsModel,
    index: usize,
    fallback: f64,
) -> f64 {
    settings.draft.as_ref().map_or(fallback, |draft| {
        if index == 2 {
            f64::from(draft.workflows.train.request.epochs)
        } else {
            crate::generated::effective_workflowstrainrequestlr(&draft.workflows.train.request)
        }
    })
}

fn advanced_snapshot_value(model: &ApplicationModel, index: usize) -> Option<f64> {
    model.settings_snapshot.as_ref().map(|snapshot| {
        if index == 2 {
            f64::from(snapshot.settingsstate.workflows.train.request.epochs)
        } else {
            crate::generated::effective_workflowstrainrequestlr(
                &snapshot.settingsstate.workflows.train.request,
            )
        }
    })
}

fn numeric_edit_target(
    settings: &crate::view::settings::SettingsModel,
    index: usize,
) -> Option<(f64, String, usize)> {
    let draft = settings.draft.as_ref()?;
    let (current_text, target, target_text) = if index == 2 {
        let current = draft.workflows.train.request.epochs;
        let constraint = crate::generated::constraint_workflowstrainrequestepochs();
        let minimum = constraint.minimum.map_or(i32::MIN, |value| value as i32);
        let maximum = constraint.maximum.map_or(i32::MAX, |value| value as i32);
        let target = if current < maximum {
            current + 1
        } else if current > minimum {
            current - 1
        } else {
            return None;
        };
        (current.to_string(), f64::from(target), target.to_string())
    } else {
        let current =
            crate::generated::effective_workflowstrainrequestlr(&draft.workflows.train.request);
        let constraint = crate::generated::constraint_workflowstrainrequestlr();
        let minimum = constraint.minimum.unwrap_or(f64::MIN);
        let maximum = constraint.maximum.unwrap_or(f64::MAX);
        let step = 0.0001;
        let target = if minimum.is_finite() && current != minimum {
            minimum
        } else if current + step <= maximum {
            current + step
        } else if current - step >= minimum {
            current - step
        } else {
            return None;
        };
        (current.to_string(), target, target.to_string())
    };
    let selection_length = current_text.chars().count();
    (!target_text.is_empty() && selection_length > 0).then_some((
        target,
        target_text,
        selection_length,
    ))
}

fn same_numeric_value(left: f64, right: f64) -> bool {
    (left - right).abs() <= f64::EPSILON * left.abs().max(right.abs()).max(1.0) * 8.0
}

fn annotation_compact_scale(current_scale: f32, logical_width: f32) -> Result<f32, &'static str> {
    let constraint = crate::generated::constraint_uiuiscale();
    let (minimum, maximum) = constraint
        .minimum
        .zip(constraint.maximum)
        .ok_or("Annotation compact scale requires native bounds")?;
    let (minimum, maximum) = (minimum as f32, maximum as f32);
    if !constraint.finite
        || !minimum.is_finite()
        || !maximum.is_finite()
        || minimum <= 0.0
        || maximum < minimum
        || !current_scale.is_finite()
        || current_scale <= 0.0
        || !logical_width.is_finite()
        || logical_width <= 0.0
    {
        return Err("Annotation compact scale has invalid bounds or viewport dimensions");
    }
    let unscaled_width = current_scale * logical_width;
    // Leave a small margin inside the component's single layout breakpoint.
    let scale = (unscaled_width / (annotation::COMPACT_BREAKPOINT * 0.98)).clamp(minimum, maximum);
    if !unscaled_width.is_finite() || unscaled_width / scale >= annotation::COMPACT_BREAKPOINT {
        return Err("Native UI-scale bounds cannot reach the compact Annotation layout");
    }
    Ok(scale)
}

fn route_edit_available(
    model: &ApplicationModel,
    settings: &crate::view::settings::SettingsModel,
) -> bool {
    !settings.has_local_edits() && model.settings_edit_available()
}

fn report_advanced_field(control: &str, detail: &str, bounds: Rectangle) {
    report(
        "integration.advanced_field",
        control,
        detail,
        [
            f64::from(bounds.x),
            f64::from(bounds.y),
            f64::from(bounds.width),
            f64::from(bounds.height),
        ],
    );
}

pub struct Controller {
    phase: Phase,
    reported_phase: Option<Phase>,
    location_pending: bool,
    window_close: bool,
    dataset_source: String,
    compiled_directory: String,
    resolution: String,
    viewer_scenario: String,
    annotation_probe: Option<(u64, u64, u32, u32, Vec<f64>)>,
    annotation_pixels_revision: u64,
    annotation_pixels_pending: bool,
    copy_step: u8,
    copy_product: annotation_product::Pass,
    copy_product_gesture: Option<[f64; 4]>,
    copy_product_cancel: bool,
    copy_product_cancelled: bool,
    copy_product_frame: u64,
    copy_compact: bool,
    copy_original_scale: f32,
    copy_requested_scale: f32,
    copy_layout_bounds: Rectangle,
    copy_viewport_width: f32,
    copy_inspector_offset: f32,
    copy_swatch_color: [f64; 3],
    copy_swatch_ready: bool,
    copy_capability_ready: bool,
    copy_capability_available: bool,
    copy_shape_points: usize,
    copy_before: Option<crate::generated::AnnotationObject>,
    copy_after: Option<crate::generated::AnnotationObject>,
    copy_objects: usize,
    copy_categories: Vec<crate::generated::ArtifactClassName>,
    sweep_baseline: Option<(u64, crate::generated::ExploreViewport, Option<u32>)>,
    explore_dataset_pane: Option<Rectangle>,
    explore_details_pane: Option<Rectangle>,
    reveal_offset: AbsoluteOffset,
    annotation_sample_baseline: u64,
    annotation_frame_ready: Option<SampleablePresentation>,
    annotation_drawn: Option<(u64, u64)>,
    viewer_drawn: Option<(u64, u64, ViewerDraw)>,
    upscale_button: Option<Rectangle>,
    upscale_pixel_pending: bool,
    upscale_pixels: Option<(u64, u64, u32, u32)>,
    upscale_repeat_revision: Option<u64>,
    upscale_repeat_observed: bool,
    upscale_cache_pass: bool,
    upscale_cached_frames: [Option<crate::generated::VisualFrame>; 3],
    viewer_continuity_request: Option<crate::generated::UpscaleRequest>,
    viewer_continuity_settings_revision: u64,
    viewer_continuity_performance: bool,
    viewer_route_persistence: bool,
    gallery_drawn: Option<(u64, u64)>,
    atlas_drawn: Option<(u64, u8)>,
    atlas_clip: (f32, f32),
    atlas_row_extent: f32,
    atlas_receipt: Option<AtlasDraw>,
    atlas_pixels: Option<AtlasDraw>,
    atlas_composition: Option<AtlasDraw>,
    atlas_baseline: Option<(u64, u64)>,
    atlas_columns: u32,
    atlas_fixture_checked: bool,
    atlas_fixture_labels: bool,
    atlas_fixture_masks: bool,
    atlas_fixture_boxes: bool,
    spinner_baseline: f64,
    show_fps_baseline: bool,
    benchmark_baseline: bool,
    settings_revision: u64,
    numeric_target: f64,
    numeric_replacement: String,
    numeric_selection_length: usize,
    denoising_target: bool,
    ui_scale_baseline: f32,
    ui_scale_first: Option<f32>,
    input_scale: f32,
    explore_snapshot_revision: u64,
    reopen_wait_revision: u64,
    oversized_capacity: Option<crate::generated::VisualExtent>,
    scroll_placeholder_reported: bool,
    selection_grid: Option<(u32, u32, u32, u64, u64)>,
    resolved_primary: [f64; 4],
    resolved_benchmark: [f64; 4],
    reported_style_bits: u8,
}

impl Controller {
    pub fn subscription(&self) -> iced::Subscription<Message> {
        if self.running() {
            iced::Subscription::batch([
                iced::Subscription::run(surface_draw_stream),
                iced::event::listen_with(|event, _, _| {
                    matches!(
                        event,
                        iced::Event::Mouse(iced::mouse::Event::WheelScrolled { .. })
                    )
                    .then_some(Message::NumberWheelDelivered)
                }),
            ])
        } else {
            iced::Subscription::none()
        }
    }

    fn running(&self) -> bool {
        !matches!(
            self.phase,
            Phase::Disabled | Phase::Complete | Phase::Failed
        )
    }

    pub fn observe_workspace_message(
        &self,
        message: &crate::view::router::Message,
        active: FeatureId,
    ) {
        if !self.running() {
            return;
        }
        if let crate::view::router::Message::Navigation(
            crate::view::navigation::Message::PageSelected(feature),
        ) = message
        {
            report(
                "integration.navigation_message",
                crate::view::navigation::stable_id(*feature),
                crate::view::navigation::label(active),
                [0.0; 4],
            );
        }
        if matches!(
            self.phase,
            Phase::AwaitAdvancedNumericDraft(_) | Phase::AwaitAdvancedNumericSnapshot(_)
        ) && matches!(
            message,
            crate::view::router::Message::Train(crate::view::train::Message::Advanced(_))
        ) {
            report(
                "integration.advanced_message",
                "",
                &format!("{message:?}"),
                [0.0; 4],
            );
        }
        if let crate::view::router::Message::Explore(message) = message {
            report(
                "integration.explore_message",
                "",
                &format!("{message:?}"),
                [0.0; 4],
            );
        }
    }

    pub fn observe_navigation_outcome(&self, selected: FeatureId, active: FeatureId) {
        if self.running() {
            report(
                "integration.navigation_outcome",
                crate::view::navigation::stable_id(selected),
                crate::view::navigation::label(active),
                [0.0; 4],
            );
        }
    }

    pub fn observe_authoritative_route(
        &self,
        source: &'static str,
        authoritative: FeatureId,
        active: FeatureId,
    ) {
        if self.running() {
            report(
                "integration.route_state",
                crate::view::navigation::stable_id(authoritative),
                source,
                [
                    if authoritative == active { 1.0 } else { 0.0 },
                    0.0,
                    0.0,
                    0.0,
                ],
            );
        }
    }

    pub fn new(
        enabled: bool,
        window_close: bool,
        dataset_source: String,
        compiled_directory: String,
        resolution: String,
        viewer_scenario: String,
    ) -> Self {
        Self {
            phase: if enabled {
                Phase::AwaitBootstrap
            } else {
                Phase::Disabled
            },
            reported_phase: None,
            location_pending: false,
            window_close,
            dataset_source,
            compiled_directory,
            resolution,
            viewer_scenario,
            annotation_probe: None,
            annotation_pixels_revision: 0,
            annotation_pixels_pending: false,
            copy_step: 0,
            copy_product: annotation_product::Pass::default(),
            copy_product_gesture: None,
            copy_product_cancel: false,
            copy_product_cancelled: false,
            copy_product_frame: 0,
            copy_compact: false,
            copy_original_scale: 1.0,
            copy_requested_scale: 1.0,
            copy_layout_bounds: Rectangle::default(),
            copy_viewport_width: 0.0,
            copy_inspector_offset: 0.0,
            copy_swatch_color: [0.0; 3],
            copy_swatch_ready: false,
            copy_capability_ready: false,
            copy_capability_available: false,
            copy_shape_points: 0,
            copy_before: None,
            copy_after: None,
            copy_objects: 0,
            copy_categories: Vec::new(),
            sweep_baseline: None,
            explore_dataset_pane: None,
            explore_details_pane: None,
            reveal_offset: AbsoluteOffset::default(),
            annotation_sample_baseline: 0,
            annotation_frame_ready: None,
            annotation_drawn: None,
            viewer_drawn: None,
            upscale_button: None,
            upscale_pixel_pending: false,
            upscale_pixels: None,
            upscale_repeat_revision: None,
            upscale_repeat_observed: false,
            upscale_cache_pass: false,
            upscale_cached_frames: std::array::from_fn(|_| None),
            viewer_continuity_request: None,
            viewer_continuity_settings_revision: 0,
            viewer_continuity_performance: false,
            viewer_route_persistence: false,
            gallery_drawn: None,
            atlas_drawn: None,
            atlas_clip: (0.0, 0.0),
            atlas_row_extent: 0.0,
            atlas_receipt: None,
            atlas_pixels: None,
            atlas_composition: None,
            atlas_baseline: None,
            atlas_columns: 0,
            atlas_fixture_checked: false,
            atlas_fixture_labels: false,
            atlas_fixture_masks: false,
            atlas_fixture_boxes: false,
            spinner_baseline: 0.0,
            show_fps_baseline: false,
            benchmark_baseline: false,
            settings_revision: 0,
            numeric_target: 0.0,
            numeric_replacement: String::new(),
            numeric_selection_length: 0,
            denoising_target: false,
            ui_scale_baseline: 1.0,
            ui_scale_first: None,
            input_scale: 1.0,
            explore_snapshot_revision: 0,
            reopen_wait_revision: 0,
            oversized_capacity: None,
            scroll_placeholder_reported: false,
            selection_grid: None,
            resolved_primary: [0.0; 4],
            resolved_benchmark: [0.0; 4],
            reported_style_bits: 0,
        }
    }

    fn observe_presentation(
        &mut self,
        model: &ApplicationModel,
        frame: Option<crate::presentation_surface::FrameReady>,
    ) {
        if !matches!(
            self.phase,
            Phase::AwaitAnnotation
                | Phase::AnnotationTool { .. }
                | Phase::AnnotationSidebar { .. }
                | Phase::AnnotationTimeline { .. }
                | Phase::AnnotationOperation { .. }
                | Phase::AnnotationStop { .. }
                | Phase::AnnotationBrush { .. }
                | Phase::AwaitTool { .. }
                | Phase::AnnotationSurface(_)
                | Phase::AwaitAnnotationFrame(_)
                | Phase::AnnotationPointer(_)
                | Phase::AwaitPointer(_)
                | Phase::CopyProductStart
                | Phase::CopyProductWait
        ) {
            return;
        }
        let Some(annotation) = model.annotation.snapshot.as_ref() else {
            return;
        };
        let Some(sampleable) = sampleable_presentation(
            model,
            frame,
            crate::generated::PresentationSourceKind::Annotation,
            annotation.frame.revision,
        ) else {
            return;
        };
        if sampleable.presentation_revision > self.annotation_sample_baseline {
            self.annotation_frame_ready = Some(sampleable);
        }
    }

    fn fail(&mut self, detail: &str) {
        report("integration.failed", "", detail, [0.0; 4]);
        self.phase = Phase::Failed;
        self.location_pending = false;
    }

    pub fn observe_native_frame(
        &self,
        frame: crate::presentation_surface::FrameReady,
        surface: Option<crate::presentation_surface::Surface>,
    ) {
        if !self.running() {
            return;
        }
        let matching_surface = surface.is_some_and(|candidate| {
            candidate.high == frame.high
                && candidate.low == frame.low
                && frame.content_width <= candidate.width
                && frame.content_height <= candidate.height
        });
        report(
            "integration.native_frame",
            "presentation.surface",
            if matching_surface {
                "accepted"
            } else if surface.is_some() {
                "rejected"
            } else {
                "surface-missing"
            },
            [
                frame.content_sequence as f64,
                frame.presentation_revision as f64,
                surface.map_or(0.0, |candidate| candidate.generation as f64),
                if matching_surface { 1.0 } else { 0.0 },
            ],
        );
    }

    pub fn observe_explore_open_submission(&self, submitted: bool) {
        if self.running() {
            report(
                "integration.explore_open_submission",
                EXPLORE_OPEN,
                if submitted { "submitted" } else { "rejected" },
                [0.0; 4],
            );
        }
    }

    pub fn observe_explore_open_layout(
        &self,
        measured: bool,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        columns: u32,
    ) {
        if self.running() {
            report(
                "integration.explore_open_layout",
                EXPLORE_GALLERY,
                if measured {
                    "measured"
                } else {
                    "waiting-for-measurement"
                },
                [
                    snapshot.map_or(0.0, |value| value.dataset.identity as f64),
                    snapshot.map_or(0.0, |value| value.revision as f64),
                    columns as f64,
                    snapshot.map_or(0.0, |value| value.order.matchingcount as f64),
                ],
            );
        }
    }

    pub fn observe_explore_filter_request(
        &self,
        request: &crate::generated::ExploreFilterUpdate,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        local_edits: bool,
        mutation_available: bool,
    ) {
        if !self.running() {
            return;
        }
        let detail = if local_edits {
            "rejected-local-edits"
        } else if !mutation_available {
            "rejected-unavailable"
        } else {
            "preconditions-accepted"
        };
        let state = snapshot.map_or(0.0, |value| {
            (u8::from(value.ready)
                | (u8::from(value.busy) << 1)
                | (u8::from(value.cancellationrequested) << 2)) as f64
        });
        report(
            "integration.explore_filter_request",
            if request.filter.order == crate::generated::ExploreOrder::Shuffled {
                "shuffled"
            } else {
                "sequential"
            },
            detail,
            [
                snapshot.map_or(0.0, |value| value.revision as f64),
                request.filter.minimumcompiledindex as f64,
                state,
                if mutation_available { 1.0 } else { 0.0 },
            ],
        );
    }

    pub fn observe_explore_filter_submission(&self, submitted: bool) {
        if self.running() {
            report(
                "integration.explore_filter_submission",
                "explore.UpdateFilter",
                if submitted { "submitted" } else { "rejected" },
                [0.0; 4],
            );
        }
    }

    pub fn observe_explore_filter_settlement(
        &self,
        source: &'static str,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        mutation_available: bool,
    ) {
        if !self.running() {
            return;
        }
        let Some(snapshot) = snapshot else {
            report(
                "integration.explore_filter_settlement",
                source,
                "snapshot-unavailable",
                [0.0; 4],
            );
            return;
        };
        let state = (u8::from(snapshot.ready)
            | (u8::from(snapshot.busy) << 1)
            | (u8::from(snapshot.cancellationrequested) << 2)) as f64;
        report(
            "integration.explore_filter_settlement",
            source,
            if snapshot.filter.order == crate::generated::ExploreOrder::Shuffled {
                "shuffled"
            } else {
                "sequential"
            },
            [
                snapshot.revision as f64,
                snapshot.filter.minimumcompiledindex as f64,
                state,
                if mutation_available { 1.0 } else { 0.0 },
            ],
        );
    }

    pub fn observe_explore_open_request(&self, local_edits: bool, open_available: bool) {
        if self.running() {
            report(
                "integration.explore_open_request",
                EXPLORE_OPEN,
                "received",
                [
                    if local_edits { 1.0 } else { 0.0 },
                    if open_available { 1.0 } else { 0.0 },
                    0.0,
                    0.0,
                ],
            );
        }
    }

    fn begin_copy_object_edit(
        &mut self,
        snapshot: &crate::generated::AnnotationSnapshot,
        index: u16,
        mask: bool,
    ) -> Task<RootMessage> {
        if snapshot.ui.editor.selectedobject != Some(index) {
            self.phase = Phase::CopyAwaitObject { index, mask };
            return annotation_message(annotation::Message::Sidebar(
                annotation::sidebar::Message::ObjectSelected(index),
            ));
        }
        let tool = if mask {
            if self.copy_step == 3 {
                crate::generated::AnnotationTool::MaskErase
            } else {
                crate::generated::AnnotationTool::MaskPaint
            }
        } else {
            crate::generated::AnnotationTool::Select
        };
        if snapshot.ui.editor.tool == tool {
            self.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
            return self.arm_scrolled(ANNOTATION_SURFACE, RelativeOffset::START);
        }
        self.phase = Phase::AnnotationTool {
            revision: snapshot.ui.interactionrevision,
            tool,
        };
        self.arm_scrolled(annotation::tool_id(tool), RelativeOffset::START)
    }

    pub(crate) fn observe_upscale_request(&mut self, kernel: crate::generated::UpscaleKernel) {
        if let Phase::AwaitUpscale {
            kernel: selected, ..
        } = self.phase
            && self.upscale_repeat_revision.is_some()
            && crate::generated::UPSCALE_KERNEL_VALUES[selected] == kernel
        {
            self.upscale_repeat_observed = true;
        }
    }

    fn begin_upscale_series(
        &mut self,
        model: &ApplicationModel,
        source: &crate::generated::VisualFrame,
    ) -> Task<RootMessage> {
        self.phase = Phase::StartUpscale {
            kernel: 0,
            source_width: source.extent.width,
            source_height: source.extent.height,
            upscale_revision: model
                .upscale_snapshot
                .as_ref()
                .map_or(0, |value| value.revision),
            upscale_frame_revision: model
                .upscale_snapshot
                .as_ref()
                .map_or(0, |value| value.frame.revision),
            presentation_revision: model
                .presentation
                .as_ref()
                .map_or(0, |value| value.presentationrevision),
        };
        self.arm(EXPLORE_UPSCALE_ACTIONS[0])
    }

    fn arm(&mut self, control: impl Into<String>) -> Task<RootMessage> {
        if self.location_pending {
            return Task::none();
        }
        self.location_pending = true;
        locate(control.into())
    }

    fn arm_scrolled(
        &mut self,
        control: impl Into<String>,
        offset: RelativeOffset,
    ) -> Task<RootMessage> {
        if self.location_pending {
            return Task::none();
        }
        self.location_pending = true;
        let control = control.into();
        let inspector = control.starts_with("annotation.") && control != ANNOTATION_SURFACE;
        let offset = if inspector
            && (control == "annotation.undo"
                || control == "annotation.redo"
                || control.starts_with("annotation.tool."))
        {
            RelativeOffset::START
        } else {
            offset
        };
        let scroll = if inspector {
            iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, RelativeOffset::START)
                .chain(iced::widget::operation::scroll_by(
                    crate::view::PAGE_SCROLL_ID,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.copy_inspector_offset,
                    },
                ))
                .chain(iced::widget::operation::snap_to(
                    "annotation.inspector.scroll",
                    offset,
                ))
        } else {
            iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, offset)
        };
        scroll.chain(locate(control))
    }

    fn arm_revealed(
        &mut self,
        scrollable: &'static str,
        control: impl Into<String>,
    ) -> Task<RootMessage> {
        if self.location_pending {
            return Task::none();
        }
        self.location_pending = true;
        iced::widget::operation::scroll_by(scrollable, self.reveal_offset)
            .chain(locate(control.into()))
    }

    fn begin_advanced_numeric_edit(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        index: usize,
    ) -> Task<RootMessage> {
        let Some((target, replacement, selection_length)) = numeric_edit_target(settings, index)
        else {
            self.fail("representative numeric field has no generated-valid alternate value");
            return Task::none();
        };
        self.numeric_target = target;
        self.numeric_replacement = replacement;
        self.numeric_selection_length = selection_length;
        self.settings_revision = model
            .settings_snapshot
            .as_ref()
            .map_or(0, |snapshot| snapshot.revision);
        self.phase = Phase::AdvancedNumericEdit(index);
        self.arm_scrolled(advanced_field_id(index), RelativeOffset::END)
    }

    pub fn update(&mut self, message: Message) -> Option<train::Message> {
        if !self.running() {
            return None;
        }
        let (control, bounds) = match message {
            Message::Advance => return None,
            Message::NumberWheelDelivered => {
                if let Phase::AwaitAdvancedSpinnerWheel(index) = self.phase {
                    self.phase = Phase::AdvancedSpinnerWheelVerify(index);
                    report(
                        "integration.number_wheel_delivered",
                        &advanced_field_id(index),
                        "iced-widget-update-complete",
                        [0.0; 4],
                    );
                }
                return None;
            }
            #[cfg(target_arch = "wasm32")]
            Message::UpscalePixels {
                source,
                presentation,
                checksum,
                blue,
            } => {
                self.upscale_pixels = Some((source, presentation, checksum, blue));
                return None;
            }
            Message::Located { control, bounds } => (control, bounds),
            Message::AtlasDrawn {
                source_revision,
                visibility,
                clipped_top,
                clipped_bottom,
                row_extent,
                receipt,
            } => {
                self.atlas_receipt = Some(receipt);
                self.atlas_row_extent = row_extent;
                self.atlas_clip = (clipped_top, clipped_bottom);
                self.atlas_drawn = Some((source_revision, visibility));
                return None;
            }
            #[cfg(target_arch = "wasm32")]
            Message::AnnotationControlPixels { expected, matched } => {
                if expected != 1 || matched != 1 {
                    self.fail("Rendered tool availability differs from the native capability");
                } else {
                    self.copy_capability_ready = true;
                }
                return None;
            }
            #[cfg(target_arch = "wasm32")]
            Message::AnnotationPixels {
                revision,
                expected,
                matched,
            } => {
                self.annotation_pixels_pending = false;
                if expected == 0 || expected != matched {
                    self.fail("Annotation pixels do not match source geometry and native palette");
                } else if revision == 0 {
                    self.copy_swatch_ready = true;
                } else {
                    self.annotation_pixels_revision = revision;
                }
                return None;
            }
            Message::AtlasComposition {
                receipt,
                expected,
                matched,
            } => {
                if expected != 0 && expected == matched {
                    self.atlas_composition = Some(receipt);
                }
                return None;
            }
            Message::AtlasPixels {
                receipt,
                visible,
                nonblack,
            } => {
                let snapshot = &receipt.snapshot;
                report(
                    "integration.atlas_canvas_pixels",
                    EXPLORE_GALLERY,
                    "visible-tile-interiors",
                    [
                        snapshot.frame.revision as f64,
                        receipt
                            .surface
                            .frame
                            .map_or(0, |frame| frame.presentation_revision)
                            as f64,
                        visible as f64,
                        nonblack as f64,
                    ],
                );
                if visible != 0 && visible == nonblack {
                    self.atlas_pixels = Some(receipt);
                }
                return None;
            }
            Message::GalleryDrawn {
                presentation_revision,
                source_revision,
            } => {
                if let Phase::AwaitExploreDatasetReopen {
                    revision,
                    frame_revision,
                } = &self.phase
                {
                    report(
                        "integration.explore_reopen_draw",
                        EXPLORE_GALLERY,
                        "physical-gallery-draw",
                        [
                            *revision as f64,
                            *frame_revision as f64,
                            source_revision as f64,
                            presentation_revision as f64,
                        ],
                    );
                }
                self.gallery_drawn = Some((presentation_revision, source_revision));
                return None;
            }
            Message::SurfaceDrawn {
                presentation_revision,
                source_revision,
                viewer,
            } => {
                if let Some(viewer) = viewer {
                    self.viewer_drawn = Some((presentation_revision, source_revision, viewer));
                } else {
                    self.annotation_drawn = Some((presentation_revision, source_revision));
                }
                return None;
            }
        };
        self.location_pending = false;
        if matches!(self.phase, Phase::ViewerNoAspect) {
            if bounds.width > 0.0 || bounds.height > 0.0 {
                self.fail("Explore still renders an aspect-ratio selector");
            } else if let Some((presentation, source, _)) = self.viewer_drawn {
                report(
                    "integration.viewer_complete",
                    "explore.detail.aspect",
                    &self.viewer_scenario,
                    [presentation as f64, source as f64, 1.0, 0.0],
                );
                self.phase = Phase::Complete;
            }
            return None;
        }
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            report(
                "integration.locate_failed",
                &control,
                "stable identity absent from rendered tree",
                [0.0; 4],
            );
            self.fail("Iced widget operation could not locate the stable identity");
            return None;
        }
        report(
            "integration.control_bounds",
            &control,
            "",
            [
                f64::from(bounds.x),
                f64::from(bounds.y),
                f64::from(bounds.width),
                f64::from(bounds.height),
            ],
        );
        match self.phase.clone() {
            Phase::AdvancedField(index) => {
                report_advanced_field(&control, &format!("fixed-{index}"), bounds)
            }
            Phase::AdvancedAssignment => report_advanced_field(&control, "assignment", bounds),
            Phase::AdvancedMatchFree(index) => {
                report_advanced_field(&control, &format!("match-free-{index}"), bounds)
            }
            Phase::AdvancedDenoisingToggle => report_advanced_field(&control, "dn-toggle", bounds),
            Phase::AdvancedDenoising(index) => {
                report_advanced_field(&control, &format!("dn-{index}"), bounds)
            }
            _ => {}
        }
        let expected = match self.phase {
            Phase::SettingsOpen => "navigation.settings".to_owned(),
            Phase::SettingsModal => SETTINGS_MODAL.to_owned(),
            Phase::SettingsGroup(index) => SETTINGS_GROUPS[index].to_owned(),
            Phase::SettingsScaleDrag => SETTINGS_NUMERIC_CONTROLS[0].to_owned(),
            Phase::SettingsShowFps => SETTINGS_SHOW_FPS.to_owned(),
            Phase::SettingsRestoreShowFps => SETTINGS_SHOW_FPS.to_owned(),
            Phase::SettingsNumeric { index, part } => settings_numeric_id(index, part),
            Phase::SettingsFooter => SETTINGS_FOOTER.to_owned(),
            Phase::SettingsReset => SETTINGS_RESET.to_owned(),
            Phase::SettingsClose => SETTINGS_CLOSE.to_owned(),
            Phase::TrainNavigation => {
                crate::view::navigation::stable_id(FeatureId::Train).to_owned()
            }
            Phase::PageNavigation(page) => crate::view::navigation::stable_id(page).to_owned(),
            Phase::PageRegion { page, index } => region_id(page, index).to_owned(),
            Phase::TrainModelCard => TRAIN_MODEL_CARD.to_owned(),
            Phase::TrainModelPart(index) => TRAIN_MODEL_PARTS[index].to_owned(),
            Phase::TrainModelProgress => TRAIN_MODEL_PROGRESS.to_owned(),
            Phase::ReturnTrain => crate::view::navigation::stable_id(FeatureId::Train).to_owned(),
            Phase::AdvancedField(index) => advanced_field_id(index),
            Phase::AdvancedSpinnerEdge { index, .. } => advanced_field_id(index),
            Phase::AdvancedSpinnerWheel(index) => advanced_field_id(index),
            Phase::AdvancedNumericEdit(index) => advanced_field_id(index),
            Phase::AdvancedAssignment => train::MATCH_FREE_ASSIGNMENT_ID.to_owned(),
            Phase::AdvancedMatchFree(index) => match_free_field_id(index),
            Phase::AdvancedDenoisingToggle => crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled()
                .stable_field_id
                .to_string(),
            Phase::AdvancedDenoising(index) => denoising_field_id(index),
            Phase::ErrorModal => ERROR_MODAL.to_owned(),
            Phase::ErrorCopy => ERROR_COPY.to_owned(),
            Phase::ErrorDismiss => ERROR_DISMISS.to_owned(),
            Phase::TrainCard => TRAIN_CARD.to_owned(),
            Phase::DatasetBrowse => DATASET_BROWSE.to_owned(),
            Phase::BenchmarkOverride => BENCHMARK_OVERRIDE.to_owned(),
            Phase::BenchmarkRestore => BENCHMARK_OVERRIDE.to_owned(),
            Phase::DatasetSource => DATASET_SOURCE.to_owned(),
            Phase::CompiledDirectory => COMPILED_DIRECTORY.to_owned(),
            Phase::CompileDimensions => COMPILE_DIMENSIONS.to_owned(),
            Phase::CompileResolution => COMPILE_RESOLUTION.to_owned(),
            Phase::Compile | Phase::CompileActionWithProgress => COMPILE_DATASET.to_owned(),
            Phase::CompileProgress => COMPILE_PROGRESS.to_owned(),
            Phase::DatasetStatus => DATASET_STATUS.to_owned(),
            Phase::ExploreNavigation => {
                crate::view::navigation::stable_id(FeatureId::Explore).to_owned()
            }
            Phase::ExploreOpen => EXPLORE_OPEN.to_owned(),
            Phase::ExploreDatasetPane => EXPLORE_DATASET_PANE.to_owned(),
            Phase::ExploreDetailsPane => EXPLORE_DETAILS_PANE.to_owned(),
            Phase::ExplorePolicyOrder(_) => explore::ORDER_SHUFFLED_ID.to_owned(),
            Phase::ExplorePolicyRange(_) | Phase::ExplorePolicyRangeVisible(_) => {
                explore::RANGE_START_ONE_ID.to_owned()
            }
            Phase::ExplorePolicyOverlay(_) | Phase::ExplorePolicyOverlayVisible(_) => {
                explore::OVERLAY_NONE_ID.to_owned()
            }
            Phase::ExploreAugmentationToggle { .. } => EXPLORE_AUGMENTATION_TOGGLE.to_owned(),
            Phase::ExploreAugmentationReroll { .. } => EXPLORE_AUGMENTATION_REROLL.to_owned(),
            Phase::ExploreReshuffle { .. } => EXPLORE_RESHUFFLE.to_owned(),
            Phase::ExploreCard { .. } => EXPLORE_CARD.to_owned(),
            Phase::GallerySweep => EXPLORE_GALLERY.to_owned(),
            Phase::GalleryLater(_) => EXPLORE_LATER.to_owned(),
            Phase::GalleryImage(_) => EXPLORE_GALLERY.to_owned(),
            Phase::DetailOriginal { .. } => EXPLORE_DETAIL_ORIGINAL.to_owned(),
            Phase::DetailFit => explore::DETAIL_FIT_ID.to_owned(),
            Phase::ViewerSelect => EXPLORE_GALLERY.to_owned(),
            Phase::AtlasCapacity => explore::GALLERY_CAPACITY_ID.to_owned(),
            Phase::AtlasEmpty => explore::GALLERY_EMPTY_ID.to_owned(),
            Phase::AtlasOverlay(index) => overlay_control(index, false).to_owned(),
            Phase::ViewerOverlay(index) => overlay_control(index, true).to_owned(),
            Phase::ViewerNoAspect => "explore.detail.aspect".to_owned(),
            Phase::CopyUndo { .. } => "annotation.undo".to_owned(),
            Phase::CopyRedo { .. } => "annotation.redo".to_owned(),
            Phase::CopySave => VIEWER_SAVE.to_owned(),
            Phase::CopyCapability=>annotation::tool_id(crate::generated::AnnotationTool::ColorSample),
            Phase::CopyLayout(0)=>"workflow.workspace_and_advanced".into(),
            Phase::CopyLayout(1)=>"annotation.inspector.scroll".into(),
            Phase::CopyLayout(_)=>"annotation.class.active.swatch".into(),
            Phase::StartUpscale { kernel, .. } => EXPLORE_UPSCALE_ACTIONS[kernel].to_owned(),
            Phase::DetailNext(_) => EXPLORE_NEXT.to_owned(),
            Phase::DetailPrevious(_) => EXPLORE_PREVIOUS.to_owned(),
            Phase::DetailCloseEvidence => EXPLORE_DETAIL_CLOSE.to_owned(),
            Phase::ExploreDatasetReopen { .. } => EXPLORE_OPEN.to_owned(),
            Phase::GalleryReselect => EXPLORE_GALLERY.to_owned(),
            Phase::OpenAnnotation => EXPLORE_ANNOTATE.to_owned(),
            Phase::AnnotationSidebar { .. } => ANNOTATION_SIDEBAR.to_owned(),
            Phase::AnnotationTimeline { .. } => ANNOTATION_TIMELINE.to_owned(),
            Phase::AnnotationOperation { .. } => ANNOTATION_OPERATION.to_owned(),
            Phase::AnnotationStop { .. } => ANNOTATION_STOP.to_owned(),
            Phase::AnnotationBrush { .. } => ANNOTATION_BRUSH_RADIUS.to_owned(),
            Phase::AnnotationTool { tool, .. } => annotation::tool_id(tool),
            Phase::AnnotationSurface(_) | Phase::AnnotationPointer(_) | Phase::AwaitPointer(_) | Phase::CopyProductWait => {
                ANNOTATION_SURFACE.to_owned()
            }
            _ => {
                self.fail("unexpected Iced widget operation result");
                return None;
            }
        };
        if control != expected {
            self.fail("Iced widget operation returned the wrong stable identity");
            return None;
        }
        let resolved_style = if control == BENCHMARK_OVERRIDE {
            Some(("benchmark-purple", self.resolved_benchmark))
        } else if matches!(
            control.as_str(),
            DATASET_BROWSE
                | COMPILE_DATASET
                | crate::view::workflow::model_card::TRAIN_CUSTOM_ID
                | crate::view::workflow::model_card::TRAIN_ACTION_ID
                | EXPLORE_OPEN
        ) {
            Some(("shared-primary", self.resolved_primary))
        } else {
            None
        };
        if let Some((semantic, color)) = resolved_style {
            let bit = match control.as_str() {
                DATASET_BROWSE => 1,
                COMPILE_DATASET => 2,
                crate::view::workflow::model_card::TRAIN_CUSTOM_ID => 4,
                crate::view::workflow::model_card::TRAIN_ACTION_ID => 8,
                EXPLORE_OPEN => 16,
                BENCHMARK_OVERRIDE => 32,
                _ => 0,
            };
            if self.reported_style_bits & bit == 0 {
                self.reported_style_bits |= bit;
                report_rendered_control_style(&control, semantic, color, bounds);
            }
        }
        let input_bounds = crate::presentation_surface::physical_bounds(bounds, self.input_scale);
        match self.phase.clone() {
            Phase::CopyCapability => {
                self.phase = Phase::CopyCapabilityWait;
                #[cfg(target_arch = "wasm32")]
                if let Some(mut output) =
                    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
                {
                    let callback = pixel_result_callback(move |expected, matched| {
                        let _ =
                            output.try_send(Message::AnnotationControlPixels { expected, matched });
                    });
                    annotation_swatch_js(
                        &[
                            f64::from(input_bounds.x),
                            f64::from(input_bounds.y),
                            f64::from(input_bounds.width),
                            f64::from(input_bounds.height),
                        ],
                        &self.copy_swatch_color,
                        &control,
                        if self.copy_capability_available {
                            "enabled"
                        } else {
                            "disabled"
                        },
                        true,
                        &callback,
                    );
                }
                None
            }
            Phase::CopyLayout(0) => {
                self.copy_layout_bounds = bounds;
                self.phase = Phase::CopyLayout(1);
                None
            }
            Phase::CopyLayout(1) => {
                let image = self.copy_layout_bounds;
                let stacked = bounds.y >= image.y + image.height;
                let bounded = image.width > 0.0
                    && bounds.width > 0.0
                    && image.x >= 0.0
                    && bounds.x >= 0.0
                    && image.x + image.width <= self.copy_viewport_width + 1.0
                    && bounds.x + bounds.width <= self.copy_viewport_width + 1.0;
                let placed = if stacked {
                    (image.x - bounds.x).abs() <= 1.0 && (image.width - bounds.width).abs() <= 1.0
                } else {
                    bounds.x >= image.x + image.width && (bounds.y - image.y).abs() <= 1.0
                };
                report(
                    "integration.annotation_layout_viewport",
                    "annotation.inspector.scroll",
                    if self.copy_compact { "compact" } else { "wide" },
                    [
                        f64::from(self.copy_viewport_width),
                        f64::from(self.input_scale),
                        f64::from(self.copy_requested_scale),
                        if !bounded {
                            1.0
                        } else if !placed {
                            2.0
                        } else if stacked != self.copy_compact {
                            3.0
                        } else {
                            0.0
                        },
                    ],
                );
                if !bounded || !placed || stacked != self.copy_compact {
                    self.fail("Annotation canvas and inspector do not fit the actual viewport");
                    return None;
                }
                report(
                    "integration.annotation_layout",
                    "annotation.inspector.scroll",
                    if stacked { "compact" } else { "wide" },
                    [
                        f64::from(image.width),
                        f64::from(bounds.width),
                        f64::from(self.copy_viewport_width),
                        f64::from(if stacked {
                            bounds.y - image.y - image.height
                        } else {
                            bounds.x - image.x - image.width
                        }),
                    ],
                );
                self.copy_inspector_offset =
                    (bounds.y - crate::view::NAVIGATION_HEIGHT - 10.0).max(0.0);
                self.phase = Phase::CopyLayout(2);
                None
            }
            Phase::CopyLayout(_) => {
                self.phase = Phase::CopySwatchWait;
                #[cfg(target_arch = "wasm32")]
                if let Some(mut output) =
                    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
                {
                    let callback = pixel_result_callback(move |expected, matched| {
                        let _ = output.try_send(Message::AnnotationPixels {
                            revision: 0,
                            expected,
                            matched,
                        });
                    });
                    annotation_swatch_js(
                        &[
                            f64::from(input_bounds.x),
                            f64::from(input_bounds.y),
                            f64::from(input_bounds.width),
                            f64::from(input_bounds.height),
                        ],
                        &self.copy_swatch_color,
                        &control,
                        "native-hsv-completed-canvas",
                        false,
                        &callback,
                    );
                }
                None
            }
            Phase::AwaitPointer(_) | Phase::CopyProductWait if self.annotation_probe.is_some() => {
                #[cfg(target_arch = "wasm32")]
                if let Some((source, presentation, width, height, probes)) =
                    self.annotation_probe.take()
                {
                    if let Some(mut output) =
                        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
                    {
                        let callback = pixel_result_callback(move |expected, matched| {
                            let _ = output.try_send(Message::AnnotationPixels {
                                revision: source,
                                expected,
                                matched,
                            });
                        });
                        annotation_pixels_js(
                            &[
                                f64::from(input_bounds.x),
                                f64::from(input_bounds.y),
                                f64::from(input_bounds.width),
                                f64::from(input_bounds.height),
                            ],
                            &[f64::from(width), f64::from(height)],
                            &probes,
                            source as f64,
                            presentation as f64,
                            &callback,
                        );
                    }
                }
                None
            }
            Phase::SettingsOpen => {
                self.phase = Phase::AwaitSettings;
                if !click(input_bounds) {
                    self.fail("Firefox Settings click dispatch failed");
                }
                None
            }
            Phase::SettingsModal => {
                self.phase = Phase::SettingsGroup(0);
                None
            }
            Phase::SettingsGroup(index) => {
                self.phase = if index + 1 < SETTINGS_GROUPS.len() {
                    Phase::SettingsGroup(index + 1)
                } else {
                    Phase::SettingsScaleDrag
                };
                None
            }
            Phase::SettingsScaleDrag => {
                #[cfg(target_arch = "wasm32")]
                let dispatched = slider_drag_js(
                    f64::from(input_bounds.x),
                    f64::from(input_bounds.y),
                    f64::from(input_bounds.width),
                    f64::from(input_bounds.height),
                ) == 1;
                #[cfg(not(target_arch = "wasm32"))]
                let dispatched = false;
                if !dispatched {
                    self.fail("Firefox UI-scale pointer drag dispatch failed");
                    return None;
                }
                self.phase = Phase::AwaitSettingsScaleDrag;
                None
            }
            Phase::SettingsShowFps => {
                self.phase = Phase::AwaitSettingsShowFps;
                if !click(input_bounds) {
                    self.fail("Firefox Show FPS click dispatch failed");
                }
                None
            }
            Phase::SettingsRestoreShowFps => {
                self.phase = Phase::AwaitSettingsShowFpsRestored;
                if !click(input_bounds) {
                    self.fail("Firefox Show FPS restore click dispatch failed");
                }
                None
            }
            Phase::SettingsNumeric { index, part } => {
                self.phase = if part < 2 {
                    Phase::SettingsNumeric {
                        index,
                        part: part + 1,
                    }
                } else if index + 1 < SETTINGS_NUMERIC_CONTROLS.len() {
                    Phase::SettingsNumeric {
                        index: index + 1,
                        part: 0,
                    }
                } else {
                    Phase::SettingsFooter
                };
                None
            }
            Phase::SettingsFooter => {
                self.phase = Phase::SettingsReset;
                None
            }
            Phase::SettingsReset => {
                self.phase = Phase::SettingsClose;
                None
            }
            Phase::SettingsClose => {
                self.phase = Phase::AwaitSettingsClosed;
                if !click(input_bounds) {
                    self.fail("Firefox Settings close dispatch failed");
                }
                None
            }
            Phase::TrainNavigation => {
                self.phase = Phase::AwaitTrain;
                None
            }
            Phase::PageNavigation(page) => {
                self.phase = Phase::AwaitPage(page);
                if !click(input_bounds) {
                    self.fail("Firefox navigation click dispatch failed");
                }
                None
            }
            Phase::PageRegion { page, index } => {
                report(
                    "integration.page_region",
                    region_id(page, index),
                    crate::view::navigation::label(page),
                    [
                        f64::from(bounds.x),
                        f64::from(bounds.y),
                        f64::from(bounds.width),
                        f64::from(bounds.height),
                    ],
                );
                let composition = crate::view::workflow::Composition::new(page, 0.0);
                if index + 1 < composition.audit_regions().len() {
                    self.phase = Phase::PageRegion {
                        page,
                        index: index + 1,
                    };
                } else if page == FeatureId::Train {
                    self.phase = Phase::TrainModelCard;
                } else if let Some(next) = composition.next_ordinary_page() {
                    self.phase = Phase::PageNavigation(next);
                } else {
                    self.phase = Phase::ReturnTrain;
                }
                None
            }
            Phase::TrainModelCard => {
                self.phase = Phase::TrainModelPart(0);
                None
            }
            Phase::TrainModelPart(index) => {
                self.phase = if index + 1 < TRAIN_MODEL_PARTS.len() {
                    Phase::TrainModelPart(index + 1)
                } else {
                    Phase::TrainModelProgress
                };
                None
            }
            Phase::TrainModelProgress => {
                self.phase = crate::view::workflow::Composition::new(FeatureId::Train, 0.0)
                    .next_ordinary_page()
                    .map_or(Phase::ReturnTrain, Phase::PageNavigation);
                None
            }
            Phase::ReturnTrain => {
                self.phase = Phase::AwaitReturnTrain;
                if !click(input_bounds) {
                    self.fail("Firefox Train navigation click dispatch failed");
                }
                None
            }
            Phase::AdvancedField(index) => {
                self.phase = if index == 2 || index == 4 {
                    Phase::AdvancedSpinnerEdge { index, upper: true }
                } else if index + 1 < 8 {
                    Phase::AdvancedField(index + 1)
                } else {
                    Phase::AdvancedAssignment
                };
                None
            }
            Phase::AdvancedSpinnerEdge { index, upper } => {
                if !click_number_edge(input_bounds, upper) {
                    self.fail("Firefox numeric-field edge click dispatch failed");
                    return None;
                }
                self.phase = Phase::AdvancedSpinnerVerify { index, upper };
                None
            }
            Phase::AdvancedSpinnerWheel(index) => {
                if !wheel_number_input(input_bounds) {
                    self.fail("Firefox numeric-field wheel dispatch failed");
                    return None;
                }
                // The containing page may scroll even when the number ignores
                // the wheel. Locate the next input after Iced applies that event.
                self.phase = Phase::AwaitAdvancedSpinnerWheel(index);
                None
            }
            Phase::AdvancedNumericEdit(index) => {
                if !replace_number_input(
                    input_bounds,
                    &self.numeric_replacement,
                    self.numeric_selection_length,
                ) {
                    self.fail("Firefox numeric-field text replacement dispatch failed");
                    return None;
                }
                self.phase = Phase::AwaitAdvancedNumericDraft(index);
                None
            }
            Phase::AdvancedAssignment => {
                self.phase = Phase::AwaitAdvancedAssignmentDraft;
                if !click(input_bounds) {
                    self.fail("Firefox Match-Free assignment click dispatch failed");
                }
                None
            }
            Phase::AdvancedMatchFree(index) => {
                self.phase = if index + 1 < 3 {
                    Phase::AdvancedMatchFree(index + 1)
                } else {
                    Phase::AdvancedDenoisingToggle
                };
                None
            }
            Phase::AdvancedDenoisingToggle => {
                self.phase = Phase::AwaitAdvancedDenoisingDraft;
                if !click(input_bounds) {
                    self.fail("Firefox denoising toggle click dispatch failed");
                }
                None
            }
            Phase::AdvancedDenoising(index) => {
                self.phase = if index + 1 < 4 {
                    Phase::AdvancedDenoising(index + 1)
                } else {
                    Phase::TriggerError
                };
                None
            }
            Phase::ErrorModal => {
                self.phase = Phase::ErrorCopy;
                None
            }
            Phase::ErrorCopy => {
                self.phase = Phase::AwaitErrorCopy;
                if !click(input_bounds) {
                    self.fail("Firefox error Copy click dispatch failed");
                }
                None
            }
            Phase::ErrorDismiss => {
                self.phase = Phase::AwaitErrorDismissed;
                if !click(input_bounds) {
                    self.fail("Firefox error Dismiss click dispatch failed");
                }
                None
            }
            Phase::TrainCard => {
                self.phase = Phase::DatasetBrowse;
                None
            }
            Phase::DatasetBrowse => {
                self.phase = Phase::BenchmarkOverride;
                None
            }
            Phase::BenchmarkOverride => {
                self.phase = Phase::AwaitBenchmarkOverride;
                if !click(input_bounds) {
                    self.fail("Firefox benchmark override click dispatch failed");
                }
                None
            }
            Phase::BenchmarkRestore => {
                self.phase = Phase::AwaitBenchmarkRestored;
                if !click(input_bounds) {
                    self.fail("Firefox benchmark override restore click dispatch failed");
                }
                None
            }
            Phase::DatasetSource => {
                self.phase = Phase::AwaitDatasetSource;
                Some(train::Message::Dataset(
                    train::dataset::Message::SourceChanged(self.dataset_source.clone()),
                ))
            }
            Phase::CompiledDirectory => {
                self.phase = Phase::AwaitCompiledDirectory;
                Some(train::Message::Dataset(
                    train::dataset::Message::CompiledDirectoryChanged(
                        self.compiled_directory.clone(),
                    ),
                ))
            }
            Phase::CompileDimensions => {
                self.phase = Phase::AwaitCompileDimensions;
                Some(train::Message::Dataset(
                    train::dataset::Message::CompileDimensionsChanged(true),
                ))
            }
            Phase::CompileResolution => {
                self.phase = Phase::AwaitCompileResolution;
                Some(train::Message::Dataset(
                    train::dataset::Message::ResolutionChanged(
                        self.resolution.parse().unwrap_or_default(),
                    ),
                ))
            }
            Phase::DatasetStatus => {
                self.phase = Phase::ExploreNavigation;
                None
            }
            Phase::CompileProgress => {
                self.phase = Phase::CompileActionWithProgress;
                None
            }
            Phase::CompileActionWithProgress => {
                self.phase = Phase::AwaitCompileCompletion;
                None
            }
            Phase::ExploreCard {
                revision,
                frame_revision,
            } => {
                self.phase = Phase::AwaitGalleryPatch {
                    revision,
                    frame_revision,
                };
                None
            }
            Phase::ExploreDatasetPane => {
                self.explore_dataset_pane = Some(bounds);
                self.phase = Phase::ExploreDetailsPane;
                None
            }
            Phase::ExploreDetailsPane => {
                self.explore_details_pane = Some(bounds);
                self.phase = Phase::ExplorePolicyOrderReady;
                None
            }
            Phase::ExplorePolicyOrder(revision) => {
                self.phase = Phase::AwaitExplorePolicyOrder(revision);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyRange(revision) => {
                let Some(pane) = self.explore_dataset_pane else {
                    self.fail("Explore dataset pane bounds are unavailable");
                    return None;
                };
                if let Some(offset) = sidebar_reveal_offset(pane, bounds) {
                    self.reveal_offset = offset;
                    self.phase = Phase::ExplorePolicyRangeVisible(revision);
                    return None;
                }
                self.phase = Phase::AwaitExplorePolicyRange(revision);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyRangeVisible(revision) => {
                let Some(pane) = self.explore_dataset_pane else {
                    self.fail("Explore dataset pane bounds are unavailable");
                    return None;
                };
                if !sidebar_control_visible(pane, bounds) {
                    self.fail("Explore range control was not revealed inside its sidebar");
                    return None;
                }
                self.phase = Phase::AwaitExplorePolicyRange(revision);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyOverlay(revision) => {
                let Some(pane) = self.explore_details_pane else {
                    self.fail("Explore details pane bounds are unavailable");
                    return None;
                };
                if let Some(offset) = sidebar_reveal_offset(pane, bounds) {
                    self.reveal_offset = offset;
                    self.phase = Phase::ExplorePolicyOverlayVisible(revision);
                    return None;
                }
                self.phase = Phase::AwaitExplorePolicyOverlay(revision);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExplorePolicyOverlayVisible(revision) => {
                let Some(pane) = self.explore_details_pane else {
                    self.fail("Explore details pane bounds are unavailable");
                    return None;
                };
                if !sidebar_control_visible(pane, bounds) {
                    self.fail("Explore overlay control was not revealed inside its sidebar");
                    return None;
                }
                self.phase = Phase::AwaitExplorePolicyOverlay(revision);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExploreAugmentationToggle {
                revision,
                frame_revision,
            } => {
                self.phase = Phase::AwaitExploreAugmentationToggle {
                    revision,
                    frame_revision,
                };
                if !click(input_bounds) {
                    self.fail("Firefox augmentation toggle click dispatch failed");
                }
                None
            }
            Phase::ExploreAugmentationReroll {
                revision,
                frame_revision,
            } => {
                self.phase = Phase::AwaitExploreAugmentationReroll {
                    revision,
                    frame_revision,
                };
                if !click(input_bounds) {
                    self.fail("Firefox augmentation reroll click dispatch failed");
                }
                None
            }
            Phase::ExploreReshuffle {
                revision,
                frame_revision,
                shuffle_seed,
                augmentation_seed,
                order_signature,
            } => {
                self.phase = Phase::AwaitExploreReshuffle {
                    revision,
                    frame_revision,
                    shuffle_seed,
                    augmentation_seed,
                    order_signature,
                };
                if !click(input_bounds) {
                    self.fail("Firefox Reshuffle click dispatch failed");
                }
                None
            }
            Phase::DetailOriginal {
                revision,
                frame_revision,
                padded_width,
                padded_height,
            } => {
                self.phase = Phase::AwaitDetailOriginal {
                    revision,
                    frame_revision,
                    padded_width,
                    padded_height,
                };
                if !click(input_bounds) {
                    self.fail("Firefox original-detail click dispatch failed");
                }
                None
            }
            Phase::ViewerSelect => {
                let Some((columns, _rows, _, _, revision)) = self.selection_grid else {
                    return None;
                };
                let selected = Rectangle {
                    x: input_bounds.x,
                    y: input_bounds.y,
                    width: input_bounds.width / columns as f32,
                    height: input_bounds.width / columns as f32,
                };
                self.phase = Phase::AwaitDetail(0);
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, revision, true) {
                    self.fail("viewer first-image click failed");
                }
                None
            }
            Phase::AtlasCapacity | Phase::AtlasEmpty => {
                report(
                    "integration.atlas_notice",
                    &control,
                    "rendered-local-outcome",
                    [
                        bounds.x as f64,
                        bounds.y as f64,
                        bounds.width as f64,
                        bounds.height as f64,
                    ],
                );
                self.phase = if matches!(self.phase, Phase::AtlasCapacity) {
                    Phase::AtlasRestoreColumns
                } else {
                    Phase::AtlasRestoreFilter
                };
                None
            }
            Phase::AtlasOverlay(index) => {
                self.phase = Phase::AwaitAtlasOverlay(index);
                if !click(input_bounds) {
                    self.fail("atlas visibility checkbox click failed");
                }
                None
            }
            Phase::ViewerOverlay(index) => {
                self.phase = Phase::AwaitViewerOverlay(index);
                if !click(input_bounds) {
                    self.fail("viewer semantic checkbox click failed");
                }
                None
            }
            Phase::CopyUndo { index, mask } => {
                self.phase = Phase::CopyAwaitUndo { index, mask };
                if !click(input_bounds) {
                    self.fail("imported object undo click failed");
                }
                None
            }
            Phase::CopyRedo { index, mask } => {
                self.phase = Phase::CopyAwaitRedo { index, mask };
                if !click(input_bounds) {
                    self.fail("imported object redo click failed");
                }
                None
            }
            Phase::CopySave => {
                self.phase = Phase::CopyAwaitSave;
                if !click(input_bounds) {
                    self.fail("imported document save click failed");
                }
                None
            }
            Phase::DetailFit => {
                self.viewer_drawn = None;
                self.phase = Phase::AwaitDetailFit;
                if !click(input_bounds) {
                    self.fail("Firefox detail Fit click dispatch failed");
                }
                None
            }
            Phase::StartUpscale {
                kernel,
                source_width,
                source_height,
                upscale_revision,
                upscale_frame_revision,
                presentation_revision,
            } => {
                self.upscale_button = Some(input_bounds);
                self.upscale_pixel_pending = false;
                self.upscale_pixels = None;
                self.upscale_repeat_revision = None;
                self.upscale_repeat_observed = false;
                self.phase = Phase::AwaitUpscale {
                    kernel,
                    source_width,
                    source_height,
                    upscale_revision,
                    upscale_frame_revision,
                    presentation_revision,
                };
                if !click(input_bounds) {
                    self.fail("Firefox Upscale action click dispatch failed");
                }
                if self.viewer_scenario == "rapid" && kernel + 1 < EXPLORE_UPSCALE_ACTIONS.len() {
                    self.phase = Phase::StartUpscale {
                        kernel: kernel + 1,
                        source_width,
                        source_height,
                        upscale_revision,
                        upscale_frame_revision,
                        presentation_revision,
                    };
                }
                None
            }
            Phase::DetailCloseEvidence => {
                self.phase = Phase::AwaitDetailClose;
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::ExploreDatasetReopen {
                revision,
                frame_revision,
            } => {
                self.phase = Phase::AwaitExploreDatasetReopen {
                    revision,
                    frame_revision,
                };
                if !click(input_bounds) {
                    self.fail("Firefox Explore dataset-reopen click dispatch failed");
                }
                None
            }
            Phase::GalleryReselect => {
                let Some((columns, _, slot, _, revision)) = self.selection_grid else {
                    self.fail("reopened Explore selection has no source revision");
                    return None;
                };
                let selected = gallery_slot_bounds(input_bounds, columns, slot, self.atlas_clip.0);
                report(
                    "integration.explore_pointer_scheduled",
                    EXPLORE_GALLERY,
                    "reopened-grid-slot",
                    [
                        revision as f64,
                        slot as f64,
                        f64::from(selected.center_x()),
                        f64::from(selected.center_y()),
                    ],
                );
                self.phase = Phase::AwaitDetailAgain;
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, revision, false) {
                    self.fail("Firefox reopened-gallery click dispatch failed");
                }
                None
            }
            Phase::GallerySweep => {
                #[cfg(target_arch = "wasm32")]
                let dispatched = sweep_js(
                    f64::from(input_bounds.x),
                    f64::from(input_bounds.y),
                    f64::from(input_bounds.width),
                    f64::from(input_bounds.height),
                ) == 1;
                #[cfg(not(target_arch = "wasm32"))]
                let dispatched = false;
                if !dispatched {
                    self.fail("Firefox sweep dispatch failed");
                    return None;
                }
                self.phase = Phase::AwaitGallerySweep;
                None
            }
            Phase::AnnotationPointer(revision) => {
                #[cfg(target_arch = "wasm32")]
                let gesture = {
                    let frame = self
                        .annotation_frame_ready
                        .expect("sampleable annotation frame");
                    let (width, height) = (
                        f64::from(frame.content_width),
                        f64::from(frame.content_height),
                    );
                    let points = self.copy_product_gesture.unwrap_or_else(|| {
                        if let Some(object) = self
                            .copy_before
                            .as_ref()
                            .filter(|_| self.viewer_scenario == "copy")
                        {
                            let b = &object.box_;
                            let mut start = [
                                f64::from((b.first.x + b.second.x) / 2.0),
                                f64::from((b.first.y + b.second.y) / 2.0),
                            ];
                            if self.copy_step == 0 || self.copy_step == 3 {
                                if let Some(run) = object.mask.runs.first() {
                                    start = [
                                        (f64::from(run.first) + f64::from(run.last)) / 2.0,
                                        f64::from(run.row) + 0.5,
                                    ];
                                }
                            }
                            if self.copy_step == 2 {
                                start = [
                                    (f64::from(b.second.x) + 3.0).min(width - 1.0),
                                    f64::from((b.first.y + b.second.y) / 2.0),
                                ];
                            }
                            if self.copy_step == 1 {
                                start = [f64::from(b.second.x), f64::from(b.second.y)];
                            }
                            [
                                start[0],
                                start[1],
                                (start[0] + 8.0).min(width - 1.0),
                                (start[1] + 8.0).min(height - 1.0),
                            ]
                        } else {
                            let top = match self.copy_step {
                                5 => 0.15,
                                6 => 0.35,
                                7 => 0.7,
                                _ => 0.3,
                            };
                            let [x, y, ex, ey] = match self.copy_shape_points {
                                0 => [0.25, top, 0.3, top + 0.05],
                                1 => [0.6, top, 0.65, top + 0.05],
                                _ => [0.45, 0.65, 0.5, 0.7],
                            };
                            [x * width, y * height, ex * width, ey * height]
                        }
                    });
                    annotation_checks::place_gesture(input_bounds, (width, height), points)
                };
                #[cfg(target_arch = "wasm32")]
                let dispatched = annotation_pointer_js(
                    f64::from(input_bounds.x),
                    f64::from(input_bounds.y),
                    f64::from(input_bounds.width),
                    f64::from(input_bounds.height),
                    gesture[0],
                    gesture[1],
                    gesture[2],
                    gesture[3],
                    self.copy_product_cancel,
                ) == 1;
                #[cfg(not(target_arch = "wasm32"))]
                let dispatched = false;
                if !dispatched {
                    self.fail("Firefox annotation pointer dispatch failed");
                    return None;
                }
                self.phase = if self.copy_step == 8 {
                    Phase::CopyProductWait
                } else {
                    Phase::AwaitPointer(revision)
                };
                None
            }
            Phase::GalleryLater(baseline) => {
                self.phase = Phase::AwaitGalleryScroll(baseline);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::DetailNext(selected) => {
                self.phase = Phase::AwaitNext(selected);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::DetailPrevious(selected) => {
                self.phase = Phase::AwaitPrevious(selected);
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::AnnotationTool { revision, tool } => {
                self.phase = Phase::AwaitTool { revision, tool };
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            Phase::AnnotationSidebar { revision, tool } => {
                self.phase = Phase::AnnotationOperation { revision, tool };
                None
            }
            Phase::AnnotationOperation { revision, tool } => {
                self.phase = Phase::AnnotationStop { revision, tool };
                None
            }
            Phase::AnnotationStop { revision, tool } => {
                self.phase = Phase::AnnotationBrush { revision, tool };
                None
            }
            Phase::AnnotationBrush { revision, tool } => {
                self.phase = Phase::AnnotationTimeline { revision, tool };
                None
            }
            Phase::AnnotationTimeline { revision, tool } => {
                self.phase = Phase::AnnotationTool { revision, tool };
                None
            }
            Phase::AnnotationSurface(revision) => {
                self.phase = Phase::AwaitAnnotationFrame(revision);
                None
            }
            Phase::GalleryImage(index) => {
                let Some((columns, rows, expected_slot, snapshot_revision, frame_revision)) =
                    self.selection_grid
                else {
                    self.fail("Explore selection grid is unavailable");
                    return None;
                };
                let resolved = (rows / 2)
                    .saturating_mul(columns)
                    .saturating_add(columns / 2);
                let selected =
                    gallery_slot_bounds(bounds, columns, expected_slot, self.atlas_clip.0);
                let pointer_x = selected.x + selected.width * 0.5;
                let pointer_y = selected.y + selected.height * 0.5;
                report(
                    "integration.explore_pointer_inverse",
                    EXPLORE_GALLERY,
                    "rendered-grid-slot",
                    [
                        expected_slot as f64,
                        resolved as f64,
                        index as f64,
                        snapshot_revision as f64,
                    ],
                );
                report(
                    "integration.explore_pointer_scheduled",
                    EXPLORE_GALLERY,
                    "real-canvas-pointer",
                    [
                        frame_revision as f64,
                        expected_slot as f64,
                        f64::from(pointer_x),
                        f64::from(pointer_y),
                    ],
                );
                self.phase = Phase::AwaitDetail(index);
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, frame_revision, false) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
            _ => {
                self.phase = match self.phase {
                    Phase::TrainNavigation => Phase::AwaitTrain,
                    Phase::Compile => Phase::AwaitCompileProgress,
                    Phase::ExploreNavigation => Phase::AwaitExplore,
                    Phase::ExploreOpen => {
                        COMPLETION_WITHOUT_INPUT.with(|active| active.set(true));
                        Phase::AwaitExploreReady
                    }
                    Phase::OpenAnnotation => Phase::AwaitAnnotation,
                    _ => self.phase.clone(),
                };
                if !click(input_bounds) {
                    self.fail("Firefox click dispatch failed");
                }
                None
            }
        }
    }

    fn advance_to(&mut self, phase: Phase) -> Task<RootMessage> {
        report(
            "integration.phase_advanced",
            "",
            &format!("{phase:?}"),
            [0.0; 4],
        );
        let reveal_annotation = matches!(phase, Phase::CopyProductWait);
        self.phase = phase;
        // A completed local step has no pending native event to wake its
        // successor. Queue one continuation without requiring another draw.
        let continuation = Task::done(RootMessage::Integration(Message::Advance));
        if reveal_annotation {
            // The compact inspector scrolls the canvas offscreen. A pixel
            // assertion must reveal it before waiting for a completed draw.
            iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, RelativeOffset::START)
                .chain(continuation)
        } else {
            continuation
        }
    }

    fn report_phase_progress(&mut self) {
        if self.reported_phase.as_ref() == Some(&self.phase) {
            return;
        }
        report(
            "integration.phase_progress",
            self.phase.deadline_class(),
            &format!("{:?}", self.phase),
            [0.0; 4],
        );
        self.reported_phase = Some(self.phase.clone());
    }

    pub fn advance(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
        router: &crate::view::router::Router,
        active: FeatureId,
        frame: Option<crate::presentation_surface::FrameReady>,
    ) -> Task<RootMessage> {
        self.report_phase_progress();
        if !self.running() {
            return Task::none();
        }
        self.input_scale = applied_scale;
        self.observe_presentation(model, frame);
        if let Some(snapshot) = model.explore.snapshot.as_ref()
            && snapshot.revision > self.explore_snapshot_revision
        {
            self.explore_snapshot_revision = snapshot.revision;
            let aspect = settings.draft.as_ref().map_or(
                crate::generated::WorkspaceAspectRatio::Widescreen,
                |draft| draft.ui.workspaceaspectratio,
            );
            report(
                "integration.explore_state",
                if snapshot.mode == crate::generated::ExploreMode::Detail {
                    explore::DETAIL_WORKSPACE_ID
                } else {
                    EXPLORE_GALLERY
                },
                if snapshot.augmentation.enabled {
                    "augmentation-enabled"
                } else {
                    "augmentation-disabled"
                },
                [
                    snapshot.revision as f64,
                    snapshot.augmentation.seed as f64,
                    if snapshot.detail.showoriginaldimensions {
                        1.0
                    } else {
                        0.0
                    },
                    f64::from(crate::view::aspect_ratio::height_factor(aspect)),
                ],
            );
            report(
                "integration.explore_frame",
                explore::DETAIL_WORKSPACE_ID,
                if snapshot.detail.showoriginaldimensions {
                    "original"
                } else {
                    "padded"
                },
                [
                    snapshot.frame.revision as f64,
                    snapshot.frame.extent.width as f64,
                    snapshot.frame.extent.height as f64,
                    snapshot.selectedimage.map_or(-1.0, f64::from),
                ],
            );
            if snapshot.mode == crate::generated::ExploreMode::Gallery {
                for (slot, compiled_index) in
                    snapshot.order.visibleindices.iter().copied().enumerate()
                {
                    report(
                        "integration.explore_slot",
                        EXPLORE_GALLERY,
                        if snapshot.focusedimage.is_some() {
                            "current-focused"
                        } else {
                            "current"
                        },
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            slot as f64,
                            f64::from(compiled_index),
                        ],
                    );
                }
            }
        }
        if let Some(error) = model.error.as_ref()
            && !matches!(
                self.phase,
                Phase::Disabled
                    | Phase::Complete
                    | Phase::Failed
                    | Phase::AwaitErrorModal
                    | Phase::ErrorModal
                    | Phase::ErrorCopy
                    | Phase::AwaitErrorCopy
                    | Phase::ErrorDismiss
                    | Phase::AwaitErrorDismissed
                    | Phase::ViewerAwaitDisconnect
                    | Phase::ViewerReconnect
            )
        {
            self.fail(&format!("{}: {}", error.title, error.detail));
            return Task::none();
        }
        match self.phase.clone() {
            Phase::AtlasPixelColumns(columns) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.viewport.columns != columns
                    || snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.overlay.showlabels
                    || !snapshot.overlay.showmasks
                    || !snapshot.overlay.showboxes
                    || self.atlas_composition.as_ref().is_none_or(|draw| {
                        draw.snapshot.frame != snapshot.frame
                            || draw.snapshot.viewport.columns != columns
                    })
                    || self.atlas_pixels.as_ref().is_none_or(|draw| {
                        draw.snapshot.frame != snapshot.frame
                            || draw.snapshot.gallery.slots.iter().any(|ready| !*ready)
                    })
                {
                    return Task::none();
                }
                let next = if columns == 4 { 10 } else { self.atlas_columns };
                self.phase = if columns == 4 {
                    Phase::AtlasPixelColumns(10)
                } else {
                    Phase::AtlasPixelRestore
                };
                let next_columns = explore_message(explore::Message::Gallery(
                    explore::gallery::Message::ColumnsChanged(next as i32),
                ));
                if columns == 10 {
                    next_columns
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::LabelsToggled(self.atlas_fixture_labels),
                            ),
                        )))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::MasksToggled(self.atlas_fixture_masks),
                            ),
                        )))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::BoxesToggled(self.atlas_fixture_boxes),
                            ),
                        )))
                } else {
                    next_columns
                }
            }
            Phase::AtlasPixelRestore => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.viewport.columns != self.atlas_columns
                    || snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.overlay.showlabels != self.atlas_fixture_labels
                    || snapshot.overlay.showmasks != self.atlas_fixture_masks
                    || snapshot.overlay.showboxes != self.atlas_fixture_boxes
                    || self
                        .atlas_pixels
                        .as_ref()
                        .is_none_or(|draw| draw.snapshot.frame != snapshot.frame)
                {
                    return Task::none();
                }
                self.advance_to(Phase::AwaitExploreReady)
            }
            Phase::ViewerConfirmSettings | Phase::ViewerRestoreSettings => {
                let restore = self.phase == Phase::ViewerRestoreSettings;
                let Some(snapshot) = settled_settings_snapshot(
                    model,
                    settings,
                    self.viewer_continuity_settings_revision,
                ) else {
                    return Task::none();
                };
                let expected = if restore {
                    self.viewer_continuity_performance
                } else {
                    !self.viewer_continuity_performance
                };
                if snapshot.settingsstate.ui.showworkspaceperformance != expected {
                    return Task::none();
                }
                if model.explore.requested_upscale != self.viewer_continuity_request
                    || model.current_upscale().is_none_or(|upscale| {
                        self.upscale_cached_frames[2].as_ref() != Some(&upscale.frame)
                    })
                {
                    self.fail("same-route Settings confirmation changed the resident viewer demand or result");
                    return Task::none();
                }
                self.viewer_continuity_settings_revision = snapshot.revision;
                if !restore {
                    self.phase = Phase::ViewerRestoreSettings;
                    return Task::done(RootMessage::Settings(
                        crate::view::settings::Message::PerformanceChanged(
                            self.viewer_continuity_performance,
                        ),
                    ));
                }
                report(
                    "integration.viewer_settings_preserved",
                    explore::DETAIL_WORKSPACE_ID,
                    "confirmed-native-settings",
                    [snapshot.revision as f64, 1.0, 0.0, 0.0],
                );
                self.viewer_route_persistence =
                    settings.draft.is_some() && model.settings_edit_available();
                report(
                    "integration.viewer_departure_started",
                    explore::DETAIL_WORKSPACE_ID,
                    "upscale-observation",
                    [
                        model
                            .upscale_snapshot
                            .as_ref()
                            .map_or(0, |state| state.revision) as f64,
                        u8::from(self.viewer_route_persistence) as f64,
                        snapshot.revision as f64,
                        0.0,
                    ],
                );
                self.phase = Phase::ViewerDepart;
                Task::done(RootMessage::Settings(crate::view::settings::Message::Close)).chain(
                    Task::done(RootMessage::Workspace(
                        crate::view::router::Message::Navigation(
                            crate::view::navigation::Message::PageSelected(FeatureId::Train),
                        ),
                    )),
                )
            }
            Phase::ViewerDepart => {
                if active != FeatureId::Train
                    || model.explore.requested_upscale.is_some()
                    || model.has_pending(crate::generated::ApplicationIntentEndpoint::UpscaleStop)
                    || !route_edit_available(model, settings)
                {
                    return Task::none();
                }
                if model.foreground_visual().is_some() {
                    self.fail("mapped Train departure retained a visual foreground");
                    return Task::none();
                }
                if self.viewer_route_persistence
                    && !model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.revision > self.viewer_continuity_settings_revision
                            && snapshot.settingsstate.currentview == FeatureId::Train
                    })
                {
                    return Task::none();
                }
                self.viewer_continuity_settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                if crate::presentation_surface::pixel_trace::enabled() {
                    report(
                        "integration.viewer_route_confirmed",
                        "navigation.train",
                        "None",
                        [
                            self.viewer_continuity_settings_revision as f64,
                            u8::from(self.viewer_route_persistence) as f64,
                            1.0,
                            0.0,
                        ],
                    );
                }
                report(
                    "integration.viewer_abandoned",
                    explore::DETAIL_WORKSPACE_ID,
                    "mapped-route-departure",
                    [
                        model
                            .upscale_snapshot
                            .as_ref()
                            .map_or(0, |state| state.revision) as f64,
                        0.0,
                        0.0,
                        0.0,
                    ],
                );
                self.phase = Phase::ViewerReenter;
                self.viewer_drawn = None;
                SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().viewer = None);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Navigation(
                        crate::view::navigation::Message::PageSelected(FeatureId::Explore),
                    ),
                ))
            }
            Phase::ViewerReenter => {
                if active != FeatureId::Explore {
                    return Task::none();
                }
                if !route_edit_available(model, settings) {
                    return Task::none();
                }
                if self.viewer_route_persistence
                    && !model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.revision > self.viewer_continuity_settings_revision
                            && snapshot.settingsstate.currentview == FeatureId::Explore
                    })
                {
                    return Task::none();
                }
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                let Some(request) = model.explore.requested_upscale.as_ref() else {
                    return Task::none();
                };
                if request.kernel != crate::generated::UpscaleKernel::Default
                    || self.viewer_continuity_request.as_ref().is_none_or(|prior| {
                        prior.source != request.source || prior.document != request.document
                    })
                    || model.displayed_upscale_kernel()
                        != Some(crate::generated::UpscaleKernel::Default)
                    || self.upscale_cached_frames[0].as_ref() != Some(&upscale.frame)
                    || self
                        .viewer_drawn
                        .is_none_or(|(_, source, _)| source != upscale.frame.revision)
                {
                    return Task::none();
                }
                if !matches!(
                    model.foreground_visual(),
                    Some(
                        crate::generated::PresentationSourceKind::Explore
                            | crate::generated::PresentationSourceKind::Upscale
                    )
                ) {
                    self.fail("mapped Explore reentry has a mismatched visual foreground");
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    return Task::none();
                };
                if self
                    .viewer_drawn
                    .is_none_or(|(drawn, _, _)| drawn != sampleable.presentation_revision)
                {
                    return Task::none();
                }
                if crate::presentation_surface::pixel_trace::enabled() {
                    report(
                        "integration.viewer_route_confirmed",
                        "navigation.explore",
                        if model.foreground_visual()
                            == Some(crate::generated::PresentationSourceKind::Upscale)
                        {
                            "Upscale"
                        } else {
                            "Explore"
                        },
                        [
                            model
                                .settings_snapshot
                                .as_ref()
                                .map_or(0, |snapshot| snapshot.revision)
                                as f64,
                            u8::from(self.viewer_route_persistence) as f64,
                            1.0,
                            0.0,
                        ],
                    );
                }
                report(
                    "integration.viewer_basic_reentry",
                    explore::DETAIL_WORKSPACE_ID,
                    "automatic-completed-draw",
                    [
                        upscale.frame.revision as f64,
                        sampleable.presentation_revision as f64,
                        upscale.frame.source.instance as f64,
                        upscale.frame.cleanrevision as f64,
                    ],
                );
                self.viewer_continuity_request = Some(request.clone());
                self.phase = Phase::ViewerAwaitDisconnect;
                // Retiring the real outbound owner closes the worker's socket;
                // the ordinary transport subscription performs the reconnect.
                Task::done(RootMessage::Transport(
                    crate::transport::TransportEvent::Disconnected(
                        "rendered continuity acceptance".into(),
                    ),
                ))
            }
            Phase::ViewerAwaitDisconnect => {
                if model.connection == crate::view_model::ConnectionState::Connected {
                    return Task::none();
                }
                self.viewer_drawn = None;
                SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().viewer = None);
                self.phase = Phase::ViewerReconnect;
                Task::none()
            }
            Phase::ViewerReconnect => {
                if model.connection != crate::view_model::ConnectionState::Connected
                    || model.error.is_some()
                    || active != FeatureId::Explore
                    || model.explore.requested_upscale != self.viewer_continuity_request
                {
                    return Task::none();
                }
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                let Some((drawn, source, _)) = self.viewer_drawn else {
                    return Task::none();
                };
                if source != upscale.frame.revision
                    || model.displayed_upscale_kernel()
                        != Some(crate::generated::UpscaleKernel::Default)
                    || self.upscale_cached_frames[0].as_ref() != Some(&upscale.frame)
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    source,
                ) else {
                    return Task::none();
                };
                if drawn != sampleable.presentation_revision {
                    return Task::none();
                }
                report(
                    "integration.viewer_reconnected",
                    explore::DETAIL_WORKSPACE_ID,
                    "matching-completed-draw",
                    [
                        source as f64,
                        drawn as f64,
                        upscale.frame.source.instance as f64,
                        upscale.frame.cleanrevision as f64,
                    ],
                );
                self.annotation_sample_baseline = drawn;
                let (next, control) = match self.viewer_scenario.as_str() {
                    "copy" => (Phase::OpenAnnotation, EXPLORE_ANNOTATE),
                    "semantics" => (Phase::ViewerNoAspect, "explore.detail.aspect"),
                    _ => (
                        Phase::DetailNext(
                            model
                                .explore
                                .snapshot
                                .as_ref()
                                .and_then(|snapshot| snapshot.selectedimage)
                                .unwrap_or_default(),
                        ),
                        EXPLORE_NEXT,
                    ),
                };
                self.phase = next;
                self.arm(control)
            }
            Phase::Disabled | Phase::Complete | Phase::Failed => Task::none(),
            Phase::AwaitBootstrap
                if model.connection == ConnectionState::Connected
                    && model.settings_snapshot.is_some()
                    && model.window_width > 0
                    && model.window_height > 0
                    && !self.dataset_source.is_empty()
                    && !self.compiled_directory.is_empty()
                    && !self.resolution.is_empty() =>
            {
                report(
                    "integration.bootstrap",
                    "",
                    "typed-bootstrap",
                    [
                        model.window_width.into(),
                        model.window_height.into(),
                        0.0,
                        0.0,
                    ],
                );
                if !self.viewer_scenario.is_empty() {
                    self.phase = Phase::TrainNavigation;
                    return self.arm(crate::view::navigation::stable_id(FeatureId::Train));
                }
                let dark_mode = settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.ui.darkmode);
                let fluent =
                    crate::fluent_theme::conformance(&crate::fluent_theme::app_theme(dark_mode));
                self.resolved_primary = [
                    f64::from(fluent.primary_color.r),
                    f64::from(fluent.primary_color.g),
                    f64::from(fluent.primary_color.b),
                    f64::from(fluent.primary_color.a),
                ];
                self.resolved_benchmark = [
                    f64::from(fluent.benchmark_color.r),
                    f64::from(fluent.benchmark_color.g),
                    f64::from(fluent.benchmark_color.b),
                    f64::from(fluent.benchmark_color.a),
                ];
                report(
                    "integration.fluent_shell",
                    "navigation",
                    if fluent.dark { "dark" } else { "light" },
                    [
                        f64::from(fluent.navigation_border),
                        f64::from(fluent.card_border),
                        f64::from(fluent.primary_border),
                        if fluent.resolved { 1.0 } else { 0.0 },
                    ],
                );
                let status = crate::view::workflow::model_card::status_presentation(
                    model.model_snapshot.as_ref(),
                );
                report(
                    "integration.model_copy",
                    TRAIN_MODEL_CARD,
                    crate::view::workflow::model_card::card_title(FeatureId::Train),
                    [
                        if status.label.is_empty() { 0.0 } else { 1.0 },
                        0.0,
                        0.0,
                        0.0,
                    ],
                );
                self.phase = Phase::SettingsOpen;
                self.arm("navigation.settings")
            }
            Phase::SettingsOpen => self.arm("navigation.settings"),
            Phase::AwaitSettings if settings.open => {
                self.phase = Phase::SettingsModal;
                self.arm(SETTINGS_MODAL)
            }
            Phase::SettingsModal => self.arm(SETTINGS_MODAL),
            Phase::SettingsGroup(index) => self.arm(SETTINGS_GROUPS[index]),
            Phase::SettingsScaleDrag => {
                self.ui_scale_baseline = applied_scale;
                self.ui_scale_first = None;
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                self.arm(SETTINGS_NUMERIC_CONTROLS[0])
            }
            Phase::AwaitSettingsScaleDrag => {
                let Some(current) = settings.draft.as_ref().map(|draft| draft.ui.uiscale) else {
                    return Task::none();
                };
                if same_numeric_value(f64::from(current), f64::from(self.ui_scale_baseline)) {
                    return Task::none();
                }
                if !same_numeric_value(f64::from(applied_scale), f64::from(self.ui_scale_baseline))
                {
                    self.fail("UI scale changed before the pointer was released");
                    return Task::none();
                }
                if self.ui_scale_first.is_none() {
                    self.ui_scale_first = Some(current);
                    report(
                        "integration.ui_scale_drag",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "first-position",
                        [
                            f64::from(self.ui_scale_baseline),
                            f64::from(current),
                            f64::from(applied_scale),
                            1.0,
                        ],
                    );
                    return Task::none();
                }
                if self
                    .ui_scale_first
                    .is_some_and(|first| same_numeric_value(f64::from(first), f64::from(current)))
                {
                    return Task::none();
                }
                if current <= 0.85 {
                    self.fail("UI-scale drag did not move the draft beyond 0.85");
                    return Task::none();
                }
                report(
                    "integration.ui_scale_drag",
                    SETTINGS_NUMERIC_CONTROLS[0],
                    "second-position",
                    [
                        f64::from(self.ui_scale_baseline),
                        f64::from(current),
                        f64::from(applied_scale),
                        2.0,
                    ],
                );
                self.phase = Phase::AwaitSettingsScaleRelease;
                Task::none()
            }
            Phase::AwaitSettingsScaleRelease => {
                let Some(current) = settings.draft.as_ref().map(|draft| draft.ui.uiscale) else {
                    return Task::none();
                };
                if !same_numeric_value(f64::from(applied_scale), f64::from(current)) {
                    return Task::none();
                }
                self.phase = Phase::AwaitSettingsScaleSnapshot;
                Task::none()
            }
            Phase::AwaitSettingsScaleSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.uiscale > 0.85
                            && same_numeric_value(
                                f64::from(snapshot.settingsstate.ui.uiscale),
                                f64::from(applied_scale),
                            )
                    }) =>
            {
                report(
                    "integration.ui_scale_drag",
                    SETTINGS_NUMERIC_CONTROLS[0],
                    "released-and-settled",
                    ui_scale_evidence(model, settings, self.ui_scale_baseline, applied_scale),
                );
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(self.settings_revision, |snapshot| snapshot.revision);
                self.phase = Phase::AwaitSettingsScaleRestoreDraft;
                Task::done(RootMessage::Settings(
                    crate::view::settings::Message::UiScaleChanged(self.ui_scale_baseline),
                ))
            }
            Phase::AwaitSettingsScaleRestoreDraft
                if settings.draft.as_ref().is_some_and(|draft| {
                    same_numeric_value(
                        f64::from(draft.ui.uiscale),
                        f64::from(self.ui_scale_baseline),
                    )
                }) =>
            {
                self.phase = Phase::AwaitSettingsScaleRestored;
                Task::done(RootMessage::Settings(
                    crate::view::settings::Message::UiScaleReleased,
                ))
            }
            Phase::AwaitSettingsScaleRestored
                if same_numeric_value(
                    f64::from(applied_scale),
                    f64::from(self.ui_scale_baseline),
                ) && settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        same_numeric_value(
                            f64::from(snapshot.settingsstate.ui.uiscale),
                            f64::from(self.ui_scale_baseline),
                        )
                    }) =>
            {
                report(
                    "integration.ui_scale_restored",
                    SETTINGS_NUMERIC_CONTROLS[0],
                    "baseline",
                    ui_scale_evidence(model, settings, self.ui_scale_baseline, applied_scale),
                );
                self.phase = Phase::SettingsShowFps;
                self.arm(SETTINGS_SHOW_FPS)
            }
            Phase::SettingsShowFps => {
                self.show_fps_baseline = settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.ui.showworkspaceperformance);
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                self.arm(SETTINGS_SHOW_FPS)
            }
            Phase::AwaitSettingsShowFps
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.ui.showworkspaceperformance != self.show_fps_baseline
                }) =>
            {
                self.phase = Phase::AwaitSettingsShowFpsChangedSnapshot;
                Task::none()
            }
            Phase::AwaitSettingsShowFpsChangedSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.showworkspaceperformance != self.show_fps_baseline
                    }) =>
            {
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(self.settings_revision, |snapshot| snapshot.revision);
                self.phase = Phase::SettingsRestoreShowFps;
                self.arm(SETTINGS_SHOW_FPS)
            }
            Phase::SettingsRestoreShowFps => self.arm(SETTINGS_SHOW_FPS),
            Phase::AwaitSettingsShowFpsRestored
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.ui.showworkspaceperformance == self.show_fps_baseline
                }) =>
            {
                self.phase = Phase::AwaitSettingsShowFpsSnapshot;
                Task::none()
            }
            Phase::AwaitSettingsShowFpsSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.showworkspaceperformance == self.show_fps_baseline
                    }) =>
            {
                report(
                    "integration.show_fps",
                    SETTINGS_SHOW_FPS,
                    "round-trip",
                    [
                        1.0,
                        1.0,
                        self.settings_revision as f64,
                        model
                            .settings_snapshot
                            .as_ref()
                            .map_or(0.0, |snapshot| snapshot.revision as f64),
                    ],
                );
                self.phase = Phase::SettingsNumeric { index: 0, part: 0 };
                self.arm(settings_numeric_id(0, 0))
            }
            Phase::SettingsNumeric { index, part } => self.arm(settings_numeric_id(index, part)),
            Phase::SettingsFooter => self.arm(SETTINGS_FOOTER),
            Phase::SettingsReset => self.arm(SETTINGS_RESET),
            Phase::SettingsClose => self.arm(SETTINGS_CLOSE),
            Phase::AwaitSettingsClosed if !settings.open => {
                self.phase = Phase::TrainNavigation;
                self.arm(crate::view::navigation::stable_id(FeatureId::Train))
            }
            Phase::TrainNavigation => {
                self.arm(crate::view::navigation::stable_id(FeatureId::Train))
            }
            Phase::AwaitTrain if active == FeatureId::Train => {
                if !self.viewer_scenario.is_empty() {
                    self.phase = Phase::DatasetSource;
                    return self.arm(DATASET_SOURCE);
                }
                self.phase = Phase::PageRegion {
                    page: FeatureId::Train,
                    index: 0,
                };
                self.arm(region_id(FeatureId::Train, 0))
            }
            Phase::PageNavigation(page) if route_edit_available(model, settings) => {
                self.arm(crate::view::navigation::stable_id(page))
            }
            Phase::AwaitPage(page) if active == page => {
                self.phase = Phase::PageRegion { page, index: 0 };
                self.arm(region_id(page, 0))
            }
            Phase::PageRegion { page, index } => self.arm(region_id(page, index)),
            Phase::TrainModelCard => self.arm(TRAIN_MODEL_CARD),
            Phase::TrainModelPart(index) => self.arm(TRAIN_MODEL_PARTS[index]),
            Phase::TrainModelProgress => self.arm(TRAIN_MODEL_PROGRESS),
            Phase::ReturnTrain if route_edit_available(model, settings) => {
                self.arm(crate::view::navigation::stable_id(FeatureId::Train))
            }
            Phase::AwaitReturnTrain if active == FeatureId::Train => {
                self.phase = Phase::AdvancedField(0);
                report(
                    "integration.phase",
                    &advanced_field_id(0),
                    "advanced-field-0",
                    [0.0; 4],
                );
                self.arm_scrolled(advanced_field_id(0), RelativeOffset::END)
            }
            Phase::AdvancedField(index) => {
                if index == 2 || index == 4 {
                    self.spinner_baseline = advanced_control_value(settings, index, 0.0);
                }
                self.arm(advanced_field_id(index))
            }
            Phase::AdvancedSpinnerEdge { index, .. } => self.arm(advanced_field_id(index)),
            Phase::AdvancedSpinnerWheel(index) => self.arm(advanced_field_id(index)),
            Phase::AdvancedSpinnerVerify { index, upper } => {
                let value = advanced_control_value(settings, index, self.spinner_baseline);
                if value != self.spinner_baseline {
                    self.fail("numeric edge exposed an increment or decrement hit target");
                    return Task::none();
                }
                if upper {
                    self.phase = Phase::AdvancedSpinnerEdge {
                        index,
                        upper: false,
                    };
                    self.arm(advanced_field_id(index))
                } else {
                    self.phase = Phase::AdvancedSpinnerWheel(index);
                    self.arm(advanced_field_id(index))
                }
            }
            Phase::AdvancedSpinnerWheelVerify(index) => {
                let value = advanced_control_value(settings, index, self.spinner_baseline);
                if value != self.spinner_baseline {
                    self.fail("numeric field allowed wheel mutation");
                    return Task::none();
                }
                report(
                    "integration.spinnerless",
                    &advanced_field_id(index),
                    if index == 2 {
                        "integer-upper-lower-edges"
                    } else {
                        "floating-upper-lower-edges"
                    },
                    [value, 1.0, 1.0, 1.0],
                );
                self.begin_advanced_numeric_edit(model, settings, index)
            }
            Phase::AdvancedNumericEdit(index) => self.arm(advanced_field_id(index)),
            Phase::AwaitAdvancedNumericDraft(index)
                if same_numeric_value(
                    advanced_control_value(settings, index, f64::NAN),
                    self.numeric_target,
                ) =>
            {
                self.phase = Phase::AwaitAdvancedNumericSnapshot(index);
                Task::none()
            }
            Phase::AwaitAdvancedNumericSnapshot(index)
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|_| {
                        advanced_snapshot_value(model, index)
                            .is_some_and(|value| same_numeric_value(value, self.numeric_target))
                    }) =>
            {
                report(
                    "integration.advanced_edit",
                    &advanced_field_id(index),
                    if index == 2 { "integer" } else { "floating" },
                    [
                        self.numeric_target,
                        self.settings_revision as f64,
                        model
                            .settings_snapshot
                            .as_ref()
                            .map_or(0.0, |snapshot| snapshot.revision as f64),
                        1.0,
                    ],
                );
                self.phase = Phase::AdvancedField(index + 1);
                self.arm(advanced_field_id(index + 1))
            }
            Phase::AdvancedAssignment => {
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                self.arm_scrolled(train::MATCH_FREE_ASSIGNMENT_ID, RelativeOffset::END)
            }
            Phase::AwaitAdvancedAssignmentDraft
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.request.trainingsupervision.assignment
                        == crate::generated::TrainAssignmentKind::MatchFree
                }) =>
            {
                self.phase = Phase::AwaitAdvancedAssignmentSnapshot;
                Task::none()
            }
            Phase::AwaitAdvancedAssignmentSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .request
                            .trainingsupervision
                            .assignment
                            == crate::generated::TrainAssignmentKind::MatchFree
                    }) =>
            {
                self.phase = Phase::AdvancedMatchFree(0);
                self.arm(match_free_field_id(0))
            }
            Phase::AdvancedMatchFree(index) => self.arm(match_free_field_id(index)),
            Phase::AdvancedDenoisingToggle => {
                self.denoising_target = settings.draft.as_ref().is_none_or(|draft| {
                    !draft
                        .workflows
                        .train
                        .request
                        .trainingsupervision
                        .denoising
                        .enabled
                });
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                self.arm(
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled()
                        .stable_field_id
                        .to_string(),
                )
            }
            Phase::AwaitAdvancedDenoisingDraft
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft
                        .workflows
                        .train
                        .request
                        .trainingsupervision
                        .denoising
                        .enabled
                        == self.denoising_target
                }) =>
            {
                self.phase = Phase::AwaitAdvancedDenoisingSnapshot;
                Task::none()
            }
            Phase::AwaitAdvancedDenoisingSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .request
                            .trainingsupervision
                            .denoising
                            .enabled
                            == self.denoising_target
                    }) =>
            {
                if self.denoising_target {
                    self.phase = Phase::AdvancedDenoising(0);
                    self.arm(denoising_field_id(0))
                } else {
                    self.phase = Phase::AdvancedDenoisingToggle;
                    Task::none()
                }
            }
            Phase::AdvancedDenoising(index) => self.arm(denoising_field_id(index)),
            Phase::TriggerError => {
                self.phase = Phase::AwaitErrorModal;
                Task::done(RootMessage::Workspace(crate::view::router::Message::Train(
                    train::Message::StartRequested,
                )))
            }
            Phase::AwaitErrorModal if model.error.is_some() => {
                self.phase = Phase::ErrorModal;
                self.arm(ERROR_MODAL)
            }
            Phase::ErrorModal => self.arm(ERROR_MODAL),
            Phase::ErrorCopy => self.arm(ERROR_COPY),
            Phase::AwaitErrorCopy if model.error.is_some() => {
                self.phase = Phase::ErrorDismiss;
                self.arm(ERROR_DISMISS)
            }
            Phase::ErrorDismiss => self.arm(ERROR_DISMISS),
            Phase::AwaitErrorDismissed if model.error.is_none() => {
                report(
                    "integration.error_modal",
                    ERROR_MODAL,
                    "copy-and-dismiss",
                    [1.0, 1.0, 1.0, 0.0],
                );
                self.phase = Phase::TrainCard;
                self.arm_scrolled(TRAIN_CARD, RelativeOffset::START)
            }
            Phase::TrainCard => self.arm_scrolled(TRAIN_CARD, RelativeOffset::START),
            Phase::DatasetBrowse => self.arm(DATASET_BROWSE),
            Phase::BenchmarkOverride => {
                self.benchmark_baseline = settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.workflows.train.compilebenchmarkdatasetoverride);
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                self.arm(BENCHMARK_OVERRIDE)
            }
            Phase::AwaitBenchmarkOverride
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compilebenchmarkdatasetoverride != self.benchmark_baseline
                }) =>
            {
                self.phase = Phase::AwaitBenchmarkChangedSnapshot;
                Task::none()
            }
            Phase::AwaitBenchmarkChangedSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .compilebenchmarkdatasetoverride
                            != self.benchmark_baseline
                    }) =>
            {
                self.settings_revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(self.settings_revision, |snapshot| snapshot.revision);
                self.phase = Phase::BenchmarkRestore;
                self.arm(BENCHMARK_OVERRIDE)
            }
            Phase::BenchmarkRestore => self.arm(BENCHMARK_OVERRIDE),
            Phase::AwaitBenchmarkRestored
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compilebenchmarkdatasetoverride == self.benchmark_baseline
                }) =>
            {
                self.phase = Phase::AwaitBenchmarkSnapshot;
                Task::none()
            }
            Phase::AwaitBenchmarkSnapshot
                if settled_settings_snapshot(model, settings, self.settings_revision)
                    .is_some_and(|snapshot| {
                        snapshot
                            .settingsstate
                            .workflows
                            .train
                            .compilebenchmarkdatasetoverride
                            == self.benchmark_baseline
                    }) =>
            {
                report(
                    "integration.benchmark_override",
                    BENCHMARK_OVERRIDE,
                    "round-trip",
                    [
                        1.0,
                        1.0,
                        self.settings_revision as f64,
                        model
                            .settings_snapshot
                            .as_ref()
                            .map_or(0.0, |snapshot| snapshot.revision as f64),
                    ],
                );
                self.phase = Phase::DatasetSource;
                self.arm(DATASET_SOURCE)
            }
            Phase::DatasetSource => self.arm(DATASET_SOURCE),
            Phase::AwaitDatasetSource
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.datasetsourcedir == self.dataset_source
                }) =>
            {
                self.phase = Phase::CompiledDirectory;
                self.arm(COMPILED_DIRECTORY)
            }
            Phase::CompiledDirectory => self.arm(COMPILED_DIRECTORY),
            Phase::AwaitCompiledDirectory
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.compileddatasetdir == self.compiled_directory
                }) =>
            {
                self.phase = Phase::CompileDimensions;
                self.arm(COMPILE_DIMENSIONS)
            }
            Phase::CompileDimensions => self.arm(COMPILE_DIMENSIONS),
            Phase::AwaitCompileDimensions
                if settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| draft.workflows.train.compiledimensions) =>
            {
                self.phase = Phase::CompileResolution;
                self.arm(COMPILE_RESOLUTION)
            }
            Phase::CompileResolution => self.arm(COMPILE_RESOLUTION),
            Phase::AwaitCompileResolution
                if settings.draft.as_ref().is_some_and(|draft| {
                    draft.workflows.train.request.resolution.to_string() == self.resolution
                }) =>
            {
                let revision = model
                    .settings_snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.revision);
                self.phase = Phase::AwaitDatasetSettings(revision);
                Task::none()
            }
            Phase::AwaitDatasetSettings(revision) => {
                let Some(snapshot) = model.settings_snapshot.as_ref() else {
                    return Task::none();
                };
                let train = &snapshot.settingsstate.workflows.train;
                if snapshot.revision <= revision
                    || train.datasetsourcedir != self.dataset_source
                    || train.compileddatasetdir != self.compiled_directory
                    || train.request.resolution.to_string() != self.resolution
                    || !train.compiledimensions
                    || !train.usecompileddirectorydefaults
                    || !snapshot.exploresource.available
                    || snapshot.exploresource.selection
                        != crate::generated::ExploreDatasetSource::Train
                    || settings.has_local_edits()
                    || !model.dataset_compile_available()
                {
                    return Task::none();
                }
                report(
                    "integration.dataset_configured",
                    COMPILE_RESOLUTION,
                    "typed-settings",
                    [snapshot.revision as f64, 1.0, 0.0, 0.0],
                );
                self.phase = Phase::Compile;
                self.arm_scrolled(COMPILE_DATASET, RelativeOffset::END)
            }
            Phase::Compile => self.arm_scrolled(COMPILE_DATASET, RelativeOffset::END),
            phase @ (Phase::AwaitCompileProgress | Phase::AwaitCompileCompletion) => {
                let Some(dataset) = model.workflow.dataset.as_ref() else {
                    return Task::none();
                };
                let compile_succeeded = dataset.terminal.outcome
                    == crate::generated::ArtifactTerminalOutcome::Succeeded;
                if matches!(phase, Phase::AwaitCompileProgress)
                    && (dataset.active || compile_succeeded)
                    && (!dataset.progress.activity.is_empty() || dataset.progress.total != 0)
                    && dataset.progress.droppedinstances != 0
                {
                    report(
                        "integration.compile_progress",
                        COMPILE_PROGRESS,
                        &dataset.progress.activity,
                        [
                            dataset.generation as f64,
                            dataset.progress.completed as f64,
                            dataset.progress.total as f64,
                            dataset.progress.droppedinstances as f64,
                        ],
                    );
                    report(
                        "integration.compile_metrics",
                        COMPILE_PROGRESS,
                        "elapsed-eta-throughput-dropped",
                        [
                            dataset.progress.elapsedseconds as f64,
                            dataset.progress.remainingseconds as f64,
                            dataset.progress.throughputpersecond as f64,
                            dataset.progress.droppedinstances as f64,
                        ],
                    );
                    self.phase = Phase::CompileProgress;
                    return self.arm(COMPILE_PROGRESS);
                }
                if compile_succeeded {
                    let split = dataset.inspection.splits.first();
                    report(
                        "integration.dataset_complete",
                        DATASET_STATUS,
                        &dataset.terminal.artifact,
                        [
                            dataset.generation as f64,
                            split.map_or(0.0, |value| value.imagecount as f64),
                            split.map_or(0.0, |value| value.width as f64),
                            split.map_or(0.0, |value| value.height as f64),
                        ],
                    );
                    self.phase = Phase::DatasetStatus;
                    return self.arm(DATASET_STATUS);
                }
                Task::none()
            }
            Phase::CompileProgress => self.arm(COMPILE_PROGRESS),
            Phase::CompileActionWithProgress => self.arm(COMPILE_DATASET),
            Phase::DatasetStatus => self.arm(DATASET_STATUS),
            Phase::ExploreNavigation => {
                self.arm(crate::view::navigation::stable_id(FeatureId::Explore))
            }
            Phase::AwaitExplore
                if active == FeatureId::Explore
                    && !settings.has_local_edits()
                    && model.explore_open_available()
                    && router.explore_measured_viewport(3, 0, 0).is_some() =>
            {
                self.phase = Phase::ExploreOpen;
                self.arm(EXPLORE_OPEN)
            }
            Phase::ExploreOpen => self.arm(EXPLORE_OPEN),
            Phase::AwaitExploreReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.ready
                    && !snapshot.busy
                    && snapshot.frame.revision != 0
                    && !settings.has_local_edits()
                    && model.explore_viewport_available()
                {
                    if snapshot.dataset.classcatalogidentity == 0
                        || model.explore.presentation_state()
                            != crate::view_model::ExplorePresentationState::Populated
                    {
                        self.fail(
                            "Explore typed snapshot did not reach the populated catalog state",
                        );
                        return Task::none();
                    }
                    let measured = router.explore_measured_viewport(
                        snapshot.viewport.columns,
                        snapshot.viewport.firstrow,
                        snapshot.order.matchingcount,
                    );
                    let Some(measured) = measured else {
                        return Task::none();
                    };
                    if measured.extent != snapshot.viewport.extent {
                        report(
                            "integration.explore_extent_pending",
                            EXPLORE_GALLERY,
                            "measured-native-convergence",
                            [
                                measured.extent.width as f64,
                                measured.extent.height as f64,
                                snapshot.viewport.extent.width as f64,
                                snapshot.viewport.extent.height as f64,
                            ],
                        );
                        return Task::none();
                    }
                    report(
                        "integration.explore_ready",
                        "",
                        "",
                        [
                            snapshot.dataset.imagecount as f64,
                            snapshot.dataset.imagewidth as f64,
                            snapshot.dataset.imageheight as f64,
                            snapshot.dataset.classnames.len() as f64,
                        ],
                    );
                    let Some(draw) = self.atlas_pixels.as_ref().filter(|draw| {
                        draw.snapshot.dataset.identity == snapshot.dataset.identity
                            && draw.snapshot.gallery.generation == snapshot.gallery.generation
                            && draw.snapshot.frame == snapshot.frame
                            && draw.snapshot.viewport == snapshot.viewport
                            && draw.surface.frame == frame
                            && !snapshot.gallery.slots.is_empty()
                            && snapshot.gallery.slots.len() == snapshot.order.visibleindices.len()
                            && snapshot.gallery.slots.iter().all(|ready| *ready)
                            && self.atlas_receipt.as_ref() == Some(*draw)
                    }) else {
                        return Task::none();
                    };
                    report(
                        "integration.initial_atlas_identity",
                        EXPLORE_GALLERY,
                        &format!(
                            "{:016x}:{:016x}{:016x}",
                            snapshot.dataset.identity, draw.surface.high, draw.surface.low
                        ),
                        [
                            snapshot.viewport.firstrow as f64,
                            snapshot.viewport.columns as f64,
                            snapshot.viewport.extent.width as f64,
                            snapshot.viewport.extent.height as f64,
                        ],
                    );
                    report(
                        "integration.initial_atlas_complete",
                        EXPLORE_GALLERY,
                        "no-input-canvas-pixels",
                        [
                            snapshot.dataset.identity as f64,
                            snapshot.gallery.generation as f64,
                            snapshot.frame.revision as f64,
                            draw.surface
                                .frame
                                .map_or(0, |frame| frame.presentation_revision)
                                as f64,
                        ],
                    );
                    COMPLETION_WITHOUT_INPUT.with(|active| active.set(false));
                    if pixel_fixture_enabled() && !self.atlas_fixture_checked {
                        self.atlas_fixture_checked = true;
                        self.atlas_columns = snapshot.viewport.columns;
                        self.atlas_fixture_labels = snapshot.overlay.showlabels;
                        self.atlas_fixture_masks = snapshot.overlay.showmasks;
                        self.atlas_fixture_boxes = snapshot.overlay.showboxes;
                        self.phase = Phase::AtlasPixelColumns(4);
                        return explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::LabelsToggled(false),
                            ),
                        ))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::MasksToggled(true),
                            ),
                        )))
                        .chain(explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Overlay(
                                explore::overlay::Message::BoxesToggled(true),
                            ),
                        )))
                        .chain(explore_message(
                            explore::Message::Gallery(explore::gallery::Message::ColumnsChanged(4)),
                        ));
                    }
                    if !self.viewer_scenario.is_empty() {
                        self.selection_grid = Some((
                            snapshot.viewport.columns,
                            snapshot.viewport.rowcount,
                            0,
                            snapshot.revision,
                            snapshot.frame.revision,
                        ));
                        self.phase = Phase::ViewerSelect;
                        if self.viewer_scenario == "rapid" {
                            self.phase = Phase::ViewerRapidGallery;
                            COMPLETION_WITHOUT_INPUT.with(|active| active.set(true));
                            return explore_message(explore::Message::Gallery(
                                explore::gallery::Message::AugmentationToggled(true),
                            ))
                            .chain(explore_message(explore::Message::Gallery(
                                explore::gallery::Message::AugmentationRerollRequested,
                            )))
                            .chain(explore_message(
                                explore::Message::Gallery(
                                    explore::gallery::Message::AugmentationRerollRequested,
                                ),
                            ));
                        }
                        return self.arm(EXPLORE_GALLERY);
                    }
                    report(
                        "integration.explore_policy",
                        explore::ORDER_CONTROL_ID,
                        if snapshot.filter.order == crate::generated::ExploreOrder::Shuffled {
                            "shuffled"
                        } else {
                            "sequential"
                        },
                        [
                            snapshot.filter.minimumcompiledindex as f64,
                            snapshot.filter.maximumcompiledindex as f64,
                            snapshot.overlay.classselection.classes.len() as f64,
                            snapshot.order.shuffleseed as f64,
                        ],
                    );
                    self.sweep_baseline = Some((
                        snapshot.revision,
                        snapshot.viewport.clone(),
                        snapshot.focusedimage,
                    ));
                    return self.advance_to(Phase::AwaitExploreInitialPatch {
                        revision: snapshot.revision,
                        frame_revision: snapshot.frame.revision,
                    });
                }
                Task::none()
            }
            Phase::AwaitExploreInitialPatch {
                revision,
                frame_revision,
            }
            | Phase::AwaitExploreExactGridPatch {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let initial_patch = matches!(self.phase, Phase::AwaitExploreInitialPatch { .. });
                let revision_pending =
                    snapshot.revision < revision || snapshot.frame.revision < frame_revision;
                // The two padded fixtures must reach the published snapshot before
                // resizing can supersede their generation and rendered probes.
                let initial_overlays_pending = initial_patch
                    && [7, 8].iter().any(|index| {
                        !snapshot
                            .labels
                            .iter()
                            .any(|label| label.compiledindex == *index)
                    });
                let sampleable = sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                );
                if initial_patch {
                    report(
                        "integration.explore_patch_wait",
                        EXPLORE_GALLERY,
                        "revision-labels-busy-publication",
                        [
                            f64::from(u8::from(revision_pending)),
                            f64::from(u8::from(initial_overlays_pending)),
                            f64::from(u8::from(snapshot.busy)),
                            f64::from(u8::from(sampleable.is_none())),
                        ],
                    );
                    report(
                        "integration.explore_patch_draw",
                        EXPLORE_GALLERY,
                        "drawn-and-sampleable-publication",
                        [
                            self.gallery_drawn
                                .map_or(0.0, |(presentation, _)| presentation as f64),
                            self.gallery_drawn.map_or(0.0, |(_, source)| source as f64),
                            sampleable.map_or(0.0, |sample| sample.presentation_revision as f64),
                            sampleable.map_or(0.0, |sample| sample.source_revision as f64),
                        ],
                    );
                }
                if revision_pending
                    || initial_overlays_pending
                    || snapshot.busy
                    || sampleable.is_none()
                    || sampleable.is_some_and(|sample| {
                        self.gallery_drawn
                            != Some((sample.presentation_revision, sample.source_revision))
                    })
                {
                    return Task::none();
                }
                if matches!(self.phase, Phase::AwaitExploreExactGridPatch { .. }) {
                    self.phase = Phase::ExploreDatasetPane;
                    return self.arm(EXPLORE_DATASET_PANE);
                }
                report(
                    "integration.explore_initial_patch",
                    EXPLORE_GALLERY,
                    "stable-gallery-generation",
                    [
                        revision as f64,
                        snapshot.revision as f64,
                        frame_revision as f64,
                        snapshot.frame.revision as f64,
                    ],
                );
                let columns = snapshot.viewport.columns.max(1);
                let capacity = snapshot.maximumatlasextent.clone();
                self.oversized_capacity = Some(capacity.clone());
                self.phase = Phase::AwaitExploreExactGrid(snapshot.revision);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Gallery(
                        explore::gallery::Message::Measured {
                            size: iced::Size::new(10_000.0, 10_000.0),
                            maximum_extent: capacity,
                            columns,
                        },
                    )),
                ))
            }
            Phase::AwaitExploreExactGrid(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(capacity) = self.oversized_capacity.as_ref() else {
                    self.fail("oversized Explore capacity was not retained");
                    return Task::none();
                };
                let columns = snapshot.viewport.columns.max(1);
                let rows = snapshot.viewport.rowcount.max(1);
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.viewport.extent.width > capacity.width
                    || snapshot.viewport.extent.height > capacity.height
                {
                    return Task::none();
                }
                if snapshot.viewport.extent.width % columns != 0
                    || snapshot.viewport.extent.height % rows != 0
                    || snapshot.viewport.extent.width / columns
                        != snapshot.viewport.extent.height / rows
                {
                    self.fail("oversized Explore measurement did not produce an exact square grid");
                    return Task::none();
                }
                report(
                    "integration.explore_exact_grid",
                    EXPLORE_GALLERY,
                    "oversized-logical-fill",
                    [
                        snapshot.viewport.extent.width as f64,
                        snapshot.viewport.extent.height as f64,
                        columns as f64,
                        rows as f64,
                    ],
                );
                report(
                    "integration.explore_exact_grid_capacity",
                    EXPLORE_GALLERY,
                    "measured-revision-capacity",
                    [
                        snapshot.revision as f64,
                        snapshot.frame.revision as f64,
                        capacity.width as f64,
                        capacity.height as f64,
                    ],
                );
                self.phase = Phase::AwaitExploreExactGridPatch {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                Task::none()
            }
            Phase::ExploreDatasetPane => self.arm(EXPLORE_DATASET_PANE),
            Phase::ExploreDetailsPane => self.arm(EXPLORE_DETAILS_PANE),
            Phase::ExplorePolicyOrderReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                report(
                    "integration.explore_policy_order_arm",
                    explore::ORDER_SHUFFLED_ID,
                    "model-availability",
                    [
                        snapshot.revision as f64,
                        if snapshot.busy { 1.0 } else { 0.0 },
                        if settings.has_local_edits() { 1.0 } else { 0.0 },
                        if model.explore_mutation_available() {
                            1.0
                        } else {
                            0.0
                        },
                    ],
                );
                self.phase = Phase::ExplorePolicyOrder(snapshot.revision);
                self.arm(explore::ORDER_SHUFFLED_ID)
            }
            Phase::ExplorePolicyOrder(_) => self.arm(explore::ORDER_SHUFFLED_ID),
            Phase::AwaitExplorePolicyOrder(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.filter.order != crate::generated::ExploreOrder::Shuffled
                {
                    return Task::none();
                }
                self.phase = Phase::ExplorePolicyRangeReady;
                Task::none()
            }
            Phase::ExplorePolicyRangeReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                self.phase = Phase::ExplorePolicyRange(snapshot.revision);
                self.arm(explore::RANGE_START_ONE_ID)
            }
            Phase::ExplorePolicyRange(_) => self.arm(explore::RANGE_START_ONE_ID),
            Phase::ExplorePolicyRangeVisible(_) => {
                self.arm_revealed(explore::DATASET_SCROLL_ID, explore::RANGE_START_ONE_ID)
            }
            Phase::AwaitExplorePolicyRange(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.filter.minimumcompiledindex != 1
                {
                    return Task::none();
                }
                self.phase = Phase::ExplorePolicyOverlayReady;
                Task::none()
            }
            Phase::ExplorePolicyOverlayReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                self.phase = Phase::ExplorePolicyOverlay(snapshot.revision);
                self.arm(explore::OVERLAY_NONE_ID)
            }
            Phase::ExplorePolicyOverlay(_) => self.arm(explore::OVERLAY_NONE_ID),
            Phase::ExplorePolicyOverlayVisible(_) => {
                self.arm_revealed(explore::DETAILS_SCROLL_ID, explore::OVERLAY_NONE_ID)
            }
            Phase::AwaitExplorePolicyOverlay(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::None
                {
                    return Task::none();
                }
                self.phase = Phase::AwaitExploreOverlayAll(snapshot.revision);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Details(
                        explore::details::Message::AllClasses,
                    )),
                ))
            }
            Phase::AwaitExploreOverlayAll(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::All
                {
                    return Task::none();
                }
                self.phase = Phase::AwaitExploreOverlaySubset(snapshot.revision);
                return Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Details(
                        explore::details::Message::ClassToggled(0),
                    )),
                ));
            }
            Phase::AwaitExploreOverlaySubset(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::Subset
                {
                    return Task::none();
                }
                self.phase = Phase::AwaitExploreOverlayRestored(snapshot.revision);
                return Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Explore(explore::Message::Details(
                        explore::details::Message::AllClasses,
                    )),
                ));
            }
            Phase::AwaitExploreOverlayRestored(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.overlay.classselection.mode
                        != crate::generated::ExploreClassSelectionMode::All
                {
                    return Task::none();
                }
                self.phase = Phase::ExploreAugmentationToggle {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                self.arm(EXPLORE_AUGMENTATION_TOGGLE)
            }
            Phase::ExploreAugmentationToggle { .. } => self.arm(EXPLORE_AUGMENTATION_TOGGLE),
            Phase::AwaitExploreAugmentationToggle {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision || !snapshot.augmentation.enabled {
                    return Task::none();
                }
                if snapshot.augmentation.seed != 0 {
                    self.fail("augmentation enable changed its seed or did not publish a new rendered frame");
                    return Task::none();
                }
                if snapshot.frame.revision <= frame_revision
                    || fully_drawn_gallery(model, frame, snapshot, self.gallery_drawn).is_none()
                {
                    return Task::none();
                }
                report(
                    "integration.explore_augmentation",
                    EXPLORE_AUGMENTATION_TOGGLE,
                    "enabled-rendered-seed-zero",
                    [
                        revision as f64,
                        snapshot.revision as f64,
                        frame_revision as f64,
                        snapshot.frame.revision as f64,
                    ],
                );
                self.phase = Phase::ExploreAugmentationReroll {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                self.arm(EXPLORE_AUGMENTATION_REROLL)
            }
            Phase::ExploreAugmentationReroll { .. } => self.arm(EXPLORE_AUGMENTATION_REROLL),
            Phase::AwaitExploreAugmentationReroll {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision || snapshot.augmentation.seed != 1 {
                    return Task::none();
                }
                if snapshot.frame.revision <= frame_revision
                    || fully_drawn_gallery(model, frame, snapshot, self.gallery_drawn).is_none()
                {
                    return Task::none();
                }
                report(
                    "integration.explore_augmentation",
                    EXPLORE_AUGMENTATION_REROLL,
                    "rerolled-distinct-seed",
                    [
                        revision as f64,
                        snapshot.revision as f64,
                        frame_revision as f64,
                        snapshot.frame.revision as f64,
                    ],
                );
                self.phase = Phase::ExploreReshuffle {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                    shuffle_seed: snapshot.order.shuffleseed,
                    augmentation_seed: snapshot.augmentation.seed,
                    order_signature: explore_order_signature(&snapshot.order.visibleindices),
                };
                self.arm(EXPLORE_RESHUFFLE)
            }
            Phase::ExploreReshuffle { .. } => self.arm(EXPLORE_RESHUFFLE),
            Phase::AwaitExploreReshuffle {
                revision,
                frame_revision,
                shuffle_seed,
                augmentation_seed,
                order_signature,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || snapshot.order.shuffleseed <= shuffle_seed
                {
                    return Task::none();
                }
                if snapshot.augmentation.seed != augmentation_seed
                    || snapshot.frame.revision <= frame_revision
                {
                    self.fail("Reshuffle changed augmentation state or did not publish the reordered gallery");
                    return Task::none();
                }
                let reshuffled_signature = explore_order_signature(&snapshot.order.visibleindices);
                if reshuffled_signature == order_signature {
                    self.fail("Reshuffle did not change the visible gallery order");
                    return Task::none();
                }
                report(
                    "integration.explore_reshuffle",
                    EXPLORE_RESHUFFLE,
                    "order-only",
                    [
                        shuffle_seed as f64,
                        snapshot.order.shuffleseed as f64,
                        augmentation_seed as f64,
                        snapshot.augmentation.seed as f64,
                    ],
                );
                report(
                    "integration.explore_patch_baseline",
                    EXPLORE_CARD,
                    "reshuffle-placeholder",
                    [
                        snapshot.revision as f64,
                        snapshot.frame.revision as f64,
                        0.0,
                        0.0,
                    ],
                );
                self.phase = Phase::ExploreCard {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                Task::none()
            }
            Phase::ExploreCard { .. } => self.arm(EXPLORE_CARD),
            Phase::AwaitGalleryPatch {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision < revision
                    || snapshot.frame.revision < frame_revision
                    || snapshot.busy
                    || sampleable_presentation(
                        model,
                        frame,
                        crate::generated::PresentationSourceKind::Explore,
                        snapshot.frame.revision,
                    )
                    .is_none()
                {
                    return Task::none();
                }
                report(
                    "integration.explore_patch_observed",
                    EXPLORE_CARD,
                    "post-placeholder-frame",
                    [
                        revision as f64,
                        snapshot.revision as f64,
                        frame_revision as f64,
                        snapshot.frame.revision as f64,
                    ],
                );
                self.sweep_baseline = Some((
                    snapshot.revision,
                    snapshot.viewport.clone(),
                    snapshot.focusedimage,
                ));
                self.phase = Phase::GallerySweep;
                self.arm(EXPLORE_GALLERY)
            }
            Phase::GallerySweep => self.arm(EXPLORE_GALLERY),
            Phase::AwaitGallerySweep => {
                let Some((revision, viewport, focused)) = &self.sweep_baseline else {
                    self.fail("Explore sweep baseline is unavailable");
                    return Task::none();
                };
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.revision <= *revision
                    || (&snapshot.viewport == viewport && snapshot.focusedimage == *focused)
                    || settings.has_local_edits()
                    || !model.explore_viewport_available()
                {
                    return Task::none();
                }
                report(
                    "integration.explore_sweep_observed",
                    EXPLORE_GALLERY,
                    "typed-viewport",
                    [
                        *revision as f64,
                        snapshot.revision as f64,
                        snapshot.viewport.extent.width as f64,
                        snapshot.viewport.extent.height as f64,
                    ],
                );
                self.phase = Phase::GalleryLaterReady;
                Task::none()
            }
            Phase::GalleryLaterReady => {
                if settings.has_local_edits() || !model.explore_viewport_available() {
                    return Task::none();
                }
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                self.scroll_placeholder_reported = false;
                self.phase = Phase::GalleryLater(snapshot.viewport.firstrow);
                self.arm(EXPLORE_LATER)
            }
            Phase::GalleryLater(_) => self.arm(EXPLORE_LATER),
            Phase::AwaitGalleryScroll(baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.viewport.firstrow <= baseline
                    || snapshot.busy
                    || settings.has_local_edits()
                    || !model.explore_mutation_available()
                {
                    return Task::none();
                }
                if !self.scroll_placeholder_reported {
                    self.scroll_placeholder_reported = true;
                    report(
                        "integration.explore_scroll_placeholder",
                        EXPLORE_GALLERY,
                        "newest-without-wait-all",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            snapshot.viewport.firstrow as f64,
                            snapshot.order.visibleindices.len() as f64,
                        ],
                    );
                }
                if sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                    || self
                        .gallery_drawn
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                let slot = (snapshot.viewport.rowcount / 2)
                    .saturating_mul(snapshot.viewport.columns)
                    .saturating_add(snapshot.viewport.columns / 2)
                    as usize;
                let Some(index) = snapshot.order.visibleindices.get(slot).copied() else {
                    self.fail("Explore gallery has no visible image");
                    return Task::none();
                };
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    slot as u32,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                report(
                    "integration.explore_scrolled",
                    EXPLORE_GALLERY,
                    "",
                    [
                        baseline as f64,
                        snapshot.viewport.firstrow as f64,
                        index as f64,
                        0.0,
                    ],
                );
                self.phase = Phase::GalleryImage(index);
                self.arm(EXPLORE_GALLERY)
            }
            Phase::GalleryImage(_) => self.arm(EXPLORE_GALLERY),
            Phase::AwaitDetail(expected) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Detail
                    || snapshot.busy
                    || settings.has_local_edits()
                    || !model.explore_mutation_available()
                {
                    return Task::none();
                }
                let Some(selected) = snapshot.selectedimage else {
                    return Task::none();
                };
                if selected != expected {
                    report(
                        "integration.explore_pointer_mismatch",
                        EXPLORE_GALLERY,
                        "selected-image",
                        [
                            expected as f64,
                            selected as f64,
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    );
                    self.fail("gallery pointer selection did not match the rendered grid slot");
                    return Task::none();
                }
                if let Some((_, _, slot, revision, _)) = self.selection_grid {
                    report(
                        "integration.explore_pointer_selected",
                        EXPLORE_GALLERY,
                        "selected-from-dispatched-pointer",
                        [
                            revision as f64,
                            slot as f64,
                            f64::from(expected),
                            f64::from(selected),
                        ],
                    );
                }
                report(
                    "integration.explore_detail",
                    "",
                    "",
                    [selected as f64, snapshot.frame.revision as f64, 0.0, 0.0],
                );
                if snapshot.detail.showoriginaldimensions
                    || snapshot.frame.extent.width != snapshot.dataset.imagewidth
                    || snapshot.frame.extent.height != snapshot.dataset.imageheight
                {
                    self.fail("initial Explore detail was not the native padded product");
                    return Task::none();
                }
                if sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                self.phase = Phase::DetailOriginal {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                    padded_width: snapshot.frame.extent.width,
                    padded_height: snapshot.frame.extent.height,
                };
                self.arm(EXPLORE_DETAIL_ORIGINAL)
            }
            Phase::DetailOriginal { .. } => self.arm(EXPLORE_DETAIL_ORIGINAL),
            Phase::AwaitDetailOriginal {
                revision,
                frame_revision,
                padded_width,
                padded_height,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision <= revision
                    || snapshot.busy
                    || !snapshot.detail.showoriginaldimensions
                {
                    return Task::none();
                }
                if snapshot.frame.revision != frame_revision
                    || snapshot.frame.extent.width != padded_width
                    || snapshot.frame.extent.height != padded_height
                {
                    self.fail("original crop changed the native product");
                    return Task::none();
                }
                // CLEANUP-IGNORE: Original-detail and dataset-reopen evidence call the shared presentation join for distinct completion contracts.
                if sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                let Some((_, drawn_revision, drawn)) = self.viewer_drawn else {
                    return Task::none();
                };
                let content = &snapshot.frame.content;
                let Some(viewed) = model.viewed_explore_frame() else {
                    return Task::none();
                };
                let viewed_content = &viewed.content;
                if drawn_revision != viewed.revision
                    || drawn.crop
                        != [
                            viewed_content.x,
                            viewed_content.y,
                            viewed_content.width,
                            viewed_content.height,
                        ]
                {
                    return Task::none();
                }
                report(
                    "integration.explore_detail_source",
                    EXPLORE_DETAIL_ORIGINAL,
                    "padded-to-original-sampling",
                    [
                        padded_width as f64,
                        padded_height as f64,
                        content.width as f64,
                        content.height as f64,
                    ],
                );
                self.phase = Phase::DetailFit;
                self.arm(explore::DETAIL_FIT_ID)
            }
            Phase::DetailFit => self.arm(explore::DETAIL_FIT_ID),
            Phase::ViewerSelect => self.arm(EXPLORE_GALLERY),
            Phase::AwaitAtlasCapacity
                if !settings.has_local_edits()
                    && model.settings_edit_available()
                    && model
                        .explore
                        .snapshot
                        .as_ref()
                        .and_then(|snapshot| snapshot.viewportresult.as_ref())
                        .is_some_and(|result| {
                            result.outcome
                                == crate::generated::ExploreViewportOutcome::VisibleCapacityExceeded
                        }) =>
            {
                report(
                    "integration.atlas_capacity",
                    explore::GALLERY_CAPACITY_ID,
                    "native-visible-capacity-exceeded",
                    [1.0, 0.0, 0.0, 0.0],
                );
                self.phase = Phase::AtlasCapacity;
                self.arm(explore::GALLERY_CAPACITY_ID)
            }
            Phase::AtlasCapacity => self.arm(explore::GALLERY_CAPACITY_ID),
            Phase::AtlasRestoreColumns => {
                self.phase = Phase::AwaitAtlasColumns;
                explore_message(explore::Message::Gallery(
                    explore::gallery::Message::ColumnsChanged(self.atlas_columns as i32),
                ))
            }
            Phase::AwaitAtlasColumns
                if !settings.has_local_edits()
                    && model.explore_mutation_available()
                    && settings.draft.as_ref().is_some_and(|draft| {
                        draft.workflows.explore.gridwidth == self.atlas_columns as i32
                    }) =>
            {
                self.phase = Phase::AwaitAtlasEmpty;
                explore_message(explore::Message::Dataset(
                    explore::dataset::Message::NoClasses,
                ))
            }
            Phase::AwaitAtlasEmpty
                if model.explore.snapshot.as_ref().is_some_and(|value| {
                    value.ready && !value.busy && value.order.matchingcount == 0
                }) =>
            {
                self.phase = Phase::AtlasEmpty;
                self.arm(explore::GALLERY_EMPTY_ID)
            }
            Phase::AtlasEmpty => self.arm(explore::GALLERY_EMPTY_ID),
            Phase::AtlasRestoreFilter => {
                self.phase = Phase::AwaitAtlasRestored;
                explore_message(explore::Message::Dataset(
                    explore::dataset::Message::AllClasses,
                ))
            }
            Phase::AwaitAtlasRestored => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !snapshot.ready
                    || snapshot.busy
                    || snapshot.order.matchingcount == 0
                    || self
                        .gallery_drawn
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.phase = Phase::ViewerSelect;
                self.arm(EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasWindow(fullscreen) => {
                #[cfg(target_arch = "wasm32")]
                let settled = fullscreen_settled_js(fullscreen);
                #[cfg(not(target_arch = "wasm32"))]
                let settled = false;
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let gallery_matches = self
                    .gallery_drawn
                    .is_some_and(|(_, source)| source == snapshot.frame.revision);
                let viewport_matches = router
                    .explore_measured_viewport(
                        snapshot.viewport.columns,
                        snapshot.viewport.firstrow,
                        snapshot.order.matchingcount,
                    )
                    .as_ref()
                    == Some(&snapshot.viewport);
                if !settled
                    || snapshot.busy
                    || model.has_explore_pending()
                    || !gallery_matches
                    || !viewport_matches
                {
                    report(
                        "integration.atlas_window_wait",
                        EXPLORE_GALLERY,
                        if fullscreen { "fullscreen" } else { "restored" },
                        [
                            settled as u8 as f64,
                            (snapshot.busy || model.has_explore_pending()) as u8 as f64,
                            gallery_matches as u8 as f64,
                            viewport_matches as u8 as f64,
                        ],
                    );
                    return Task::none();
                }
                report(
                    "integration.atlas_window_draw",
                    EXPLORE_GALLERY,
                    if fullscreen { "fullscreen" } else { "restored" },
                    [
                        snapshot.frame.revision as f64,
                        model.window_width as f64,
                        model.window_height as f64,
                        snapshot.viewport.columns as f64,
                    ],
                );
                if fullscreen {
                    self.phase = Phase::AwaitAtlasWindow(false);
                    #[cfg(target_arch = "wasm32")]
                    fullscreen_js(false);
                    Task::none()
                } else {
                    self.atlas_columns = snapshot.viewport.columns;
                    self.phase = Phase::AwaitAtlasCapacity;
                    let maximum = crate::generated::constraint_workflowsexploregridwidth()
                        .maximum
                        .unwrap_or(self.atlas_columns as f64)
                        as i32;
                    explore_message(explore::Message::Gallery(
                        explore::gallery::Message::ColumnsChanged(maximum),
                    ))
                }
            }
            Phase::AwaitAtlasRows { rows, next_stage } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.viewport.rowcount != rows
                    || self
                        .atlas_drawn
                        .is_none_or(|(source, _)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                self.phase = Phase::AwaitAtlasScroll(next_stage);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: next_stage as f32 * self.atlas_row_extent
                            + if matches!(next_stage, 0 | 2) {
                                37.25
                            } else {
                                0.0
                            },
                    },
                )
            }
            Phase::AwaitAtlasScroll(stage) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || model.has_explore_pending()
                    || self
                        .atlas_drawn
                        .is_none_or(|(source, _)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                let Some(receipt) = self.atlas_receipt.as_ref().filter(|draw| {
                    draw.snapshot.frame == snapshot.frame
                        && draw.snapshot.viewport == snapshot.viewport
                }) else {
                    return Task::none();
                };
                let total_rows = snapshot
                    .order
                    .matchingcount
                    .div_ceil(snapshot.viewport.columns.max(1));
                let settled = match stage {
                    0 => self.atlas_clip.0 > 0.0,
                    1..=3 => snapshot.viewport.firstrow == [0, 1, 2, 10][stage as usize],
                    4 => {
                        snapshot.viewport.firstrow + snapshot.viewport.rowcount == total_rows
                            && self.atlas_clip.1.abs() < 1.0
                    }
                    _ => snapshot.viewport.firstrow == 0 && self.atlas_clip.0.abs() < 1.0,
                };
                if !settled {
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage(
                    ["fractional", "row1", "row2", "row10", "end", "restored"][stage as usize],
                    receipt,
                );
                report(
                    "integration.atlas_scroll",
                    EXPLORE_GALLERY,
                    ["fractional", "row1", "row2", "row10", "end", "restored"][stage as usize],
                    [
                        snapshot.frame.revision as f64,
                        snapshot.viewport.firstrow as f64,
                        self.atlas_clip.0 as f64,
                        self.atlas_clip.1 as f64,
                    ],
                );
                match stage {
                    0 | 1 => {
                        let rows = 5 - u32::from(stage);
                        self.phase = Phase::AwaitAtlasRows {
                            rows,
                            next_stage: stage + 1,
                        };
                        let columns = snapshot.viewport.columns.max(1);
                        let row_extent = self.atlas_row_extent.max(1.0);
                        explore_message(explore::Message::Gallery(
                            explore::gallery::Message::Measured {
                                size: iced::Size::new(
                                    row_extent * columns as f32,
                                    row_extent * (rows as f32 - 0.5),
                                ),
                                maximum_extent: snapshot.maximumatlasextent.clone(),
                                columns,
                            },
                        ))
                    }
                    2 => {
                        self.phase = Phase::AwaitAtlasScroll(3);
                        iced::widget::operation::scroll_to(
                            EXPLORE_GALLERY,
                            AbsoluteOffset {
                                x: 0.0,
                                y: 10.0 * self.atlas_row_extent,
                            },
                        )
                    }
                    3 => {
                        self.phase = Phase::AwaitAtlasScroll(4);
                        iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::END)
                    }
                    4 => {
                        self.phase = Phase::AwaitAtlasScroll(5);
                        iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::START)
                    }
                    _ => {
                        self.phase = Phase::AwaitAtlasWindow(true);
                        #[cfg(target_arch = "wasm32")]
                        fullscreen_js(true);
                        Task::none()
                    }
                }
            }
            Phase::AtlasOverlay(index) => self.arm(overlay_control(index, false)),
            Phase::AwaitAtlasOverlay(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let expected = [6, 4, 5, 1, 0, 2, 3, 7][index];
                if snapshot.busy
                    || model.has_explore_pending()
                    || self.atlas_drawn != Some((snapshot.frame.revision, expected))
                {
                    return Task::none();
                }
                if let Some((revision, seed)) = self.atlas_baseline {
                    if snapshot.augmentation.seed != seed
                        || (index % 4 == 3 && snapshot.frame.revision != revision)
                    {
                        self.fail(
                            "atlas visibility changed augmentation or labels changed the GPU frame",
                        );
                        return Task::none();
                    }
                }
                report(
                    "integration.atlas_checkbox",
                    overlay_control(index, false),
                    "rendered-saved-visibility",
                    [
                        index as f64,
                        expected as f64,
                        snapshot.frame.revision as f64,
                        snapshot.augmentation.seed as f64,
                    ],
                );
                self.atlas_baseline = Some((snapshot.frame.revision, snapshot.augmentation.seed));
                if index == 7 {
                    self.phase = Phase::AwaitAtlasRows {
                        rows: 4,
                        next_stage: 0,
                    };
                    let columns = snapshot.viewport.columns.max(1);
                    let row_extent = self.atlas_row_extent.max(1.0);
                    explore_message(explore::Message::Gallery(
                        explore::gallery::Message::Measured {
                            size: iced::Size::new(row_extent * columns as f32, row_extent * 3.5),
                            maximum_extent: snapshot.maximumatlasextent.clone(),
                            columns,
                        },
                    ))
                } else {
                    self.phase = Phase::AtlasOverlay(index + 1);
                    self.arm(overlay_control(index + 1, false))
                }
            }
            Phase::ViewerOverlay(index) => self.arm(overlay_control(index, true)),
            Phase::AwaitViewerOverlay(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let expected = [6, 4, 5, 1, 0, 2, 3, 7, 6][index];
                let actual = u8::from(snapshot.overlay.showboxes)
                    | (u8::from(snapshot.overlay.showmasks) << 1)
                    | (u8::from(snapshot.overlay.showlabels) << 2);
                if actual != expected
                    || snapshot.busy
                    || !model.explore_mutation_available()
                    || self.viewer_drawn.is_none_or(|(_, source, _)| {
                        model
                            .viewed_explore_frame()
                            .is_none_or(|viewed| source != viewed.revision)
                    })
                {
                    return Task::none();
                }
                report(
                    "integration.viewer_overlay",
                    overlay_control(index, true),
                    "actual-draw",
                    [
                        index as f64,
                        actual as f64,
                        snapshot.frame.revision as f64,
                        snapshot.frame.cleanrevision as f64,
                    ],
                );
                if index == 8 {
                    self.begin_upscale_series(model, &snapshot.frame)
                } else {
                    self.phase = Phase::ViewerOverlay(index + 1);
                    self.arm(overlay_control(index + 1, true))
                }
            }
            Phase::ViewerSquareBasic => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(request) = model.explore.requested_upscale.as_ref() else {
                    return Task::none();
                };
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                if snapshot.frame.extent.width != 384
                    || snapshot.frame.extent.height != 384
                    || request.source != snapshot.frame
                    || request.kernel != crate::generated::UpscaleKernel::Default
                    || upscale.frame.extent.width != 1536
                    || upscale.frame.extent.height != 1536
                    || model.displayed_upscale_kernel()
                        != Some(crate::generated::UpscaleKernel::Default)
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    return Task::none();
                };
                if sampleable.content_width != 1536
                    || sampleable.content_height != 1536
                    || self.viewer_drawn.is_none_or(|(drawn, source, _)| {
                        drawn != sampleable.presentation_revision
                            || source != upscale.frame.revision
                    })
                {
                    return Task::none();
                }
                self.phase = Phase::ViewerNoAspect;
                self.arm("explore.detail.aspect")
            }
            Phase::ViewerNoAspect => self.arm("explore.detail.aspect"),
            Phase::ViewerRapidGallery => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || !snapshot.augmentation.enabled
                    || snapshot.augmentation.seed == 0
                    || model.has_explore_pending()
                    || model.explore.desired_augmentation_reroll
                    || snapshot.gallery.generation == 0
                    || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                    || self.atlas_pixels.as_ref().is_none_or(|draw| {
                        draw.snapshot.frame != snapshot.frame
                            || draw.snapshot.gallery.generation != snapshot.gallery.generation
                            || draw.snapshot.dataset.identity != snapshot.dataset.identity
                            || draw.surface.frame != frame
                    })
                    || self
                        .gallery_drawn
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                report(
                    "integration.gallery_no_input_complete",
                    EXPLORE_GALLERY,
                    "matching-pixels-and-semantics",
                    [
                        snapshot.gallery.generation as f64,
                        snapshot.gallery.slots.len() as f64,
                        snapshot.frame.revision as f64,
                        snapshot.augmentation.seed as f64,
                    ],
                );
                COMPLETION_WITHOUT_INPUT.with(|active| active.set(false));
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.atlas_baseline = Some((snapshot.frame.revision, snapshot.augmentation.seed));
                self.phase = Phase::AtlasOverlay(0);
                self.arm(overlay_control(0, false))
            }
            Phase::ViewerRapidSelection(revision) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.selectedimage != Some(0)
                    || snapshot.revision <= revision
                    || model.has_explore_pending()
                    || model.explore.desired_navigation.is_some()
                {
                    return Task::none();
                }
                self.begin_upscale_series(model, &snapshot.frame)
            }
            Phase::AwaitDetailFit => {
                let Some((_, _, drawn)) = self.viewer_drawn else {
                    return Task::none();
                };
                if drawn.fit_revision == 0 {
                    return Task::none();
                }
                let center = (drawn.image.center().x - drawn.container.center().x).abs() < 1.0
                    && (drawn.image.center().y - drawn.container.center().y).abs() < 1.0;
                let contain = drawn.image.width <= drawn.container.width + 1.0
                    && drawn.image.height <= drawn.container.height + 1.0
                    && ((drawn.image.width - drawn.container.width).abs() < 1.0
                        || (drawn.image.height - drawn.container.height).abs() < 1.0);
                if !center || !contain {
                    self.fail(
                        "Fit did not center and contain the stored content in the actual viewer",
                    );
                    return Task::none();
                }
                report(
                    "integration.explore_detail_fit",
                    explore::DETAIL_FIT_ID,
                    "centered-contained",
                    [
                        drawn.container.width as f64,
                        drawn.container.height as f64,
                        drawn.image.width as f64,
                        drawn.image.height as f64,
                    ],
                );
                if self.viewer_scenario == "square" {
                    return self.advance_to(Phase::ViewerSquareBasic);
                }
                if !self.viewer_scenario.is_empty()
                    && !matches!(self.viewer_scenario.as_str(), "copy" | "rapid")
                {
                    self.phase = if self.viewer_scenario == "semantics" {
                        Phase::ViewerOverlay(0)
                    } else {
                        Phase::ViewerNoAspect
                    };
                    return self.arm(if self.viewer_scenario == "semantics" {
                        overlay_control(0, true)
                    } else {
                        "explore.detail.aspect"
                    });
                }
                if self.viewer_scenario == "rapid" {
                    self.phase = Phase::ViewerRapidSelection(
                        model
                            .explore
                            .snapshot
                            .as_ref()
                            .map_or(0, |snapshot| snapshot.revision),
                    );
                    return explore_message(explore::Message::Detail(
                        explore::detail::Message::NextRequested,
                    ))
                    .chain(explore_message(explore::Message::Detail(
                        explore::detail::Message::PreviousRequested,
                    )));
                }
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                report(
                    "integration.upscale_action_arm",
                    EXPLORE_UPSCALE_ACTIONS[0],
                    "after-detail-fit",
                    [
                        0.0,
                        snapshot.frame.extent.width as f64,
                        snapshot.frame.extent.height as f64,
                        snapshot.frame.revision as f64,
                    ],
                );
                self.begin_upscale_series(model, &snapshot.frame)
            }
            Phase::StartUpscale { kernel, .. } => self.arm(EXPLORE_UPSCALE_ACTIONS[kernel]),
            Phase::AwaitUpscale {
                kernel,
                source_width,
                source_height,
                upscale_revision: _,
                upscale_frame_revision,
                presentation_revision,
            } => {
                let Some(upscale) = model.current_upscale() else {
                    return Task::none();
                };
                if upscale.kernel != crate::generated::UPSCALE_KERNEL_VALUES[kernel] {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    return Task::none();
                };
                if upscale.busy || !upscale.ready {
                    return Task::none();
                }
                if upscale.frame.revision != upscale_frame_revision
                    && sampleable.presentation_revision <= presentation_revision
                {
                    return Task::none();
                }
                let Some(expected_width) = source_width.checked_mul(4) else {
                    self.fail("Upscale source width cannot be represented at four-times extent");
                    return Task::none();
                };
                let Some(expected_height) = source_height.checked_mul(4) else {
                    self.fail("Upscale source height cannot be represented at four-times extent");
                    return Task::none();
                };
                if upscale.frame.extent.width != expected_width
                    || upscale.frame.extent.height != expected_height
                {
                    self.fail("Upscale did not publish the exact four-times output extent");
                    return Task::none();
                }
                report(
                    "integration.upscale_growth",
                    EXPLORE_UPSCALE_ACTIONS[kernel],
                    upscale_acceptance_label(crate::generated::UPSCALE_KERNEL_VALUES[kernel]),
                    [
                        source_width as f64,
                        source_height as f64,
                        upscale.frame.extent.width as f64,
                        upscale.frame.extent.height as f64,
                    ],
                );
                if sampleable.content_width != expected_width
                    || sampleable.content_height != expected_height
                    || sampleable.capability_width < expected_width
                    || sampleable.capability_height < expected_height
                {
                    self.fail("Presentation did not import the exact Upscale output capability");
                    return Task::none();
                }
                report(
                    "integration.upscale_presentation",
                    EXPLORE_UPSCALE_ACTIONS[kernel],
                    "complete-four-times-exported-frame",
                    [
                        sampleable.content_width as f64,
                        sampleable.content_height as f64,
                        sampleable.capability_width as f64,
                        sampleable.capability_height as f64,
                    ],
                );
                if model.displayed_upscale_kernel() != Some(upscale.kernel) {
                    return Task::none();
                }
                let Some((drawn, source, viewer)) =
                    self.viewer_drawn.filter(|(drawn, source, _)| {
                        *drawn == sampleable.presentation_revision
                            && *source == upscale.frame.revision
                    })
                else {
                    return Task::none();
                };
                let Some(button) = self.upscale_button else {
                    return Task::none();
                };
                if !self.upscale_pixel_pending {
                    self.upscale_pixel_pending = true;
                    sample_upscale_pixels(viewer.image, button, source, drawn);
                    return Task::none();
                }
                let Some((pixel_source, pixel_presentation, checksum, blue)) = self.upscale_pixels
                else {
                    return Task::none();
                };
                if pixel_source != source
                    || pixel_presentation != drawn
                    || checksum == 0
                    || blue < 32
                {
                    self.fail("Upscale actual canvas image or completed blue method did not match its displayed result");
                    return Task::none();
                }
                report(
                    "integration.upscale_completed_pixels",
                    EXPLORE_UPSCALE_ACTIONS[kernel],
                    "exact-completed-blue",
                    [source as f64, drawn as f64, checksum as f64, blue as f64],
                );
                if let Some(revision) = self.upscale_repeat_revision {
                    if !self.upscale_repeat_observed {
                        return Task::none();
                    }
                    if upscale.revision != revision {
                        self.fail(
                            "Same completed Upscale method unnecessarily submitted native work",
                        );
                        return Task::none();
                    }
                    report(
                        "integration.upscale_same_method",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "same-completed-result",
                        [source as f64, drawn as f64, revision as f64, 1.0],
                    );
                } else {
                    self.upscale_repeat_revision = Some(upscale.revision);
                    if !click(button) {
                        self.fail("Upscale completed method re-click failed");
                    }
                    return Task::none();
                }
                if self.upscale_cache_pass {
                    if self.upscale_cached_frames[kernel].as_ref() != Some(&upscale.frame) {
                        self.fail("cached method changed its completed physical product");
                        return Task::none();
                    }
                    report(
                        "integration.upscale_cached",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "same-resident-product-drawn",
                        [
                            source as f64,
                            drawn as f64,
                            kernel as f64,
                            upscale.revision as f64,
                        ],
                    );
                    if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                        && self.viewer_continuity_request.is_none()
                    {
                        let Some(snapshot) = model.settings_snapshot.as_ref() else {
                            return Task::none();
                        };
                        if !route_edit_available(model, settings) {
                            return Task::none();
                        }
                        self.viewer_continuity_request = model.explore.requested_upscale.clone();
                        self.viewer_continuity_settings_revision = snapshot.revision;
                        self.viewer_continuity_performance =
                            snapshot.settingsstate.ui.showworkspaceperformance;
                        self.phase = Phase::ViewerConfirmSettings;
                        return Task::done(RootMessage::Workspace(
                            crate::view::router::Message::Navigation(
                                crate::view::navigation::Message::SettingsRequested,
                            ),
                        ))
                        .chain(Task::done(RootMessage::Settings(
                            crate::view::settings::Message::PerformanceChanged(
                                !self.viewer_continuity_performance,
                            ),
                        )));
                    }
                } else {
                    self.upscale_cached_frames[kernel] = Some(upscale.frame.clone());
                    if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                        && self.viewer_scenario != "rapid"
                    {
                        self.upscale_cache_pass = true;
                        self.phase = Phase::StartUpscale {
                            kernel: 0,
                            source_width,
                            source_height,
                            upscale_revision: upscale.revision,
                            upscale_frame_revision: upscale.frame.revision,
                            presentation_revision: sampleable.presentation_revision,
                        };
                        return self.arm(EXPLORE_UPSCALE_ACTIONS[0]);
                    }
                }
                if self.viewer_scenario == "copy" && kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len() {
                    if self.viewer_drawn.is_none_or(|(drawn, source, _)| {
                        drawn != sampleable.presentation_revision
                            || source != upscale.frame.revision
                    }) {
                        return Task::none();
                    }
                    self.phase = Phase::OpenAnnotation;
                    return self.arm(EXPLORE_ANNOTATE);
                }
                if self.viewer_scenario == "semantics"
                    && kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                {
                    if self.viewer_drawn.is_none_or(|(drawn, source, _)| {
                        drawn != sampleable.presentation_revision
                            || source != upscale.frame.revision
                    }) {
                        return Task::none();
                    }
                    self.phase = Phase::ViewerNoAspect;
                    return self.arm("explore.detail.aspect");
                }
                if self.viewer_scenario == "rapid" {
                    if self.viewer_drawn.is_none_or(|(drawn, source, _)| {
                        drawn != sampleable.presentation_revision
                            || source != upscale.frame.revision
                    }) {
                        return Task::none();
                    }
                    report(
                        "integration.viewer_complete",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "rapid",
                        [
                            sampleable.presentation_revision as f64,
                            upscale.frame.revision as f64,
                            1.0,
                            kernel as f64,
                        ],
                    );
                    self.phase = Phase::Complete;
                    return Task::none();
                }
                if kernel + 1 < EXPLORE_UPSCALE_ACTIONS.len() {
                    let next_kernel = kernel + 1;
                    report(
                        "integration.upscale_action_arm",
                        EXPLORE_UPSCALE_ACTIONS[next_kernel],
                        "after-upscale-publication",
                        [
                            next_kernel as f64,
                            upscale.revision as f64,
                            upscale.frame.revision as f64,
                            sampleable.presentation_revision as f64,
                        ],
                    );
                    self.phase = Phase::StartUpscale {
                        kernel: next_kernel,
                        source_width,
                        source_height,
                        upscale_revision: upscale.revision,
                        upscale_frame_revision: upscale.frame.revision,
                        presentation_revision: sampleable.presentation_revision,
                    };
                    self.arm(EXPLORE_UPSCALE_ACTIONS[next_kernel])
                } else {
                    self.annotation_sample_baseline = sampleable.presentation_revision;
                    self.phase = Phase::DetailNext(
                        model
                            .explore
                            .snapshot
                            .as_ref()
                            .and_then(|snapshot| snapshot.selectedimage)
                            .unwrap_or_default(),
                    );
                    self.arm(EXPLORE_NEXT)
                }
            }
            Phase::DetailNext(_) => self.arm(EXPLORE_NEXT),
            Phase::AwaitNext(previous) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(selected) = snapshot.selectedimage else {
                    return Task::none();
                };
                if snapshot.busy
                    || selected == previous
                    || settings.has_local_edits()
                    || !model.explore_mutation_available()
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                ) else {
                    return Task::none();
                };
                if sampleable.presentation_revision <= self.annotation_sample_baseline {
                    return Task::none();
                }
                report(
                    "integration.upscale_later_frame",
                    EXPLORE_GALLERY,
                    "distinct-imported-frame",
                    [
                        self.annotation_sample_baseline as f64,
                        sampleable.presentation_revision as f64,
                        sampleable.content_width as f64,
                        sampleable.content_height as f64,
                    ],
                );
                self.phase = Phase::DetailPrevious(selected);
                self.arm(EXPLORE_PREVIOUS)
            }
            Phase::DetailPrevious(_) => self.arm(EXPLORE_PREVIOUS),
            Phase::AwaitPrevious(next) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.selectedimage == Some(next)
                    || settings.has_local_edits()
                    || !model.annotation_open_available()
                {
                    return Task::none();
                }
                self.annotation_sample_baseline = model
                    .presentation
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.browsercompletedsample);
                self.annotation_frame_ready = None;
                self.phase = Phase::DetailCloseEvidence;
                self.arm(EXPLORE_DETAIL_CLOSE)
            }
            Phase::DetailCloseEvidence => self.arm(EXPLORE_DETAIL_CLOSE),
            Phase::AwaitDetailClose => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || snapshot.mode != crate::generated::ExploreMode::Gallery {
                    return Task::none();
                }
                self.phase = Phase::ExploreDatasetReopen {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                };
                self.arm(EXPLORE_OPEN)
            }
            Phase::ExploreDatasetReopen { .. } => self.arm(EXPLORE_OPEN),
            Phase::AwaitExploreDatasetReopen {
                revision,
                frame_revision,
            } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.revision > self.reopen_wait_revision {
                    let sampleable = sampleable_presentation(
                        model,
                        frame,
                        crate::generated::PresentationSourceKind::Explore,
                        snapshot.frame.revision,
                    );
                    let blocker = if !snapshot.ready {
                        "snapshot-not-ready"
                    } else if snapshot.busy {
                        "snapshot-busy"
                    } else if snapshot.revision <= revision {
                        "snapshot-revision"
                    } else if snapshot.frame.revision <= frame_revision {
                        "frame-revision"
                    } else if snapshot.gallery.generation == 0 {
                        "gallery-generation"
                    } else if snapshot.gallery.slots.len() != snapshot.order.visibleindices.len() {
                        "slot-cardinality"
                    } else if snapshot.gallery.slots.iter().any(|ready| !*ready) {
                        "slot-readiness"
                    } else if self.selection_grid.is_none() {
                        "selection-grid"
                    } else if snapshot.viewport.columns == 0 || snapshot.viewport.rowcount == 0 {
                        "viewport"
                    } else if sampleable.is_none() {
                        "sampleable-presentation"
                    } else if self.gallery_drawn.is_none_or(|(presentation, source)| {
                        source != snapshot.frame.revision
                            || model
                                .presentation
                                .as_ref()
                                .is_none_or(|current| current.presentationrevision != presentation)
                    }) {
                        "physical-gallery-draw"
                    } else {
                        "ready"
                    };
                    report(
                        "integration.explore_reopen_wait",
                        EXPLORE_OPEN,
                        blocker,
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            snapshot.gallery.generation as f64,
                            snapshot
                                .gallery
                                .slots
                                .iter()
                                .filter(|ready| **ready)
                                .count() as f64,
                        ],
                    );
                    self.reopen_wait_revision = snapshot.revision;
                }
                if !snapshot.ready
                    || snapshot.busy
                    || snapshot.revision <= revision
                    || snapshot.frame.revision <= frame_revision
                    || snapshot.gallery.generation == 0
                    || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                {
                    return Task::none();
                }
                let Some(_) = self.selection_grid else {
                    self.fail("Explore selection grid is unavailable after reopen");
                    return Task::none();
                };
                if snapshot.viewport.columns == 0 || snapshot.viewport.rowcount == 0 {
                    return Task::none();
                }
                if sampleable_presentation(
                    model,
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                if self.gallery_drawn.is_none_or(|(presentation, source)| {
                    source != snapshot.frame.revision
                        || model
                            .presentation
                            .as_ref()
                            .is_none_or(|current| current.presentationrevision != presentation)
                }) {
                    return Task::none();
                }
                report(
                    "integration.explore_reopened",
                    EXPLORE_OPEN,
                    "usable-after-reopen",
                    [
                        snapshot.revision as f64,
                        snapshot.frame.revision as f64,
                        snapshot.order.visibleindices.len() as f64,
                        snapshot.dataset.imagecount as f64,
                    ],
                );
                report(
                    "integration.explore_final_cursor",
                    EXPLORE_GALLERY,
                    "coherent-placeholder",
                    [
                        snapshot.revision as f64,
                        snapshot.frame.revision as f64,
                        snapshot.gallery.generation as f64,
                        snapshot.order.visibleindices.len() as f64,
                    ],
                );
                let columns = snapshot.viewport.columns;
                let slot = (snapshot.viewport.rowcount / 2)
                    .saturating_mul(columns)
                    .saturating_add(columns / 2) as usize;
                if snapshot.order.visibleindices.get(slot).is_none() {
                    self.fail("reopened Explore gallery has no usable image");
                    return Task::none();
                }
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    slot as u32,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.phase = Phase::GalleryReselect;
                self.arm(EXPLORE_GALLERY)
            }
            Phase::GalleryReselect => self.arm(EXPLORE_GALLERY),
            Phase::AwaitDetailAgain => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || snapshot.mode != crate::generated::ExploreMode::Detail {
                    return Task::none();
                }
                if self.viewer_drawn.is_none_or(|(presentation, source, _)| {
                    model
                        .viewed_explore_frame()
                        .is_none_or(|viewed| source != viewed.revision)
                        || model
                            .presentation
                            .as_ref()
                            .is_none_or(|value| value.presentationrevision != presentation)
                }) {
                    return Task::none();
                }
                self.phase = Phase::OpenAnnotation;
                self.arm(EXPLORE_ANNOTATE)
            }
            Phase::OpenAnnotation => self.arm(EXPLORE_ANNOTATE),
            Phase::CopyAwaitObject { index, mask } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.ui.editor.selectedobject != Some(index)
                    || !model.annotation_edit_available()
                {
                    return Task::none();
                }
                self.begin_copy_object_edit(snapshot, index, mask)
            }
            Phase::CopyUndo { .. } => self.arm_scrolled("annotation.undo", RelativeOffset::END),
            Phase::CopyRedo { .. } => self.arm_scrolled("annotation.redo", RelativeOffset::END),
            Phase::CopyAwaitUndo { index, mask } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || snapshot.ui.scene.objects.get(index as usize) != self.copy_before.as_ref()
                {
                    return Task::none();
                }
                self.phase = Phase::CopyRedo { index, mask };
                self.arm_scrolled("annotation.redo", RelativeOffset::END)
            }
            Phase::CopyAwaitRedo { index, mask: _ } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || snapshot.ui.scene.objects.get(index as usize) != self.copy_after.as_ref()
                {
                    return Task::none();
                }
                report(
                    "integration.viewer_import_edit",
                    ANNOTATION_SURFACE,
                    match self.copy_step {
                        0 => "box-move-undo-redo",
                        1 => "box-resize-undo-redo",
                        2 => "mask-paint-undo-redo",
                        3 => "mask-erase-undo-redo",
                        _ => "class-undo-redo",
                    },
                    [
                        index as f64,
                        snapshot.ui.scene.objects.len() as f64,
                        snapshot.ui.documentrevision as f64,
                        snapshot.frame.revision as f64,
                    ],
                );
                if self.copy_step == 3 && snapshot.ui.scene.categories.len() > 1 {
                    self.copy_step = 4;
                    self.copy_before = Some(snapshot.ui.scene.objects[index as usize].clone());
                    self.phase = Phase::CopyAwaitClass { index };
                    return annotation_message(annotation::Message::Sidebar(
                        annotation::sidebar::Message::SelectedObjectApplied(
                            (snapshot.ui.scene.objects[index as usize].category + 1)
                                % snapshot.ui.scene.categories.len() as u16,
                        ),
                    ));
                }
                if self.copy_step >= 3 {
                    self.copy_step = 5;
                    self.copy_shape_points = 0;
                    self.copy_before = None;
                    self.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool: crate::generated::AnnotationTool::Point,
                    };
                    return self.arm_scrolled(
                        annotation::tool_id(crate::generated::AnnotationTool::Point),
                        RelativeOffset::START,
                    );
                }
                {
                    self.copy_step += 1;
                    if self.copy_step == 1 {
                        self.copy_before = Some(snapshot.ui.scene.objects[index as usize].clone());
                        self.copy_after = None;
                        return self.begin_copy_object_edit(snapshot, index, false);
                    }
                    let Some((index, object)) =
                        snapshot
                            .ui
                            .scene
                            .objects
                            .iter()
                            .enumerate()
                            .find(|(_, object)| {
                                object.shape == crate::generated::AnnotationShape::Mask
                            })
                    else {
                        self.fail("copied document lost its mask object");
                        return Task::none();
                    };
                    self.copy_before = Some(object.clone());
                    self.copy_after = None;
                    self.begin_copy_object_edit(snapshot, index as u16, true)
                }
            }
            Phase::CopyAwaitClass { index } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || self.copy_before.as_ref().is_none_or(|before| {
                        before.category == snapshot.ui.scene.objects[index as usize].category
                    })
                {
                    return Task::none();
                }
                self.copy_after = Some(snapshot.ui.scene.objects[index as usize].clone());
                self.phase = Phase::CopyUndo { index, mask: true };
                self.arm_scrolled("annotation.undo", RelativeOffset::START)
            }
            Phase::CopyLayout(step) => {
                self.copy_viewport_width = model.window_width as f32;
                if let Some(ui) = model
                    .annotation
                    .snapshot
                    .as_ref()
                    .map(|snapshot| &snapshot.ui)
                {
                    if let Some(color) = ui
                        .editor
                        .selectedobject
                        .and_then(|index| ui.scene.objects.get(index as usize))
                        .and_then(|object| ui.scene.palette.get(object.category as usize))
                    {
                        let color = crate::presentation_surface::labels::class_color(color);
                        self.copy_swatch_color = [
                            f64::from(color.r) * 255.0,
                            f64::from(color.g) * 255.0,
                            f64::from(color.b) * 255.0,
                        ];
                    }
                }
                if step == 0 {
                    self.arm_scrolled("workflow.workspace_and_advanced", RelativeOffset::START)
                } else if step == 1 {
                    self.arm("annotation.inspector.scroll")
                } else {
                    if self.location_pending {
                        return Task::none();
                    }
                    self.location_pending = true;
                    self.copy_swatch_ready = false;
                    iced::widget::operation::snap_to(
                        crate::view::PAGE_SCROLL_ID,
                        RelativeOffset::START,
                    )
                    .chain(iced::widget::operation::scroll_by(
                        crate::view::PAGE_SCROLL_ID,
                        AbsoluteOffset {
                            x: 0.0,
                            y: self.copy_inspector_offset,
                        },
                    ))
                    .chain(iced::widget::operation::snap_to(
                        "annotation.inspector.scroll",
                        RelativeOffset::START,
                    ))
                    .chain(locate("annotation.class.active.swatch".into()))
                }
            }
            Phase::CopySwatchWait => {
                if self.copy_swatch_ready {
                    self.advance_to(Phase::CopyProductStart)
                } else {
                    Task::none()
                }
            }
            Phase::CopyCapability => {
                let Some(ui) = model
                    .annotation
                    .snapshot
                    .as_ref()
                    .map(|snapshot| &snapshot.ui)
                else {
                    return Task::none();
                };
                self.copy_capability_available = ui.toolcapabilities.iter().any(|fact| {
                    fact.tool == crate::generated::AnnotationTool::ColorSample && fact.available
                });
                let theme = crate::fluent_theme::app_theme(
                    settings
                        .draft
                        .as_ref()
                        .is_some_and(|draft| draft.ui.darkmode),
                );
                let style = iced_fluent_theme::button::rounded::secondary(
                    &theme,
                    if self.copy_capability_available {
                        iced::widget::button::Status::Active
                    } else {
                        iced::widget::button::Status::Disabled
                    },
                );
                let Some(iced::Background::Color(color)) = style.background else {
                    self.fail("Annotation tool background is not sampleable");
                    return Task::none();
                };
                self.copy_swatch_color = [
                    f64::from(color.r) * 255.0,
                    f64::from(color.g) * 255.0,
                    f64::from(color.b) * 255.0,
                ];
                self.copy_capability_ready = false;
                self.arm_scrolled(
                    annotation::tool_id(crate::generated::AnnotationTool::ColorSample),
                    RelativeOffset::START,
                )
            }
            Phase::CopyCapabilityWait => {
                if self.copy_capability_ready {
                    self.advance_to(Phase::CopyProductStart)
                } else {
                    Task::none()
                }
            }
            Phase::CopyAwaitScale(compact) => {
                let scale = self.copy_requested_scale;
                let Some(settled) =
                    settled_settings_snapshot(model, settings, self.settings_revision)
                else {
                    return Task::none();
                };
                if !same_numeric_value(
                    f64::from(settled.settingsstate.ui.uiscale),
                    f64::from(scale),
                ) || !same_numeric_value(f64::from(applied_scale), f64::from(scale))
                {
                    self.fail("Annotation scale did not settle at the requested valid value");
                    return Task::none();
                }
                let width = model.window_width as f32;
                if compact && (!width.is_finite() || width >= annotation::COMPACT_BREAKPOINT) {
                    self.fail(
                        "Settled native UI scale did not reach the compact Annotation layout",
                    );
                    return Task::none();
                }
                if compact {
                    let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                        return Task::none();
                    };
                    if let Err(detail) = self.copy_product.start(&snapshot.ui, self.copy_objects) {
                        self.fail(detail);
                        return Task::none();
                    }
                    self.advance_to(Phase::CopyLayout(0))
                } else {
                    self.phase = Phase::CopyAwaitOutput;
                    annotation_message(annotation::Message::OutputDirectoryChanged(format!(
                        "{}/viewer-annotations.cbor",
                        self.compiled_directory
                    )))
                }
            }
            Phase::CopyProductStart => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available() {
                    return Task::none();
                }
                let Some(action) = self.copy_product.action(&snapshot.ui) else {
                    if !self.copy_product.finished() {
                        self.fail("Annotation product step lost its required object geometry");
                        return Task::none();
                    }
                    self.copy_product_gesture = None;
                    let compact = !self.copy_compact;
                    let scale = if compact {
                        match annotation_compact_scale(applied_scale, model.window_width as f32) {
                            Ok(scale) => scale,
                            Err(detail) => {
                                self.fail(detail);
                                return Task::none();
                            }
                        }
                    } else {
                        self.copy_original_scale
                    };
                    self.settings_revision = model
                        .settings_snapshot
                        .as_ref()
                        .map_or(0, |snapshot| snapshot.revision);
                    self.copy_compact = true;
                    self.copy_requested_scale = scale;
                    self.phase = Phase::CopyAwaitScale(compact);
                    return Task::done(RootMessage::Settings(
                        crate::view::settings::Message::UiScaleChanged(scale),
                    ))
                    .chain(Task::done(RootMessage::Settings(
                        crate::view::settings::Message::UiScaleReleased,
                    )));
                };
                self.copy_product_frame = snapshot.frame.revision;
                self.copy_product_gesture = action.gesture;
                self.copy_product_cancel = action.cancel;
                self.copy_product_cancelled = false;
                if let Some(tool) = action.tool {
                    self.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool,
                    };
                    self.arm_scrolled(annotation::tool_id(tool), RelativeOffset::START)
                } else if action.gesture.is_some() {
                    self.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
                    self.arm_scrolled(ANNOTATION_SURFACE, RelativeOffset::START)
                } else {
                    let completion = self.advance_to(Phase::CopyProductWait);
                    action
                        .messages
                        .into_iter()
                        .fold(Task::none(), |tasks, message| {
                            tasks.chain(annotation_message(message))
                        })
                        .chain(completion)
                }
            }
            Phase::CopyAwaitOutput => {
                let destination = format!("{}/viewer-annotations.cbor", self.compiled_directory);
                if settings.has_local_edits()
                    || !model.annotation_save_available()
                    || !model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.settingsstate.workflows.annotate.outputdir == destination
                    })
                {
                    return Task::none();
                }
                self.phase = Phase::CopySave;
                self.arm_scrolled(VIEWER_SAVE, RelativeOffset::END)
            }
            Phase::CopySave => self.arm_scrolled(VIEWER_SAVE, RelativeOffset::END),
            Phase::CopyAwaitSave => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.ui.savestatus != crate::generated::AnnotationSaveStatus::Saved
                {
                    return Task::none();
                }
                let Some((presentation, source)) = self.annotation_drawn else {
                    return Task::none();
                };
                report(
                    "integration.viewer_complete",
                    VIEWER_SAVE,
                    "copy",
                    [presentation as f64, source as f64, 1.0, 0.0],
                );
                self.phase = Phase::Complete;
                Task::none()
            }
            Phase::AwaitAnnotation => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !snapshot.ready
                    || snapshot.busy
                    || snapshot.frame.revision == 0
                    || !model.annotation_edit_available()
                {
                    return Task::none();
                }
                report(
                    "integration.annotation_ready",
                    "",
                    "receiver-owned",
                    [
                        snapshot.frame.revision as f64,
                        snapshot.ui.documentrevision as f64,
                        snapshot.ui.interactionrevision as f64,
                        0.0,
                    ],
                );
                if self.viewer_scenario == "copy" {
                    let scene = &snapshot.ui.scene;
                    let Some((index, object)) =
                        scene.objects.iter().enumerate().find(|(_, object)| {
                            matches!(
                                object.shape,
                                crate::generated::AnnotationShape::Box
                                    | crate::generated::AnnotationShape::Mask
                            ) && object.box_.first.x < object.box_.second.x
                                && object.box_.first.y < object.box_.second.y
                        })
                    else {
                        self.fail("viewed document imported no editable box geometry");
                        return Task::none();
                    };
                    if !scene.objects.iter().any(|object| {
                        object.shape == crate::generated::AnnotationShape::Mask
                            && !object.mask.runs.is_empty()
                    }) {
                        self.fail("viewed document imported no editable mask");
                        return Task::none();
                    }
                    let Some(upscale) = model.upscale_snapshot.as_ref() else {
                        return Task::none();
                    };
                    if snapshot.frame.extent.width != upscale.frame.content.width
                        || snapshot.frame.extent.height != upscale.frame.content.height
                    {
                        self.fail("Annotation did not copy the viewed upscale crop");
                        return Task::none();
                    }
                    self.copy_before = Some(object.clone());
                    self.copy_objects = scene.objects.len();
                    self.copy_categories = scene.categories.clone();
                    return self.begin_copy_object_edit(snapshot, index as u16, false);
                }
                let Some(tool) = crate::generated::ANNOTATION_TOOL_VALUES
                    .iter()
                    .copied()
                    .find(|tool| *tool != snapshot.ui.editor.tool)
                else {
                    self.fail("generated Annotation tool inventory has no selectable tool");
                    return Task::none();
                };
                self.phase = Phase::AnnotationSidebar {
                    revision: snapshot.ui.interactionrevision,
                    tool,
                };
                self.arm(ANNOTATION_SIDEBAR)
            }
            Phase::AnnotationSidebar { revision, tool } => {
                self.phase = Phase::AnnotationSidebar { revision, tool };
                self.arm(ANNOTATION_SIDEBAR)
            }
            Phase::AnnotationTimeline { revision, tool } => {
                self.phase = Phase::AnnotationTimeline { revision, tool };
                self.arm(ANNOTATION_TIMELINE)
            }
            Phase::AnnotationOperation { revision, tool } => {
                self.phase = Phase::AnnotationOperation { revision, tool };
                self.arm(ANNOTATION_OPERATION)
            }
            Phase::AnnotationStop { revision, tool } => {
                self.phase = Phase::AnnotationStop { revision, tool };
                self.arm(ANNOTATION_STOP)
            }
            Phase::AnnotationBrush { revision, tool } => {
                self.phase = Phase::AnnotationBrush { revision, tool };
                self.arm(ANNOTATION_BRUSH_RADIUS)
            }
            Phase::AnnotationTool { revision, tool } => {
                self.phase = Phase::AnnotationTool { revision, tool };
                self.arm_scrolled(annotation::tool_id(tool), RelativeOffset::START)
            }
            Phase::AwaitTool { revision, tool } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || (self.copy_step != 8 && snapshot.ui.interactionrevision <= revision)
                    || (self.copy_step == 8 && snapshot.frame.revision <= self.copy_product_frame)
                    || snapshot.ui.editor.tool != tool
                    || !model.annotation_edit_available()
                {
                    return Task::none();
                }
                report(
                    "integration.annotation_tool_observed",
                    &annotation::tool_id(tool),
                    "typed-tool",
                    [
                        revision as f64,
                        snapshot.ui.interactionrevision as f64,
                        0.0,
                        0.0,
                    ],
                );
                if self.copy_step == 8 {
                    self.advance_to(Phase::CopyProductWait)
                } else {
                    self.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
                    self.arm_scrolled(ANNOTATION_SURFACE, RelativeOffset::START)
                }
            }
            Phase::AnnotationSurface(revision) => {
                self.phase = Phase::AnnotationSurface(revision);
                self.arm_scrolled(ANNOTATION_SURFACE, RelativeOffset::START)
            }
            Phase::AwaitAnnotationFrame(revision) => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !self
                    .annotation_frame_ready
                    .is_some_and(|sampleable| sampleable.source_revision == snapshot.frame.revision)
                {
                    return Task::none();
                }
                self.phase = Phase::AnnotationPointer(revision);
                self.arm(ANNOTATION_SURFACE)
            }
            Phase::AnnotationPointer(revision) => {
                self.phase = Phase::AnnotationPointer(revision);
                self.arm(ANNOTATION_SURFACE)
            }
            Phase::AwaitPointer(_) | Phase::CopyProductWait => {
                let revision = if let Phase::AwaitPointer(revision) = self.phase {
                    revision
                } else {
                    0
                };
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(presentation) = model.presentation.as_ref() else {
                    return Task::none();
                };
                let Some(sampleable) = self.annotation_frame_ready else {
                    return Task::none();
                };
                let presentation_revision = sampleable.presentation_revision;
                if !model.annotation_edit_available() || (self.copy_step!=8 && snapshot.ui.interactionrevision <= revision)
                    || (self.copy_step==8 && snapshot.frame.revision<=self.copy_product_frame)
                    // The browser helper emits Begin, Update, End (or holds after
                    // Update). Each ordered native pointer publishes one frame.
                    || (self.copy_step==8 && self.copy_product_gesture.is_some() && !self.copy_product_cancelled
                        && snapshot.frame.revision<self.copy_product_frame+if self.copy_product_cancel{2}else{3})
                    || sampleable.source_revision != snapshot.frame.revision
                    || presentation.completed.source.kind
                        != crate::generated::PresentationSourceKind::Annotation
                    || presentation.completed.revision != snapshot.frame.revision
                    || presentation.timelineready == 0
                    || presentation.presentationrevision != presentation_revision
                    || presentation.browsercompletedsample != presentation_revision
                    || self.annotation_drawn
                        != Some((presentation_revision, snapshot.frame.revision))
                {
                    return Task::none();
                }
                if self.copy_step == 8 && self.copy_product_cancel && !self.copy_product_cancelled {
                    report(
                        "integration.annotation_preview",
                        ANNOTATION_SURFACE,
                        "visible-before-cancel",
                        [
                            self.copy_product_frame as f64,
                            snapshot.frame.revision as f64,
                            presentation_revision as f64,
                            0.0,
                        ],
                    );
                    self.copy_product_cancelled = true;
                    self.copy_product_frame = snapshot.frame.revision;
                    return annotation_message(annotation::Message::CancelRequested);
                }
                if self.viewer_scenario == "copy"
                    && self.annotation_pixels_revision != snapshot.frame.revision
                {
                    if self.annotation_pixels_pending {
                        return Task::none();
                    }
                    self.annotation_pixels_pending = true;
                    self.annotation_probe = Some((
                        snapshot.frame.revision,
                        presentation_revision,
                        snapshot.frame.extent.width,
                        snapshot.frame.extent.height,
                        annotation_checks::probes(&snapshot.ui),
                    ));
                    return self.arm(ANNOTATION_SURFACE);
                }
                report(
                    "integration.annotation_pointer_observed",
                    ANNOTATION_SURFACE,
                    "typed-interaction",
                    [
                        revision as f64,
                        snapshot.ui.interactionrevision as f64,
                        presentation_revision as f64,
                        0.0,
                    ],
                );
                if self.viewer_scenario == "copy" && self.copy_step == 8 {
                    #[cfg(target_arch = "wasm32")]
                    if self.copy_product_cancel {
                        annotation_release_js();
                    }
                    match self.copy_product.observe(&snapshot.ui) {
                        Ok(detail) => {
                            report(
                                "integration.annotation_product",
                                ANNOTATION_SURFACE,
                                &detail,
                                [
                                    snapshot.frame.revision as f64,
                                    presentation_revision as f64,
                                    snapshot.ui.documentrevision as f64,
                                    snapshot
                                        .ui
                                        .toolcapabilities
                                        .iter()
                                        .filter(|fact| fact.available)
                                        .count() as f64,
                                ],
                            );
                            return self.advance_to(if detail == "selection" {
                                Phase::CopyCapability
                            } else {
                                Phase::CopyProductStart
                            });
                        }
                        Err(detail) => {
                            self.fail(detail);
                            return Task::none();
                        }
                    }
                }
                if self.viewer_scenario == "copy" && (5..=7).contains(&self.copy_step) {
                    let Some(index) = snapshot.ui.editor.selectedobject else {
                        return Task::none();
                    };
                    let object = &snapshot.ui.scene.objects[index as usize];
                    let expected = match self.copy_step {
                        5 => crate::generated::AnnotationShape::Point,
                        6 => crate::generated::AnnotationShape::Spline,
                        _ => crate::generated::AnnotationShape::Skeleton,
                    };
                    if object.shape != expected
                        || snapshot.ui.scene.objects.len()
                            != self.copy_objects + usize::from(self.copy_step - 4)
                    {
                        self.fail("annotation shape creation did not publish its rendered object");
                        return Task::none();
                    }
                    self.copy_shape_points += 1;
                    if (self.copy_step == 6 && self.copy_shape_points < 3)
                        || (self.copy_step == 7 && self.copy_shape_points < 2)
                    {
                        self.phase = Phase::AnnotationSurface(snapshot.ui.interactionrevision);
                        return self.arm_scrolled(ANNOTATION_SURFACE, RelativeOffset::START);
                    }
                    report(
                        "integration.annotation_shape",
                        ANNOTATION_SURFACE,
                        &format!("{:?}", object.shape),
                        [
                            index as f64,
                            snapshot.frame.revision as f64,
                            object.splineknots.len() as f64,
                            object.skeletonnodes.len() as f64,
                        ],
                    );
                    if self.copy_step == 7 {
                        if let Err(detail) =
                            self.copy_product.start(&snapshot.ui, self.copy_objects)
                        {
                            self.fail(detail);
                            return Task::none();
                        }
                        self.copy_step = 8;
                        self.copy_original_scale = settings
                            .draft
                            .as_ref()
                            .map_or(1.0, |draft| draft.ui.uiscale);
                        return self.advance_to(Phase::CopyLayout(0));
                    }
                    self.copy_step += 1;
                    self.copy_shape_points = 0;
                    let tool = if self.copy_step == 6 {
                        crate::generated::AnnotationTool::Spline
                    } else {
                        crate::generated::AnnotationTool::Skeleton
                    };
                    self.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool,
                    };
                    return self.arm_scrolled(annotation::tool_id(tool), RelativeOffset::START);
                }
                if self.viewer_scenario == "copy" {
                    let Some(index) = snapshot.ui.editor.selectedobject else {
                        return Task::none();
                    };
                    let object = &snapshot.ui.scene.objects[index as usize];
                    if self.copy_before.as_ref() == Some(object)
                        || snapshot.ui.scene.objects.len() != self.copy_objects
                        || snapshot.ui.scene.categories != self.copy_categories
                    {
                        self.fail("pointer did not independently edit the imported object");
                        return Task::none();
                    }
                    let mask = matches!(
                        snapshot.ui.editor.tool,
                        crate::generated::AnnotationTool::MaskPaint
                            | crate::generated::AnnotationTool::MaskErase
                    );
                    if self.copy_before.as_ref().is_none_or(|before| {
                        if mask {
                            object.mask == before.mask
                        } else {
                            object.box_ == before.box_
                        }
                    }) {
                        self.fail("editing imported geometry changed its independent counterpart");
                        return Task::none();
                    }
                    self.copy_after = Some(object.clone());
                    self.phase = Phase::CopyUndo { index, mask };
                    return self.arm_scrolled("annotation.undo", RelativeOffset::END);
                }
                report(
                    "integration.complete",
                    "",
                    "typed-mvc-wayland",
                    [
                        snapshot.frame.revision as f64,
                        presentation_revision as f64,
                        presentation_revision as f64,
                        presentation.browsercompletedsample as f64,
                    ],
                );
                self.phase = Phase::Complete;
                self.report_phase_progress();
                if self.window_close {
                    #[cfg(target_arch = "wasm32")]
                    if window_close_js() != 1 {
                        self.fail("Firefox window close dispatch failed");
                    }
                }
                Task::none()
            }
            _ => Task::none(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn integration_phases_assign_bounded_progress_deadline_classes() {
        assert_eq!(Phase::AwaitBootstrap.deadline_class(), "startup");
        assert_eq!(Phase::AwaitCompileCompletion.deadline_class(), "work");
        assert_eq!(
            Phase::AwaitExploreDatasetReopen {
                revision: 1,
                frame_revision: 1,
            }
            .deadline_class(),
            "work"
        );
        assert_eq!(
            Phase::AwaitUpscale {
                kernel: 0,
                source_width: 1,
                source_height: 1,
                upscale_revision: 1,
                upscale_frame_revision: 1,
                presentation_revision: 1,
            }
            .deadline_class(),
            "work"
        );
        assert_eq!(Phase::AwaitPointer(1).deadline_class(), "work");
        assert_eq!(Phase::Complete.deadline_class(), "work");
        assert_eq!(Phase::AwaitSettings.deadline_class(), "interaction");
    }

    #[test]
    fn annotation_compact_scale_obeys_native_bounds_at_packaged_dpi_widths() {
        let constraint = crate::generated::constraint_uiuiscale();
        for (unscaled_width, current_scale) in
            [(1500.0, 1.0), (1000.0, 1.0), (1500.0, 1.5), (1000.0, 1.25)]
        {
            let logical_width = unscaled_width / current_scale;
            let scale = annotation_compact_scale(current_scale, logical_width)
                .expect("packaged width reaches compact layout");
            assert!(f64::from(scale) >= constraint.minimum.unwrap());
            assert!(f64::from(scale) <= constraint.maximum.unwrap());
            assert!(unscaled_width / scale < annotation::COMPACT_BREAKPOINT);
        }
        let maximum = constraint.maximum.unwrap() as f32;
        assert!(
            annotation_compact_scale(1.0, annotation::COMPACT_BREAKPOINT * maximum * 2.0).is_err()
        );
        assert!(annotation_compact_scale(1.0, f32::NAN).is_err());
    }

    #[test]
    fn atlas_pixel_evidence_covers_only_ready_interiors_inside_the_real_clip() {
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.viewport.columns = 2;
        snapshot.viewport.rowcount = 2;
        snapshot.gallery.slots = vec![true, true, false, true];
        let mut draw = AtlasDraw {
            surface: crate::presentation_surface::Surface {
                high: 1,
                low: 2,
                generation: 1,
                width: 200,
                height: 200,
                timeline_ready: 1,
                frame: None,
                integration: true,
                crop: None,
                viewer_identity: None,
                fit_revision: 0,
            },
            snapshot: std::sync::Arc::new(snapshot),
            bounds: Rectangle {
                x: 10.0,
                y: 20.0,
                width: 200.0,
                height: 200.0,
            },
            image: Rectangle {
                x: 10.0,
                y: 20.0,
                width: 200.0,
                height: 200.0,
            },
            clip: Rectangle {
                x: 10.0,
                y: 20.0,
                width: 200.0,
                height: 200.0,
            },
        };
        assert_eq!(atlas_pixel_rectangles(&draw).len(), 12);
        draw.clip = Rectangle {
            x: 10.0,
            y: 120.0,
            width: 100.0,
            height: 100.0,
        };
        assert!(atlas_pixel_rectangles(&draw).is_empty());
        draw.clip = Rectangle {
            x: 150.0,
            y: 150.0,
            width: 50.0,
            height: 30.0,
        };
        let rectangles = atlas_pixel_rectangles(&draw);
        assert_eq!(rectangles.len(), 4);
        for (actual, expected) in rectangles.iter().zip([150.0, 150.0, 40.0, 30.0]) {
            assert!((actual - expected).abs() < 0.001);
        }
        let mut controller = Controller::new(
            false,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        );
        controller.phase = Phase::AwaitExploreReady;
        let _ = controller.update(Message::AtlasPixels {
            receipt: draw.clone(),
            visible: 1,
            nonblack: 0,
        });
        assert!(controller.atlas_pixels.is_none());
        let _ = controller.update(Message::AtlasPixels {
            receipt: draw.clone(),
            visible: 0,
            nonblack: 0,
        });
        assert!(controller.atlas_pixels.is_none());
        let _ = controller.update(Message::AtlasPixels {
            receipt: draw.clone(),
            visible: 1,
            nonblack: 1,
        });
        assert_eq!(controller.atlas_pixels, Some(draw.clone()));
        let _ = controller.update(Message::AtlasComposition {
            receipt: draw.clone(),
            expected: 1,
            matched: 0,
        });
        assert!(controller.atlas_composition.is_none());
        let _ = controller.update(Message::AtlasComposition {
            receipt: draw.clone(),
            expected: 0,
            matched: 0,
        });
        assert!(controller.atlas_composition.is_none());
        let _ = controller.update(Message::AtlasComposition {
            receipt: draw.clone(),
            expected: 1,
            matched: 1,
        });
        assert_eq!(controller.atlas_composition, Some(draw));
    }

    #[test]
    fn inactive_controllers_do_not_observe_snapshots_or_consume_callbacks() {
        let mut model = ApplicationModel::default();
        model
            .install_bootstrap(
                crate::generated::SCHEMA_FINGERPRINT,
                crate::generated::application_snapshot_defaults()
                    .expect("generated snapshots")
                    .into_iter()
                    .map(|fact| fact.value)
                    .collect(),
            )
            .expect("bootstrap");
        let snapshot = model.explore.snapshot.as_mut().expect("Explore snapshot");
        snapshot.revision = 1;
        snapshot.order.visibleindices = vec![0, 1, 2];
        let settings = crate::view::settings::SettingsModel::default();
        let router = crate::view::router::Router::default();

        for phase in [Phase::Disabled, Phase::Complete, Phase::Failed] {
            let mut controller = Controller::new(
                false,
                false,
                String::new(),
                String::new(),
                String::new(),
                String::new(),
            );
            controller.phase = phase;
            assert_eq!(
                controller
                    .advance(&model, &settings, 2.0, &router, FeatureId::Explore, None)
                    .units(),
                0
            );
            assert_eq!(controller.input_scale, 1.0);
            assert_eq!(controller.explore_snapshot_revision, 0);

            controller.location_pending = true;
            let phase = std::mem::discriminant(&controller.phase);
            for message in [
                Message::Advance,
                Message::SurfaceDrawn {
                    presentation_revision: 1,
                    source_revision: 1,
                    viewer: None,
                },
                Message::Located {
                    control: EXPLORE_CARD.to_string(),
                    bounds: Rectangle::default(),
                },
            ] {
                assert!(controller.update(message).is_none());
                assert_eq!(std::mem::discriminant(&controller.phase), phase);
                assert!(controller.location_pending);
                assert!(controller.annotation_drawn.is_none());
            }
        }
    }
}
