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
    static DRIVER_ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static PIXEL_FIXTURE_ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static REPORTING_ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    static COMPLETION_WITHOUT_INPUT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

pub(crate) fn initialize_reporting(enabled: bool, pixel_fixture: bool) {
    if !enabled && reporting_enabled() {
        reset_observer();
    }
    PIXEL_FIXTURE_ENABLED.with(|flag| flag.set(enabled && pixel_fixture));
    #[cfg(target_arch = "wasm32")]
    initialize_js(enabled);
    REPORTING_ENABLED.with(|flag| flag.set(enabled));
    COMPLETION_WITHOUT_INPUT.with(|flag| flag.set(false));
}

pub(crate) fn reporting_enabled() -> bool {
    REPORTING_ENABLED.with(std::cell::Cell::get)
}

pub(crate) fn notify_driver_draw(control: &'static str, source_revision: u64, presentation_revision: u64) {
    if !DRIVER_ENABLED.with(std::cell::Cell::get) {
        return;
    }
    #[cfg(target_arch = "wasm32")]
    driver_draw_js(control, source_revision as f64, presentation_revision as f64);
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (control, source_revision, presentation_revision);
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
    Scoped {
        generation: u64,
        receipt: Option<ProbeReceipt>,
        message: Box<Message>,
    },
    // A physical receipt can be resampled; only its current request owns settlement.
    ProbeCompleted {
        owner: std::sync::Arc<()>,
        message: Box<Message>,
    },
    Advance,
    NumberWheelDelivered,
    UpscalePixels {
        source: u64,
        presentation: u64,
        outcome: ProbeOutcome,
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
    AnnotationPixels {
        revision: u64,
        outcome: ProbeOutcome,
    },
    AnnotationControlPixels {
        outcome: ProbeOutcome,
    },
    AtlasPixels {
        receipt: AtlasDraw,
        outcome: ProbeOutcome,
    },
    AtlasComposition {
        receipt: AtlasDraw,
        outcome: ProbeOutcome,
    },
}

/// Invalidation is request retirement, never measured pixel evidence.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProbeOutcome {
    Invalidated,
    Observed(u32, u32),
    Failed,
}

impl ProbeOutcome {
    #[cfg(any(target_arch = "wasm32", test))]
    fn decode(status: Option<&str>, values: [Option<f64>; 2]) -> Self {
        let [Some(first), Some(second)] = values else { return Self::Failed; };
        if ![first, second].into_iter().all(|value| value.is_finite() && value >= 0.0
            && value <= f64::from(u32::MAX) && value.fract() == 0.0) {
            return Self::Failed;
        }
        match status {
            Some("invalidated") if first == 0.0 && second == 0.0 => Self::Invalidated,
            Some("observed") => Self::Observed(first as u32, second as u32),
            _ => Self::Failed,
        }
    }
}

#[derive(Clone)]
struct ControlProbe {
    output: ScenarioOutput,
    color: [f64; 3],
    available: bool,
}

#[derive(Clone)]
struct AnnotationProbe {
    output: ScenarioOutput,
    source: u64,
    presentation: u64,
    extent: [u32; 2],
    pixels: Vec<f64>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ViewerDraw {
    pub crop: [u32; 4],
    pub container: Rectangle,
    pub image: Rectangle,
    pub fit_revision: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ProbeReceipt {
    generation: u64,
    control: &'static str,
    surface: crate::presentation_surface::Surface,
    bounds: Rectangle,
    image: Rectangle,
    clip: Rectangle,
}

#[derive(Clone)]
struct ScenarioOutput {
    generation: u64,
    receipt: Option<ProbeReceipt>,
    probe: Option<std::sync::Arc<()>>,
    // Captured with the Rust receipt, before any widget-location task.
    #[cfg(target_arch = "wasm32")]
    canvas_probe: wasm_bindgen::JsValue,
    sender: iced::futures::channel::mpsc::Sender<Message>,
}

impl ScenarioOutput {
    fn new(generation: u64, sender: iced::futures::channel::mpsc::Sender<Message>) -> Self {
        Self {
            generation,
            receipt: None,
            probe: None,
            #[cfg(target_arch = "wasm32")]
            canvas_probe: wasm_bindgen::JsValue::UNDEFINED,
            sender,
        }
    }

    fn try_send(
        &mut self,
        message: Message,
    ) -> Result<(), iced::futures::channel::mpsc::TrySendError<Message>> {
        let message = if let Some(owner) = &self.probe {
            Message::ProbeCompleted { owner: owner.clone(), message: Box::new(message) }
        } else { message };
        self.sender.try_send(Message::Scoped {
            generation: self.generation,
            receipt: self.receipt.clone(),
            message: Box::new(message),
        })
    }
}

#[derive(Default)]
struct SurfaceDrawObserver {
    generation: u64,
    subscription: Option<std::sync::Arc<()>>,
    receipts: std::collections::BTreeMap<&'static str, ProbeReceipt>,
    output: Option<ScenarioOutput>,
    identity: (u64, u64),
    viewer: Option<(u64, u64, ViewerDraw)>,
    gallery: Option<(u64, u64)>,
    atlas: Option<AtlasDraw>,
    atlas_pixels_owner: Option<std::sync::Arc<()>>,
    atlas_composition_owner: Option<std::sync::Arc<()>>,
}

impl SurfaceDrawObserver {
    fn output_for(&mut self, control: &str) -> Option<&mut ScenarioOutput> {
        let receipt = self.receipts.get(control)?.clone();
        let output = self.output.as_mut()?;
        output.receipt = Some(receipt);
        Some(output)
    }
}

thread_local! {
    static SURFACE_DRAW_OBSERVER: std::cell::RefCell<SurfaceDrawObserver> =
        std::cell::RefCell::new(SurfaceDrawObserver::default());
}

struct SurfaceDrawSubscription(std::sync::Arc<()>);

impl Drop for SurfaceDrawSubscription {
    fn drop(&mut self) {
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            if observer
                .subscription
                .as_ref()
                .is_some_and(|owner| std::sync::Arc::ptr_eq(owner, &self.0))
            {
                let generation = observer.generation;
                *observer = SurfaceDrawObserver {
                    generation,
                    ..Default::default()
                };
            }
        });
    }
}

fn surface_draw_stream() -> impl iced::futures::Stream<Item = Message> {
    iced::stream::channel(1, async move |sender| {
        let owner = std::sync::Arc::new(());
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            observer.subscription = Some(owner.clone());
            observer.output = Some(ScenarioOutput::new(observer.generation, sender));
        });
        let _subscription = SurfaceDrawSubscription(owner);
        std::future::pending::<()>().await;
    })
}

fn reset_observer() -> u64 {
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let generation = observer
            .generation
            .checked_add(1)
            .expect("integration scenario generation exhausted");
        let output = observer.output.take().map(|mut output| {
            output.generation = generation;
            output.receipt = None;
            output.probe = None;
            #[cfg(target_arch = "wasm32")]
            { output.canvas_probe = wasm_bindgen::JsValue::UNDEFINED; }
            output
        });
        let subscription = observer.subscription.take();
        *observer = SurfaceDrawObserver {
            generation,
            output,
            subscription,
            ..Default::default()
        };
        generation
    })
}

fn current_receipt(control: &str) -> Option<ProbeReceipt> {
    SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().receipts.get(control).cloned())
}

fn probe_output(control: &str) -> Option<ScenarioOutput> {
    let mut output = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output_for(control).cloned())?;
    output.probe = Some(std::sync::Arc::new(()));
    #[cfg(target_arch = "wasm32")]
    { output.canvas_probe = capture_probe_js(control); }
    Some(output)
}

#[cfg(any(target_arch = "wasm32", test))]
fn atlas_probe_output(composition: bool) -> Option<ScenarioOutput> {
    let output = probe_output(EXPLORE_GALLERY)?;
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let pending = if composition { &mut observer.atlas_composition_owner } else { &mut observer.atlas_pixels_owner };
        *pending = output.probe.clone();
    });
    Some(output)
}

fn same_probe(owner: &Option<std::sync::Arc<()>>, request: Option<&std::sync::Arc<()>>) -> bool {
    match (owner, request) {
        (Some(owner), Some(request)) => std::sync::Arc::ptr_eq(owner, request),
        _ => false,
    }
}

pub(crate) fn record_probe_draw(
    control: &'static str,
    surface: crate::presentation_surface::Surface,
    bounds: Rectangle,
    image: Rectangle,
    clip: Rectangle,
) {
    if !surface.integration || !reporting_enabled() {
        return;
    }
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let receipt = ProbeReceipt {
            generation: observer.generation,
            control,
            surface,
            bounds,
            image,
            clip,
        };
        #[cfg(target_arch = "wasm32")]
        if let Some(frame) = surface.frame {
            receipt_js(control, &format!("{receipt:?}"), frame.content_sequence as f64,
                frame.presentation_revision as f64);
        }
        if observer.receipts.get(control) != Some(&receipt) {
            match control {
                EXPLORE_GALLERY => { observer.gallery = None; observer.atlas = None; }
                explore::DETAIL_WORKSPACE_ID => observer.viewer = None,
                crate::view::workspace::STABLE_ID => observer.identity = (0, 0),
                _ => {}
            }
            observer.receipts.insert(control, receipt);
        }
    });
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

fn locate(control: String, generation: u64) -> Task<RootMessage> {
    let target = control.clone();
    widget::operate(FindControl {
        target: Id::from(control),
        translation: Vector::ZERO,
        pending_translation: Vector::ZERO,
        bounds: None,
    })
    .map(move |bounds| {
        RootMessage::Integration(Message::Scoped { generation, receipt: None, message: Box::new(Message::Located {
            control: target.clone(), bounds,
        }) })
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
#[wasm_bindgen::prelude::wasm_bindgen(module = "/src/integration_control/browser.mjs")]
extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationInitialize)]
    fn initialize_js(enabled: bool);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationDriver)]
    fn initialize_driver_js(enabled: bool);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationDriverDraw)]
    fn driver_draw_js(control: &str, source: f64, presentation: f64);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationResetScenario)]
    fn reset_scenario_js();
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationReceipt)]
    fn receipt_js(control: &str, receipt: &str, source: f64, presentation: f64);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationProbe)]
    fn capture_probe_js(control: &str) -> wasm_bindgen::JsValue;
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
        receipt: &wasm_bindgen::JsValue,
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
        receipt: &wasm_bindgen::JsValue,
        css_bounds: &[f64],
        color: &[f64],
        control: &str,
        detail: &str,
        button: bool,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAnnotationPixels)]
    fn annotation_pixels_js(
        receipt: &wasm_bindgen::JsValue,
        css_bounds: &[f64],
        extent: &[f64],
        probes: &[f64],
        source: f64,
        presentation: f64,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationAtlasPixels)]
    fn atlas_pixels_js(
        receipt: &wasm_bindgen::JsValue,
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
        receipt: &wasm_bindgen::JsValue,
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
        steps: u32,
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

#[cfg(any(target_arch = "wasm32", test))]
const ATLAS_GRID_SAMPLES: usize = 13;
#[cfg(any(target_arch = "wasm32", test))]
const ATLAS_COMPOSITION_SAMPLES: usize = 256 * 4 + ATLAS_GRID_SAMPLES;

#[cfg(any(target_arch = "wasm32", test))]
struct AtlasCompositionSamples {
    points: [f32; ATLAS_COMPOSITION_SAMPLES * 10],
    count: usize,
    cards: [u32; 256],
    card_count: usize,
}

#[cfg(any(target_arch = "wasm32", test))]
fn atlas_composition_samples(draw: &AtlasDraw) -> Option<AtlasCompositionSamples> {
    let snapshot = &draw.snapshot;
    if !pixel_fixture_enabled()
        || snapshot.augmentation.enabled
        || snapshot.overlay.showlabels
        || !snapshot.overlay.showmasks
        || !snapshot.overlay.showboxes
        || !matches!(snapshot.viewport.columns, 4 | 10)
        || snapshot.gallery.slots.iter().any(|ready| !*ready)
    {
        return None;
    }
    let Some(frame) = draw.surface.frame else {
        return None;
    };
    // The fixture is a constant clean image and a rectangular mask with a
    // central hole. Four isolated samples exclude all label geometry.
    let mut points = [0.0f32; ATLAS_COMPOSITION_SAMPLES * 10];
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
            return None;
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
            return None;
        }
    }
    // Real canvas samples bound each black/white/black line by adjacent
    // clean fixture pixels. The outer edges have one image-side neighbor;
    // the interior boundary has two. No CPU shader implementation is used.
    // Pick a visible row interior so horizontal grid lines cannot mask a defect.
    let cell = draw.image.width / columns as f32;
    let first_row = ((draw.clip.y - draw.image.y) / cell - 0.5).ceil().max(0.0);
    let y = draw.image.y + (first_row + 0.5) * cell;
    let centers = [draw.image.x + 1.5, draw.image.x + cell + 0.5,
        draw.image.x + draw.image.width - 1.5];
    for (edge, center) in centers.into_iter().enumerate() {
        let offsets: &[f32] = match edge {
            0 => &[-1.0, 0.0, 1.0, 2.0],
            1 => &[-2.0, -1.0, 0.0, 1.0, 2.0],
            _ => &[-2.0, -1.0, 0.0, 1.0],
        };
        for &offset in offsets {
            let point = iced::Point::new(center + offset, y);
            if !draw.clip.contains(point) || !draw.image.contains(point) { return None; }
            let color = if offset.abs() == 2.0 { [48.0, 80.0, 112.0] }
                else if offset == 0.0 { [255.0; 3] } else { [0.0; 3] };
            points[count..count + 10].copy_from_slice(&[
                point.x - draw.image.x, point.y - draw.image.y, point.x, point.y,
                color[0], color[1], color[2], 255.0, 4.0, 0.0,
            ]);
            count += 10;
        }
    }
    Some(AtlasCompositionSamples { points, count, cards, card_count })
}

fn sample_atlas_composition(draw: &AtlasDraw) {
    #[cfg(target_arch = "wasm32")]
    {
        if !crate::presentation_surface::pixel_trace::enabled() {
            return;
        }
        let Some(samples) = atlas_composition_samples(draw) else { return; };
        let frame = draw.surface.frame.expect("sampleable atlas composition");
        let Some(mut output) =
            atlas_probe_output(true)
        else {
            return;
        };
        let receipt = draw.clone();
        let canvas_probe = output.canvas_probe.clone();
        let completed = pixel_result_callback(move |outcome| {
            let _ = output.try_send(Message::AtlasComposition {
                receipt,
                outcome,
            });
        });
        atlas_composition_js(
            &canvas_probe,
            &samples.points[..samples.count],
            &samples.cards[..samples.card_count],
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
            draw.snapshot.viewport.columns,
            &completed,
        );
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = draw;
}

fn pixel_fixture_enabled() -> bool {
    PIXEL_FIXTURE_ENABLED.with(std::cell::Cell::get)
}

#[cfg(target_arch = "wasm32")]
fn pixel_result_callback(completed: impl FnOnce(ProbeOutcome) + 'static) -> wasm_bindgen::JsValue {
    // JsValue parameters keep malformed adapter values observable before conversion.
    let generation = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation);
    wasm_bindgen::closure::Closure::once_into_js(
        move |status: wasm_bindgen::JsValue, first: wasm_bindgen::JsValue, second: wasm_bindgen::JsValue| {
            if SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation) != generation || !reporting_enabled() { return; }
            let status = status.as_string();
            let outcome = ProbeOutcome::decode(status.as_deref(), [first.as_f64(), second.as_f64()]);
            if outcome == ProbeOutcome::Failed && status.as_deref() != Some("failed") {
                report("integration.failure", "", "pixel callback returned invalid counts", [0.0; 4]);
            }
            completed(outcome);
        },
    )
}

#[cfg(target_arch = "wasm32")]
fn sample_upscale_pixels(
    mut output: ScenarioOutput,
    image_pixels: Rectangle,
    button_css: Rectangle,
    source: u64,
    presentation: u64,
) {
    let canvas_probe = output.canvas_probe.clone();
    let completed = pixel_result_callback(move |outcome| {
        let _ = output.try_send(Message::UpscalePixels { source, presentation, outcome });
    });
    upscale_pixels_js(
        &canvas_probe,
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
fn sample_upscale_pixels(_output: ScenarioOutput, _image: Rectangle, _button: Rectangle, _source: u64, _presentation: u64) {}

#[cfg(target_arch = "wasm32")]
fn report(event: &str, control: &str, detail: &str, values: [f64; 4]) {
    if !reporting_enabled() {
        return;
    }
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
            && observer.output_for(EXPLORE_GALLERY).is_some_and(|output| {
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
    let Some(mut output) = atlas_probe_output(false)
    else {
        return;
    };
    let canvas_probe = output.canvas_probe.clone();
    let completed = pixel_result_callback(move |outcome| {
        let _ = output.try_send(Message::AtlasPixels {
            receipt: draw,
            outcome,
        });
    });
    atlas_pixels_js(
        &canvas_probe,
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
                && let Some(output) = observer.output_for(control)
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
                && observer.output_for(control).is_some_and(|output| {
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
                && observer.output_for(control).is_some_and(|output| {
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
    if !reporting_enabled() {
        return;
    }
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

#[derive(Clone, Default)]
struct SessionInputs {
    profile: String,
    source: String,
    compiled: String,
}

impl SessionInputs {
    fn scenario(&self, index: usize) -> Option<(&'static str, bool)> {
        match self.profile.as_str() {
            "retained" => [
                ("square", false), ("", false), ("wide", false), ("tall", false),
                ("semantics", false), ("copy", false), ("copy", true), ("rapid", false),
            ].get(index).copied(),
            "dpi" => [("copy", false), ("copy", true), ("rapid", true)].get(index).copied(),
            "terminal" => [("terminal", false)].get(index).copied(),
            _ => None,
        }
    }
}

pub struct Controller {
    generation: u64,
    phase: Phase,
    reported_phase: Option<Phase>,
    location_pending: bool,
    window_close: bool,
    dataset_source: String,
    compiled_directory: String,
    resolution: String,
    viewer_scenario: String,
    session: SessionInputs,
    control_sequence: u64,
    control_phase: Option<Phase>,
    control_progress: u64,
    desired_dark: Option<bool>,
    reuse_compiled: bool,
    bounded_document_revision: u64,
    bounded_object_count: usize,
    annotation_probe: Option<AnnotationProbe>,
    annotation_pixels_receipt: Option<ProbeReceipt>,
    annotation_pixels_pending: Option<ProbeReceipt>,
    annotation_pixels_owner: Option<std::sync::Arc<()>>,
    control_probe_receipt: Option<ProbeReceipt>,
    control_probe_owner: Option<std::sync::Arc<()>>,
    control_probe: Option<ControlProbe>,
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
    upscale_pixel_pending: Option<ProbeReceipt>,
    upscale_pixel_owner: Option<std::sync::Arc<()>>,
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
                if reporting_enabled() {
                    iced::Subscription::run(surface_draw_stream)
                } else {
                    iced::Subscription::none()
                },
                iced::event::listen_with(|event, _, _| {
                    matches!(
                        event,
                        iced::Event::Mouse(iced::mouse::Event::WheelScrolled { .. })
                    )
                    .then_some(Message::NumberWheelDelivered)
                })
                .with(self.generation)
                .map(|(generation, message)| Message::Scoped {
                    generation,
                    receipt: None,
                    message: Box::new(message),
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
        if !reporting_enabled() || !self.running() {
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
        DRIVER_ENABLED.with(|flag| flag.set(enabled));
        #[cfg(target_arch = "wasm32")]
        initialize_driver_js(enabled);
        let generation = if enabled { reset_observer() } else { 0 };
        Self {
            generation,
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
            session: SessionInputs::default(),
            control_sequence: 1,
            control_phase: None,
            control_progress: 0,
            desired_dark: None,
            reuse_compiled: false,
            bounded_document_revision: 0,
            bounded_object_count: 0,
            annotation_probe: None,
            annotation_pixels_receipt: None,
            annotation_pixels_pending: None,
            annotation_pixels_owner: None,
            control_probe_receipt: None,
            control_probe_owner: None,
            control_probe: None,
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
            upscale_pixel_pending: None,
            upscale_pixel_owner: None,
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

    pub(crate) fn reset_scenario(
        &mut self,
        dataset_source: String,
        compiled_directory: String,
        resolution: String,
        viewer_scenario: String,
    ) -> Result<(), &'static str> {
        if self.running() || matches!(self.phase, Phase::Failed) {
            return Err("scenario reset requires successful settlement");
        }
        let enabled = self.generation != 0;
        *self = Self::new(
            enabled,
            self.window_close,
            dataset_source,
            compiled_directory,
            resolution,
            viewer_scenario,
        );
        COMPLETION_WITHOUT_INPUT.with(|flag| flag.set(false));
        #[cfg(target_arch = "wasm32")]
        if enabled {
            reset_scenario_js();
        }
        Ok(())
    }

    pub(crate) fn configure_session(&mut self, profile: &str, square_source: String, square_compiled: String) {
        self.session = SessionInputs {
            profile: profile.into(),
            source: self.dataset_source.clone(),
            compiled: self.compiled_directory.clone(),
        };
        if let Some((scenario, dark)) = self.session.scenario(0) {
            self.viewer_scenario = scenario.into();
            self.desired_dark = Some(dark);
            self.reuse_compiled = true;
            if scenario == "square" {
                self.dataset_source = square_source;
                self.compiled_directory = square_compiled;
                self.resolution = "384".into();
            }
        } else {
            self.reuse_compiled = profile != "compile";
        }
    }

    pub(crate) fn receive_control(&mut self, receipt: crate::generated::IntegrationControlReceipt) -> Result<(), &'static str> {
        if receipt.kind != crate::generated::IntegrationControlKind::Advance
            || self.control_sequence.checked_add(1) != Some(receipt.sequence)
            || !matches!(self.phase, Phase::Complete)
            || self.control_phase.as_ref() != Some(&Phase::Complete)
        {
            self.fail("premature, duplicate or stale scenario advance");
            return Err("premature, duplicate or stale scenario advance");
        }
        let index = usize::try_from(receipt.sequence - 1).map_err(|_| "scenario index overflow")?;
        let Some((scenario, dark)) = self.session.scenario(index) else {
            if self.window_close {
                #[cfg(target_arch = "wasm32")]
                if window_close_js() != 1 {
                    self.fail("Firefox window close dispatch failed");
                    return Err("Firefox window close dispatch failed");
                }
                self.control_sequence = receipt.sequence;
                return Ok(());
            }
            self.fail("advance beyond the configured workflow");
            return Err("advance beyond the configured workflow");
        };
        let session = self.session.clone();
        self.reset_scenario(session.source.clone(), session.compiled.clone(), "512".into(), scenario.into())?;
        self.session = session;
        self.control_sequence = receipt.sequence;
        self.desired_dark = Some(dark);
        self.reuse_compiled = !scenario.is_empty();
        Ok(())
    }

    pub(crate) fn publish_control(&mut self, connection: &mut crate::transport_connection::Connection) {
        if self.generation == 0 || self.control_phase.as_ref() == Some(&self.phase) {
            return;
        }
        if self.viewer_scenario == "quiet" && matches!(self.phase, Phase::Complete)
            && !connection.integration_pressure_settled()
        {
            return;
        }
        use crate::generated::IntegrationControlKind as Kind;
        let kind = match self.phase {
            Phase::Disabled => return,
            Phase::Complete => Kind::Settled,
            Phase::Failed => Kind::Failed,
            _ => Kind::Progress,
        };
        let class = match self.phase.deadline_class() {
            "startup" => 1,
            "work" => 2,
            _ => 3,
        };
        let Some(progress) = self.control_progress.checked_add(4).and_then(|value| value.checked_add(class)) else {
            self.fail("integration progress identity exhausted");
            return;
        };
        match connection.send_integration_control(crate::generated::IntegrationControlReceipt {
            kind, sequence: self.control_sequence, progress,
        }) {
            Ok(_) => {
                self.control_phase = Some(self.phase.clone());
                self.control_progress = progress & !3;
            }
            Err(crate::transport_connection::OutboundSendError::Capacity) => {}
            Err(_) => self.fail("integration control delivery failed"),
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
        locate(control.into(), self.generation)
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
        scroll.chain(locate(control, self.generation))
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
            .chain(locate(control.into(), self.generation))
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

    fn prepare_upscale_probe(&mut self, image: Rectangle, source: u64, presentation: u64) -> Option<ScenarioOutput> {
        let output = probe_output(explore::DETAIL_WORKSPACE_ID)?;
        let receipt = output.receipt.as_ref()?;
        let frame = receipt.surface.frame?;
        if receipt.image != image || frame.content_sequence != source || frame.presentation_revision != presentation {
            return None;
        }
        self.upscale_pixel_owner = output.probe.clone();
        self.upscale_pixel_pending = output.receipt.clone();
        self.upscale_pixels = None;
        Some(output)
    }

    fn prepare_control_probe(&mut self) -> bool {
        if self.location_pending { return false; }
        let Some(output) = probe_output("workflow.visual.workspace") else { return false; };
        self.control_probe_owner = output.probe.clone();
        self.control_probe_receipt = output.receipt.clone();
        self.control_probe = Some(ControlProbe {
            output, color: self.copy_swatch_color, available: self.copy_capability_available,
        });
        true
    }

    fn prepare_annotation_probe(&mut self, source: u64, presentation: u64, extent: [u32; 2], pixels: Vec<f64>) -> bool {
        if self.location_pending { return false; }
        let Some(output) = probe_output("workflow.visual.workspace") else { return false; };
        if self.annotation_pixels_pending == output.receipt { return false; }
        self.annotation_pixels_owner = output.probe.clone();
        self.annotation_pixels_pending = output.receipt.clone();
        self.annotation_probe = Some(AnnotationProbe { output, source, presentation, extent, pixels });
        true
    }

    fn invalidate_atlas_draw(&mut self) {
        // The next ordinary physical draw can arm the same frame at settled geometry.
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().atlas = None);
    }

    pub(crate) fn accepts_message(&self, message: &Message) -> bool {
        if !self.running() {
            return false;
        }
        match message {
            Message::Scoped { generation, receipt, message } => {
                let (probe, message) = match message.as_ref() {
                    Message::ProbeCompleted { owner, message } => (Some(owner), message.as_ref()),
                    message => (None, message),
                };
                *generation == self.generation
                    && *generation == SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation)
                    && match message {
                        Message::UpscalePixels { .. } => same_probe(&self.upscale_pixel_owner, probe),
                        Message::AnnotationControlPixels { .. } | Message::AnnotationPixels { revision: 0, .. } => same_probe(&self.control_probe_owner, probe),
                        Message::AnnotationPixels { .. } => same_probe(&self.annotation_pixels_owner, probe),
                        Message::AtlasPixels { .. } => SURFACE_DRAW_OBSERVER.with(|observer| same_probe(&observer.borrow().atlas_pixels_owner, probe)),
                        Message::AtlasComposition { .. } => SURFACE_DRAW_OBSERVER.with(|observer| same_probe(&observer.borrow().atlas_composition_owner, probe)),
                        _ => probe.is_none(),
                    }
                    && match receipt {
                        Some(receipt) => current_receipt(receipt.control).as_ref() == Some(receipt),
                        None => matches!(message, Message::Advance | Message::Located { .. } | Message::NumberWheelDelivered),
                    }
            }
            Message::Advance | Message::Located { .. } | Message::NumberWheelDelivered => true,
            _ => false,
        }
    }

    pub fn update(&mut self, message: Message) -> Option<train::Message> {
        if !self.accepts_message(&message) {
            return None;
        }
        let (message, request_receipt) = match message {
            Message::Scoped { message, receipt, .. } => (*message, receipt),
            message => (message, None),
        };
        let message = match message {
            Message::ProbeCompleted { message, .. } => *message,
            message => message,
        };
        let (control, bounds) = match message {
            Message::Scoped { .. } | Message::ProbeCompleted { .. } | Message::Advance => return None,
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
            Message::UpscalePixels { source, presentation, outcome } => {
                if self.upscale_pixel_pending != request_receipt { return None; }
                self.upscale_pixel_owner = None;
                match outcome {
                    ProbeOutcome::Invalidated => self.upscale_pixel_pending = None,
                    ProbeOutcome::Observed(checksum, blue) => self.upscale_pixels = Some((source, presentation, checksum, blue)),
                    ProbeOutcome::Failed => self.fail("Upscale canvas sampling failed"),
                }
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
            Message::AnnotationControlPixels { outcome } => {
                if self.control_probe_receipt != request_receipt { return None; }
                self.control_probe_owner = None;
                match outcome {
                    ProbeOutcome::Invalidated => self.control_probe_receipt = None,
                    ProbeOutcome::Observed(1, 1) => self.copy_capability_ready = true,
                    _ => self.fail("Rendered tool availability differs from the native capability"),
                }
                return None;
            }
            Message::AnnotationPixels { revision, outcome } => {
                let pending = if revision == 0 { &mut self.control_probe_receipt } else { &mut self.annotation_pixels_pending };
                if *pending != request_receipt { return None; }
                *pending = None;
                if revision == 0 { self.control_probe_owner = None; } else { self.annotation_pixels_owner = None; }
                match outcome {
                    ProbeOutcome::Invalidated => {}
                    ProbeOutcome::Observed(expected, matched) if expected != 0 && expected == matched => {
                        if revision == 0 {
                            self.control_probe_receipt = request_receipt;
                            self.copy_swatch_ready = true;
                        } else {
                            self.annotation_pixels_receipt = request_receipt;
                        }
                    }
                    _ => self.fail("Annotation pixels do not match source geometry and native palette"),
                }
                return None;
            }
            Message::AtlasComposition { receipt, outcome } => {
                SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().atlas_composition_owner = None);
                match outcome {
                    ProbeOutcome::Invalidated => self.invalidate_atlas_draw(),
                    ProbeOutcome::Observed(expected, matched) if expected != 0 && expected == matched => self.atlas_composition = Some(receipt),
                    ProbeOutcome::Failed => self.fail("Atlas composition canvas sampling failed"),
                    _ => {}
                }
                return None;
            }
            Message::AtlasPixels { receipt, outcome } => {
                SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().atlas_pixels_owner = None);
                match outcome {
                    ProbeOutcome::Invalidated => self.invalidate_atlas_draw(),
                    ProbeOutcome::Observed(visible, nonblack) => {
                        report("integration.atlas_canvas_pixels", EXPLORE_GALLERY, "visible-tile-interiors",
                            [receipt.snapshot.frame.revision as f64,
                             receipt.surface.frame.map_or(0, |frame| frame.presentation_revision) as f64,
                             visible as f64, nonblack as f64]);
                        if visible != 0 && visible == nonblack { self.atlas_pixels = Some(receipt); }
                    }
                    ProbeOutcome::Failed => self.fail("Atlas canvas sampling failed"),
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
        if let Some(probe) = &self.annotation_probe {
            if probe.output.receipt != current_receipt("workflow.visual.workspace") {
                if self.annotation_pixels_pending == probe.output.receipt {
                    self.annotation_pixels_pending = None;
                    self.annotation_pixels_owner = None;
                }
                self.annotation_probe = None;
                return None;
            }
        }
        if let Some(probe) = &self.control_probe {
            if probe.output.receipt != current_receipt("workflow.visual.workspace") {
                if self.control_probe_receipt == probe.output.receipt {
                    self.control_probe_receipt = None;
                    self.control_probe_owner = None;
                }
                self.control_probe = None;
                return None;
            }
        }
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
                self.phase = if self.viewer_scenario == "terminal" {
                    Phase::OpenAnnotation
                } else {
                    Phase::Complete
                };
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
                if let Some(probe) = self.control_probe.take() {
                    #[cfg(not(target_arch = "wasm32"))]
                    let _ = probe;
                    #[cfg(target_arch = "wasm32")]
                    {
                        let mut output = probe.output;
                        let canvas_probe = output.canvas_probe.clone();
                        let callback = pixel_result_callback(move |outcome| {
                            let _ =
                                output.try_send(Message::AnnotationControlPixels { outcome });
                        });
                        annotation_swatch_js(
                            &canvas_probe,
                            &[
                                f64::from(input_bounds.x),
                                f64::from(input_bounds.y),
                                f64::from(input_bounds.width),
                                f64::from(input_bounds.height),
                            ],
                            &probe.color,
                            &control,
                            if probe.available {
                                "enabled"
                            } else {
                                "disabled"
                            },
                            true,
                            &callback,
                        );
                    }
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
                if let Some(probe) = self.control_probe.take() {
                    #[cfg(not(target_arch = "wasm32"))]
                    let _ = probe;
                    #[cfg(target_arch = "wasm32")]
                    {
                        let mut output = probe.output;
                        let canvas_probe = output.canvas_probe.clone();
                        let callback = pixel_result_callback(move |outcome| {
                            let _ = output.try_send(Message::AnnotationPixels {
                                revision: 0,
                                outcome,
                            });
                        });
                        annotation_swatch_js(
                            &canvas_probe,
                            &[
                                f64::from(input_bounds.x),
                                f64::from(input_bounds.y),
                                f64::from(input_bounds.width),
                                f64::from(input_bounds.height),
                            ],
                            &probe.color,
                            &control,
                            "native-hsv-completed-canvas",
                            false,
                            &callback,
                        );
                    }
                }
                None
            }
            Phase::AwaitPointer(_) | Phase::CopyProductWait if self.annotation_probe.is_some() => {
                if let Some(probe) = self.annotation_probe.take() {
                    #[cfg(not(target_arch = "wasm32"))]
                    let _ = probe;
                    #[cfg(target_arch = "wasm32")]
                    {
                        let mut output = probe.output;
                        let source = probe.source;
                        let canvas_probe = output.canvas_probe.clone();
                        let callback = pixel_result_callback(move |outcome| {
                            let _ = output.try_send(Message::AnnotationPixels { revision: source, outcome });
                        });
                        annotation_pixels_js(
                            &canvas_probe,
                            &[f64::from(input_bounds.x), f64::from(input_bounds.y),
                              f64::from(input_bounds.width), f64::from(input_bounds.height)],
                            &probe.extent.map(f64::from), &probe.pixels,
                            source as f64, probe.presentation as f64, &callback,
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
                let index = if self.viewer_scenario == "tall" { 7 } else { 0 };
                let side = input_bounds.width / columns as f32;
                let selected = Rectangle {
                    x: input_bounds.x + (index % columns) as f32 * side,
                    y: input_bounds.y + (index / columns) as f32 * side,
                    width: side,
                    height: side,
                };
                self.phase = Phase::AwaitDetail(index);
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
                self.upscale_pixel_pending = None;
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
                    if self.viewer_scenario == "quiet" { 160 } else { 1 },
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
        if reporting_enabled() {
            report(
                "integration.phase_advanced",
                "",
                &format!("{phase:?}"),
                [0.0; 4],
            );
        }
        let reveal_annotation = matches!(phase, Phase::CopyProductWait);
        self.phase = phase;
        // A completed local step has no pending native event to wake its
        // successor. Queue one continuation without requiring another draw.
        let continuation = Task::done(RootMessage::Integration(Message::Scoped {
            generation: self.generation, receipt: None, message: Box::new(Message::Advance),
        }));
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
        if matches!(self.phase, Phase::Disabled) {
            return;
        }
        if self.reported_phase.as_ref() == Some(&self.phase) {
            return;
        }
        if reporting_enabled() {
            report(
                "integration.phase_progress",
                self.phase.deadline_class(),
                &format!("{:?}", self.phase),
                [0.0; 4],
            );
        }
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
        if let Some(dark) = self.desired_dark {
            let Some(snapshot) = model.settings_snapshot.as_ref() else { return Task::none(); };
            if settings.has_local_edits() { return Task::none(); }
            if snapshot.settingsstate.ui.darkmode != dark {
                return Task::done(RootMessage::Settings(crate::view::settings::Message::DarkModeChanged(dark)));
            }
            self.desired_dark = None;
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
            if reporting_enabled() {
                self.fail(&format!("{}: {}", error.title, error.detail));
            } else {
                self.fail("application reported an error");
            }
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
            Phase::AwaitDatasetSettings(_) => {
                let Some(snapshot) = model.settings_snapshot.as_ref() else {
                    return Task::none();
                };
                let train = &snapshot.settingsstate.workflows.train;
                if train.datasetsourcedir != self.dataset_source
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
                if self.reuse_compiled {
                    self.phase = Phase::ExploreNavigation;
                    self.arm(crate::view::navigation::stable_id(FeatureId::Explore))
                } else {
                    self.phase = Phase::Compile;
                    self.arm_scrolled(COMPILE_DATASET, RelativeOffset::END)
                }
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
                    if self.viewer_scenario == "quiet" {
                        self.selection_grid = Some((
                            snapshot.viewport.columns, snapshot.viewport.rowcount, 0,
                            snapshot.revision, snapshot.frame.revision,
                        ));
                        COMPLETION_WITHOUT_INPUT.with(|active| active.set(false));
                        self.phase = Phase::ViewerSelect;
                        return self.arm(EXPLORE_GALLERY);
                    }
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
                if self.viewer_scenario == "quiet" {
                    self.phase = Phase::OpenAnnotation;
                    return self.arm(EXPLORE_ANNOTATE);
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
                let Some(receipt) = current_receipt(explore::DETAIL_WORKSPACE_ID) else { return Task::none(); };
                if self.upscale_pixel_pending.as_ref() != Some(&receipt) {
                    if let Some(output) = self.prepare_upscale_probe(viewer.image, source, drawn) {
                        sample_upscale_pixels(output, viewer.image, button, source, drawn);
                    }
                    return Task::none();
                }
                let Some((pixel_source, pixel_presentation, checksum, blue)) = self.upscale_pixels
                else {
                    return Task::none();
                };
                if pixel_source != source || pixel_presentation != drawn {
                    if let Some(output) = self.prepare_upscale_probe(viewer.image, source, drawn) {
                        sample_upscale_pixels(output, viewer.image, button, source, drawn);
                    }
                    return Task::none();
                }
                if checksum == 0 || blue < 32 {
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
                    if !self.prepare_control_probe() { return Task::none(); }
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
                    .chain(locate("annotation.class.active.swatch".into(), self.generation))
                }
            }
            Phase::CopySwatchWait => {
                if self.control_probe_receipt != current_receipt("workflow.visual.workspace") {
                    self.copy_swatch_ready = false;
                    return self.advance_to(Phase::CopyLayout(2));
                }
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
                if !self.prepare_control_probe() { return Task::none(); }
                self.copy_capability_ready = false;
                self.arm_scrolled(
                    annotation::tool_id(crate::generated::AnnotationTool::ColorSample),
                    RelativeOffset::START,
                )
            }
            Phase::CopyCapabilityWait => {
                if self.control_probe_receipt != current_receipt("workflow.visual.workspace") {
                    self.copy_capability_ready = false;
                    return self.advance_to(Phase::CopyCapability);
                }
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
                if matches!(self.viewer_scenario.as_str(), "quiet" | "terminal") {
                    self.bounded_document_revision = snapshot.ui.documentrevision;
                    self.bounded_object_count = snapshot.ui.scene.objects.len();
                    self.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool: crate::generated::AnnotationTool::Box,
                    };
                    return self.arm_scrolled(annotation::tool_id(crate::generated::AnnotationTool::Box), RelativeOffset::START);
                }
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
                if matches!(self.viewer_scenario.as_str(), "quiet" | "terminal") {
                    let edited = model.annotation_edit_available()
                        && snapshot.ui.documentrevision > self.bounded_document_revision
                        && snapshot.ui.scene.objects.len() == self.bounded_object_count + 1
                        && snapshot.ui.canundo
                        && snapshot.ui.scene.objects.last().is_some_and(|object| {
                            object.shape == crate::generated::AnnotationShape::Box
                                && object.box_.second.x > object.box_.first.x
                                && object.box_.second.y > object.box_.first.y
                        });
                    if !edited {
                        return Task::none();
                    }
                    if self.viewer_scenario == "quiet" {
                        self.phase = Phase::Complete;
                        return Task::none();
                    }
                }
                let Some(presentation) = model.presentation.as_ref() else {
                    return Task::none();
                };
                let Some(sampleable) = self.annotation_frame_ready else {
                    return Task::none();
                };
                let presentation_revision = sampleable.presentation_revision;
                if !model.annotation_edit_available()
                    || (self.copy_step != 8 && snapshot.ui.interactionrevision <= revision)
                    || (self.copy_step == 8 && snapshot.frame.revision <= self.copy_product_frame)
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
                let receipt = current_receipt("workflow.visual.workspace");
                if self.viewer_scenario == "copy"
                    && (receipt.is_none() || self.annotation_pixels_receipt != receipt)
                {
                    if self.location_pending || self.annotation_pixels_pending == receipt { return Task::none(); }
                    if !self.prepare_annotation_probe(snapshot.frame.revision, presentation_revision,
                        [snapshot.frame.extent.width, snapshot.frame.extent.height], annotation_checks::probes(&snapshot.ui)) {
                        return Task::none();
                    }
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
    fn destructive_profile_continues_viewer_completion_into_annotation() {
        initialize_reporting(false, false);
        for window_close in [false, true] {
            let mut driver = Controller::new(true, window_close, "source".into(), "compiled".into(), "512".into(), "terminal".into());
            driver.configure_session("terminal", String::new(), String::new());
            assert_eq!(driver.session.scenario(0), Some(("terminal", false)));
            assert_eq!(driver.session.scenario(1), None);
            assert!(driver.reuse_compiled);
            driver.phase = Phase::ViewerNoAspect;
            driver.viewer_drawn = Some((7, 3, ViewerDraw {
                crop: [0, 0, 512, 512],
                container: Rectangle::default(),
                image: Rectangle::default(),
                fit_revision: 1,
            }));
            driver.update(Message::Located {
                control: "explore.detail.aspect".into(),
                bounds: Rectangle::default(),
            });
            assert!(matches!(driver.phase, Phase::OpenAnnotation));
            assert!(driver.running());
            assert!(driver.receive_control(crate::generated::IntegrationControlReceipt {
                kind: crate::generated::IntegrationControlKind::Advance, sequence: 2, progress: 0,
            }).is_err(), "viewer evidence cannot settle a destructive Annotation workflow");
        }
    }

    #[test]
    fn retained_workflows_require_the_unique_settled_control_owner() {
        initialize_reporting(false, false);
        let mut driver = Controller::new(true, false, "mixed-source".into(), "mixed-output".into(), "512".into(), "retained".into());
        driver.configure_session("retained", "square-source".into(), "square-output".into());
        assert_eq!(driver.viewer_scenario, "square");
        assert_eq!(driver.resolution, "384");
        assert_eq!(driver.dataset_source, "square-source");
        let generation = driver.generation;
        let advance = crate::generated::IntegrationControlReceipt {
            kind: crate::generated::IntegrationControlKind::Advance,
            sequence: 2,
            progress: 0,
        };
        driver.phase = Phase::Complete;
        driver.control_phase = Some(Phase::Complete);
        driver.receive_control(advance.clone()).unwrap();
        assert_ne!(driver.generation, generation);
        assert_eq!(driver.control_sequence, 2);
        assert_eq!(driver.dataset_source, "mixed-source");
        assert_eq!(driver.compiled_directory, "mixed-output");
        assert_eq!(driver.resolution, "512");
        assert!(driver.viewer_scenario.is_empty());
        assert!(!driver.reuse_compiled);
        assert!(driver.receive_control(advance).is_err());
        assert!(matches!(driver.phase, Phase::Failed));

        let mut premature = Controller::new(true, false, String::new(), String::new(), "512".into(), "dpi".into());
        premature.configure_session("dpi", String::new(), String::new());
        premature.phase = Phase::Complete;
        assert!(premature.receive_control(crate::generated::IntegrationControlReceipt {
            kind: crate::generated::IntegrationControlKind::Advance, sequence: 2, progress: 0,
        }).is_err(), "local completion is insufficient before the typed receipt was admitted");
    }

    #[test]
    fn quiet_driver_and_disabled_frontend_collect_no_probe_state() {
        initialize_reporting(false, false);
        let mut driver = Controller::new(true, false, String::new(), String::new(), String::new(), String::new());
        assert!(driver.running());
        assert!(DRIVER_ENABLED.with(std::cell::Cell::get));
        assert!(!reporting_enabled());
        assert!(!pixel_fixture_enabled());
        let (_, frame) = crate::view_model::test_support::explore_presentation();
        let mut surface = crate::view_model::test_support::physical_surface(frame);
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
        // Even a diagnostic-marked surface cannot activate collection in a quiet driver.
        surface.integration = true;
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let observer = observer.borrow();
            assert!(observer.receipts.is_empty());
            assert!(observer.output.is_none());
            assert!(observer.subscription.is_none());
            assert_eq!(observer.identity, (0, 0));
        });
        driver.phase = Phase::Complete;
        driver.reset_scenario(String::new(), String::new(), String::new(), String::new()).unwrap();
        assert!(driver.running());
        assert!(DRIVER_ENABLED.with(std::cell::Cell::get));
        assert!(!reporting_enabled());
        let generation = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation);
        let disabled = Controller::new(false, false, String::new(), String::new(), String::new(), String::new());
        assert!(!disabled.running());
        assert!(!DRIVER_ENABLED.with(std::cell::Cell::get));
        notify_driver_draw(EXPLORE_GALLERY, frame.content_sequence, frame.presentation_revision);
        assert_eq!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation), generation);
    }

    #[test]
    fn scenario_reset_isolates_queued_messages_geometry_and_obsolete_subscription_teardown() {
        initialize_reporting(true, true);
        let mut controller = Controller::new(true, false, "old-source".into(), "old-output".into(), "384".into(), "atlas".into());
        let (sender, mut receiver) = iced::futures::channel::mpsc::channel(8);
        let old_subscription = std::sync::Arc::new(());
        let mut old_output = ScenarioOutput::new(controller.generation, sender);
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let mut observer = observer.borrow_mut();
            observer.subscription = Some(old_subscription.clone());
            observer.output = Some(old_output.clone());
            observer.identity = (3, 4);
        });
        let (_, frame) = crate::view_model::test_support::explore_presentation();
        let mut surface = crate::view_model::test_support::physical_surface(frame);
        surface.integration = true;
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        let old_receipt = current_receipt(EXPLORE_GALLERY).unwrap();
        old_output.try_send(Message::Located { control: EXPLORE_GALLERY.into(), bounds }).unwrap();
        record_probe_draw(crate::view::workspace::STABLE_ID, surface, bounds, bounds, bounds);
        let viewer = ViewerDraw { crop: surface.content_region(), container: bounds, image: bounds, fit_revision: 0 };
        report_surface_draw(crate::view::workspace::STABLE_ID, 5, 1, false, 640, 480, 1, viewer);
        controller.location_pending = true;
        controller.atlas_baseline = Some((1, 5));
        controller.reported_style_bits = 7;
        controller.annotation_pixels_receipt = Some(old_receipt.clone());
        controller.upscale_pixel_pending = Some(old_receipt);
        assert!(controller.reset_scenario("new-source".into(), "new-output".into(), "512".into(), "upscale".into()).is_err());
        controller.phase = Phase::Complete;
        controller.reset_scenario("new-source".into(), "new-output".into(), "512".into(), "upscale".into()).unwrap();
        assert!(!controller.location_pending);
        assert!(controller.atlas_baseline.is_none());
        assert!(controller.upscale_pixel_pending.is_none());
        assert!(controller.annotation_pixels_receipt.is_none());
        assert_eq!(controller.reported_style_bits, 0);
        assert!(current_receipt(EXPLORE_GALLERY).is_none());
        assert_eq!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().identity), (0, 0));
        controller.location_pending = true; // Same widget may already be armed in the replacement.
        while let Ok(message) = receiver.try_recv() { assert!(controller.update(message).is_none()); }
        assert!(controller.location_pending);
        assert!(controller.annotation_drawn.is_none());
        // A late result retains the original sender/generation even after reset.
        old_output.try_send(Message::SurfaceDrawn { presentation_revision: 5, source_revision: 1, viewer: None }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.annotation_drawn.is_none());
        for control in [EXPLORE_GALLERY, explore::DETAIL_WORKSPACE_ID, crate::view::workspace::STABLE_ID] {
            assert!(probe_output(control).is_none(), "no physical receipt cannot schedule a probe");
            report_surface_draw(control, 5, 1, false, 640, 480, 1, viewer);
            assert!(receiver.try_recv().is_err(), "no physical receipt cannot enqueue a draw");
            record_probe_draw(control, surface, bounds, bounds, bounds);
            report_surface_draw(control, 5, 1, false, 640, 480, 1, viewer);
            let queued = receiver.try_recv().unwrap();
            let moved = Rectangle { x: 17.0, ..bounds };
            record_probe_draw(control, surface, bounds, moved, bounds);
            assert!(!controller.accepts_message(&queued));
            controller.update(queued);
            assert!(controller.annotation_drawn.is_none());
            assert!(controller.viewer_drawn.is_none());
            assert!(controller.gallery_drawn.is_none());
            // Unchanged revisions and viewer fields must not suppress a new geometry receipt.
            report_surface_draw(control, 5, 1, true, 640, 480, 2, viewer);
            let queued = receiver.try_recv().unwrap();
            assert!(controller.accepts_message(&queued));
            controller.update(queued);
            match control {
                EXPLORE_GALLERY => assert_eq!(controller.gallery_drawn.take(), Some((5, 1))),
                explore::DETAIL_WORKSPACE_ID => assert_eq!(controller.viewer_drawn.take(), Some((5, 1, viewer))),
                _ => assert_eq!(controller.annotation_drawn.take(), Some((5, 1))),
            }
        }
        let snapshot = std::sync::Arc::new(crate::view_model::test_support::explore_snapshot());
        let draw = AtlasDraw { surface, snapshot, bounds, image: bounds, clip: bounds };
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().receipts.remove(EXPLORE_GALLERY));
        report_atlas_draw(draw.clone(), true, 1.0);
        assert!(receiver.try_recv().is_err());
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        report_atlas_draw(draw.clone(), true, 1.0);
        let queued = receiver.try_recv().unwrap();
        let moved_draw = AtlasDraw { image: Rectangle { x: 17.0, ..bounds }, ..draw };
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, moved_draw.image, bounds);
        assert!(!controller.accepts_message(&queued));
        controller.update(queued);
        assert!(controller.atlas_receipt.is_none());
        report_atlas_draw(moved_draw.clone(), true, 1.0);
        let queued = receiver.try_recv().unwrap();
        assert!(controller.accepts_message(&queued));
        controller.update(queued);
        assert_eq!(controller.atlas_receipt, Some(moved_draw));
        old_output.generation = controller.generation;
        old_output.try_send(Message::SurfaceDrawn { presentation_revision: 5, source_revision: 1, viewer: None }).unwrap();
        assert!(!controller.accepts_message(&receiver.try_recv().unwrap()), "physical messages cannot use a generation-only output");
        let replacement_subscription = std::sync::Arc::new(());
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().subscription = Some(replacement_subscription.clone()));
        drop(SurfaceDrawSubscription(old_subscription));
        assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.is_some()));
        assert!(current_receipt(EXPLORE_GALLERY).is_some());
        drop(SurfaceDrawSubscription(replacement_subscription));
        assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.is_none()));
        controller.phase = Phase::Failed;
        assert!(controller.reset_scenario(String::new(), String::new(), String::new(), String::new()).is_err());
        initialize_reporting(false, false);
    }

    #[test]
    fn atlas_invalidation_cannot_retire_a_replacement_request_on_the_same_draw() {
        initialize_reporting(true, true);
        let mut controller = Controller::new(true, false, String::new(), String::new(), "512".into(), "atlas".into());
        let (sender, mut receiver) = iced::futures::channel::mpsc::channel(8);
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = Some(ScenarioOutput::new(controller.generation, sender)));
        let (_, frame) = crate::view_model::test_support::explore_presentation();
        let mut surface = crate::view_model::test_support::physical_surface(frame);
        surface.integration = true;
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
        let draw = AtlasDraw { surface, bounds, image: bounds, clip: bounds,
            snapshot: std::sync::Arc::new(crate::view_model::test_support::explore_snapshot()) };
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        report_atlas_draw(draw.clone(), false, 1.0);
        controller.update(receiver.try_recv().unwrap());
        let mut old_pixels = atlas_probe_output(false).unwrap();
        let mut old_composition = atlas_probe_output(true).unwrap();
        old_pixels.try_send(Message::AtlasPixels { receipt: draw.clone(), outcome: ProbeOutcome::Invalidated }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().atlas.is_none()));
        // A normal draw rearms the same physical frame after CSS/backing settles.
        report_atlas_draw(draw.clone(), false, 1.0);
        controller.update(receiver.try_recv().unwrap());
        let mut pixels = atlas_probe_output(false).unwrap();
        let mut composition = atlas_probe_output(true).unwrap();
        for (output, message) in [
            (&mut old_pixels, Message::AtlasPixels { receipt: draw.clone(), outcome: ProbeOutcome::Invalidated }),
            (&mut old_composition, Message::AtlasComposition { receipt: draw.clone(), outcome: ProbeOutcome::Invalidated }),
        ] {
            output.try_send(message).unwrap();
            let stale = receiver.try_recv().unwrap();
            assert!(!controller.accepts_message(&stale));
            controller.update(stale);
        }
        assert_eq!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().atlas.clone()), Some(draw.clone()));
        assert!(controller.atlas_pixels.is_none() && controller.atlas_composition.is_none());
        pixels.try_send(Message::AtlasPixels { receipt: draw.clone(), outcome: ProbeOutcome::Observed(1, 1) }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        composition.try_send(Message::AtlasComposition { receipt: draw.clone(), outcome: ProbeOutcome::Observed(13, 13) }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.atlas_pixels, Some(draw.clone()));
        assert_eq!(controller.atlas_composition, Some(draw));
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
        initialize_reporting(false, false);
    }

    #[test]
    fn pixel_outcome_adapter_preserves_invalidated_observed_and_failed() {
        assert_eq!(ProbeOutcome::decode(Some("invalidated"), [Some(0.0); 2]), ProbeOutcome::Invalidated);
        assert_eq!(ProbeOutcome::decode(Some("observed"), [Some(1.0), Some(0.0)]), ProbeOutcome::Observed(1, 0));
        for (status, values) in [
            (None, [Some(0.0); 2]),
            (Some("invalidated"), [Some(1.0), Some(0.0)]),
            (Some("observed"), [None, Some(0.0)]),
            (Some("observed"), [Some(f64::NAN), Some(0.0)]),
            (Some("observed"), [Some(f64::INFINITY), Some(0.0)]),
            (Some("observed"), [Some(-1.0), Some(0.0)]),
            (Some("observed"), [Some(0.5), Some(0.0)]),
            (Some("observed"), [Some(f64::from(u32::MAX) + 1.0), Some(0.0)]),
            (Some("failed"), [Some(0.0); 2]),
        ] {
            assert_eq!(ProbeOutcome::decode(status, values), ProbeOutcome::Failed);
        }
    }

    #[test]
    fn annotation_and_upscale_consumers_retire_invalidations_without_pixel_evidence() {
        for consumer in 0..4 {
            initialize_reporting(true, true);
            let mut controller = Controller::new(true, false, String::new(), String::new(), "512".into(), "copy".into());
            let (sender, mut receiver) = iced::futures::channel::mpsc::channel(8);
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = Some(ScenarioOutput::new(controller.generation, sender)));
            let (_, frame) = crate::view_model::test_support::explore_presentation();
            let mut surface = crate::view_model::test_support::physical_surface(frame);
            surface.integration = true;
            let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
            let control = if consumer == 3 { explore::DETAIL_WORKSPACE_ID } else { "workflow.visual.workspace" };
            record_probe_draw(control, surface, bounds, bounds, bounds);
            let arm = |controller: &mut Controller, image| {
                match consumer {
                    0 | 1 => {
                        controller.phase = if consumer == 0 { Phase::CopyCapabilityWait } else { Phase::CopySwatchWait };
                        assert!(controller.prepare_control_probe());
                        controller.control_probe.take().unwrap().output
                    }
                    2 => {
                        controller.phase = Phase::CopyProductWait;
                        assert!(controller.prepare_annotation_probe(frame.content_sequence, frame.presentation_revision,
                            [frame.content_width, frame.content_height], vec![1.0; 7]));
                        controller.annotation_probe.take().unwrap().output
                    }
                    _ => {
                        controller.phase = Phase::AwaitExploreReady;
                        controller.prepare_upscale_probe(image, frame.content_sequence, frame.presentation_revision).unwrap()
                    }
                }
            };
            let message = |outcome| match consumer {
                0 => Message::AnnotationControlPixels { outcome },
                1 => Message::AnnotationPixels { revision: 0, outcome },
                2 => Message::AnnotationPixels { revision: frame.content_sequence, outcome },
                _ => Message::UpscalePixels { source: frame.content_sequence, presentation: frame.presentation_revision, outcome },
            };
            let mut old = arm(&mut controller, bounds);
            let phase = controller.phase.clone();
            // JavaScript CSS/backing replacement may invalidate while this exact
            // Rust receipt is still current. Exercise the real message consumer.
            old.try_send(message(ProbeOutcome::Invalidated)).unwrap();
            controller.update(receiver.try_recv().unwrap());
            assert_eq!(controller.phase, phase);
            assert!(controller.annotation_pixels_pending.is_none());
            assert!(controller.control_probe_receipt.is_none());
            assert!(controller.upscale_pixel_pending.is_none());
            assert!(!controller.copy_capability_ready && !controller.copy_swatch_ready);
            assert!(controller.annotation_pixels_receipt.is_none() && controller.upscale_pixels.is_none());

            let mut same_frame = arm(&mut controller, bounds);
            old.try_send(message(ProbeOutcome::Invalidated)).unwrap();
            let stale = receiver.try_recv().unwrap();
            assert!(!controller.accepts_message(&stale), "a new request can own the same physical receipt");
            controller.update(stale);
            same_frame.try_send(message(ProbeOutcome::Invalidated)).unwrap();
            controller.update(receiver.try_recv().unwrap());
            assert_eq!(controller.phase, phase);

            let moved = Rectangle { x: 17.0, ..bounds };
            record_probe_draw(control, surface, bounds, moved, bounds);
            let mut replacement = arm(&mut controller, moved);
            for outcome in [ProbeOutcome::Invalidated, ProbeOutcome::Failed, ProbeOutcome::Observed(1, 1)] {
                old.try_send(message(outcome)).unwrap();
                let stale = receiver.try_recv().unwrap();
                assert!(!controller.accepts_message(&stale));
                controller.update(stale);
            }
            assert_eq!(controller.phase, phase);
            let pending = if consumer == 3 { &controller.upscale_pixel_pending }
                else if consumer == 2 { &controller.annotation_pixels_pending } else { &controller.control_probe_receipt };
            assert_eq!(*pending, replacement.receipt);
            replacement.try_send(message(ProbeOutcome::Observed(1, 1))).unwrap();
            controller.update(receiver.try_recv().unwrap());
            match consumer {
                0 => assert!(controller.copy_capability_ready),
                1 => assert!(controller.copy_swatch_ready),
                2 => assert_eq!(controller.annotation_pixels_receipt, replacement.receipt),
                _ => assert_eq!(controller.upscale_pixels, Some((frame.content_sequence, frame.presentation_revision, 1, 1))),
            }
            // A new current observation must retain both measured and adapter
            // failures. Upscale's wait phase owns checksum/color validation.
            if consumer == 2 { controller.annotation_pixels_pending = None; }
            let mut current = arm(&mut controller, moved);
            current.try_send(message(ProbeOutcome::Observed(1, 0))).unwrap();
            controller.update(receiver.try_recv().unwrap());
            if consumer == 3 {
                assert_eq!(controller.upscale_pixels, Some((frame.content_sequence, frame.presentation_revision, 1, 0)));
                let mut current = arm(&mut controller, moved);
                current.try_send(message(ProbeOutcome::Failed)).unwrap();
                controller.update(receiver.try_recv().unwrap());
            }
            assert_eq!(controller.phase, Phase::Failed);
            let mut malformed = arm(&mut controller, moved);
            malformed.try_send(message(ProbeOutcome::decode(Some("observed"), [None, Some(1.0)]))).unwrap();
            controller.update(receiver.try_recv().unwrap());
            assert_eq!(controller.phase, Phase::Failed);
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
            initialize_reporting(false, false);
        }
    }

    #[test]
    fn probe_preparation_keeps_original_frame_through_widget_location() {
        initialize_reporting(true, true);
        let mut controller = Controller::new(true, false, String::new(), String::new(), "512".into(), "copy".into());
        let (sender, _receiver) = iced::futures::channel::mpsc::channel(8);
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = Some(ScenarioOutput::new(controller.generation, sender)));
        let (_, frame) = crate::view_model::test_support::explore_presentation();
        let mut surface = crate::view_model::test_support::physical_surface(frame);
        surface.integration = true;
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
        for swatch in [false, true] {
            record_probe_draw("workflow.visual.workspace", surface, bounds, bounds, bounds);
            controller.phase = if swatch { Phase::CopyCapability } else { Phase::CopyProductWait };
            if swatch {
                controller.copy_swatch_color = [48.0, 80.0, 112.0];
                controller.copy_capability_available = true;
                assert!(controller.prepare_control_probe());
                let prepared = controller.control_probe.as_ref().unwrap();
                assert_eq!(prepared.color, controller.copy_swatch_color);
                assert!(prepared.available);
            } else {
                assert!(controller.prepare_annotation_probe(frame.content_sequence, frame.presentation_revision,
                    [frame.content_width, frame.content_height], vec![1.0; 7]));
                let prepared = controller.annotation_probe.as_ref().unwrap();
                assert_eq!(prepared.source, frame.content_sequence);
                assert_eq!(prepared.presentation, frame.presentation_revision);
                assert_eq!(prepared.extent, [frame.content_width, frame.content_height]);
                assert_eq!(prepared.pixels, vec![1.0; 7]);
            }
            controller.location_pending = true;
            let moved = Rectangle { x: 17.0, ..bounds };
            record_probe_draw("workflow.visual.workspace", surface, bounds, moved, bounds);
            assert!(!controller.prepare_annotation_probe(999, 999, [1, 1], Vec::new()), "pending location cannot be overwritten");
            assert!(!controller.prepare_control_probe());
            let phase = controller.phase.clone();
            // The obsolete location cannot validate bounds or stamp a new
            // output onto the saved old frame. Normal advance can now rearm.
            controller.update(Message::Located { control: ANNOTATION_SURFACE.into(), bounds: Rectangle::default() });
            assert_eq!(controller.phase, phase);
            assert!(!controller.location_pending);
            assert!(controller.annotation_probe.is_none() && controller.control_probe.is_none());
            assert!(controller.annotation_pixels_pending.is_none() && controller.control_probe_receipt.is_none());
        }
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
        initialize_reporting(false, false);
    }

    #[test]
    fn atlas_composition_samples_bound_all_three_grid_lines_at_required_columns_and_dpi() {
        initialize_reporting(true, true);
        PIXEL_FIXTURE_ENABLED.with(|enabled| enabled.set(true));
        for columns in [4, 10] {
            for dpi in [1.0, 1.5] {
                let mut snapshot = crate::view_model::test_support::explore_snapshot();
                snapshot.viewport.columns = columns;
                snapshot.augmentation.enabled = false;
                snapshot.overlay.showlabels = false;
                snapshot.overlay.showmasks = true;
                snapshot.overlay.showboxes = true;
                snapshot.labels.clear(); // Background row, no annotation or source padding.
                snapshot.gallery.slots = vec![true; columns as usize];
                let (_, frame) = crate::view_model::test_support::explore_presentation();
                let surface = crate::view_model::test_support::physical_surface(frame);
                let width = 800.0 * dpi;
                let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(width, width));
                let draw = AtlasDraw { surface, snapshot: std::sync::Arc::new(snapshot), bounds, image: bounds, clip: bounds };
                let samples = atlas_composition_samples(&draw).unwrap();
                assert_eq!(samples.count, ATLAS_GRID_SAMPLES * 10);
                assert_eq!(samples.card_count, 0);
                assert_eq!(samples.cards.len(), 256);
                let cell = width / columns as f32;
                // Independent raster strips at known physical positions, not
                // the shader's rounding/distance algorithm.
                let positions = [0.5, 1.5, 2.5, 3.5,
                    cell - 1.5, cell - 0.5, cell + 0.5, cell + 1.5, cell + 2.5,
                    width - 3.5, width - 2.5, width - 1.5, width - 0.5];
                let clean_indices = [3, 4, 8, 9];
                let white_indices = [1, 6, 11];
                for (index, point) in samples.points[..samples.count].chunks_exact(10).enumerate() {
                    assert_eq!(point[2], positions[index]);
                    assert_eq!(point[3], cell / 2.0);
                    let expected = if clean_indices.contains(&index) { [48.0, 80.0, 112.0] }
                        else if white_indices.contains(&index) { [255.0; 3] } else { [0.0; 3] };
                    assert_eq!(point[4..7], expected);
                    assert_eq!(point[7], 255.0);
                    assert_eq!(point[8], 4.0);
                }
            }
        }
        initialize_reporting(false, false);
    }

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
        initialize_reporting(true, true);
        let mut controller = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        );
        controller.phase = Phase::AwaitExploreReady;
        let (sender, mut receiver) = iced::futures::channel::mpsc::channel(8);
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = Some(ScenarioOutput::new(controller.generation, sender)));
        record_probe_draw(EXPLORE_GALLERY, draw.surface, draw.bounds, draw.image, draw.clip);
        atlas_probe_output(false).unwrap().try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 0),
        }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_pixels.is_none());
        atlas_probe_output(false).unwrap().try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(0, 0),
        }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_pixels.is_none());
        atlas_probe_output(false).unwrap().try_send(Message::AtlasPixels {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 1),
        }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.atlas_pixels, Some(draw.clone()));
        atlas_probe_output(true).unwrap().try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 0),
        }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_composition.is_none());
        atlas_probe_output(true).unwrap().try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(0, 0),
        }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_composition.is_none());
        atlas_probe_output(true).unwrap().try_send(Message::AtlasComposition {
            receipt: draw.clone(),
            outcome: ProbeOutcome::Observed(1, 1),
        }).unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.atlas_composition, Some(draw));
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
        initialize_reporting(false, false);
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
