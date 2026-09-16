mod annotation_checks;
mod annotation_product;
mod reporting;
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
    let disabling = !enabled && reporting_enabled();
    REPORTING_ENABLED.with(|flag| flag.set(enabled));
    if disabling {
        crate::presentation_surface::reset_reconstruction_probe();
        // Cancellation belongs to the still-running scenario, even though its
        // diagnostic receipts are about to retire. It also wakes pre-capture FPS.
        let output = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone());
        if let Some(mut output) = output {
            output.receipt = None;
            output.probe = None;
            output.send(Message::ReportingDisabled);
        }
        reset_observer();
    }
    PIXEL_FIXTURE_ENABLED.with(|flag| flag.set(enabled && pixel_fixture));
    #[cfg(target_arch = "wasm32")]
    initialize_js(enabled);
    COMPLETION_WITHOUT_INPUT.with(|flag| flag.set(false));
}

pub(crate) fn reporting_enabled() -> bool {
    REPORTING_ENABLED.with(std::cell::Cell::get)
}

pub(crate) fn notify_driver_draw(
    control: &'static str,
    source_revision: u64,
    presentation_revision: u64,
) {
    if !DRIVER_ENABLED.with(std::cell::Cell::get) {
        return;
    }
    #[cfg(target_arch = "wasm32")]
    driver_draw_js(
        control,
        source_revision as f64,
        presentation_revision as f64,
    );
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (control, source_revision, presentation_revision);
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

fn explore_scenario_preparation(
    snapshot: &crate::generated::ExploreSnapshot,
) -> Option<explore::Message> {
    use explore::{Message, dataset, detail, gallery, overlay};
    if snapshot.detail.showoriginaldimensions {
        return Some(Message::Detail(detail::Message::DetailSourceSelected(
            false,
        )));
    }
    if snapshot.augmentation.enabled {
        return Some(Message::Gallery(gallery::Message::AugmentationToggled(
            false,
        )));
    }
    if snapshot.filter.order != crate::generated::ExploreOrder::Sequential {
        return Some(Message::Dataset(dataset::Message::OrderSelected(
            crate::generated::ExploreOrder::Sequential,
        )));
    }
    if snapshot.filter.minimumcompiledindex != 0 {
        return Some(Message::Dataset(
            dataset::Message::MinimumCompiledIndexChanged(0),
        ));
    }
    if snapshot.filter.classselection.mode != crate::generated::ExploreClassSelectionMode::All {
        return Some(Message::Dataset(dataset::Message::AllClasses));
    }
    if snapshot.overlay.classselection.mode != crate::generated::ExploreClassSelectionMode::All {
        return Some(Message::Details(explore::details::Message::AllClasses));
    }
    let visibility = if !snapshot.overlay.showlabels {
        overlay::Message::LabelsToggled(true)
    } else if !snapshot.overlay.showmasks {
        overlay::Message::MasksToggled(true)
    } else if !snapshot.overlay.showboxes {
        overlay::Message::BoxesToggled(true)
    } else {
        return None;
    };
    Some(Message::Gallery(gallery::Message::Overlay(visibility)))
}

pub(crate) fn report_viewer_label(
    category: u16,
    color: iced::Color,
    catalog_count: usize,
    overlay: &crate::generated::ExploreOverlay,
    frame: crate::presentation_surface::FrameReady,
    detail: bool,
) {
    reporting::emit(|sink| {
        sink.record(
            "integration.viewer_label_rgb",
            explore::DETAIL_LABELS_ID,
            "actual-iced-label",
            [
                f64::from(category),
                f64::from(color.r),
                f64::from(color.g),
                f64::from(color.b),
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.viewer_label_catalog",
            explore::DETAIL_LABELS_ID,
            "full-native-catalog",
            [
                f64::from(category),
                catalog_count as f64,
                f64::from(u8::from(overlay.showboxes)),
                f64::from(u8::from(overlay.showmasks)),
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
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
        )
    });
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

// Fixture demand windows are disjoint and separated beyond the native four
// neighbour rows. This does not change the producer's admission/cache policy.
const COLD_GALLERY_ROW_GAP: u32 = 5;

fn cold_gallery_scroll_offset(
    row: u32,
    viewport: &crate::generated::ExploreViewport,
    matching: u32,
    row_extent: f32,
) -> Option<f32> {
    if !row_extent.is_finite() || row_extent <= 0.0 || row == 0 || viewport.columns == 0 {
        return None;
    }
    let total_rows = matching.div_ceil(viewport.columns);
    let sweep_rows = (768.0 / row_extent).ceil() as u32;
    if row
        .saturating_add(viewport.rowcount)
        .saturating_add(sweep_rows)
        .saturating_add(2)
        >= total_rows
    {
        return None;
    }
    // A fractional row avoids floating rounding selecting the preceding row.
    let offset = row_extent * (row as f32 + 0.25);
    offset.is_finite().then_some(offset)
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
const WORKSPACE_FPS_PIXEL_FAILURE: &str =
    "Workspace FPS canvas capture did not contain its upper-right counter background and text";
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
    WorkspaceFpsDrawn(reporting::FpsEvidence),
    WorkspaceFpsPixels(FpsPixelOutcome),
    ReportingDisabled,
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
    NumberInvalidDelivered,
    NumberClipboardPrepared {
        revision: u64,
        result: Result<(), iced::clipboard::Error>,
    },
    NumberPasteDelivered(bool),
    NumberPasteRead {
        target: String,
        result: Result<std::sync::Arc<iced::clipboard::Content>, iced::clipboard::Error>,
    },
    GalleryMouseDelivered,
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

#[derive(Debug, Clone)]
pub enum FpsPixelOutcome {
    Invalidated,
    Cancelled,
    Captured(reporting::FpsPixels),
    Failed,
}

impl reporting::FpsEvidence {
    #[cfg(target_arch = "wasm32")]
    fn canvas_values(self) -> [f64; 11] {
        [
            f64::from(self.bounds.x),
            f64::from(self.bounds.y),
            f64::from(self.bounds.width),
            f64::from(self.bounds.height),
            f64::from(self.clip.x),
            f64::from(self.clip.y),
            f64::from(self.clip.width),
            f64::from(self.clip.height),
            self.frames as f64,
            self.seconds,
            f64::from(u8::from(self.dark)),
        ]
    }
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
        let [Some(first), Some(second)] = values else {
            return Self::Failed;
        };
        if ![first, second].into_iter().all(|value| {
            value.is_finite()
                && value >= 0.0
                && value <= f64::from(u32::MAX)
                && value.fract() == 0.0
        }) {
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

impl ControlProbe {
    fn sample(self, bounds: Rectangle, control: &str, capability: bool) {
        #[cfg(not(target_arch = "wasm32"))]
        let _ = (self, bounds, control, capability);
        #[cfg(target_arch = "wasm32")]
        {
            let mut output = self.output;
            let canvas_probe = output.canvas_probe.clone();
            let callback = pixel_result_callback(move |outcome| {
                let message = if capability {
                    Message::AnnotationControlPixels { outcome }
                } else {
                    Message::AnnotationPixels {
                        revision: 0,
                        outcome,
                    }
                };
                let _ = output.try_send(message);
            });
            let detail = if capability {
                if self.available {
                    "enabled"
                } else {
                    "disabled"
                }
            } else {
                "native-hsv-completed-canvas"
            };
            annotation_swatch_js(
                &canvas_probe,
                &[
                    f64::from(bounds.x),
                    f64::from(bounds.y),
                    f64::from(bounds.width),
                    f64::from(bounds.height),
                ],
                &self.color,
                control,
                detail,
                capability,
                &callback,
            );
        }
    }
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
            Message::ProbeCompleted {
                owner: owner.clone(),
                message: Box::new(message),
            }
        } else {
            message
        };
        self.sender.try_send(Message::Scoped {
            generation: self.generation,
            receipt: self.receipt.clone(),
            message: Box::new(message),
        })
    }
    fn send(&mut self, message: Message) {
        if let Err(error) = self.try_send(message) {
            if !error.is_full() {
                return;
            }
            // A full observation channel cannot discard its terminal handoff.
            let message = error.into_inner();
            let mut sender = self.sender.clone();
            use iced::Executor;
            iced::executor::Default::new()
                .expect("integration completion executor")
                .spawn(async move {
                    use iced::futures::SinkExt;
                    let _ = sender.send(message).await;
                });
        }
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
    fps_sample: Option<iced::time::Instant>,
    fps_draw: Option<reporting::FpsEvidence>,
}

pub(crate) fn report_workspace_fps(
    control: &'static str,
    meter: &crate::workspace_fps::Meter,
    bounds: Rectangle,
    clip: Rectangle,
    dark: bool,
) {
    if !reporting_enabled() || meter.frames == 0 || meter.seconds < 0.5 {
        return;
    }
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let evidence = reporting::FpsEvidence {
            bounds,
            clip,
            dark,
            frames: meter.frames,
            seconds: meter.seconds,
        };
        #[cfg(target_arch = "wasm32")]
        fps_draw_js(control, &evidence.canvas_values());
        let unchanged = if control == EXPLORE_GALLERY {
            let unchanged = observer.fps_draw == Some(evidence);
            observer.fps_draw = Some(evidence);
            unchanged
        } else {
            observer.fps_draw = None;
            true
        };
        if unchanged && observer.fps_sample == Some(meter.sample_time()) {
            return;
        }
        if let Some(output) = observer.output_for(control)
            && output
                .try_send(Message::WorkspaceFpsDrawn(evidence))
                .is_ok()
        {
            observer.fps_sample = Some(meter.sample_time());
            reporting::workspace_fps(control, meter, evidence);
        }
    });
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
            {
                output.canvas_probe = wasm_bindgen::JsValue::UNDEFINED;
            }
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
    let mut output = SURFACE_DRAW_OBSERVER
        .with(|observer| observer.borrow_mut().output_for(control).cloned())?;
    output.probe = Some(std::sync::Arc::new(()));
    #[cfg(target_arch = "wasm32")]
    {
        output.canvas_probe = capture_probe_js(control);
    }
    Some(output)
}

#[cfg(any(target_arch = "wasm32", test))]
fn atlas_probe_output(composition: bool) -> Option<ScenarioOutput> {
    let output = probe_output(EXPLORE_GALLERY)?;
    SURFACE_DRAW_OBSERVER.with(|observer| {
        let mut observer = observer.borrow_mut();
        let pending = if composition {
            &mut observer.atlas_composition_owner
        } else {
            &mut observer.atlas_pixels_owner
        };
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
    if !reporting_enabled() {
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
            receipt_js(
                control,
                &format!("{receipt:?}"),
                frame.content_sequence as f64,
                frame.presentation_revision as f64,
            );
        }
        if observer.receipts.get(control) != Some(&receipt) {
            match control {
                EXPLORE_GALLERY => {
                    observer.gallery = None;
                    observer.atlas = None;
                }
                explore::DETAIL_WORKSPACE_ID => observer.viewer = None,
                crate::view::workspace::STABLE_ID => observer.identity = (0, 0),
                _ => {}
            }
            observer.receipts.insert(control, receipt);
        }
    });
}

#[derive(Debug, Clone, Copy, Default)]
struct ControlBounds {
    target: Rectangle,
    page: Rectangle,
    horizontal: Rectangle,
}

struct FindControl {
    target: Id,
    translation: Vector,
    pending_translation: Vector,
    bounds: Option<Rectangle>,
    page: Rectangle,
    horizontal: Rectangle,
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

impl Operation<ControlBounds> for FindControl {
    fn traverse(&mut self, operate: &mut dyn FnMut(&mut dyn Operation<ControlBounds>)) {
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
        let visible = Rectangle {
            x: bounds.x - self.translation.x,
            y: bounds.y - self.translation.y,
            ..bounds
        };
        if id == Some(&Id::from(crate::view::PAGE_SCROLL_ID)) {
            self.page = visible;
        } else if id == Some(&Id::from(crate::view::HORIZONTAL_SCROLL_ID)) {
            self.horizontal = visible;
        }
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

    fn finish(&self) -> Outcome<ControlBounds> {
        Outcome::Some(ControlBounds {
            target: self.bounds.unwrap_or_default(),
            page: self.page,
            horizontal: self.horizontal,
        })
    }
}

fn measure_control(control: String) -> Task<ControlBounds> {
    widget::operate(FindControl {
        target: Id::from(control),
        translation: Vector::ZERO,
        pending_translation: Vector::ZERO,
        bounds: None,
        page: Rectangle::default(),
        horizontal: Rectangle::default(),
    })
}

fn locate(control: String, generation: u64) -> Task<RootMessage> {
    measure_control(control.clone()).map(move |bounds| {
        RootMessage::Integration(Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::Located {
                control: control.clone(),
                bounds: bounds.target,
            }),
        })
    })
}

fn reveal_axis(start: f32, size: f32, viewport_start: f32, viewport_size: f32) -> f32 {
    if viewport_size <= 0.0 || size <= 0.0 {
        return 0.0;
    }
    // Scrollable rounds its translation to whole logical pixels. Leave room
    // for that rounding while still requiring the entire target to be visible.
    let inset = ((viewport_size - size) * 0.5).clamp(0.0, 1.0);
    let viewport_start = viewport_start + inset;
    let viewport_size = viewport_size - inset * 2.0;
    if start < viewport_start || size > viewport_size {
        start - viewport_start
    } else {
        (start + size - viewport_start - viewport_size).max(0.0)
    }
}

#[derive(Debug, Clone, Copy)]
enum AnnotationReveal {
    Control,
    Tail {
        count: usize,
        narrow: bool,
    },
    Geometry,
    Source {
        extent: [f32; 2],
        region: Rectangle,
        margin: f32,
    },
}

fn contains_rectangle(outer: Rectangle, inner: Rectangle) -> bool {
    outer.width > 0.0
        && outer.height > 0.0
        && inner.width > 0.0
        && inner.height > 0.0
        && inner.x >= outer.x - 0.01
        && inner.y >= outer.y - 0.01
        && inner.x + inner.width <= outer.x + outer.width + 0.01
        && inner.y + inner.height <= outer.y + outer.height + 0.01
}

impl ControlBounds {
    fn visible(self) -> Option<Rectangle> {
        self.target
            .intersection(&self.page)?
            .intersection(&self.horizontal)
            .filter(|bounds| bounds.width > 0.0 && bounds.height > 0.0)
    }

    fn requested(self, reveal: AnnotationReveal) -> Option<Rectangle> {
        if self.page.width <= 0.0
            || self.page.height <= 0.0
            || self.horizontal.width <= 0.0
            || self.horizontal.height <= 0.0
            || self.target.width <= 0.0
            || self.target.height <= 0.0
        {
            return None;
        }
        Some(match reveal {
            AnnotationReveal::Control | AnnotationReveal::Tail { .. } => self.target,
            // Metadata-only inspection has no click or pixel sample.
            AnnotationReveal::Geometry => Rectangle {
                width: 1.0,
                height: 1.0,
                ..self.target
            },
            AnnotationReveal::Source {
                extent,
                region,
                margin,
            } => {
                if extent[0] <= 0.0 || extent[1] <= 0.0 {
                    return None;
                }
                let scale = (self.target.width / extent[0]).min(self.target.height / extent[1]);
                Rectangle {
                    x: self.target.x
                        + (self.target.width - extent[0] * scale) * 0.5
                        + region.x * scale
                        - margin,
                    y: self.target.y
                        + (self.target.height - extent[1] * scale) * 0.5
                        + region.y * scale
                        - margin,
                    width: region.width * scale + 2.0 * margin,
                    height: region.height * scale + 2.0 * margin,
                }
            }
        })
    }
}

fn scroll_control_into_view(control: String, reveal: AnnotationReveal) -> Task<RootMessage> {
    measure_control(control).then(move |bounds| {
        let Some(target) = bounds.requested(reveal) else {
            return Task::none();
        };
        let x = reveal_axis(
            target.x,
            target.width,
            bounds.horizontal.x,
            bounds.horizontal.width,
        );
        let y = reveal_axis(target.y, target.height, bounds.page.y, bounds.page.height);
        iced::widget::operation::scroll_by(
            crate::view::PAGE_SCROLL_ID,
            AbsoluteOffset { x: 0.0, y },
        )
        .chain(iced::widget::operation::scroll_by(
            crate::view::HORIZONTAL_SCROLL_ID,
            AbsoluteOffset { x, y: 0.0 },
        ))
    })
}

fn reveal_control(control: String, generation: u64, reveal: AnnotationReveal) -> Task<RootMessage> {
    measure_control(control.clone()).then(move |before| {
        let control = control.clone();
        scroll_control_into_view(control.clone(), reveal).chain(
            measure_control(control.clone()).map(move |bounds| {
                let viewport = bounds.page.intersection(&bounds.horizontal);
                let verified = bounds.visible().is_some()
                    && viewport.zip(bounds.requested(reveal)).is_some_and(
                        |(viewport, requested)| contains_rectangle(viewport, requested),
                    );
                let tail_verified = if let AnnotationReveal::Tail { count, narrow } = reveal {
                    let before_viewport = before.page.intersection(&before.horizontal);
                    let offscreen_gap = before_viewport.map_or(-1.0, |viewport| {
                        before.target.y - viewport.y - viewport.height
                    });
                    let valid = count >= 32 && offscreen_gap > 0.0 && verified;
                    if let Some(viewport) = viewport.filter(|_| valid) {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.annotation_tail",
                                &control,
                                if narrow { "narrow" } else { "wide" },
                                [
                                    count as f64,
                                    f64::from(offscreen_gap),
                                    f64::from(bounds.target.y - viewport.y),
                                    f64::from(
                                        viewport.y + viewport.height
                                            - bounds.target.y
                                            - bounds.target.height,
                                    ),
                                ],
                            )
                        });
                    }
                    valid
                } else {
                    true
                };
                if !verified || !tail_verified {
                    reporting::emit(|sink| {
                        for (detail, rectangle) in [
                            ("before-target", before.target),
                            ("before-page", before.page),
                            ("before-horizontal", before.horizontal),
                            ("after-target", bounds.target),
                            ("after-page", bounds.page),
                            ("after-horizontal", bounds.horizontal),
                        ] {
                            sink.record(
                                "integration.reveal_bounds",
                                &control,
                                detail,
                                [
                                    f64::from(rectangle.x),
                                    f64::from(rectangle.y),
                                    f64::from(rectangle.width),
                                    f64::from(rectangle.height),
                                ],
                            );
                        }
                    });
                }
                RootMessage::Integration(Message::Scoped {
                    generation,
                    receipt: None,
                    message: Box::new(Message::Located {
                        control: control.clone(),
                        // Source conversion always retains full surface geometry.
                        bounds: if verified && tail_verified {
                            bounds.target
                        } else {
                            Rectangle::default()
                        },
                    }),
                })
            }),
        )
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
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFpsDraw)]
    fn fps_draw_js(control: &str, values: &[f64]);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFpsCurrent)]
    fn fps_current_js(receipt: &wasm_bindgen::JsValue, values: &[f64]) -> bool;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFpsPixels)]
    fn fps_pixels_js(
        receipt: &wasm_bindgen::JsValue,
        values: &[f64],
        scale: f64,
        completed: &wasm_bindgen::JsValue,
    );
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
        cards: &[u32],
        fields: &str,
        source_revision: f64,
        presentation_revision: f64,
        completed: &wasm_bindgen::JsValue,
    );
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFullscreen)]
    fn fullscreen_js(enabled: bool);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationCanvasSize)]
    fn canvas_size_js(width: f64, height: f64) -> bool;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationCanvasSizeSettled)]
    fn canvas_size_settled_js(width: f64, height: f64) -> bool;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationRestoreCanvasSize)]
    fn restore_canvas_size_js();
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationFullscreenSettled)]
    fn fullscreen_settled_js(enabled: bool) -> bool;

    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationUpscalePixels)]
    fn upscale_pixels_js(
        receipt: &wasm_bindgen::JsValue,
        image_pixels: &[f32],
        button_css: &[f32],
        source: f64,
        presentation: f64,
        completed: &wasm_bindgen::JsValue,
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
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationHoverAfterSurfaceDraw)]
    fn hover_after_surface_draw_js(x: f64, y: f64, control: &str, source_revision: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationSweep)]
    fn sweep_js(x: f64, y: f64, width: f64, height: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationWheel)]
    fn wheel_js(x: f64, y: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationSliderDrag)]
    fn slider_drag_js(x: f64, y: f64, width: f64, height: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationReplaceNumber)]
    fn replace_number_js(x: f64, y: f64, value: &str, selection_length: u32) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationPasteNumber)]
    fn paste_number_js(x: f64, y: f64, completed: &wasm_bindgen::JsValue) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationCancelNumberEdit)]
    fn cancel_number_edit_js();
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
    if snapshot.gallery.layout.cardextent == 0
        || snapshot.gallery.layout.rowcount == 0
        || snapshot.gallery.layout.columns != columns
    {
        return None;
    }
    let side = snapshot.gallery.layout.cardextent as f32;
    let logical_height = side * snapshot.gallery.layout.rowcount as f32;
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
                + (slot / columns as usize) as f32 * side * draw.image.height / logical_height,
        );
        let card_end = iced::Point::new(
            card_origin.x + side * draw.image.width / frame.content_width as f32 - 1.0,
            card_origin.y + side * draw.image.height / logical_height - 1.0,
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
        for (kind, (relative_x, relative_y)) in [(0.2, 0.7), (0.5, 0.5), (0.0, 0.7), (-0.25, 0.7)]
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
                draw.image.y + y * draw.image.height / logical_height,
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
            let texel_y =
                (screen.y.floor() + 0.5 - draw.image.y) * logical_height / draw.image.height - 0.5;
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
    // Pick visible cell interiors so crossing grid lines cannot mask a defect.
    // The vertical scrollbar covers the right image edge. Sample the top
    // horizontal outer edge over the fixture's second, unpadded background
    // card, using the same three-pixel raster strip.
    let cell = draw.image.width / columns as f32;
    let first_row = ((draw.clip.y - draw.image.y) / cell - 0.5).ceil().max(0.0);
    let y = draw.image.y + (first_row + 0.5) * cell;
    let centers = [
        draw.image.x + 1.5,
        draw.image.x + cell + 0.5,
        draw.image.y + 1.5,
    ];
    for (edge, center) in centers.into_iter().enumerate() {
        let offsets: &[f32] = if edge == 1 {
            &[-2.0, -1.0, 0.0, 1.0, 2.0]
        } else {
            &[-1.0, 0.0, 1.0, 2.0]
        };
        for &offset in offsets {
            let point = if edge == 2 {
                iced::Point::new(draw.image.x + cell * 1.5, center + offset)
            } else {
                iced::Point::new(center + offset, y)
            };
            if !draw.clip.contains(point) || !draw.image.contains(point) {
                return None;
            }
            let color = if offset.abs() == 2.0 {
                [48.0, 80.0, 112.0]
            } else if offset == 0.0 {
                [255.0; 3]
            } else {
                [0.0; 3]
            };
            points[count..count + 10].copy_from_slice(&[
                point.x - draw.image.x,
                point.y - draw.image.y,
                point.x,
                point.y,
                color[0],
                color[1],
                color[2],
                255.0,
                4.0,
                0.0,
            ]);
            count += 10;
        }
    }
    Some(AtlasCompositionSamples {
        points,
        count,
        cards,
        card_count,
    })
}

fn sample_atlas_composition(draw: &AtlasDraw) {
    #[cfg(target_arch = "wasm32")]
    {
        if !crate::presentation_surface::pixel_trace::enabled() {
            return;
        }
        let Some(samples) = atlas_composition_samples(draw) else {
            return;
        };
        let frame = draw.surface.frame.expect("sampleable atlas composition");
        let Some(mut output) = atlas_probe_output(true) else {
            return;
        };
        let receipt = draw.clone();
        let canvas_probe = output.canvas_probe.clone();
        let completed = pixel_result_callback(move |outcome| {
            let _ = output.try_send(Message::AtlasComposition { receipt, outcome });
        });
        atlas_composition_js(
            &canvas_probe,
            &samples.points[..samples.count],
            &samples.cards[..samples.card_count],
            &format!(
                "{{{}{},\"image_x\":{},\"image_y\":{},\"image_width\":{},\"image_height\":{}}}",
                crate::presentation_surface::surface_trace_fields(draw.surface, draw.surface),
                crate::presentation_surface::gallery_trace_fields(Some(&draw.snapshot)),
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
        move |status: wasm_bindgen::JsValue,
              first: wasm_bindgen::JsValue,
              second: wasm_bindgen::JsValue| {
            if SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation) != generation
                || !reporting_enabled()
            {
                return;
            }
            let status = status.as_string();
            let outcome =
                ProbeOutcome::decode(status.as_deref(), [first.as_f64(), second.as_f64()]);
            if outcome == ProbeOutcome::Failed && status.as_deref() != Some("failed") {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.failure",
                        "",
                        "pixel callback returned invalid counts",
                        [0.0; 4],
                    )
                });
            }
            completed(outcome);
        },
    )
}

#[cfg(target_arch = "wasm32")]
fn sample_workspace_fps(mut output: ScenarioOutput, evidence: reporting::FpsEvidence, scale: f32) {
    use wasm_bindgen::JsCast;
    let receipt = output.canvas_probe.clone();
    let completed = wasm_bindgen::closure::Closure::once_into_js(
        move |status: wasm_bindgen::JsValue,
              extent: wasm_bindgen::JsValue,
              pixels: wasm_bindgen::JsValue| {
            // Reset/disable still settles the once callback. It never copies stale bytes.
            let current = reporting_enabled()
                && SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation)
                    == output.generation;
            let status = status.as_string();
            let outcome = if !current {
                FpsPixelOutcome::Cancelled
            } else if status.as_deref() == Some("invalidated")
                && extent.as_f64() == Some(0.0)
                && pixels.as_f64() == Some(0.0)
            {
                FpsPixelOutcome::Invalidated
            } else if status.as_deref() == Some("observed")
                && js_sys::Array::is_array(&extent)
                && pixels.is_instance_of::<js_sys::Uint8ClampedArray>()
            {
                let extent = js_sys::Array::from(&extent);
                let pixels = pixels.unchecked_into::<js_sys::Uint8ClampedArray>();
                match (extent.get(0).as_f64(), extent.get(1).as_f64()) {
                    (Some(width), Some(height))
                        if extent.length() == 2
                            && [width, height].into_iter().all(|v| {
                                v.is_finite()
                                    && v > 0.0
                                    && v <= f64::from(u32::MAX)
                                    && v.fract() == 0.0
                            })
                            && reporting::FpsPixels::valid_extent(
                                width as u32,
                                height as u32,
                                pixels.length() as usize,
                            ) =>
                    {
                        FpsPixelOutcome::Captured(reporting::FpsPixels {
                            width: width as u32,
                            height: height as u32,
                            rgba: pixels.to_vec(),
                        })
                    }
                    _ => FpsPixelOutcome::Failed,
                }
            } else {
                FpsPixelOutcome::Failed
            };
            output.send(Message::WorkspaceFpsPixels(outcome));
        },
    );
    fps_pixels_js(
        &receipt,
        &evidence.canvas_values(),
        f64::from(scale),
        &completed,
    );
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
        let _ = output.try_send(Message::UpscalePixels {
            source,
            presentation,
            outcome,
        });
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
fn sample_upscale_pixels(
    _output: ScenarioOutput,
    _image: Rectangle,
    _button: Rectangle,
    _source: u64,
    _presentation: u64,
) {
}

pub(crate) fn report_snapshot_conflict(family: &str, revision: u64, fields: &str) {
    reporting::emit(|sink| {
        sink.record(
            "integration.snapshot_conflict",
            family,
            fields,
            [revision as f64, 0.0, 0.0, 0.0],
        )
    });
}

#[derive(Debug, Clone, PartialEq)]
pub struct AtlasDraw {
    pub surface: crate::presentation_surface::Surface,
    pub snapshot: std::sync::Arc<crate::generated::ExploreImageMetadata>,
    pub bounds: Rectangle,
    pub image: Rectangle,
    pub clip: Rectangle,
}

impl AtlasDraw {
    fn visible_slot(&self, compiled_index: u32) -> Option<usize> {
        self.snapshot
            .order
            .visibleindices
            .iter()
            .position(|value| *value == compiled_index)
    }
}

#[cfg(target_arch = "wasm32")]
fn atlas_resize_dimensions(step: u8) -> (f64, f64) {
    [
        (1200.0, 850.0),
        (1000.0, 1020.0),
        (1300.0, 760.0),
        (1500.0, 600.0),
    ][step as usize]
}

fn atlas_scroll_window(size: iced::Size, columns: u32) -> (u32, f32) {
    let visible = size.height / (size.width / columns.max(1) as f32);
    let rows = visible.ceil().max(1.0) as u32;
    // Cross the next row boundary with margin on either side, using the
    // widget's actual logical size rather than rounded GPU clip coordinates.
    (rows, (rows as f32 - visible + 1.0) * 0.5)
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
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_geometry",
            EXPLORE_GALLERY,
            "uniform-square",
            [
                frame.presentation_revision as f64,
                frame.content_sequence as f64,
                (image.width / snapshot.viewport.columns.max(1) as f32) as f64,
                (image.height / snapshot.viewport.rowcount.max(1) as f32) as f64,
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_scale",
            EXPLORE_GALLERY,
            "source-to-screen",
            [
                frame.presentation_revision as f64,
                frame.content_sequence as f64,
                (image.width / frame.content_width.max(1) as f32) as f64,
                (image.height
                    / (snapshot.gallery.layout.cardextent.max(1) as f32
                        * snapshot.viewport.rowcount.max(1) as f32)) as f64,
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.atlas_clip",
            EXPLORE_GALLERY,
            "scrollable-clip",
            [
                frame.presentation_revision as f64,
                frame.content_sequence as f64,
                (clip.y - image.y) as f64,
                (image.y + image.height - clip.y - clip.height) as f64,
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
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
        )
    });
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
        && (pixel_fixture_enabled() || snapshot.gallery.slots.iter().all(|ready| *ready))
    {
        sample_atlas_pixels(draw);
    }
}

#[cfg(target_arch = "wasm32")]
fn sample_atlas_pixels(draw: AtlasDraw) {
    let rectangles = atlas_pixel_rectangles(&draw);
    let frame = draw.surface.frame.expect("drawn atlas publication");
    let columns = draw.snapshot.viewport.columns.max(1);
    let side = draw.image.width / columns as f32;
    let cards: Vec<u32> = rectangles
        .chunks_exact(4)
        .map(|rect| {
            let column = ((rect[0] + rect[2] * 0.5 - draw.image.x) / side) as usize;
            let row = ((rect[1] + rect[3] * 0.5 - draw.image.y) / side) as usize;
            draw.snapshot.order.visibleindices[row * columns as usize + column]
        })
        .collect();
    let fields = format!(
        "{{{}{},\"image\":[{},{},{},{}],\"overlay_boxes\":{},\"overlay_masks\":{},\"overlay_labels\":{}}}",
        crate::presentation_surface::surface_trace_fields(draw.surface, draw.surface),
        crate::presentation_surface::gallery_trace_fields(Some(&draw.snapshot)),
        draw.image.x,
        draw.image.y,
        draw.image.width,
        draw.image.height,
        draw.snapshot.overlay.showboxes,
        draw.snapshot.overlay.showmasks,
        draw.snapshot.overlay.showlabels
    );
    let Some(mut output) = atlas_probe_output(false) else {
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
        &cards,
        &fields,
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
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_draw",
            control,
            if redraw { "redraw" } else { "draw" },
            [
                revision as f64,
                source_revision as f64,
                content_width as f64,
                content_height as f64,
            ],
        )
    });
    if control == explore::DETAIL_WORKSPACE_ID {
        reporting::emit(|sink| {
            sink.record(
                "integration.viewer_sample",
                control,
                "actual-draw",
                [
                    viewer.crop[2] as f64,
                    viewer.crop[3] as f64,
                    viewer.image.width as f64,
                    viewer.image.height as f64,
                ],
            )
        });
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
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_draw_ordinal",
            control,
            if redraw { "redraw" } else { "draw" },
            [
                revision as f64,
                source_revision as f64,
                count as f64,
                if redraw { 1.0 } else { 0.0 },
            ],
        )
    });
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

pub(crate) fn report_surface_geometry(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    geometry: Rectangle,
) {
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_geometry",
            control,
            "shader-viewport",
            [
                revision as f64,
                source_revision as f64,
                f64::from(geometry.width),
                f64::from(geometry.height),
            ],
        )
    });
}

pub(crate) fn report_surface_container(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    bounds: Rectangle,
    content_width: u32,
    content_height: u32,
) {
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_container",
            control,
            "rendered-contain-container",
            [
                revision as f64,
                source_revision as f64,
                f64::from(bounds.width),
                f64::from(bounds.height),
            ],
        )
    });
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_content",
            control,
            "exported-native-frame",
            [
                revision as f64,
                source_revision as f64,
                content_width as f64,
                content_height as f64,
            ],
        )
    });
}

pub(crate) fn report_surface_scale(
    control: &'static str,
    revision: u64,
    source_revision: u64,
    scale: f32,
) {
    reporting::emit(|sink| {
        sink.record(
            "integration.surface_scale",
            control,
            "physical-to-logical",
            [
                revision as f64,
                source_revision as f64,
                f64::from(scale),
                f64::from(scale),
            ],
        )
    });
}

pub(crate) fn report_workspace_mouse(mouse: &crate::generated::WorkspaceMouse) {
    reporting::emit(|sink| {
        if mouse.source != crate::generated::PresentationSourceKind::Explore {
            return;
        }
        let Some(point) = &mouse.point else {
            return;
        };
        sink.record(
            "integration.explore_mouse",
            EXPLORE_GALLERY,
            "shared-input-admitted",
            [
                f64::from(point.x),
                f64::from(point.y),
                mouse.kind as u8 as f64,
                mouse.button as u8 as f64,
            ],
        );
    });
}

pub(crate) fn report_gallery_scroll(detail: &str, values: impl FnOnce() -> [f64; 4]) {
    reporting::emit(|sink| {
        sink.record(
            "integration.gallery_scroll",
            EXPLORE_GALLERY,
            detail,
            values(),
        )
    });
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
    frame: Option<crate::presentation_surface::FrameReady>,
    source: crate::generated::PresentationSourceKind,
    source_revision: u64,
) -> Option<SampleablePresentation> {
    let frame = frame?;
    if let Some(crate::presentation_surface::ExploreDisplay::Detail(retained, content)) =
        crate::presentation_surface::explore_display(None)
        && retained.frame == Some(frame)
        && ((source == crate::generated::PresentationSourceKind::Explore
            && content.input_frame().revision == source_revision)
            || (content.frame().source.kind == source
                && content.frame().revision == source_revision))
    {
        return Some(SampleablePresentation {
            source_revision: content.frame().revision,
            presentation_revision: frame.presentation_revision,
            content_width: frame.content_width,
            content_height: frame.content_height,
            capability_width: retained.width,
            capability_height: retained.height,
        });
    }
    let product = crate::presentation_surface::metadata::product(frame)?;
    let surface = crate::presentation_surface::metadata::surface(frame)?;
    (product.source.kind == source && product.revision == source_revision).then_some(
        SampleablePresentation {
            source_revision,
            presentation_revision: frame.presentation_revision,
            content_width: frame.content_width,
            content_height: frame.content_height,
            capability_width: surface.width,
            capability_height: surface.height,
        },
    )
}

fn displayed_detail(
    surface: Option<crate::presentation_surface::Surface>,
    snapshot: &crate::generated::ExploreSnapshot,
) -> Option<crate::presentation_surface::Surface> {
    let crate::presentation_surface::ExploreDisplay::Detail(shown, content) =
        crate::presentation_surface::explore_display(surface)?
    else {
        return None;
    };
    (content.viewer_identity()
        == snapshot
            .selectedimage
            .map(|image| (snapshot.dataset.identity, image))
        && content.input_frame() == &snapshot.frame)
        .then_some(shown)
}

fn fully_drawn_gallery(
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

#[cfg(target_arch = "wasm32")]
fn paste_number_input(bounds: Rectangle) -> bool {
    let Some(mut output) = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.clone())
    else {
        return false;
    };
    let completed = wasm_bindgen::closure::Closure::once_into_js(move |delivered: bool| {
        output.send(Message::NumberPasteDelivered(delivered));
    });
    paste_number_js(
        f64::from(bounds.x + bounds.width * 0.5),
        f64::from(bounds.y + bounds.height * 0.5),
        &completed,
    ) == 1
}

#[cfg(not(target_arch = "wasm32"))]
fn paste_number_input(_bounds: Rectangle) -> bool {
    false
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CopyScaleStage {
    Wide,
    Narrow,
    Restore,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Phase {
    AwaitWorkspaceFps,
    AwaitWorkspaceFpsPixels,
    RestoreWorkspaceFps,
    AwaitWorkspaceFpsRestored,
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
    ExploreCloseDetail,
    AwaitExploreGallery,
    AwaitExplorePreparation(u64),
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
    ExploreNumericStart(u8),
    ExploreNumericControl {
        index: u8,
        step: u8,
    },
    ExploreNumericReveal {
        index: u8,
        step: u8,
    },
    AwaitExploreNumeric {
        index: u8,
        step: u8,
    },
    AwaitExploreNumericWheel(u8),
    AwaitExploreNumericInvalid(u8),
    AwaitExploreClipboard(u64),
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
    GalleryColdRead(u32),
    AwaitGalleryColdRead(u32, u64),
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
    AwaitAtlasScroll(u8),
    AtlasResizeStart,
    AwaitAtlasResizeGallery(u8),
    AtlasResizeSelect(u8),
    AwaitAtlasResizeDetail(u8),
    AwaitAtlasResizeMeasurement(u8),
    AwaitAtlasReturnReady,
    AtlasReturnSelect,
    AtlasAwaySelect(u8),
    AwaitAtlasAwayDetail(u8, u64),
    AwaitAtlasAwayFilter(u8, u64),
    AwaitAtlasAwayReturn,
    AwaitAtlasAwayRestore,
    AwaitAtlasReturnDetail,
    AwaitAtlasOscillation(u8),
    AwaitVisibleReadArm(u32),
    VisibleReadScroll(u32),
    AwaitVisibleRead(u32),
    AwaitVisibleReadPixels(u32, u64),
    AwaitVisibleReadHover(u32, u64),
    VisibleReadSelect(u32, u64),
    AwaitVisibleReadSelection(u32, u64),
    AwaitVisibleReadReturn(u32, u64),
    AwaitVisibleReadOscillation(u32, u64, u8),
    VisibleReadRelease(u32, u64),
    AwaitVisibleReadComplete(u32),
    AwaitCapacitySlots(u64),
    AwaitCapacityArm,
    CapacityPublish,
    AwaitCapacityCompletion,
    AwaitCapacityRetry,
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
    CopyListSetup {
        stage: u8,
        revision: u64,
    },
    CopySwatchWait,
    CopyCapability,
    CopyCapabilityWait,
    CopyAwaitScale(CopyScaleStage),
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
            | Self::AwaitExploreNumeric { .. }
            | Self::AwaitExploreClipboard(_)
            | Self::AwaitExploreNumericInvalid(_)
            | Self::AwaitExploreNumericWheel(_)
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
            | Self::GalleryColdRead(_)
            | Self::AwaitGalleryColdRead(_, _)
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
            | Self::AwaitAtlasScroll(_)
            | Self::AwaitAtlasResizeGallery(_)
            | Self::AwaitAtlasResizeDetail(_)
            | Self::AwaitAtlasResizeMeasurement(_)
            | Self::AwaitAtlasReturnReady
            | Self::AwaitAtlasAwayDetail(_, _)
            | Self::AwaitAtlasAwayFilter(_, _)
            | Self::AwaitAtlasAwayReturn
            | Self::AwaitAtlasAwayRestore
            | Self::AwaitAtlasReturnDetail
            | Self::AwaitAtlasOscillation(_)
            | Self::AwaitVisibleReadArm(_)
            | Self::AwaitVisibleRead(_)
            | Self::AwaitVisibleReadPixels(_, _)
            | Self::AwaitVisibleReadHover(_, _)
            | Self::VisibleReadSelect(_, _)
            | Self::AwaitVisibleReadSelection(_, _)
            | Self::AwaitVisibleReadOscillation(_, _, _)
            | Self::AwaitVisibleReadReturn(_, _)
            | Self::AwaitVisibleReadComplete(_)
            | Self::AwaitCapacitySlots(_)
            | Self::AwaitCapacityArm
            | Self::AwaitCapacityCompletion
            | Self::AwaitCapacityRetry
            | Self::AwaitAtlasOverlay(_)
            | Self::AwaitViewerOverlay(_)
            | Self::AwaitNext(_)
            | Self::AwaitPrevious(_)
            | Self::AwaitDetailClose
            | Self::AwaitExploreGallery
            | Self::AwaitExplorePreparation(_)
            | Self::AwaitExploreDatasetReopen { .. }
            | Self::AwaitDetailAgain
            | Self::AwaitAnnotation
            | Self::AwaitAnnotationFrame(_)
            | Self::AwaitPointer(_)
            | Self::CopyListSetup { .. }
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

fn explore_integer_id(index: u8) -> String {
    match index {
        0 => crate::generated::constraint_workflowsexploremininstances()
            .stable_field_id
            .to_string(),
        1 => crate::generated::constraint_workflowsexploremaxinstances()
            .stable_field_id
            .to_string(),
        2 => crate::generated::constraint_workflowsexploreshuffleseed()
            .stable_field_id
            .to_string(),
        3 => crate::generated::constraint_workflowsexploremincompiledindex()
            .stable_field_id
            .to_string(),
        4 => crate::generated::constraint_workflowsexploremaxcompiledindex()
            .stable_field_id
            .to_string(),
        _ => unreachable!("five Explore integer fields"),
    }
}

fn explore_seed_target(baseline: u64) -> u64 {
    const PRIMARY: u64 = (1_u64 << 53) + 1;
    const ALTERNATE: u64 = (1_u64 << 53) + 3;
    if baseline == PRIMARY {
        ALTERNATE
    } else {
        PRIMARY
    }
}

fn explore_integer_value(snapshot: &crate::generated::ExploreSnapshot, index: u8) -> u64 {
    match index {
        0 => u64::from(snapshot.filter.minimuminstances),
        1 => u64::from(snapshot.filter.maximuminstances),
        2 => snapshot.filter.shuffleseed,
        3 => snapshot.filter.minimumcompiledindex,
        4 => snapshot.filter.maximumcompiledindex,
        _ => unreachable!("five Explore integer fields"),
    }
}

fn explore_integer_persisted(model: &ApplicationModel, index: u8, value: u64) -> bool {
    model.settings_snapshot.as_ref().is_some_and(|snapshot| {
        let settings = &snapshot.settingsstate.workflows.explore;
        let persisted = match index {
            0 => u64::from(settings.mininstances),
            1 => u64::from(settings.maxinstances),
            2 => settings.shuffleseed,
            3 => settings.mincompiledindex,
            4 => settings.maxcompiledindex,
            _ => unreachable!("five Explore integer fields"),
        };
        persisted == value
    })
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

fn annotation_layout_scale(
    current_scale: f32,
    logical_width: f32,
    narrow: bool,
) -> Result<f32, &'static str> {
    let constraint = crate::generated::constraint_uiuiscale();
    let (minimum, maximum) = constraint
        .minimum
        .zip(constraint.maximum)
        .ok_or("Annotation narrow scale requires native bounds")?;
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
        return Err("Annotation narrow scale has invalid bounds or viewport dimensions");
    }
    let unscaled_width = current_scale * logical_width;
    let target_width = crate::view::PAGE_MIN_WIDTH * if narrow { 0.98 } else { 1.1 };
    let scale = (unscaled_width / target_width).clamp(minimum, maximum);
    if !unscaled_width.is_finite()
        || (unscaled_width / scale < crate::view::PAGE_MIN_WIDTH) != narrow
    {
        return Err("Native UI-scale bounds cannot reach the requested Annotation layout");
    }
    Ok(scale)
}

fn route_edit_available(
    model: &ApplicationModel,
    settings: &crate::view::settings::SettingsModel,
) -> bool {
    !settings.has_local_edits() && model.settings_edit_available()
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
                ("square", false),
                ("", false),
                ("wide", false),
                ("tall", false),
                ("semantics", false),
                ("copy", false),
                ("copy", true),
                ("rapid", false),
            ]
            .get(index)
            .copied(),
            "dpi" => [("copy", false), ("copy", true), ("rapid", true)]
                .get(index)
                .copied(),
            "terminal" => [("terminal", false)].get(index).copied(),
            _ => None,
        }
    }
}

pub struct Controller {
    reporting: reporting::Owner,
    generation: u64,
    phase: Phase,
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
    failure_line: u32,
    failure: String,
    annotation_open: Option<(crate::generated::AnnotationOpen, u64)>,
    desired_dark: Option<bool>,
    reuse_compiled: bool,
    bounded_document_revision: u64,
    bounded_object_count: usize,
    annotation_probe: Option<AnnotationProbe>,
    annotation_pixels_receipt: Option<ProbeReceipt>,
    annotation_pixel_progress: ((u64, u64), usize, usize),
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
    copy_product_ui_revision: u64,
    copy_product_settlement: annotation_product::Settlement,
    copy_narrow: bool,
    copy_original_scale: f32,
    copy_list_revision: u64,
    copy_list_object_target: usize,
    copy_list_class_target: usize,
    copy_layout_objects: usize,
    copy_layout_classes: usize,
    copy_requested_scale: f32,
    copy_layout_bounds: Rectangle,
    copy_viewport_width: f32,
    copy_swatch_color: [f64; 3],
    copy_swatch_ready: bool,
    copy_capability_ready: bool,
    copy_capability_available: bool,
    copy_shape_points: usize,
    copy_before: Option<crate::generated::AnnotationObject>,
    copy_after: Option<crate::generated::AnnotationObject>,
    copy_objects: usize,
    copy_categories: Vec<crate::generated::ClassName>,
    gallery_completion_held: Option<(u64, u32)>,
    sweep_baseline: Option<(u64, crate::generated::ExploreViewport)>,
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
    atlas_return_rows: u32,
    atlas_scroll_fraction: f32,
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
    workspace_fps_baseline: bool,
    workspace_fps_verified: bool,
    workspace_fps_evidence: Option<reporting::FpsEvidence>,
    workspace_fps_probe: Option<ScenarioOutput>,
    workspace_fps_receipt: Option<ProbeReceipt>,
    workspace_fps_scale: f32,
    workspace_fps_result: Option<FpsPixelOutcome>,
    workspace_fps_failure: Option<&'static str>,
    benchmark_baseline: bool,
    settings_revision: u64,
    explore_paste_read: bool,
    explore_integer_baseline: u64,
    explore_integer_target: u64,
    explore_integer_revision: u64,
    numeric_target: f64,
    numeric_replacement: String,
    numeric_selection_length: usize,
    denoising_target: bool,
    ui_scale_baseline: f32,
    ui_scale_first: Option<f32>,
    input_scale: f32,
    resize_original_size: Option<iced::Size>,
    resize_previous_size: Option<iced::Size>,
    oversized_gallery: Option<(iced::Size, crate::generated::VisualExtent)>,
    selection_grid: Option<(u32, u32, u32, u64, u64)>,
}

impl Controller {
    pub fn subscription(&self) -> iced::Subscription<Message> {
        if self.running() {
            iced::Subscription::batch([
                if reporting_enabled()
                    || matches!(
                        self.phase,
                        Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
                    )
                {
                    // Keep the existing delivery owner until cancellation reaches
                    // the driver; disabling diagnostics must not drop its receiver.
                    iced::Subscription::run(surface_draw_stream)
                } else {
                    iced::Subscription::none()
                },
                iced::event::listen_with(|event, _, _| match event {
                    iced::Event::Clipboard(iced::advanced::clipboard::Event::Read {
                        target: Some(target),
                        result,
                    }) => Some(Message::NumberPasteRead { target, result }),
                    iced::Event::Mouse(iced::mouse::Event::WheelScrolled { .. }) => {
                        Some(Message::NumberWheelDelivered)
                    }
                    iced::Event::Keyboard(iced::keyboard::Event::KeyReleased { key, .. })
                        if key.as_ref() == iced::keyboard::Key::Character("x") =>
                    {
                        Some(Message::NumberInvalidDelivered)
                    }
                    iced::Event::Mouse(iced::mouse::Event::CursorMoved { .. }) => {
                        Some(Message::GalleryMouseDelivered)
                    }
                    _ => None,
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

    pub fn observe_workspace_message<'a>(
        &self,
        payload: impl FnOnce() -> (&'a crate::view::router::Message, FeatureId),
    ) {
        self.observe_reporting(|reporting| {
            let (message, active) = payload();
            reporting.observe_workspace_message(message, active, &self.phase);
        });
    }

    pub(crate) fn observe_reporting(&self, observe: impl FnOnce(&mut reporting::State)) {
        if self.running() {
            self.reporting.observe(observe);
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
            reporting: reporting::Owner::new(),
            generation,
            phase: if enabled {
                Phase::AwaitBootstrap
            } else {
                Phase::Disabled
            },
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
            failure_line: 0,
            failure: String::new(),
            annotation_open: None,
            desired_dark: None,
            reuse_compiled: false,
            bounded_document_revision: 0,
            bounded_object_count: 0,
            annotation_probe: None,
            annotation_pixels_receipt: None,
            annotation_pixel_progress: ((0, 0), 0, 0),
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
            copy_product_ui_revision: 0,
            copy_product_settlement: annotation_product::Settlement::RenderedFrame,
            copy_narrow: false,
            copy_original_scale: 1.0,
            copy_list_revision: 0,
            copy_list_object_target: 0,
            copy_list_class_target: 0,
            copy_layout_objects: 0,
            copy_layout_classes: 0,
            copy_requested_scale: 1.0,
            copy_layout_bounds: Rectangle::default(),
            copy_viewport_width: 0.0,
            copy_swatch_color: [0.0; 3],
            copy_swatch_ready: false,
            copy_capability_ready: false,
            copy_capability_available: false,
            copy_shape_points: 0,
            copy_before: None,
            copy_after: None,
            copy_objects: 0,
            copy_categories: Vec::new(),
            gallery_completion_held: None,
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
            atlas_return_rows: 0,
            atlas_scroll_fraction: 0.0,
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
            workspace_fps_baseline: false,
            workspace_fps_verified: false,
            workspace_fps_evidence: None,
            workspace_fps_probe: None,
            workspace_fps_receipt: None,
            workspace_fps_scale: 1.0,
            workspace_fps_result: None,
            workspace_fps_failure: None,
            benchmark_baseline: false,
            settings_revision: 0,
            explore_paste_read: false,
            explore_integer_baseline: 0,
            explore_integer_target: 0,
            explore_integer_revision: 0,
            numeric_target: 0.0,
            numeric_replacement: String::new(),
            numeric_selection_length: 0,
            denoising_target: false,
            ui_scale_baseline: 1.0,
            ui_scale_first: None,
            input_scale: 1.0,
            resize_original_size: None,
            resize_previous_size: None,
            oversized_gallery: None,
            selection_grid: None,
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

    pub(crate) fn configure_session(
        &mut self,
        profile: &str,
        square_source: String,
        square_compiled: String,
    ) {
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

    pub(crate) fn receive_control(
        &mut self,
        receipt: crate::generated::IntegrationControlReceipt,
    ) -> Result<(), &'static str> {
        use crate::generated::IntegrationControlKind as Kind;
        if !crate::generated::integration_receipt_valid(&receipt)
            || !crate::generated::integration_server_command(receipt.kind)
        {
            self.fail("invalid integration control policy");
            return Err("invalid integration control policy");
        }
        if receipt.kind != Kind::Advance {
            if receipt.sequence != self.control_sequence
                || receipt.failureline != 0
                || receipt.progress != 0
            {
                self.fail("invalid capacity control identity");
                return Err("invalid capacity control identity");
            }
            if receipt.kind == Kind::GalleryReadCompletionHeld {
                if self.gallery_completion_held.is_some()
                    || matches!(self.phase, Phase::Disabled | Phase::Failed)
                {
                    self.fail("duplicate or inactive gallery completion hold");
                    return Err("duplicate or inactive gallery completion hold");
                }
                self.gallery_completion_held =
                    Some((receipt.readgeneration, receipt.compiledindex));
                return Ok(());
            }
            self.phase = match (receipt.kind, &self.phase) {
                (Kind::VisibleReadArmed, Phase::AwaitVisibleReadArm(index)) => {
                    Phase::VisibleReadScroll(*index)
                }
                (Kind::VisibleReadHeld, Phase::AwaitVisibleRead(index))
                    if receipt.compiledindex == *index && receipt.readgeneration != 0 =>
                {
                    Phase::AwaitVisibleReadPixels(*index, receipt.readgeneration)
                }
                (Kind::CapacityArmed, Phase::AwaitCapacityArm) => Phase::CapacityPublish,
                (Kind::CapacityCompletionReleased, Phase::AwaitCapacityCompletion) => {
                    Phase::AwaitCapacityRetry
                }
                _ => {
                    self.fail("duplicate, stale, or reordered capacity control");
                    return Err("duplicate, stale, or reordered capacity control");
                }
            };
            return Ok(());
        }
        if receipt.kind != crate::generated::IntegrationControlKind::Advance
            || receipt.failureline != 0
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
        self.reset_scenario(
            session.source.clone(),
            session.compiled.clone(),
            "512".into(),
            scenario.into(),
        )?;
        self.session = session;
        self.control_sequence = receipt.sequence;
        self.desired_dark = Some(dark);
        self.reuse_compiled = !scenario.is_empty();
        Ok(())
    }

    pub(crate) fn observe_annotation_open(
        &mut self,
        request: crate::generated::AnnotationOpen,
        document_epoch: u64,
    ) {
        if self.running() {
            self.annotation_open = Some((request, document_epoch));
        }
    }

    #[cfg(test)]
    pub(crate) fn annotation_open_for_test(
        &self,
    ) -> Option<(&crate::generated::AnnotationOpen, u64)> {
        self.annotation_open
            .as_ref()
            .map(|(request, epoch)| (request, *epoch))
    }

    fn require_original_crop(&mut self, frame: &crate::generated::VisualFrame) -> bool {
        let content = &frame.content;
        if self.viewer_drawn.is_none_or(|(_, _, draw)| {
            draw.crop != [content.x, content.y, content.width, content.height]
        }) {
            self.fail("returning viewer lost the selected Original-content crop");
            return false;
        }
        true
    }

    pub(crate) fn publish_control(
        &mut self,
        connection: &mut crate::transport_connection::Connection,
    ) {
        if self.generation == 0 || self.control_phase.as_ref() == Some(&self.phase) {
            return;
        }
        if self.viewer_scenario == "quiet"
            && matches!(self.phase, Phase::Complete)
            && !connection.integration_pressure_settled()
        {
            return;
        }
        use crate::generated::IntegrationControlKind as Kind;
        let kind = match self.phase {
            Phase::Disabled => return,
            Phase::Complete => Kind::Settled,
            Phase::Failed => Kind::Failed,
            Phase::AwaitCapacityArm => Kind::CapacityArmRequested,
            Phase::AwaitVisibleReadArm(_) => Kind::VisibleReadArmRequested,
            Phase::VisibleReadRelease(_, _) => Kind::VisibleReadReleaseRequested,
            _ => Kind::Progress,
        };
        let class = match self.phase.deadline_class() {
            "startup" => 1,
            "work" => 2,
            _ => 3,
        };
        let Some(progress) = self
            .control_progress
            .checked_add(4)
            .and_then(|value| value.checked_add(class))
        else {
            self.fail("integration progress identity exhausted");
            return;
        };
        match connection.send_integration_control(crate::generated::IntegrationControlReceipt {
            kind,
            sequence: self.control_sequence,
            progress,
            readgeneration: match self.phase {
                Phase::VisibleReadRelease(_, generation) => generation,
                _ => 0,
            },
            compiledindex: match self.phase {
                Phase::AwaitVisibleReadArm(index) | Phase::VisibleReadRelease(index, _) => index,
                _ => 0,
            },
            failure: self.failure.clone(),
            failureline: if matches!(self.phase, Phase::Failed) {
                self.failure_line
            } else {
                0
            },
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

    #[track_caller]
    fn fail(&mut self, detail: &str) {
        self.fail_detail(|| std::borrow::Cow::Borrowed(detail));
    }

    #[track_caller]
    fn fail_detail<'a>(&mut self, detail: impl FnOnce() -> std::borrow::Cow<'a, str>) {
        if self.generation != 0 {
            self.failure_line = std::panic::Location::caller().line();
            let detail = detail();
            let mut end = detail
                .len()
                .min(crate::generated::INTEGRATION_FAILURE_MAX_BYTES);
            while !detail.is_char_boundary(end) {
                end -= 1;
            }
            self.failure.clear();
            self.failure.push_str(&detail[..end]);
            reporting::emit(|sink| sink.record("integration.failed", "", &self.failure, [0.0; 4]));
        }
        crate::presentation_surface::end_capacity_acceptance();
        #[cfg(target_arch = "wasm32")]
        {
            restore_canvas_size_js();
            cancel_number_edit_js();
        }
        self.phase = Phase::Failed;
        self.explore_paste_read = false;
        self.location_pending = false;
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
            presentation_revision: crate::presentation_surface::retained_surface()
                .and_then(|surface| surface.frame)
                .map_or(0, |frame| frame.presentation_revision),
        };
        self.arm(EXPLORE_UPSCALE_ACTIONS[0])
    }

    fn annotation_points(&self, width: f64, height: f64) -> [f64; 4] {
        self.copy_product_gesture.unwrap_or_else(|| {
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
        })
    }

    fn annotation_reveal(&self, control: &str) -> AnnotationReveal {
        if matches!(self.phase, Phase::CopyLayout(2 | 3)) {
            return AnnotationReveal::Tail {
                count: if matches!(self.phase, Phase::CopyLayout(2)) {
                    self.copy_layout_objects
                } else {
                    self.copy_layout_classes
                },
                narrow: self.copy_narrow,
            };
        }
        if control != ANNOTATION_SURFACE {
            return if control == ANNOTATION_SIDEBAR
                || matches!(self.phase, Phase::PageRegion { .. })
            {
                AnnotationReveal::Geometry
            } else {
                AnnotationReveal::Control
            };
        }
        if let Some(probe) = &self.annotation_probe {
            if let Some(pixel) = probe.pixels.chunks_exact(7).next() {
                let radius = pixel[6] as f32;
                return AnnotationReveal::Source {
                    extent: probe.extent.map(|value| value as f32),
                    region: Rectangle {
                        x: pixel[0] as f32 - radius,
                        y: pixel[1] as f32 - radius,
                        width: radius * 2.0,
                        height: radius * 2.0,
                    },
                    margin: 2.0 / self.input_scale,
                };
            }
        }
        if matches!(self.phase, Phase::AnnotationPointer(_)) {
            if let Some(frame) = self.annotation_frame_ready {
                let extent = [frame.content_width as f32, frame.content_height as f32];
                let [x, y, ex, ey] = self
                    .annotation_points(f64::from(extent[0]), f64::from(extent[1]))
                    .map(|value| value as f32);
                return AnnotationReveal::Source {
                    extent,
                    region: Rectangle {
                        x: x.min(ex),
                        y: y.min(ey),
                        width: (ex - x).abs(),
                        height: (ey - y).abs(),
                    },
                    margin: 1.0 / self.input_scale,
                };
            }
        }
        AnnotationReveal::Geometry
    }

    fn copy_scale_transition(
        &mut self,
        model: &ApplicationModel,
        applied_scale: f32,
        stage: CopyScaleStage,
    ) -> Task<RootMessage> {
        let scale = if stage == CopyScaleStage::Restore {
            self.copy_original_scale
        } else {
            match annotation_layout_scale(
                applied_scale,
                model.window_width as f32,
                stage == CopyScaleStage::Narrow,
            ) {
                Ok(value) => value,
                Err(detail) => {
                    self.fail(detail);
                    return Task::none();
                }
            }
        };
        let unchanged = same_numeric_value(f64::from(scale), f64::from(applied_scale));
        let revision = model
            .settings_snapshot
            .as_ref()
            .map_or(0, |snapshot| snapshot.revision);
        self.settings_revision = if unchanged {
            revision.saturating_sub(1)
        } else {
            revision
        };
        self.copy_requested_scale = scale;
        self.phase = Phase::CopyAwaitScale(stage);
        if unchanged {
            return self.advance_to(self.phase.clone());
        }
        Task::done(RootMessage::Settings(
            crate::view::settings::Message::UiScaleChanged(scale),
        ))
        .chain(Task::done(RootMessage::Settings(
            crate::view::settings::Message::UiScaleReleased,
        )))
    }

    fn copy_layout_control(&self, step: u8) -> String {
        match step {
            0 => "workflow.workspace_and_advanced".into(),
            1 => "workflow.diagnostics".into(),
            2 => format!(
                "annotation.object.{}",
                self.copy_layout_objects.saturating_sub(1)
            ),
            3 => format!(
                "annotation.class.{}",
                self.copy_layout_classes.saturating_sub(1)
            ),
            4 => VIEWER_SAVE.into(),
            5 => ANNOTATION_TIMELINE.into(),
            6 => ANNOTATION_STOP.into(),
            _ => "annotation.class.active.swatch".into(),
        }
    }

    fn arm(&mut self, control: impl Into<String>) -> Task<RootMessage> {
        if self.location_pending {
            return Task::none();
        }
        self.location_pending = true;
        let control = control.into();
        if control.starts_with("annotation.") {
            reveal_control(
                control.clone(),
                self.generation,
                self.annotation_reveal(&control),
            )
        } else {
            locate(control, self.generation)
        }
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
        if control.starts_with("annotation.") {
            let reveal = self.annotation_reveal(&control);
            let task = reveal_control(control.clone(), self.generation, reveal);
            if matches!(reveal, AnnotationReveal::Tail { .. }) {
                iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, RelativeOffset::START)
                    .chain(iced::widget::operation::snap_to(
                        crate::view::HORIZONTAL_SCROLL_ID,
                        RelativeOffset::START,
                    ))
                    .chain(task)
            } else {
                task
            }
        } else {
            iced::widget::operation::snap_to(crate::view::PAGE_SCROLL_ID, offset)
                .chain(locate(control, self.generation))
        }
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

    fn prepare_upscale_probe(
        &mut self,
        image: Rectangle,
        source: u64,
        presentation: u64,
    ) -> Option<ScenarioOutput> {
        let output = probe_output(explore::DETAIL_WORKSPACE_ID)?;
        let receipt = output.receipt.as_ref()?;
        let frame = receipt.surface.frame?;
        if receipt.image != image
            || frame.content_sequence != source
            || frame.presentation_revision != presentation
        {
            return None;
        }
        self.upscale_pixel_owner = output.probe.clone();
        self.upscale_pixel_pending = output.receipt.clone();
        self.upscale_pixels = None;
        Some(output)
    }

    fn prepare_control_probe(&mut self) -> bool {
        if self.location_pending {
            return false;
        }
        let Some(output) = probe_output("workflow.visual.workspace") else {
            return false;
        };
        self.control_probe_owner = output.probe.clone();
        self.control_probe_receipt = output.receipt.clone();
        self.control_probe = Some(ControlProbe {
            output,
            color: self.copy_swatch_color,
            available: self.copy_capability_available,
        });
        true
    }

    fn copy_product_settled(&self, snapshot: &crate::generated::AnnotationSnapshot) -> bool {
        // Command admission advances the UI revision while the operation still
        // owns work. Check its result only after the native owner settles it.
        if snapshot.busy {
            return false;
        }
        match self.copy_product_settlement {
            annotation_product::Settlement::NativeUi => {
                snapshot.uirevision > self.copy_product_ui_revision
            }
            annotation_product::Settlement::RenderedFrame => {
                snapshot.frame.revision > self.copy_product_frame
            }
        }
    }

    fn prepare_annotation_probe(
        &mut self,
        source: u64,
        presentation: u64,
        extent: [u32; 2],
        pixels: Vec<f64>,
    ) -> bool {
        if self.location_pending {
            return false;
        }
        let Some(output) = probe_output("workflow.visual.workspace") else {
            return false;
        };
        let Some(frame) = output
            .receipt
            .as_ref()
            .and_then(|receipt| receipt.surface.frame)
        else {
            return false;
        };
        if frame.content_sequence != source
            || frame.presentation_revision != presentation
            || [frame.content_width, frame.content_height] != extent
        {
            return false;
        }
        if self.annotation_pixels_pending == output.receipt {
            return false;
        }
        self.annotation_pixels_owner = output.probe.clone();
        self.annotation_pixels_pending = output.receipt.clone();
        self.annotation_probe = Some(AnnotationProbe {
            output,
            source,
            presentation,
            extent,
            pixels,
        });
        true
    }

    fn invalidate_atlas_draw(&mut self) {
        // The next ordinary physical draw can arm the same frame at settled geometry.
        self.atlas_receipt = None;
        self.atlas_pixels = None;
        self.atlas_composition = None;
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().atlas = None);
    }

    fn scroll_atlas(&mut self, stage: u8) -> Task<RootMessage> {
        self.phase = Phase::AwaitAtlasScroll(stage);
        match stage {
            5 => iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::END),
            6 => iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::START),
            _ => iced::widget::operation::scroll_to(
                EXPLORE_GALLERY,
                AbsoluteOffset {
                    x: 0.0,
                    y: self.atlas_row_extent
                        * ([0.0, 1.0, 2.0, 10.0, 9.0][stage as usize]
                            + if matches!(stage, 0 | 2) {
                                self.atlas_scroll_fraction
                            } else {
                                0.0
                            }),
                },
            ),
        }
    }

    fn workspace_fps_probe_current(&self) -> bool {
        if !reporting_enabled() {
            return false;
        }
        let (Some(output), Some(evidence)) =
            (&self.workspace_fps_probe, self.workspace_fps_evidence)
        else {
            return false;
        };
        if output.receipt != current_receipt(EXPLORE_GALLERY)
            || SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().fps_draw) != Some(evidence)
        {
            return false;
        }
        #[cfg(target_arch = "wasm32")]
        {
            fps_current_js(&output.canvas_probe, &evidence.canvas_values())
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            true
        }
    }

    fn cancel_workspace_fps(&mut self) {
        self.workspace_fps_probe = None;
        self.workspace_fps_result = None;
        self.workspace_fps_failure = Some("Workspace FPS capture was cancelled");
        self.phase = Phase::RestoreWorkspaceFps;
    }

    fn restore_workspace_fps(&mut self) -> Task<RootMessage> {
        self.phase = Phase::AwaitWorkspaceFpsRestored;
        Task::done(RootMessage::Settings(
            crate::view::settings::Message::PerformanceChanged(self.workspace_fps_baseline),
        ))
    }

    fn rearm_workspace_fps(&mut self) {
        self.workspace_fps_probe = None;
        self.workspace_fps_receipt = None;
        self.workspace_fps_evidence = None;
        self.workspace_fps_result = None;
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().fps_sample = None);
        self.phase = Phase::AwaitWorkspaceFps;
    }

    fn confirmed_atlas(&self, snapshot: &crate::generated::ExploreSnapshot) -> Option<&AtlasDraw> {
        self.atlas_pixels.as_ref().filter(|draw| {
            draw.snapshot.frame == snapshot.frame && self.atlas_receipt.as_ref() == Some(*draw)
        })
    }

    fn detail_drawn(
        &self,
        frame: Option<crate::presentation_surface::FrameReady>,
        snapshot: &crate::generated::ExploreSnapshot,
    ) -> bool {
        snapshot.mode == crate::generated::ExploreMode::Detail
            && sampleable_presentation(
                frame,
                crate::generated::PresentationSourceKind::Explore,
                snapshot.frame.revision,
            )
            .is_some_and(|sampleable| {
                self.viewer_drawn.is_some_and(|(presentation, source, _)| {
                    source == sampleable.source_revision
                        && presentation == sampleable.presentation_revision
                })
            })
    }

    pub(crate) fn accepts_message(&self, message: &Message) -> bool {
        if !self.running() {
            return false;
        }
        match message {
            Message::Scoped {
                generation,
                receipt,
                message,
            } => {
                let (probe, message) = match message.as_ref() {
                    Message::ProbeCompleted { owner, message } => (Some(owner), message.as_ref()),
                    message => (None, message),
                };
                let cancellation = matches!(
                    message,
                    Message::ReportingDisabled
                        | Message::WorkspaceFpsPixels(FpsPixelOutcome::Cancelled)
                );
                *generation == self.generation
                    && (cancellation
                        || *generation
                            == SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation))
                    && match message {
                        Message::ReportingDisabled => {
                            probe.is_none()
                                && matches!(
                                    self.phase,
                                    Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
                                )
                        }
                        Message::WorkspaceFpsPixels(_) => {
                            self.phase == Phase::AwaitWorkspaceFpsPixels
                                && (cancellation || self.workspace_fps_result.is_none())
                                && self
                                    .workspace_fps_probe
                                    .as_ref()
                                    .is_some_and(|output| same_probe(&output.probe, probe))
                        }
                        Message::UpscalePixels { .. } => {
                            same_probe(&self.upscale_pixel_owner, probe)
                        }
                        Message::AnnotationControlPixels { .. }
                        | Message::AnnotationPixels { revision: 0, .. } => {
                            same_probe(&self.control_probe_owner, probe)
                        }
                        Message::AnnotationPixels { .. } => {
                            same_probe(&self.annotation_pixels_owner, probe)
                        }
                        Message::AtlasPixels { .. } => SURFACE_DRAW_OBSERVER.with(|observer| {
                            same_probe(&observer.borrow().atlas_pixels_owner, probe)
                        }),
                        Message::AtlasComposition { .. } => {
                            SURFACE_DRAW_OBSERVER.with(|observer| {
                                same_probe(&observer.borrow().atlas_composition_owner, probe)
                            })
                        }
                        _ => probe.is_none(),
                    }
                    && match receipt {
                        // An owned completion must retire its request even when its
                        // physical receipt changed while the browser was sampling.
                        Some(_) if matches!(message, Message::WorkspaceFpsPixels(_)) => true,
                        Some(receipt) => current_receipt(receipt.control).as_ref() == Some(receipt),
                        None => matches!(
                            message,
                            Message::ReportingDisabled
                                | Message::Advance
                                | Message::Located { .. }
                                | Message::NumberWheelDelivered
                                | Message::NumberInvalidDelivered
                                | Message::NumberClipboardPrepared { .. }
                                | Message::NumberPasteDelivered(_)
                                | Message::NumberPasteRead { .. }
                                | Message::GalleryMouseDelivered
                        ),
                    }
            }
            Message::Advance
            | Message::Located { .. }
            | Message::NumberWheelDelivered
            | Message::NumberInvalidDelivered
            | Message::GalleryMouseDelivered => true,
            _ => false,
        }
    }

    pub fn update(&mut self, message: Message) -> Option<train::Message> {
        if !self.accepts_message(&message) {
            return None;
        }
        let (message, request_receipt) = match message {
            Message::Scoped {
                message, receipt, ..
            } => (*message, receipt),
            message => (message, None),
        };
        let message = match message {
            Message::ProbeCompleted { message, .. } => *message,
            message => message,
        };
        let (control, bounds) = match message {
            Message::ReportingDisabled => {
                if matches!(
                    self.phase,
                    Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
                ) {
                    self.cancel_workspace_fps();
                }
                return None;
            }
            Message::WorkspaceFpsDrawn(evidence) => {
                if self.phase == Phase::AwaitWorkspaceFps
                    && request_receipt
                        .as_ref()
                        .is_some_and(|receipt| receipt.control == EXPLORE_GALLERY)
                {
                    self.workspace_fps_evidence = Some(evidence);
                    self.workspace_fps_receipt = request_receipt;
                }
                return None;
            }
            Message::WorkspaceFpsPixels(outcome) => {
                let current = self.workspace_fps_probe_current();
                if !reporting_enabled() || matches!(outcome, FpsPixelOutcome::Cancelled) {
                    self.cancel_workspace_fps();
                } else if !current || matches!(outcome, FpsPixelOutcome::Invalidated) {
                    self.rearm_workspace_fps();
                } else {
                    // The next driver advance validates the applied UI scale as
                    // well as the frozen draw before classifying or reporting pixels.
                    self.workspace_fps_result = Some(outcome);
                }
                return None;
            }
            Message::Scoped { .. } | Message::ProbeCompleted { .. } | Message::Advance => {
                return None;
            }
            Message::GalleryMouseDelivered => {
                if let Phase::AwaitVisibleReadHover(index, generation) = self.phase {
                    self.phase = Phase::VisibleReadSelect(index, generation);
                }
                return None;
            }
            Message::NumberClipboardPrepared { revision, result } => {
                if self.phase == Phase::AwaitExploreClipboard(revision) {
                    match result {
                        Ok(()) => self.phase = Phase::ExploreNumericControl { index: 2, step: 6 },
                        Err(error) => self.fail_detail(|| {
                            format!("Explore clipboard preparation failed: {error:?}").into()
                        }),
                    }
                }
                return None;
            }
            Message::NumberPasteRead { target, result } => {
                if self.phase == (Phase::AwaitExploreNumeric { index: 2, step: 6 })
                    && target == explore_integer_id(2)
                {
                    match result {
                        Ok(content)
                            if matches!(content.as_ref(), iced::clipboard::Content::Text(text)
                            if text == &self.explore_integer_target.to_string()) =>
                        {
                            self.explore_paste_read = true
                        }
                        Ok(_) => self.fail("Explore paste read different clipboard contents"),
                        Err(error) => self.fail_detail(|| {
                            format!("Explore clipboard read failed: {error:?}").into()
                        }),
                    }
                }
                return None;
            }
            Message::NumberPasteDelivered(delivered) => {
                if !delivered && self.phase == (Phase::AwaitExploreNumeric { index: 2, step: 6 }) {
                    self.fail("Explore clipboard paste shortcut delivery failed");
                }
                return None;
            }
            Message::NumberInvalidDelivered => {
                if let Phase::AwaitExploreNumericInvalid(index) = self.phase {
                    self.phase = Phase::AwaitExploreNumeric { index, step: 3 };
                }
                return None;
            }
            Message::NumberWheelDelivered => {
                if let Phase::AwaitExploreNumericWheel(index) = self.phase {
                    self.phase = Phase::AwaitExploreNumeric { index, step: 2 };
                }
                if let Phase::AwaitAdvancedSpinnerWheel(index) = self.phase {
                    self.phase = Phase::AdvancedSpinnerWheelVerify(index);
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.number_wheel_delivered",
                            &advanced_field_id(index),
                            "iced-widget-update-complete",
                            [0.0; 4],
                        )
                    });
                }
                return None;
            }
            Message::UpscalePixels {
                source,
                presentation,
                outcome,
            } => {
                if self.upscale_pixel_pending != request_receipt {
                    return None;
                }
                self.upscale_pixel_owner = None;
                match outcome {
                    ProbeOutcome::Invalidated => self.upscale_pixel_pending = None,
                    ProbeOutcome::Observed(checksum, blue) => {
                        self.upscale_pixels = Some((source, presentation, checksum, blue))
                    }
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
                if self.control_probe_receipt != request_receipt {
                    return None;
                }
                self.control_probe_owner = None;
                match outcome {
                    ProbeOutcome::Invalidated => self.control_probe_receipt = None,
                    ProbeOutcome::Observed(1, 1) => self.copy_capability_ready = true,
                    _ => self.fail("Rendered tool availability differs from the native capability"),
                }
                return None;
            }
            Message::AnnotationPixels { revision, outcome } => {
                let pending = if revision == 0 {
                    &mut self.control_probe_receipt
                } else {
                    &mut self.annotation_pixels_pending
                };
                if *pending != request_receipt {
                    return None;
                }
                *pending = None;
                if revision == 0 {
                    self.control_probe_owner = None;
                } else {
                    self.annotation_pixels_owner = None;
                }
                match outcome {
                    ProbeOutcome::Invalidated => {}
                    ProbeOutcome::Observed(expected, matched)
                        if expected != 0 && expected == matched =>
                    {
                        if revision == 0 {
                            self.control_probe_receipt = request_receipt;
                            self.copy_swatch_ready = true;
                        } else {
                            self.annotation_pixel_progress.1 += 1;
                            if self.annotation_pixel_progress.1 >= self.annotation_pixel_progress.2
                            {
                                self.annotation_pixels_receipt = request_receipt;
                            }
                        }
                    }
                    _ => self
                        .fail("Annotation pixels do not match source geometry and native palette"),
                }
                return None;
            }
            Message::AtlasComposition { receipt, outcome } => {
                SURFACE_DRAW_OBSERVER
                    .with(|observer| observer.borrow_mut().atlas_composition_owner = None);
                match outcome {
                    ProbeOutcome::Invalidated => self.invalidate_atlas_draw(),
                    ProbeOutcome::Observed(expected, matched)
                        if expected != 0 && expected == matched =>
                    {
                        self.atlas_composition = Some(receipt)
                    }
                    ProbeOutcome::Failed => self.fail("Atlas composition canvas sampling failed"),
                    _ => {}
                }
                return None;
            }
            Message::AtlasPixels { receipt, outcome } => {
                SURFACE_DRAW_OBSERVER
                    .with(|observer| observer.borrow_mut().atlas_pixels_owner = None);
                match outcome {
                    ProbeOutcome::Invalidated => self.invalidate_atlas_draw(),
                    ProbeOutcome::Observed(visible, nonblack) => {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.atlas_canvas_pixels",
                                EXPLORE_GALLERY,
                                "visible-tile-interiors",
                                [
                                    receipt.snapshot.frame.revision as f64,
                                    receipt
                                        .surface
                                        .frame
                                        .map_or(0, |frame| frame.presentation_revision)
                                        as f64,
                                    visible as f64,
                                    nonblack as f64,
                                ],
                            )
                        });
                        if visible != 0 && visible == nonblack {
                            self.atlas_pixels = Some(receipt);
                        }
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
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_reopen_draw",
                            EXPLORE_GALLERY,
                            "physical-gallery-draw",
                            [
                                *revision as f64,
                                *frame_revision as f64,
                                source_revision as f64,
                                presentation_revision as f64,
                            ],
                        )
                    });
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
                    // Returning to a retained gallery still requires a draw
                    // after Detail; an earlier receipt cannot prove that return.
                    self.invalidate_atlas_draw();
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_complete",
                        "explore.detail.aspect",
                        &self.viewer_scenario,
                        [presentation as f64, source as f64, 1.0, 0.0],
                    )
                });
                self.phase = if self.viewer_scenario == "terminal" {
                    Phase::OpenAnnotation
                } else {
                    Phase::Complete
                };
            }
            return None;
        }
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            reporting::emit(|sink| {
                sink.record(
                    "integration.locate_failed",
                    &control,
                    "stable identity absent from rendered tree",
                    [0.0; 4],
                )
            });
            self.fail("Iced widget operation could not locate the stable identity");
            return None;
        }
        self.reporting
            .observe(|reporting| reporting.located(&self.phase, &control, bounds));
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
            Phase::ExploreCloseDetail => EXPLORE_DETAIL_CLOSE.to_owned(),
            Phase::ExploreDatasetPane => EXPLORE_DATASET_PANE.to_owned(),
            Phase::ExploreDetailsPane => EXPLORE_DETAILS_PANE.to_owned(),
            Phase::ExploreNumericControl { index, .. } | Phase::ExploreNumericReveal { index, .. } => explore_integer_id(index),
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
            Phase::ViewerSelect | Phase::AtlasReturnSelect | Phase::AtlasResizeSelect(_) | Phase::AtlasAwaySelect(_) => EXPLORE_GALLERY.to_owned(),
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
            Phase::CopyLayout(1)=>"workflow.diagnostics".into(),
            Phase::CopyLayout(step) => self.copy_layout_control(step),
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
        self.reporting
            .observe(|reporting| reporting.style(&control, bounds));
        let input_bounds = crate::presentation_surface::physical_bounds(bounds, self.input_scale);
        match self.phase.clone() {
            Phase::CopyCapability => {
                self.phase = Phase::CopyCapabilityWait;
                if let Some(probe) = self.control_probe.take() {
                    probe.sample(input_bounds, &control, true);
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
                let page = crate::view::canvas_layout(self.copy_viewport_width);
                let valid = (image.width / page.page_width - 0.62).abs() < 0.002
                    && (bounds.width / page.page_width - 0.19).abs() < 0.002
                    && (bounds.x - image.x - image.width).abs() <= 1.0
                    && (bounds.y - image.y).abs() <= 1.0
                    && page.horizontal_overflow == self.copy_narrow;
                if !valid {
                    self.fail(
                        "Annotation must retain shared columns with narrow horizontal overflow",
                    );
                    return None;
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_layout",
                        "workflow.diagnostics",
                        if self.copy_narrow { "narrow" } else { "wide" },
                        [
                            f64::from(image.width),
                            f64::from(bounds.width),
                            f64::from(page.page_width),
                            f64::from(self.copy_viewport_width),
                        ],
                    )
                });
                self.phase = Phase::CopyLayout(2);
                None
            }
            Phase::CopyLayout(step @ 2..=6) => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_reachable",
                        &control,
                        if self.copy_narrow { "narrow" } else { "wide" },
                        [
                            f64::from(bounds.x),
                            f64::from(bounds.y),
                            f64::from(bounds.width),
                            f64::from(bounds.height),
                        ],
                    )
                });
                self.phase = Phase::CopyLayout(step + 1);
                None
            }
            Phase::CopyLayout(_) => {
                self.phase = Phase::CopySwatchWait;
                if let Some(probe) = self.control_probe.take() {
                    probe.sample(input_bounds, &control, false);
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
                            let _ = output.try_send(Message::AnnotationPixels {
                                revision: source,
                                outcome,
                            });
                        });
                        annotation_pixels_js(
                            &canvas_probe,
                            &[
                                f64::from(input_bounds.x),
                                f64::from(input_bounds.y),
                                f64::from(input_bounds.width),
                                f64::from(input_bounds.height),
                            ],
                            &probe.extent.map(f64::from),
                            &probe.pixels,
                            source as f64,
                            probe.presentation as f64,
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
            Phase::PageNavigation(page) => {
                self.phase = Phase::AwaitPage(page);
                if !click(input_bounds) {
                    self.fail("Firefox navigation click dispatch failed");
                }
                None
            }
            Phase::PageRegion { page, index } => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.page_region",
                        region_id(page, index),
                        crate::view::navigation::label(page),
                        [
                            f64::from(bounds.x),
                            f64::from(bounds.y),
                            f64::from(bounds.width),
                            f64::from(bounds.height),
                        ],
                    )
                });
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
                self.phase = Phase::ExploreNumericStart(0);
                None
            }
            Phase::ExploreNumericControl { index, step }
            | Phase::ExploreNumericReveal { index, step } => {
                let Some(pane) = self.explore_dataset_pane else {
                    self.fail("Explore numeric input has no sidebar bounds");
                    return None;
                };
                if let Some(offset) = sidebar_reveal_offset(pane, bounds) {
                    self.reveal_offset = offset;
                    self.phase = Phase::ExploreNumericReveal { index, step };
                    return None;
                }
                let delivered = match step {
                    0 | 1 => click_number_edge(input_bounds, step == 0),
                    2 => wheel_number_input(input_bounds),
                    6 => paste_number_input(input_bounds),
                    _ => replace_number_input(input_bounds, &self.numeric_replacement, 20),
                };
                if !delivered {
                    self.fail("Explore integer input dispatch failed");
                } else {
                    self.phase = match step {
                        2 => Phase::AwaitExploreNumericWheel(index),
                        3 => Phase::AwaitExploreNumericInvalid(index),
                        _ => Phase::AwaitExploreNumeric { index, step },
                    };
                }
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
            Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(_)
            | Phase::AtlasAwaySelect(_) => {
                let resizing = match self.phase {
                    Phase::AtlasResizeSelect(step) => Some(step),
                    _ => None,
                };
                let returning = matches!(self.phase, Phase::AtlasReturnSelect);
                let away = match self.phase {
                    Phase::AtlasAwaySelect(step) => Some(step),
                    _ => None,
                };
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
                self.phase = if let Some(step) = resizing {
                    Phase::AwaitAtlasResizeDetail(step)
                } else if returning {
                    Phase::AwaitAtlasReturnDetail
                } else if let Some(step) = away {
                    Phase::AwaitAtlasAwayDetail(step, 0)
                } else {
                    Phase::AwaitDetail(index)
                };
                if !click_after_surface_draw(selected, EXPLORE_GALLERY, revision, true) {
                    self.fail("viewer first-image click failed");
                }
                None
            }
            Phase::AtlasCapacity | Phase::AtlasEmpty => {
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_notice",
                        &control,
                        "rendered-local-outcome",
                        [
                            bounds.x as f64,
                            bounds.y as f64,
                            bounds.width as f64,
                            bounds.height as f64,
                        ],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_pointer_scheduled",
                        EXPLORE_GALLERY,
                        "reopened-grid-slot",
                        [
                            revision as f64,
                            slot as f64,
                            f64::from(selected.center_x()),
                            f64::from(selected.center_y()),
                        ],
                    )
                });
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
                    let points = self.annotation_points(width, height);
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
                    if self.viewer_scenario == "quiet" {
                        160
                    } else {
                        1
                    },
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
                let selected =
                    gallery_slot_bounds(bounds, columns, expected_slot, self.atlas_clip.0);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_pointer_inverse",
                        EXPLORE_GALLERY,
                        "rendered-grid-slot",
                        [
                            expected_slot as f64,
                            (rows / 2)
                                .saturating_mul(columns)
                                .saturating_add(columns / 2) as f64,
                            index as f64,
                            snapshot_revision as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_pointer_scheduled",
                        EXPLORE_GALLERY,
                        "real-canvas-pointer",
                        [
                            frame_revision as f64,
                            expected_slot as f64,
                            f64::from(selected.x + selected.width * 0.5),
                            f64::from(selected.y + selected.height * 0.5),
                        ],
                    )
                });
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
                    Phase::ExploreCloseDetail => Phase::AwaitExploreGallery,
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
        reporting::emit(|sink| {
            sink.record(
                "integration.phase_advanced",
                "",
                &format!("{phase:?}"),
                [0.0; 4],
            )
        });
        let reveal_annotation = matches!(phase, Phase::CopyProductWait);
        self.phase = phase;
        // A completed local step has no pending native event to wake its
        // successor. Queue one continuation without requiring another draw.
        let continuation = Task::done(RootMessage::Integration(Message::Scoped {
            generation: self.generation,
            receipt: None,
            message: Box::new(Message::Advance),
        }));
        if reveal_annotation {
            // Restore both axes before waiting for a completed canvas draw.
            scroll_control_into_view(ANNOTATION_SURFACE.into(), AnnotationReveal::Geometry)
                .chain(continuation)
        } else {
            continuation
        }
    }

    fn report_phase_progress(&self) {
        self.reporting
            .observe(|reporting| reporting.phase_progress(&self.phase));
    }

    pub fn advance(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
        router: &crate::view::router::Router,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
    ) -> Task<RootMessage> {
        let frame = surface.and_then(|surface| surface.frame);
        self.report_phase_progress();
        if !self.running() {
            return Task::none();
        }
        if let Some(dark) = self.desired_dark {
            let Some(snapshot) = model.settings_snapshot.as_ref() else {
                return Task::none();
            };
            if settings.has_local_edits() {
                return Task::none();
            }
            if snapshot.settingsstate.ui.darkmode != dark {
                return Task::done(RootMessage::Settings(
                    crate::view::settings::Message::DarkModeChanged(dark),
                ));
            }
            self.desired_dark = None;
        }
        if !reporting_enabled()
            && matches!(
                self.phase,
                Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
            )
        {
            self.cancel_workspace_fps();
        }
        self.input_scale = applied_scale;
        self.observe_presentation(model, frame);
        self.reporting
            .observe(|reporting| reporting.explore_snapshot(model, settings));
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
            self.fail_detail(|| {
                format!("{:?}: {}: {}", error.kind, error.title, error.detail).into()
            });
            return Task::none();
        }
        match self.phase.clone() {
            Phase::AwaitWorkspaceFps => {
                if self.workspace_fps_evidence.is_none()
                    || settings.has_local_edits()
                    || model.native_settings_unsettled()
                    || !crate::workspace_fps::enabled(settings)
                {
                    return Task::none();
                }
                let evidence = self.workspace_fps_evidence.expect("observed FPS evidence");
                let current = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().fps_draw);
                if current != Some(evidence)
                    || self.workspace_fps_receipt != current_receipt(EXPLORE_GALLERY)
                {
                    self.rearm_workspace_fps();
                    return Task::none();
                }
                let Some(output) = probe_output(EXPLORE_GALLERY) else {
                    return Task::none();
                };
                self.workspace_fps_probe = Some(output.clone());
                self.workspace_fps_scale = applied_scale;
                self.phase = Phase::AwaitWorkspaceFpsPixels;
                #[cfg(target_arch = "wasm32")]
                sample_workspace_fps(output, evidence, applied_scale);
                Task::none()
            }
            Phase::AwaitWorkspaceFpsPixels => {
                if self.workspace_fps_scale != applied_scale || !self.workspace_fps_probe_current()
                {
                    self.rearm_workspace_fps();
                } else if let Some(outcome) = self.workspace_fps_result.take() {
                    let valid = match (outcome, self.workspace_fps_evidence) {
                        (FpsPixelOutcome::Captured(image), Some(evidence)) => {
                            reporting::verify_workspace_fps_pixels(&image, evidence)
                        }
                        _ => false,
                    };
                    self.workspace_fps_probe = None;
                    self.workspace_fps_failure = (!valid).then_some(WORKSPACE_FPS_PIXEL_FAILURE);
                    return self.restore_workspace_fps();
                }
                Task::none()
            }
            Phase::RestoreWorkspaceFps => self.restore_workspace_fps(),
            Phase::AwaitWorkspaceFpsRestored
                if !settings.has_local_edits()
                    && !model.native_settings_unsettled()
                    && crate::workspace_fps::enabled(settings) == self.workspace_fps_baseline
                    && model.settings_snapshot.as_ref().is_some_and(|snapshot| {
                        snapshot.settingsstate.ui.showworkspaceperformance
                            == self.workspace_fps_baseline
                    }) =>
            {
                if let Some(failure) = self.workspace_fps_failure {
                    self.fail(failure);
                    Task::none()
                } else {
                    self.workspace_fps_verified = true;
                    self.advance_to(Phase::AwaitExploreReady)
                }
            }
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_settings_preserved",
                        explore::DETAIL_WORKSPACE_ID,
                        "confirmed-native-settings",
                        [snapshot.revision as f64, 1.0, 0.0, 0.0],
                    )
                });
                self.viewer_route_persistence =
                    settings.draft.is_some() && model.settings_edit_available();
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_departure_started",
                        explore::DETAIL_WORKSPACE_ID,
                        "upscale-observation",
                        [
                            model
                                .upscale_snapshot
                                .as_ref()
                                .map_or(0, |state| state.revision)
                                as f64,
                            u8::from(self.viewer_route_persistence) as f64,
                            snapshot.revision as f64,
                            0.0,
                        ],
                    )
                });
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
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.viewer_route_confirmed",
                            "navigation.train",
                            "None",
                            [
                                self.viewer_continuity_settings_revision as f64,
                                u8::from(self.viewer_route_persistence) as f64,
                                1.0,
                                0.0,
                            ],
                        )
                    });
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_abandoned",
                        explore::DETAIL_WORKSPACE_ID,
                        "mapped-route-departure",
                        [
                            model
                                .upscale_snapshot
                                .as_ref()
                                .map_or(0, |state| state.revision)
                                as f64,
                            0.0,
                            0.0,
                            0.0,
                        ],
                    )
                });
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
                    reporting::emit(|sink| {
                        sink.record(
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
                        )
                    });
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_basic_reentry",
                        explore::DETAIL_WORKSPACE_ID,
                        "automatic-completed-draw",
                        [
                            upscale.frame.revision as f64,
                            sampleable.presentation_revision as f64,
                            upscale.frame.source.instance as f64,
                            upscale.frame.cleanrevision as f64,
                        ],
                    )
                });
                if !self.require_original_crop(&upscale.frame) {
                    return Task::none();
                }
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
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    source,
                ) else {
                    return Task::none();
                };
                if drawn != sampleable.presentation_revision {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_reconnected",
                        explore::DETAIL_WORKSPACE_ID,
                        "matching-completed-draw",
                        [
                            source as f64,
                            drawn as f64,
                            upscale.frame.source.instance as f64,
                            upscale.frame.cleanrevision as f64,
                        ],
                    )
                });
                if !self.require_original_crop(&upscale.frame) {
                    return Task::none();
                }
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.bootstrap",
                        "",
                        "typed-bootstrap",
                        [
                            model.window_width.into(),
                            model.window_height.into(),
                            0.0,
                            0.0,
                        ],
                    )
                });
                self.reporting
                    .observe(|reporting| reporting.bootstrap(model, settings));
                if !self.viewer_scenario.is_empty() {
                    self.phase = Phase::TrainNavigation;
                    return self.arm(crate::view::navigation::stable_id(FeatureId::Train));
                }
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
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.ui_scale_drag",
                            SETTINGS_NUMERIC_CONTROLS[0],
                            "first-position",
                            [
                                f64::from(self.ui_scale_baseline),
                                f64::from(current),
                                f64::from(applied_scale),
                                1.0,
                            ],
                        )
                    });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.ui_scale_drag",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "second-position",
                        [
                            f64::from(self.ui_scale_baseline),
                            f64::from(current),
                            f64::from(applied_scale),
                            2.0,
                        ],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.ui_scale_drag",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "released-and-settled",
                        ui_scale_evidence(model, settings, self.ui_scale_baseline, applied_scale),
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.ui_scale_restored",
                        SETTINGS_NUMERIC_CONTROLS[0],
                        "baseline",
                        ui_scale_evidence(model, settings, self.ui_scale_baseline, applied_scale),
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
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
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.phase",
                        &advanced_field_id(0),
                        "advanced-field-0",
                        [0.0; 4],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.spinnerless",
                        &advanced_field_id(index),
                        if index == 2 {
                            "integer-upper-lower-edges"
                        } else {
                            "floating-upper-lower-edges"
                        },
                        [value, 1.0, 1.0, 1.0],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
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
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.error_modal",
                        ERROR_MODAL,
                        "copy-and-dismiss",
                        [1.0, 1.0, 1.0, 0.0],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
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
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.dataset_configured",
                        COMPILE_RESOLUTION,
                        "typed-settings",
                        [snapshot.revision as f64, 1.0, 0.0, 0.0],
                    )
                });
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
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.compile_progress",
                            COMPILE_PROGRESS,
                            &dataset.progress.activity,
                            [
                                dataset.generation as f64,
                                dataset.progress.completed as f64,
                                dataset.progress.total as f64,
                                dataset.progress.droppedinstances as f64,
                            ],
                        )
                    });
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.compile_metrics",
                            COMPILE_PROGRESS,
                            "elapsed-eta-throughput-dropped",
                            [
                                dataset.progress.elapsedseconds as f64,
                                dataset.progress.remainingseconds as f64,
                                dataset.progress.throughputpersecond as f64,
                                dataset.progress.droppedinstances as f64,
                            ],
                        )
                    });
                    self.phase = Phase::CompileProgress;
                    return self.arm(COMPILE_PROGRESS);
                }
                if compile_succeeded {
                    let split = dataset.inspection.splits.first();
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.dataset_complete",
                            DATASET_STATUS,
                            &dataset.terminal.artifact,
                            [
                                dataset.generation as f64,
                                split.map_or(0.0, |value| value.imagecount as f64),
                                split.map_or(0.0, |value| value.width as f64),
                                split.map_or(0.0, |value| value.height as f64),
                            ],
                        )
                    });
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
                    && model.explore_open_available() =>
            {
                use crate::presentation_surface::ExploreDisplay;
                let logical = model.explore.snapshot.as_ref();
                match crate::presentation_surface::explore_display(surface) {
                    Some(ExploreDisplay::Detail(_, _)) => {
                        if logical.is_some_and(|snapshot| {
                            snapshot.mode == crate::generated::ExploreMode::Detail
                        }) && model.explore_mutation_available()
                        {
                            self.phase = Phase::ExploreCloseDetail;
                            return self.arm(EXPLORE_DETAIL_CLOSE);
                        }
                        return Task::none();
                    }
                    Some(ExploreDisplay::Gallery(_, _)) => {
                        if logical.is_none_or(|snapshot| {
                            snapshot.mode != crate::generated::ExploreMode::Gallery
                        }) {
                            return Task::none();
                        }
                    }
                    None if logical.is_some_and(|snapshot| {
                        snapshot.ready || snapshot.mode == crate::generated::ExploreMode::Detail
                    }) =>
                    {
                        return Task::none();
                    }
                    None => {} // The initial empty page owns Open before any product exists.
                }
                if let Some(snapshot) = model
                    .explore
                    .snapshot
                    .as_ref()
                    .filter(|snapshot| snapshot.ready)
                    && let Some(message) = explore_scenario_preparation(snapshot)
                {
                    self.phase = Phase::AwaitExplorePreparation(snapshot.revision);
                    return explore_message(message);
                }
                if router.explore_measured_viewport(3, 0, 0).is_none() {
                    return Task::none();
                }
                self.phase = Phase::ExploreOpen;
                self.arm(EXPLORE_OPEN)
            }
            Phase::ExploreCloseDetail => self.arm(EXPLORE_DETAIL_CLOSE),
            Phase::AwaitExploreGallery
                if matches!(
                    crate::presentation_surface::explore_display(surface),
                    Some(crate::presentation_surface::ExploreDisplay::Gallery(_, _))
                ) && model.explore.snapshot.as_ref().is_some_and(|snapshot| {
                    snapshot.mode == crate::generated::ExploreMode::Gallery && !snapshot.busy
                }) && !model.explore.desired_close
                    && !model.has_explore_pending() =>
            {
                self.advance_to(Phase::AwaitExplore)
            }
            Phase::AwaitExplorePreparation(revision)
                if model
                    .explore
                    .snapshot
                    .as_ref()
                    .is_some_and(|snapshot| snapshot.revision > revision && !snapshot.busy)
                    && !model.has_explore_pending()
                    && !settings.has_local_edits()
                    && !model.native_settings_unsettled() =>
            {
                self.advance_to(Phase::AwaitExplore)
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
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.explore_extent_pending",
                                EXPLORE_GALLERY,
                                "measured-native-convergence",
                                [
                                    measured.extent.width as f64,
                                    measured.extent.height as f64,
                                    snapshot.viewport.extent.width as f64,
                                    snapshot.viewport.extent.height as f64,
                                ],
                            )
                        });
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_ready",
                            "",
                            "",
                            [
                                snapshot.dataset.imagecount as f64,
                                snapshot.dataset.imagewidth as f64,
                                snapshot.dataset.imageheight as f64,
                                snapshot.dataset.classnames.len() as f64,
                            ],
                        )
                    });
                    if self.viewer_scenario == "quiet" {
                        self.selection_grid = Some((
                            snapshot.viewport.columns,
                            snapshot.viewport.rowcount,
                            0,
                            snapshot.revision,
                            snapshot.frame.revision,
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
                    reporting::emit(|sink| {
                        sink.record(
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
                        )
                    });
                    reporting::emit(|sink| {
                        sink.record(
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
                        )
                    });
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
                    if reporting_enabled() && !self.workspace_fps_verified {
                        self.workspace_fps_baseline = crate::workspace_fps::enabled(settings);
                        self.workspace_fps_evidence = None;
                        self.workspace_fps_failure = None;
                        SURFACE_DRAW_OBSERVER
                            .with(|observer| observer.borrow_mut().fps_sample = None);
                        self.phase = Phase::AwaitWorkspaceFps;
                        return Task::done(RootMessage::Settings(
                            crate::view::settings::Message::PerformanceChanged(true),
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
                    reporting::emit(|sink| {
                        sink.record(
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
                        )
                    });
                    self.sweep_baseline = Some((snapshot.revision, snapshot.viewport.clone()));
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
                // Padding donors are background images. Their published tile
                // readiness, independent of annotations, must precede resizing.
                let initial_tiles_pending = initial_patch
                    && [7, 8].iter().any(|index| {
                        snapshot
                            .order
                            .visibleindices
                            .iter()
                            .position(|compiled| compiled == index)
                            .and_then(|slot| snapshot.gallery.slots.get(slot))
                            .copied()
                            != Some(true)
                    });
                let sampleable = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                );
                if initial_patch {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_patch_wait",
                            EXPLORE_GALLERY,
                            "revision-tiles-busy-publication",
                            [
                                f64::from(u8::from(revision_pending)),
                                f64::from(u8::from(initial_tiles_pending)),
                                f64::from(u8::from(snapshot.busy)),
                                f64::from(u8::from(sampleable.is_none())),
                            ],
                        )
                    });
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_patch_draw",
                            EXPLORE_GALLERY,
                            "drawn-and-sampleable-publication",
                            [
                                self.gallery_drawn
                                    .map_or(0.0, |(presentation, _)| presentation as f64),
                                self.gallery_drawn.map_or(0.0, |(_, source)| source as f64),
                                sampleable
                                    .map_or(0.0, |sample| sample.presentation_revision as f64),
                                sampleable.map_or(0.0, |sample| sample.source_revision as f64),
                            ],
                        )
                    });
                }
                if revision_pending
                    || initial_tiles_pending
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
                    let Some((size, maximum_extent)) = self.oversized_gallery.take() else {
                        self.fail("oversized Explore measurement lacks its original geometry");
                        return Task::none();
                    };
                    self.phase = Phase::ExploreDatasetPane;
                    // The capacity probe overrides the component's measurement,
                    // not the sensor's physical layout. No resize event is owed
                    // when the real bounds remain unchanged.
                    return explore_message(explore::Message::Gallery(
                        explore::gallery::Message::Measured {
                            size,
                            maximum_extent,
                            columns: snapshot.viewport.columns.max(1),
                        },
                    ))
                    .chain(self.arm(EXPLORE_DATASET_PANE));
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_initial_patch",
                        EXPLORE_GALLERY,
                        "stable-gallery-generation",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                let Some(size) = router.explore_gallery_size() else {
                    return Task::none();
                };
                let columns = snapshot.viewport.columns.max(1);
                let capacity = snapshot.maximumatlasextent.clone();
                self.oversized_gallery = Some((size, capacity.clone()));
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
                let Some((_, capacity)) = self.oversized_gallery.as_ref() else {
                    self.fail("oversized Explore capacity was not retained");
                    return Task::none();
                };
                let columns = snapshot.viewport.columns.max(1);
                let rows = snapshot.viewport.rowcount.max(1);
                // A different logical measurement may resolve to the same
                // bounded raster. That no-op has no new native revision.
                if snapshot.revision < revision
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_exact_grid",
                        EXPLORE_GALLERY,
                        "oversized-logical-fill",
                        [
                            snapshot.viewport.extent.width as f64,
                            snapshot.viewport.extent.height as f64,
                            columns as f64,
                            rows as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_exact_grid_capacity",
                        EXPLORE_GALLERY,
                        "measured-revision-capacity",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            capacity.width as f64,
                            capacity.height as f64,
                        ],
                    )
                });
                self.advance_to(Phase::AwaitExploreExactGridPatch {
                    revision: snapshot.revision,
                    frame_revision: snapshot.frame.revision,
                })
            }
            Phase::ExploreDatasetPane => self.arm(EXPLORE_DATASET_PANE),
            Phase::ExploreDetailsPane => self.arm(EXPLORE_DETAILS_PANE),
            Phase::ExploreNumericStart(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.explore_mutation_available()
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                {
                    return Task::none();
                }
                if index == 5 {
                    self.phase = Phase::ExplorePolicyOrderReady;
                    return iced::widget::operation::snap_to(
                        explore::DATASET_SCROLL_ID,
                        RelativeOffset::START,
                    );
                }
                let value = explore_integer_value(snapshot, index);
                self.explore_integer_baseline = value;
                self.explore_integer_target = match index {
                    0 => {
                        if value == 0 {
                            1
                        } else {
                            0
                        }
                    }
                    1 => {
                        if value == 10_000 {
                            9_999
                        } else {
                            10_000
                        }
                    }
                    2 => explore_seed_target(value),
                    3 => {
                        if value == 0 {
                            1
                        } else {
                            0
                        }
                    }
                    _ => value
                        .min(u64::from(snapshot.dataset.imagecount.saturating_sub(1)))
                        .saturating_sub(1),
                };
                self.explore_integer_revision = snapshot.revision;
                self.phase = Phase::ExploreNumericControl { index, step: 0 };
                self.arm(explore_integer_id(index))
            }
            Phase::ExploreNumericControl { index, .. } => self.arm(explore_integer_id(index)),
            Phase::ExploreNumericReveal { index, .. } => {
                self.arm_revealed(explore::DATASET_SCROLL_ID, explore_integer_id(index))
            }
            Phase::AwaitExploreNumeric { index, step } => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || model.has_explore_pending() {
                    return Task::none();
                }
                let value = explore_integer_value(snapshot, index);
                if step < 3 {
                    if value != self.explore_integer_baseline {
                        self.fail("Explore integer changed from an edge click or wheel");
                        return Task::none();
                    }
                    if step == 2 {
                        self.numeric_replacement = "x".to_owned();
                    }
                    self.phase = Phase::ExploreNumericControl {
                        index,
                        step: step + 1,
                    };
                    return self.arm(explore_integer_id(index));
                }
                if step == 3 {
                    if value != self.explore_integer_baseline {
                        self.fail("Explore unsigned integer accepted invalid text");
                        return Task::none();
                    }
                    self.numeric_replacement = self.explore_integer_target.to_string();
                    self.phase = Phase::ExploreNumericControl { index, step: 4 };
                    return self.arm(explore_integer_id(index));
                }
                let expected = if step == 4 || step == 6 {
                    self.explore_integer_target
                } else {
                    self.explore_integer_baseline
                };
                if value != expected
                    || snapshot.revision <= self.explore_integer_revision
                    || !explore_integer_persisted(model, index, expected)
                    || (step == 6 && !self.explore_paste_read)
                {
                    return Task::none();
                }
                if step == 4 {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_integer",
                            &explore_integer_id(index),
                            &value.to_string(),
                            [
                                self.explore_integer_revision as f64,
                                snapshot.revision as f64,
                                1.0,
                                1.0,
                            ],
                        )
                    });
                    self.explore_integer_revision = snapshot.revision;
                    self.numeric_replacement = self.explore_integer_baseline.to_string();
                    if index == 4 && self.explore_integer_baseline == u64::MAX {
                        self.phase = Phase::AwaitExploreNumeric { index, step: 5 };
                        return explore_message(explore::Message::Dataset(
                            explore::dataset::Message::UnlimitedCompiledIndex,
                        ));
                    }
                    self.phase = Phase::ExploreNumericControl { index, step: 5 };
                    self.arm(explore_integer_id(index))
                } else if index == 2 && step == 5 {
                    // The typed edit has already restored its baseline. Prepare
                    // clipboard contents separately; only the rendered widget
                    // may consume them through its ordinary paste shortcut.
                    self.explore_paste_read = false;
                    self.explore_integer_revision = snapshot.revision;
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_integer_paste_baseline",
                            &explore_integer_id(index),
                            &value.to_string(),
                            [snapshot.revision as f64, 0.0, 1.0, 1.0],
                        )
                    });
                    self.phase = Phase::AwaitExploreClipboard(snapshot.revision);
                    let generation = self.generation;
                    let revision = snapshot.revision;
                    iced::clipboard::write(self.explore_integer_target.to_string()).map(
                        move |result| {
                            RootMessage::Integration(Message::Scoped {
                                generation,
                                receipt: None,
                                message: Box::new(Message::NumberClipboardPrepared {
                                    revision,
                                    result,
                                }),
                            })
                        },
                    )
                } else if step == 6 {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_integer_paste",
                            &explore_integer_id(index),
                            &value.to_string(),
                            [
                                self.explore_integer_revision as f64,
                                snapshot.revision as f64,
                                1.0,
                                1.0,
                            ],
                        )
                    });
                    self.explore_integer_revision = snapshot.revision;
                    self.numeric_replacement = self.explore_integer_baseline.to_string();
                    self.phase = Phase::ExploreNumericControl { index, step: 7 };
                    self.arm(explore_integer_id(index))
                } else {
                    if step == 7 {
                        self.explore_paste_read = false;
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.explore_integer_paste_restored",
                                &explore_integer_id(index),
                                &value.to_string(),
                                [
                                    self.explore_integer_revision as f64,
                                    snapshot.revision as f64,
                                    1.0,
                                    1.0,
                                ],
                            )
                        });
                    }
                    self.advance_to(Phase::ExploreNumericStart(index + 1))
                }
            }
            Phase::ExplorePolicyOrderReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                reporting::emit(|sink| {
                    sink.record(
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
                    )
                });
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
                    || fully_drawn_gallery(frame, snapshot, self.gallery_drawn).is_none()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_augmentation",
                        EXPLORE_AUGMENTATION_TOGGLE,
                        "enabled-rendered-seed-zero",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
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
                    || fully_drawn_gallery(frame, snapshot, self.gallery_drawn).is_none()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_augmentation",
                        EXPLORE_AUGMENTATION_REROLL,
                        "rerolled-distinct-seed",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_reshuffle",
                        EXPLORE_RESHUFFLE,
                        "order-only",
                        [
                            shuffle_seed as f64,
                            snapshot.order.shuffleseed as f64,
                            augmentation_seed as f64,
                            snapshot.augmentation.seed as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_patch_baseline",
                        EXPLORE_CARD,
                        "reshuffle-placeholder",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            0.0,
                            0.0,
                        ],
                    )
                });
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
                        frame,
                        crate::generated::PresentationSourceKind::Explore,
                        snapshot.frame.revision,
                    )
                    .is_none()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_patch_observed",
                        EXPLORE_CARD,
                        "post-placeholder-frame",
                        [
                            revision as f64,
                            snapshot.revision as f64,
                            frame_revision as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
                self.sweep_baseline = Some((snapshot.revision, snapshot.viewport.clone()));
                if self.viewer_scenario.is_empty() {
                    self.phase = Phase::GalleryColdRead(
                        snapshot
                            .viewport
                            .firstrow
                            .saturating_add(snapshot.viewport.rowcount)
                            .saturating_add(COLD_GALLERY_ROW_GAP),
                    );
                    Task::none()
                } else {
                    self.phase = Phase::GallerySweep;
                    self.arm(EXPLORE_GALLERY)
                }
            }
            Phase::GalleryColdRead(row) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if model.has_explore_pending() || !model.explore_viewport_available() {
                    return Task::none();
                }
                // Leave space for the existing eight 96px wheels, the Later
                // row and the complete viewport. The five-row separation is
                // beyond the native four-neighbour prefetch window.
                let Some(offset) = cold_gallery_scroll_offset(
                    row,
                    &snapshot.viewport,
                    snapshot.order.matchingcount,
                    self.atlas_row_extent,
                ) else {
                    self.fail("Explore fixture has no cold interior viewport with sweep room");
                    return Task::none();
                };
                // A disjoint jump hides the incumbent atlas. First draw its
                // completed publication so the retained fallback can advance
                // and release the previous physical slot.
                if fully_drawn_gallery(frame, snapshot, self.gallery_drawn).is_none() {
                    return Task::none();
                }
                self.phase = Phase::AwaitGalleryColdRead(row, snapshot.gallery.generation);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset { x: 0.0, y: offset },
                )
            }
            Phase::AwaitGalleryColdRead(row, prior_generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.viewport.firstrow != row
                    || snapshot.gallery.generation <= prior_generation
                    || snapshot.gallery.layout.firstrow != row
                    || snapshot.gallery.layout.columns != snapshot.viewport.columns
                    || snapshot.gallery.layout.rowcount != snapshot.viewport.rowcount
                    || model.has_explore_pending()
                    || !model.explore_viewport_available()
                {
                    return Task::none();
                }
                if let Some((generation, index)) = self.gallery_completion_held {
                    if snapshot.gallery.generation < generation {
                        return Task::none();
                    }
                    if snapshot.gallery.generation != generation
                        || !snapshot.order.visibleindices.contains(&index)
                    {
                        self.fail(
                            "held gallery completion does not match the accepted cold viewport",
                        );
                        return Task::none();
                    }
                    self.sweep_baseline = Some((snapshot.revision, snapshot.viewport.clone()));
                    self.phase = Phase::GallerySweep;
                    return self.arm(EXPLORE_GALLERY);
                }
                // Reshuffle retains pixel identities. A previously unseen
                // order range can therefore already be cached. Only a fully
                // ready accepted range permits moving to the next disjoint
                // range; unfinished reads wait for their actual completion.
                if !snapshot.gallery.slots.is_empty()
                    && snapshot.gallery.slots.len() == snapshot.order.visibleindices.len()
                    && snapshot.gallery.slots.iter().all(|ready| *ready)
                {
                    self.phase = Phase::GalleryColdRead(
                        row.saturating_add(snapshot.viewport.rowcount)
                            .saturating_add(COLD_GALLERY_ROW_GAP),
                    );
                }
                Task::none()
            }
            Phase::GallerySweep => self.arm(EXPLORE_GALLERY),
            Phase::AwaitGallerySweep => {
                let Some((revision, viewport)) = &self.sweep_baseline else {
                    self.fail("Explore sweep baseline is unavailable");
                    return Task::none();
                };
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || snapshot.revision <= *revision
                    || &snapshot.viewport == viewport
                    || settings.has_local_edits()
                    || !model.explore_viewport_available()
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_sweep_observed",
                        EXPLORE_GALLERY,
                        "typed-viewport",
                        [
                            *revision as f64,
                            snapshot.revision as f64,
                            snapshot.viewport.extent.width as f64,
                            snapshot.viewport.extent.height as f64,
                        ],
                    )
                });
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
                self.reporting.observe(|reporting| reporting.reset_scroll());
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
                self.reporting
                    .observe(|reporting| reporting.scroll_placeholder(snapshot));
                if sampleable_presentation(
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_scrolled",
                        EXPLORE_GALLERY,
                        "",
                        [
                            baseline as f64,
                            snapshot.viewport.firstrow as f64,
                            index as f64,
                            0.0,
                        ],
                    )
                });
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
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                let Some(selected) = snapshot.selectedimage else {
                    return Task::none();
                };
                if selected != expected {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_pointer_mismatch",
                            EXPLORE_GALLERY,
                            "selected-image",
                            [
                                expected as f64,
                                selected as f64,
                                snapshot.revision as f64,
                                snapshot.frame.revision as f64,
                            ],
                        )
                    });
                    self.fail("gallery pointer selection did not match the rendered grid slot");
                    return Task::none();
                }
                if let Some((_, _, slot, revision, _)) = self.selection_grid {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.explore_pointer_selected",
                            EXPLORE_GALLERY,
                            "selected-from-dispatched-pointer",
                            [
                                revision as f64,
                                slot as f64,
                                f64::from(expected),
                                f64::from(selected),
                            ],
                        )
                    });
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_detail",
                        "",
                        "",
                        [selected as f64, snapshot.frame.revision as f64, 0.0, 0.0],
                    )
                });
                if snapshot.detail.showoriginaldimensions
                    || snapshot.frame.extent.width != snapshot.dataset.imagewidth
                    || snapshot.frame.extent.height != snapshot.dataset.imageheight
                {
                    self.fail("initial Explore detail was not the native padded product");
                    return Task::none();
                }
                if sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                )
                .is_none()
                {
                    return Task::none();
                }
                if self.viewer_scenario == "quiet" {
                    if crate::presentation_surface::viewer_annotation_request().is_none() {
                        return Task::none();
                    }
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_detail_source",
                        EXPLORE_DETAIL_ORIGINAL,
                        "padded-to-original-sampling",
                        [
                            padded_width as f64,
                            padded_height as f64,
                            content.width as f64,
                            content.height as f64,
                        ],
                    )
                });
                self.phase = Phase::DetailFit;
                self.arm(explore::DETAIL_FIT_ID)
            }
            Phase::DetailFit => self.arm(explore::DETAIL_FIT_ID),
            Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(_)
            | Phase::AtlasAwaySelect(_) => self.arm(EXPLORE_GALLERY),
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_capacity",
                        explore::GALLERY_CAPACITY_ID,
                        "native-visible-capacity-exceeded",
                        [1.0, 0.0, 0.0, 0.0],
                    )
                });
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
                }) && matches!(
                    crate::presentation_surface::explore_display(surface),
                    Some(crate::presentation_surface::ExploreDisplay::Gallery(shown, metadata))
                        if metadata.order.matchingcount == 0
                            && shown.frame.is_some_and(|frame| {
                                self.gallery_drawn
                                    == Some((frame.presentation_revision, metadata.frame.revision))
                            })
                ) =>
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
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_window_wait",
                            EXPLORE_GALLERY,
                            if fullscreen { "fullscreen" } else { "restored" },
                            [
                                settled as u8 as f64,
                                (snapshot.busy || model.has_explore_pending()) as u8 as f64,
                                gallery_matches as u8 as f64,
                                viewport_matches as u8 as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_window_draw",
                        EXPLORE_GALLERY,
                        if fullscreen { "fullscreen" } else { "restored" },
                        [
                            snapshot.frame.revision as f64,
                            model.window_width as f64,
                            model.window_height as f64,
                            snapshot.viewport.columns as f64,
                        ],
                    )
                });
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
            Phase::VisibleReadScroll(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let row = index / snapshot.viewport.columns.max(1);
                self.phase = Phase::AwaitVisibleRead(index);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.atlas_row_extent * (row as f32 + 0.5),
                    },
                )
            }
            Phase::AwaitVisibleReadHover(_, _) => Task::none(),
            Phase::AwaitVisibleReadPixels(index, generation)
            | Phase::VisibleReadSelect(index, generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    self.fail("held read is not an immediate visible image");
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&false) {
                    self.fail("held read lost its explicit placeholder");
                    return Task::none();
                }
                let columns = draw.snapshot.gallery.layout.columns;
                let scale = draw.image.width / (self.atlas_row_extent * columns as f32);
                let side = self.atlas_row_extent;
                let target = Rectangle {
                    x: draw.image.x / scale + (slot as u32 % columns) as f32 * side,
                    y: draw.image.y / scale + (slot as u32 / columns) as f32 * side,
                    width: side,
                    height: side,
                };
                let visible_clip = Rectangle {
                    x: draw.clip.x / scale,
                    y: draw.clip.y / scale,
                    width: draw.clip.width / scale,
                    height: draw.clip.height / scale,
                };
                let Some(target) = target.intersection(&visible_clip) else {
                    self.fail("held read target does not intersect the actual gallery clip");
                    return Task::none();
                };
                if matches!(self.phase, Phase::AwaitVisibleReadPixels(_, _)) {
                    crate::presentation_surface::trace_atlas_stage("held-visible", draw);
                    self.phase = Phase::AwaitVisibleReadHover(index, generation);
                    #[cfg(target_arch = "wasm32")]
                    if hover_after_surface_draw_js(
                        f64::from(target.center_x()),
                        f64::from(target.center_y()),
                        EXPLORE_GALLERY,
                        snapshot.frame.revision as f64,
                    ) != 1
                    {
                        self.fail("held-placeholder hover dispatch failed");
                    }
                } else {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.pending_hover",
                            EXPLORE_GALLERY,
                            "shared-mouse-placeholder-target",
                            [
                                index as f64,
                                generation as f64,
                                snapshot.frame.revision as f64,
                                draw.surface.frame.unwrap().presentation_revision as f64,
                            ],
                        )
                    });
                    self.phase = Phase::AwaitVisibleReadSelection(index, generation);
                    if !click_after_surface_draw(
                        target,
                        EXPLORE_GALLERY,
                        snapshot.frame.revision,
                        false,
                    ) {
                        self.fail("held-placeholder selection dispatch failed");
                    }
                }
                Task::none()
            }
            Phase::AwaitVisibleReadSelection(index, generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !self.detail_drawn(frame, snapshot)
                    || snapshot.selectedimage != Some(index)
                    || snapshot.busy
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.pending_selection",
                        EXPLORE_GALLERY,
                        "typed-image-before-thumbnail-read",
                        [
                            index as f64,
                            generation as f64,
                            snapshot.frame.revision as f64,
                            0.0,
                        ],
                    )
                });
                self.phase = Phase::AwaitVisibleReadReturn(index, generation);
                explore_message(explore::Message::Detail(
                    explore::detail::Message::CloseRequested,
                ))
            }
            Phase::AwaitVisibleReadReturn(index, generation) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&false) {
                    self.fail("pending gallery return completed before held read release");
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage("held-return", draw);
                self.phase = Phase::AwaitVisibleReadOscillation(index, generation, 0);
                let row = index / snapshot.viewport.columns.max(1);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.atlas_row_extent * row as f32,
                    },
                )
            }
            Phase::AwaitVisibleReadOscillation(index, generation, stage) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let row = index / snapshot.viewport.columns.max(1);
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.viewport.firstrow != row
                    || snapshot.viewport.rowcount != self.atlas_return_rows + u32::from(stage == 1)
                {
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&false) {
                    self.fail("held read escaped before pending-return oscillation completed");
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage(
                    ["held-aligned", "held-extra", "held-restored"][stage as usize],
                    draw,
                );
                if stage == 2 {
                    return self.advance_to(Phase::VisibleReadRelease(index, generation));
                }
                self.phase = Phase::AwaitVisibleReadOscillation(index, generation, stage + 1);
                iced::widget::operation::scroll_to(
                    EXPLORE_GALLERY,
                    AbsoluteOffset {
                        x: 0.0,
                        y: self.atlas_row_extent
                            * (row as f32
                                + if stage == 1 {
                                    0.0
                                } else {
                                    self.atlas_scroll_fraction
                                }),
                    },
                )
            }
            Phase::VisibleReadRelease(index, _) => {
                if self.control_phase.as_ref() == Some(&self.phase) {
                    self.phase = Phase::AwaitVisibleReadComplete(index);
                }
                Task::none()
            }
            Phase::AwaitVisibleReadComplete(index) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let Some(slot) = draw.visible_slot(index) else {
                    return Task::none();
                };
                if draw.snapshot.gallery.slots.get(slot) != Some(&true) {
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage("held-complete", draw);
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.phase = Phase::AtlasAwaySelect(0);
                self.arm(EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasAwayFilter(step, baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.busy
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                    || snapshot.revision <= baseline
                    || self.confirmed_atlas(snapshot).is_none()
                {
                    return Task::none();
                }
                // Filter/order updates deliberately return to Gallery. Observe
                // that completed product before reopening detail for the next
                // change; the acceptance driver follows the ordinary API.
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.phase = Phase::AtlasAwaySelect(step);
                self.arm(EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasAwayDetail(step, baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !self.detail_drawn(frame, snapshot)
                    || snapshot.busy
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                    || snapshot.revision <= baseline
                {
                    return Task::none();
                }
                self.phase = if step < 3 {
                    Phase::AwaitAtlasAwayFilter(step + 1, snapshot.revision)
                } else {
                    Phase::AwaitAtlasAwayDetail(step + 1, snapshot.revision)
                };
                match step {
                    0 => explore_message(explore::Message::Dataset(
                        explore::dataset::Message::MinimumCompiledIndexChanged(1),
                    )),
                    1 => explore_message(explore::Message::Dataset(
                        explore::dataset::Message::OrderSelected(
                            crate::generated::ExploreOrder::Shuffled,
                        ),
                    )),
                    2 => explore_message(explore::Message::Dataset(
                        explore::dataset::Message::ShuffleSeedChanged(173),
                    )),
                    3 => explore_message(explore::Message::Gallery(
                        explore::gallery::Message::Overlay(
                            explore::overlay::Message::BoxesToggled(!snapshot.overlay.showboxes),
                        ),
                    )),
                    _ => {
                        self.phase = Phase::AwaitAtlasAwayReturn;
                        explore_message(explore::Message::Detail(
                            explore::detail::Message::CloseRequested,
                        ))
                    }
                }
            }
            Phase::AwaitAtlasAwayReturn | Phase::AwaitAtlasAwayRestore => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.busy
                    || settings.has_local_edits()
                    || model.has_explore_pending()
                {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                if matches!(self.phase, Phase::AwaitAtlasAwayReturn) {
                    if snapshot.filter.minimumcompiledindex != 1
                        || snapshot.filter.order != crate::generated::ExploreOrder::Shuffled
                        || snapshot.order.shuffleseed != 173
                    {
                        self.fail("gallery return lost filter, order, or seed changed in detail");
                        return Task::none();
                    }
                    crate::presentation_surface::trace_atlas_stage("away-return", draw);
                    self.phase = Phase::AwaitAtlasAwayRestore;
                }
                if let Some(message) = explore_scenario_preparation(snapshot) {
                    return explore_message(message);
                }
                self.scroll_atlas(0)
            }
            Phase::AwaitCapacitySlots(baseline) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.frame.revision == baseline
                    || crate::presentation_surface::capacity_acceptance_slots() != 2
                    || self
                        .gallery_drawn
                        .is_none_or(|(_, source)| source != snapshot.frame.revision)
                {
                    return Task::none();
                }
                self.phase = Phase::AwaitCapacityArm;
                Task::none()
            }
            Phase::CapacityPublish => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if !crate::presentation_surface::release_capacity_sample() {
                    self.fail("capacity acquisition lacks its held physical sample");
                    return Task::none();
                }
                self.phase = Phase::AwaitCapacityCompletion;
                explore_message(explore::Message::Gallery(
                    explore::gallery::Message::Overlay(explore::overlay::Message::BoxesToggled(
                        !snapshot.overlay.showboxes,
                    )),
                ))
            }
            Phase::AwaitCapacityRetry => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || model.has_explore_pending() {
                    return Task::none();
                }
                let Some(draw) = self.confirmed_atlas(snapshot) else {
                    return Task::none();
                };
                let ready = draw.surface.frame.expect("drawn capacity retry");
                reporting::emit(|sink| {
                    sink.record(
                        "integration.capacity_retry",
                        EXPLORE_GALLERY,
                        "completed-acquisition-after-physical-slot-release",
                        [
                            ready.content_sequence as f64,
                            ready.presentation_revision as f64,
                            snapshot.revision as f64,
                            0.0,
                        ],
                    )
                });
                crate::presentation_surface::end_capacity_acceptance();
                self.phase = Phase::AwaitAtlasWindow(true);
                #[cfg(target_arch = "wasm32")]
                fullscreen_js(true);
                Task::none()
            }
            Phase::AtlasResizeStart => {
                self.resize_original_size = router.explore_gallery_size();
                self.resize_previous_size = self.resize_original_size;
                self.phase = Phase::AwaitAtlasResizeGallery(0);
                #[cfg(target_arch = "wasm32")]
                if !canvas_size_js(1500.0, 600.0) {
                    self.fail("cannot size the acceptance canvas");
                }
                Task::none()
            }
            Phase::AwaitAtlasResizeGallery(step) => {
                let (Some(snapshot), Some(size)) = (
                    model.explore.snapshot.as_ref(),
                    router.explore_gallery_size(),
                ) else {
                    return Task::none();
                };
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.busy
                    || model.has_explore_pending()
                    || (step == 3 && Some(size) != self.resize_original_size)
                {
                    return Task::none();
                }
                #[cfg(target_arch = "wasm32")]
                if step < 3 {
                    let (width, height) = if step == 1 {
                        (1000.0, 1020.0)
                    } else {
                        (1500.0, 600.0)
                    };
                    if !canvas_size_settled_js(width, height)
                        || model.window_width != width as u32
                        || model.window_height != height as u32
                    {
                        reporting::emit(|sink| {
                            sink.record(
                                "integration.atlas_resize_wait",
                                EXPLORE_GALLERY,
                                "window",
                                [
                                    model.window_width as f64,
                                    model.window_height as f64,
                                    width,
                                    height,
                                ],
                            )
                        });
                        return Task::none();
                    }
                }
                let Some(expected) = router.explore_measured_layout_request(
                    Some(snapshot),
                    snapshot.viewport.columns,
                    snapshot.order.matchingcount,
                ) else {
                    return Task::none();
                };
                if snapshot.viewport != expected.viewport {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_resize_wait",
                            EXPLORE_GALLERY,
                            "viewport",
                            [
                                snapshot.viewport.extent.width as f64,
                                snapshot.viewport.extent.height as f64,
                                expected.viewport.extent.width as f64,
                                expected.viewport.extent.height as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_resize_wait",
                            EXPLORE_GALLERY,
                            "pixels",
                            [
                                snapshot.frame.revision as f64,
                                self.atlas_receipt
                                    .as_ref()
                                    .map_or(0, |draw| draw.snapshot.frame.revision)
                                    as f64,
                                self.atlas_pixels
                                    .as_ref()
                                    .map_or(0, |draw| draw.snapshot.frame.revision)
                                    as f64,
                                snapshot.viewport.rowcount as f64,
                            ],
                        )
                    });
                    return Task::none();
                };
                let required_rows = atlas_scroll_window(size, snapshot.viewport.columns).0;
                if snapshot.viewport.rowcount < required_rows
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                {
                    return Task::none();
                }
                if step < 3 {
                    crate::presentation_surface::trace_atlas_stage(
                        [
                            "resize-landscape",
                            "resize-portrait-return",
                            "resize-landscape-return",
                        ][step as usize],
                        draw,
                    );
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.atlas_resize",
                            EXPLORE_GALLERY,
                            ["landscape", "portrait-return", "landscape-return"][step as usize],
                            [
                                f64::from(size.width),
                                f64::from(size.height),
                                f64::from(required_rows),
                                f64::from(snapshot.viewport.rowcount),
                            ],
                        )
                    });
                }
                if step == 2 {
                    self.phase = Phase::AwaitAtlasResizeGallery(3);
                    #[cfg(target_arch = "wasm32")]
                    restore_canvas_size_js();
                    return Task::none();
                }
                if step == 3 {
                    self.resize_original_size = None;
                    self.resize_previous_size = None;
                    return self.advance_to(Phase::AwaitAtlasReturnReady);
                }
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    snapshot.viewport.rowcount,
                    snapshot.viewport.firstrow,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.resize_previous_size = Some(size);
                self.phase = Phase::AtlasResizeSelect(step);
                self.arm(EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasResizeDetail(step) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || !self.detail_drawn(frame, snapshot) {
                    return Task::none();
                }
                self.phase = Phase::AwaitAtlasResizeMeasurement(step * 2);
                #[cfg(target_arch = "wasm32")]
                {
                    let (width, height) = atlas_resize_dimensions(step * 2);
                    if !canvas_size_js(width, height) {
                        self.fail("cannot resize canvas beneath Detail");
                    }
                }
                Task::none()
            }
            Phase::AwaitAtlasResizeMeasurement(step) => {
                let Some(size) = router.explore_gallery_size() else {
                    return Task::none();
                };
                if Some(size) == self.resize_previous_size {
                    return Task::none();
                }
                #[cfg(target_arch = "wasm32")]
                {
                    let (width, height) = atlas_resize_dimensions(step);
                    if !canvas_size_settled_js(width, height)
                        || model.window_width != width as u32
                        || model.window_height != height as u32
                    {
                        return Task::none();
                    }
                }
                self.resize_previous_size = Some(size);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_resize_measured",
                        EXPLORE_GALLERY,
                        "detail-layout",
                        [
                            f64::from(step),
                            f64::from(size.width),
                            f64::from(size.height),
                            1.0,
                        ],
                    )
                });
                if step % 2 == 0 {
                    self.phase = Phase::AwaitAtlasResizeMeasurement(step + 1);
                    #[cfg(target_arch = "wasm32")]
                    {
                        let (width, height) = atlas_resize_dimensions(step + 1);
                        if !canvas_size_js(width, height) {
                            self.fail("cannot repeat canvas resize beneath Detail");
                        }
                    }
                    Task::none()
                } else {
                    self.phase = Phase::AwaitAtlasResizeGallery(step / 2 + 1);
                    explore_message(explore::Message::Detail(
                        explore::detail::Message::CloseRequested,
                    ))
                }
            }
            Phase::AwaitAtlasReturnReady => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let Some(size) = router.explore_gallery_size() else {
                    return Task::none();
                };
                let (rows, fraction) = atlas_scroll_window(size, snapshot.viewport.columns);
                if snapshot.busy
                    || model.has_explore_pending()
                    || snapshot.viewport.rowcount != rows
                    || snapshot.viewport.firstrow != 0
                    || snapshot.gallery.slots.iter().any(|ready| !*ready)
                {
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    return Task::none();
                };
                crate::presentation_surface::trace_atlas_stage("return-cached", draw);
                self.atlas_return_rows = rows;
                self.atlas_scroll_fraction = fraction;
                self.selection_grid = Some((
                    snapshot.viewport.columns,
                    rows,
                    0,
                    snapshot.revision,
                    snapshot.frame.revision,
                ));
                self.phase = Phase::AtlasReturnSelect;
                self.arm(EXPLORE_GALLERY)
            }
            Phase::AwaitAtlasReturnDetail => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot.busy || !self.detail_drawn(frame, snapshot) {
                    return Task::none();
                }
                self.phase = Phase::AwaitAtlasOscillation(0);
                explore_message(explore::Message::Detail(
                    explore::detail::Message::CloseRequested,
                ))
            }
            Phase::AwaitAtlasOscillation(stage) => {
                let Some(snapshot) = model.explore.snapshot.as_ref() else {
                    return Task::none();
                };
                let expected = self.atlas_return_rows + u32::from(stage == 1);
                if snapshot.mode != crate::generated::ExploreMode::Gallery
                    || snapshot.viewport.rowcount != expected
                    || snapshot.viewport.firstrow != 0
                    || model.has_explore_pending()
                {
                    return Task::none();
                }
                let Some(draw) = self
                    .confirmed_atlas(snapshot)
                    .filter(|draw| draw.snapshot.viewport == snapshot.viewport)
                else {
                    return Task::none();
                };
                crate::presentation_surface::trace_atlas_stage(
                    ["return-aligned", "return-extra", "return-restored"][stage as usize],
                    draw,
                );
                if stage < 2 {
                    self.phase = Phase::AwaitAtlasOscillation(stage + 1);
                    iced::widget::operation::scroll_to(
                        EXPLORE_GALLERY,
                        AbsoluteOffset {
                            x: 0.0,
                            y: if stage == 0 {
                                self.atlas_row_extent * self.atlas_scroll_fraction
                            } else {
                                0.0
                            },
                        },
                    )
                } else {
                    if self.session.profile == "retained" && self.viewer_scenario == "rapid" {
                        self.phase = Phase::AwaitVisibleReadArm(snapshot.viewport.columns * 10);
                        return Task::none();
                    }
                    self.scroll_atlas(0)
                }
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
                    0..=4 => {
                        let fractional = matches!(stage, 0 | 2);
                        snapshot.viewport.firstrow == [0, 1, 2, 10, 9][stage as usize]
                            && snapshot.viewport.rowcount
                                == self.atlas_return_rows + u32::from(fractional)
                            && if fractional {
                                self.atlas_clip.0 > 0.0
                            } else {
                                self.atlas_clip.0.abs() < 1.0
                            }
                    }
                    5 => {
                        snapshot.viewport.firstrow + snapshot.viewport.rowcount == total_rows
                            && self.atlas_clip.1.abs() < 1.0
                    }
                    _ => snapshot.viewport.firstrow == 0 && self.atlas_clip.0.abs() < 1.0,
                };
                if !settled {
                    return Task::none();
                }
                crate::presentation_surface::trace_atlas_stage(
                    [
                        "fractional",
                        "row1",
                        "row2",
                        "row10",
                        "row9",
                        "end",
                        "restored",
                    ][stage as usize],
                    receipt,
                );
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_scroll",
                        EXPLORE_GALLERY,
                        [
                            "fractional",
                            "row1",
                            "row2",
                            "row10",
                            "row9",
                            "end",
                            "restored",
                        ][stage as usize],
                        [
                            snapshot.frame.revision as f64,
                            snapshot.viewport.firstrow as f64,
                            self.atlas_clip.0 as f64,
                            self.atlas_clip.1 as f64,
                        ],
                    )
                });
                match stage {
                    0..=5 => self.scroll_atlas(stage + 1),
                    _ => {
                        if self.session.profile == "retained" && self.viewer_scenario == "rapid" {
                            if !crate::presentation_surface::begin_capacity_acceptance() {
                                self.fail("capacity acceptance lacks retained fallback");
                                return Task::none();
                            }
                            self.phase = Phase::AwaitCapacitySlots(snapshot.frame.revision);
                            explore_message(explore::Message::Gallery(
                                explore::gallery::Message::Overlay(
                                    explore::overlay::Message::BoxesToggled(
                                        !snapshot.overlay.showboxes,
                                    ),
                                ),
                            ))
                        } else {
                            self.phase = Phase::AwaitAtlasWindow(true);
                            #[cfg(target_arch = "wasm32")]
                            fullscreen_js(true);
                            Task::none()
                        }
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.atlas_checkbox",
                        overlay_control(index, false),
                        "rendered-saved-visibility",
                        [
                            index as f64,
                            expected as f64,
                            snapshot.frame.revision as f64,
                            snapshot.augmentation.seed as f64,
                        ],
                    )
                });
                self.atlas_baseline = Some((snapshot.frame.revision, snapshot.augmentation.seed));
                if index == 7 {
                    let continuation = self.advance_to(Phase::AtlasResizeStart);
                    iced::widget::operation::snap_to(EXPLORE_GALLERY, RelativeOffset::START)
                        .chain(continuation)
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_overlay",
                        overlay_control(index, true),
                        "actual-draw",
                        [
                            index as f64,
                            actual as f64,
                            snapshot.frame.revision as f64,
                            snapshot.frame.cleanrevision as f64,
                        ],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.gallery_no_input_complete",
                        EXPLORE_GALLERY,
                        "matching-pixels-and-semantics",
                        [
                            snapshot.gallery.generation as f64,
                            snapshot.gallery.slots.len() as f64,
                            snapshot.frame.revision as f64,
                            snapshot.augmentation.seed as f64,
                        ],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_detail_fit",
                        explore::DETAIL_FIT_ID,
                        "centered-contained",
                        [
                            drawn.container.width as f64,
                            drawn.container.height as f64,
                            drawn.image.width as f64,
                            drawn.image.height as f64,
                        ],
                    )
                });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_action_arm",
                        EXPLORE_UPSCALE_ACTIONS[0],
                        "after-detail-fit",
                        [
                            0.0,
                            snapshot.frame.extent.width as f64,
                            snapshot.frame.extent.height as f64,
                            snapshot.frame.revision as f64,
                        ],
                    )
                });
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
                    reporting::upscale_settlement(self, model, frame, "current_upscale_missing");
                    return Task::none();
                };
                if upscale.kernel != crate::generated::UPSCALE_KERNEL_VALUES[kernel] {
                    reporting::upscale_settlement(self, model, frame, "requested_kernel_mismatch");
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Upscale,
                    upscale.frame.revision,
                ) else {
                    reporting::upscale_settlement(
                        self,
                        model,
                        frame,
                        "sampleable_presentation_mismatch",
                    );
                    return Task::none();
                };
                if upscale.busy || !upscale.ready {
                    reporting::upscale_settlement(self, model, frame, "native_work_pending");
                    return Task::none();
                }
                if upscale.frame.revision != upscale_frame_revision
                    && sampleable.presentation_revision <= presentation_revision
                {
                    reporting::upscale_settlement(
                        self,
                        model,
                        frame,
                        "presentation_not_newer_than_baseline",
                    );
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_growth",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        upscale_acceptance_label(crate::generated::UPSCALE_KERNEL_VALUES[kernel]),
                        [
                            source_width as f64,
                            source_height as f64,
                            upscale.frame.extent.width as f64,
                            upscale.frame.extent.height as f64,
                        ],
                    )
                });
                if sampleable.content_width != expected_width
                    || sampleable.content_height != expected_height
                    || sampleable.capability_width < expected_width
                    || sampleable.capability_height < expected_height
                {
                    self.fail("Presentation did not import the exact Upscale output capability");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_presentation",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "complete-four-times-exported-frame",
                        [
                            sampleable.content_width as f64,
                            sampleable.content_height as f64,
                            sampleable.capability_width as f64,
                            sampleable.capability_height as f64,
                        ],
                    )
                });
                if model.displayed_upscale_kernel() != Some(upscale.kernel) {
                    reporting::upscale_settlement(self, model, frame, "displayed_kernel_mismatch");
                    return Task::none();
                }
                let Some((drawn, source, viewer)) =
                    self.viewer_drawn.filter(|(drawn, source, _)| {
                        *drawn == sampleable.presentation_revision
                            && *source == upscale.frame.revision
                    })
                else {
                    reporting::upscale_settlement(
                        self,
                        model,
                        frame,
                        "matching_viewer_draw_missing",
                    );
                    return Task::none();
                };
                let Some(button) = self.upscale_button else {
                    reporting::upscale_settlement(self, model, frame, "method_button_missing");
                    return Task::none();
                };
                let Some(receipt) = current_receipt(explore::DETAIL_WORKSPACE_ID) else {
                    reporting::upscale_settlement(self, model, frame, "draw_probe_receipt_missing");
                    return Task::none();
                };
                if self.upscale_pixel_pending.as_ref() != Some(&receipt) {
                    reporting::upscale_settlement(self, model, frame, "probe_receipt_not_current");
                    if let Some(output) = self.prepare_upscale_probe(viewer.image, source, drawn) {
                        sample_upscale_pixels(output, viewer.image, button, source, drawn);
                    }
                    return Task::none();
                }
                let Some((pixel_source, pixel_presentation, checksum, blue)) = self.upscale_pixels
                else {
                    reporting::upscale_settlement(self, model, frame, "probe_pixels_pending");
                    return Task::none();
                };
                if pixel_source != source || pixel_presentation != drawn {
                    reporting::upscale_settlement(
                        self,
                        model,
                        frame,
                        "probe_pixels_frontier_mismatch",
                    );
                    if let Some(output) = self.prepare_upscale_probe(viewer.image, source, drawn) {
                        sample_upscale_pixels(output, viewer.image, button, source, drawn);
                    }
                    return Task::none();
                }
                if checksum == 0 || blue < 32 {
                    self.fail("Upscale actual canvas image or completed blue method did not match its displayed result");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_completed_pixels",
                        EXPLORE_UPSCALE_ACTIONS[kernel],
                        "exact-completed-blue",
                        [source as f64, drawn as f64, checksum as f64, blue as f64],
                    )
                });
                if let Some(revision) = self.upscale_repeat_revision {
                    if !self.upscale_repeat_observed {
                        reporting::upscale_settlement(
                            self,
                            model,
                            frame,
                            "repeat_request_not_observed",
                        );
                        return Task::none();
                    }
                    if upscale.revision != revision {
                        self.fail(
                            "Same completed Upscale method unnecessarily submitted native work",
                        );
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.upscale_same_method",
                            EXPLORE_UPSCALE_ACTIONS[kernel],
                            "same-completed-result",
                            [source as f64, drawn as f64, revision as f64, 1.0],
                        )
                    });
                } else {
                    self.upscale_repeat_revision = Some(upscale.revision);
                    if !click(button) {
                        self.fail("Upscale completed method re-click failed");
                    }
                    reporting::upscale_settlement(self, model, frame, "repeat_request_dispatched");
                    return Task::none();
                }
                if self.upscale_cache_pass {
                    if self.upscale_cached_frames[kernel].as_ref() != Some(&upscale.frame) {
                        self.fail("cached method changed its completed physical product");
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.upscale_cached",
                            EXPLORE_UPSCALE_ACTIONS[kernel],
                            "same-resident-product-drawn",
                            [
                                source as f64,
                                drawn as f64,
                                kernel as f64,
                                upscale.revision as f64,
                            ],
                        )
                    });
                    if kernel + 1 == EXPLORE_UPSCALE_ACTIONS.len()
                        && self.viewer_continuity_request.is_none()
                    {
                        let Some(snapshot) = model.settings_snapshot.as_ref() else {
                            reporting::upscale_settlement(
                                self,
                                model,
                                frame,
                                "continuity_settings_missing",
                            );
                            return Task::none();
                        };
                        if !route_edit_available(model, settings) {
                            reporting::upscale_settlement(
                                self,
                                model,
                                frame,
                                "continuity_route_unavailable",
                            );
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
                        reporting::upscale_settlement(
                            self,
                            model,
                            frame,
                            "annotation_handoff_draw_missing",
                        );
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
                        reporting::upscale_settlement(
                            self,
                            model,
                            frame,
                            "semantic_handoff_draw_missing",
                        );
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
                        reporting::upscale_settlement(
                            self,
                            model,
                            frame,
                            "rapid_completion_draw_missing",
                        );
                        return Task::none();
                    }
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.viewer_complete",
                            EXPLORE_UPSCALE_ACTIONS[kernel],
                            "rapid",
                            [
                                sampleable.presentation_revision as f64,
                                upscale.frame.revision as f64,
                                1.0,
                                kernel as f64,
                            ],
                        )
                    });
                    self.phase = Phase::Complete;
                    return Task::none();
                }
                if kernel + 1 < EXPLORE_UPSCALE_ACTIONS.len() {
                    let next_kernel = kernel + 1;
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.upscale_action_arm",
                            EXPLORE_UPSCALE_ACTIONS[next_kernel],
                            "after-upscale-publication",
                            [
                                next_kernel as f64,
                                upscale.revision as f64,
                                upscale.frame.revision as f64,
                                sampleable.presentation_revision as f64,
                            ],
                        )
                    });
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
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                let Some(sampleable) = sampleable_presentation(
                    frame,
                    crate::generated::PresentationSourceKind::Explore,
                    snapshot.frame.revision,
                ) else {
                    return Task::none();
                };
                if sampleable.presentation_revision <= self.annotation_sample_baseline {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.upscale_later_frame",
                        EXPLORE_GALLERY,
                        "distinct-imported-frame",
                        [
                            self.annotation_sample_baseline as f64,
                            sampleable.presentation_revision as f64,
                            sampleable.content_width as f64,
                            sampleable.content_height as f64,
                        ],
                    )
                });
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
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                self.annotation_sample_baseline = crate::presentation_surface::retained_surface()
                    .and_then(|surface| surface.frame)
                    .map_or(0, |frame| frame.presentation_revision);
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
                self.reporting.observe(|reporting| {
                    reporting.reopen_wait(
                        frame,
                        snapshot,
                        revision,
                        frame_revision,
                        self.selection_grid.is_some(),
                        self.gallery_drawn,
                    )
                });
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
                        || frame.is_none_or(|frame| frame.presentation_revision != presentation)
                }) {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_reopened",
                        EXPLORE_OPEN,
                        "usable-after-reopen",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            snapshot.order.visibleindices.len() as f64,
                            snapshot.dataset.imagecount as f64,
                        ],
                    )
                });
                reporting::emit(|sink| {
                    sink.record(
                        "integration.explore_final_cursor",
                        EXPLORE_GALLERY,
                        "coherent-placeholder",
                        [
                            snapshot.revision as f64,
                            snapshot.frame.revision as f64,
                            snapshot.gallery.generation as f64,
                            snapshot.order.visibleindices.len() as f64,
                        ],
                    )
                });
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
                if snapshot.busy
                    || snapshot.mode != crate::generated::ExploreMode::Detail
                    || displayed_detail(surface, snapshot).is_none()
                {
                    return Task::none();
                }
                if self.viewer_drawn.is_none_or(|(presentation, source, _)| {
                    model
                        .viewed_explore_frame()
                        .is_none_or(|viewed| source != viewed.revision)
                        || frame.is_none_or(|frame| frame.presentation_revision != presentation)
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
                reporting::emit(|sink| {
                    sink.record(
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
                    )
                });
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
            Phase::CopyListSetup { stage, .. } => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if !model.annotation_edit_available()
                    || model
                        .has_pending(crate::generated::ApplicationIntentEndpoint::AnnotationEdit)
                    || snapshot.busy
                    || snapshot.uirevision <= self.copy_list_revision
                {
                    return Task::none();
                }
                let ui = &snapshot.ui;
                let Some(selected) = ui
                    .editor
                    .selectedobject
                    .and_then(|index| ui.scene.objects.get(index as usize))
                else {
                    self.fail("Annotation long-list setup requires a selected object");
                    return Task::none();
                };
                self.copy_list_revision = snapshot.uirevision;
                self.phase = Phase::CopyListSetup {
                    stage,
                    revision: snapshot.uirevision,
                };
                use annotation::sidebar::Message as Sidebar;
                let command = match stage {
                    0 => {
                        self.phase = Phase::CopyListSetup {
                            stage: 1,
                            revision: snapshot.uirevision,
                        };
                        Sidebar::Sidebar(crate::generated::AnnotationSidebarCommand::Duplicate)
                    }
                    1 if selected.enabled => {
                        self.phase = Phase::CopyListSetup {
                            stage: 1,
                            revision: snapshot.uirevision,
                        };
                        return annotation_message(annotation::Message::Sidebar(
                            Sidebar::SelectedObjectEnabled(false),
                        ))
                        .chain(annotation_message(
                            annotation::Message::Sidebar(Sidebar::SelectedObjectApplied(
                                selected.category,
                            )),
                        ));
                    }
                    1 if ui.scene.objects.len() < self.copy_list_object_target => {
                        Sidebar::Sidebar(crate::generated::AnnotationSidebarCommand::Duplicate)
                    }
                    1 | 2 if ui.scene.categories.len() < self.copy_list_class_target => {
                        self.phase = Phase::CopyListSetup {
                            stage: 2,
                            revision: snapshot.uirevision,
                        };
                        return annotation_message(annotation::Message::Sidebar(Sidebar::CategoryDraftChanged(
                            format!("Acceptance long category {} with a label that wraps inside its assigned column", ui.scene.categories.len()))))
                            .chain(annotation_message(annotation::Message::Sidebar(Sidebar::CategoryApplied)));
                    }
                    1 | 2 => {
                        self.phase = Phase::CopyListSetup {
                            stage: 3,
                            revision: snapshot.uirevision,
                        };
                        Sidebar::ObjectSelected((self.copy_objects + 2) as u16)
                    }
                    _ => {
                        if ui.scene.objects.len() != self.copy_list_object_target
                            || ui.scene.categories.len() != self.copy_list_class_target
                        {
                            self.fail("Annotation long-list setup did not preserve its exact bounded inventory");
                            return Task::none();
                        }
                        return self.copy_scale_transition(
                            model,
                            applied_scale,
                            CopyScaleStage::Wide,
                        );
                    }
                };
                annotation_message(annotation::Message::Sidebar(command))
            }
            Phase::CopyLayout(step) => {
                if let Some(snapshot) = &model.annotation.snapshot {
                    self.copy_layout_objects = snapshot.ui.scene.objects.len();
                    self.copy_layout_classes = snapshot.ui.scene.categories.len();
                }
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
                    self.arm("workflow.diagnostics")
                } else if step < 7 {
                    self.arm_scrolled(self.copy_layout_control(step), RelativeOffset::START)
                } else {
                    if self.location_pending {
                        return Task::none();
                    }
                    if !self.prepare_control_probe() {
                        return Task::none();
                    }
                    self.copy_swatch_ready = false;
                    self.arm_scrolled("annotation.class.active.swatch", RelativeOffset::START)
                }
            }
            Phase::CopySwatchWait => {
                if self.control_probe_receipt != current_receipt("workflow.visual.workspace") {
                    self.copy_swatch_ready = false;
                    return self.advance_to(Phase::CopyLayout(7));
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
                if !self.prepare_control_probe() {
                    return Task::none();
                }
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
            Phase::CopyAwaitScale(stage) => {
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
                if !width.is_finite() || width <= 0.0 {
                    self.fail("Annotation settled viewport is invalid");
                    return Task::none();
                }
                self.copy_narrow = width < crate::view::PAGE_MIN_WIDTH;
                if stage != CopyScaleStage::Restore
                    && self.copy_narrow != (stage == CopyScaleStage::Narrow)
                {
                    self.fail("Annotation scale did not establish the requested layout");
                    return Task::none();
                }
                if stage != CopyScaleStage::Restore {
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
                    return self.copy_scale_transition(
                        model,
                        applied_scale,
                        if self.copy_narrow {
                            CopyScaleStage::Restore
                        } else {
                            CopyScaleStage::Narrow
                        },
                    );
                };
                self.copy_product_frame = snapshot.frame.revision;
                // Commands and completed reads can leave pixels unchanged.
                // Scene/editor matching and exact draw evidence remain required.
                self.copy_product_ui_revision = snapshot.uirevision;
                self.copy_product_settlement = action.settlement;
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.viewer_complete",
                        VIEWER_SAVE,
                        "copy",
                        [presentation as f64, source as f64, 1.0, 0.0],
                    )
                });
                self.phase = Phase::Complete;
                Task::none()
            }
            Phase::AwaitAnnotation => {
                let Some(snapshot) = model.annotation.snapshot.as_ref() else {
                    return Task::none();
                };
                if active != FeatureId::Annotate
                    || !snapshot.ready
                    || snapshot.busy
                    || snapshot.frame.revision == 0
                    || !model.annotation_edit_available()
                    || self
                        .annotation_open
                        .as_ref()
                        .is_none_or(|(_, before)| snapshot.inputdocumentepoch <= *before)
                {
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_ready",
                        "",
                        "receiver-owned",
                        [
                            snapshot.frame.revision as f64,
                            snapshot.ui.documentrevision as f64,
                            snapshot.ui.interactionrevision as f64,
                            0.0,
                        ],
                    )
                });
                if matches!(self.viewer_scenario.as_str(), "quiet" | "terminal") {
                    self.bounded_document_revision = snapshot.ui.documentrevision;
                    self.bounded_object_count = snapshot.ui.scene.objects.len();
                    self.phase = Phase::AnnotationTool {
                        revision: snapshot.ui.interactionrevision,
                        tool: crate::generated::AnnotationTool::Box,
                    };
                    return self.arm_scrolled(
                        annotation::tool_id(crate::generated::AnnotationTool::Box),
                        RelativeOffset::START,
                    );
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
                    let Some((request, _)) = self.annotation_open.as_ref() else {
                        self.fail("Annotation import has no dispatched source receipt");
                        return Task::none();
                    };
                    let expected = if request.originalcontent {
                        [request.source.content.width, request.source.content.height]
                    } else {
                        [request.source.extent.width, request.source.extent.height]
                    };
                    if !request.originalcontent
                        || snapshot.frame.extent.width != expected[0]
                        || snapshot.frame.extent.height != expected[1]
                        || u32::from(scene.framewidth) != expected[0]
                        || u32::from(scene.frameheight) != expected[1]
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
                    || (self.copy_step == 8 && !self.copy_product_settled(snapshot))
                    || snapshot.ui.editor.tool != tool
                    || !model.annotation_edit_available()
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_tool_wait",
                            &annotation::tool_id(tool),
                            &format!(
                                "observed={:?}; busy={}; editable={}; copy_step={}; ui_revision={}; settlement={:?}; ui_baseline={}",
                                snapshot.ui.editor.tool,
                                snapshot.busy,
                                model.annotation_edit_available(),
                                self.copy_step,
                                snapshot.uirevision,
                                self.copy_product_settlement,
                                self.copy_product_ui_revision,
                            ),
                            [
                                revision as f64,
                                snapshot.ui.interactionrevision as f64,
                                self.copy_product_frame as f64,
                                snapshot.frame.revision as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_tool_observed",
                        &annotation::tool_id(tool),
                        "typed-tool",
                        [
                            revision as f64,
                            snapshot.ui.interactionrevision as f64,
                            0.0,
                            0.0,
                        ],
                    )
                });
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
                let Some((_, image)) = crate::presentation_surface::retained_surface()
                    .and_then(crate::presentation_surface::drawable_annotation)
                else {
                    return Task::none();
                };
                let Some(rendered) = image.metadata.diagnostics.as_ref() else {
                    return Task::none();
                };

                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_settlement",
                        ANNOTATION_SURFACE,
                        &format!(
                            "phase={:?}; editable={}; busy={}; ui_revision={}; interaction={}; settlement={:?}; ui_baseline={}; epoch={}/{}; scene={}/{}; editor_match={}; sampleable={:?}; presentation={:?}; drawn={:?}; location_pending={}; probe_pending={}",
                            self.phase,
                            model.annotation_edit_available(),
                            snapshot.busy,
                            snapshot.uirevision,
                            snapshot.ui.interactionrevision,
                            self.copy_product_settlement,
                            self.copy_product_ui_revision,
                            snapshot.inputdocumentepoch,
                            rendered.documentepoch,
                            snapshot.ui.scenerevision,
                            rendered.scenerevision,
                            snapshot.ui.editor == rendered.editor,
                            self.annotation_frame_ready,
                            frame.and_then(|frame| crate::presentation_surface::metadata::product(frame)
                                .map(|product| (product.source.kind, product.revision, frame.presentation_revision))),
                            self.annotation_drawn,
                            self.location_pending,
                            self.annotation_pixels_pending.is_some(),
                        ),
                        [
                            self.copy_product_frame as f64,
                            snapshot.frame.revision as f64,
                            revision as f64,
                            self.copy_step as f64,
                        ],
                    )
                });
                let Some(sampleable) = self.annotation_frame_ready else {
                    return Task::none();
                };
                let presentation_revision = sampleable.presentation_revision;
                if !model.annotation_edit_available()
                    || (self.copy_step != 8 && snapshot.ui.interactionrevision <= revision)
                    || (self.copy_step == 8 && !self.copy_product_settled(snapshot))
                    || sampleable.source_revision != snapshot.frame.revision
                    || frame
                        .and_then(crate::presentation_surface::metadata::product)
                        .is_none_or(|product| {
                            product.source.kind
                                != crate::generated::PresentationSourceKind::Annotation
                                || product.revision != snapshot.frame.revision
                        })
                    || frame
                        .is_none_or(|frame| frame.presentation_revision != presentation_revision)
                    || self.annotation_drawn
                        != Some((presentation_revision, snapshot.frame.revision))
                {
                    return Task::none();
                }
                if rendered.documentepoch != snapshot.inputdocumentepoch
                    || rendered.scenerevision != snapshot.ui.scenerevision
                    || rendered.editor != snapshot.ui.editor
                {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_render_wait",
                            ANNOTATION_SURFACE,
                            "logical-scene-and-rendered-scene",
                            [
                                snapshot.inputdocumentepoch as f64,
                                rendered.documentepoch as f64,
                                snapshot.ui.scenerevision as f64,
                                rendered.scenerevision as f64,
                            ],
                        )
                    });
                    return Task::none();
                }
                if self.copy_step == 8 && self.copy_product_cancel && !self.copy_product_cancelled {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_preview",
                            ANNOTATION_SURFACE,
                            "visible-before-cancel",
                            [
                                self.copy_product_frame as f64,
                                snapshot.frame.revision as f64,
                                presentation_revision as f64,
                                0.0,
                            ],
                        )
                    });
                    self.copy_product_cancelled = true;
                    self.copy_product_frame = snapshot.frame.revision;
                    return annotation_message(annotation::Message::CancelRequested);
                }
                let receipt = current_receipt("workflow.visual.workspace");
                if self.viewer_scenario == "copy"
                    && (receipt.is_none() || self.annotation_pixels_receipt != receipt)
                {
                    if self.location_pending || self.annotation_pixels_pending == receipt {
                        return Task::none();
                    }
                    let samples = annotation_checks::probes(&snapshot.ui);
                    let key = (snapshot.frame.revision, presentation_revision);
                    if self.annotation_pixel_progress.0 != key
                        || self.annotation_pixel_progress.1 >= samples.len() / 7
                    {
                        self.annotation_pixel_progress = (key, 0, samples.len() / 7);
                    }
                    let index = self.annotation_pixel_progress.1;
                    let Some(pixel) = samples.chunks_exact(7).nth(index) else {
                        self.fail("Annotation pixel inventory lost its next expected sample");
                        return Task::none();
                    };
                    if !self.prepare_annotation_probe(
                        snapshot.frame.revision,
                        presentation_revision,
                        [snapshot.frame.extent.width, snapshot.frame.extent.height],
                        pixel.to_vec(),
                    ) {
                        return Task::none();
                    }
                    return self.arm(ANNOTATION_SURFACE);
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.annotation_pointer_observed",
                        ANNOTATION_SURFACE,
                        "typed-interaction",
                        [
                            revision as f64,
                            snapshot.ui.interactionrevision as f64,
                            presentation_revision as f64,
                            0.0,
                        ],
                    )
                });
                if self.viewer_scenario == "copy" && self.copy_step == 8 {
                    #[cfg(target_arch = "wasm32")]
                    if self.copy_product_cancel {
                        annotation_release_js();
                    }
                    match self.copy_product.observe(&snapshot.ui) {
                        Ok(step) => {
                            reporting::emit(|sink| {
                                sink.record(
                                    "integration.annotation_product",
                                    ANNOTATION_SURFACE,
                                    &step.detail(),
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
                                )
                            });
                            return self.advance_to(match step {
                                annotation_product::Step::Select(_) => Phase::CopyCapability,
                                _ => Phase::CopyProductStart,
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
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.annotation_shape",
                            ANNOTATION_SURFACE,
                            &format!("{:?}", object.shape),
                            [
                                index as f64,
                                snapshot.frame.revision as f64,
                                object.splineknots.len() as f64,
                                object.skeletonnodes.len() as f64,
                            ],
                        )
                    });
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
                        self.copy_narrow =
                            (model.window_width as f32) < crate::view::PAGE_MIN_WIDTH;
                        self.copy_list_object_target = snapshot.ui.scene.objects.len() + 32;
                        self.copy_list_class_target = snapshot.ui.scene.categories.len() + 32;
                        self.copy_list_revision = snapshot.uirevision.saturating_sub(1);
                        return self.advance_to(Phase::CopyListSetup {
                            stage: 0,
                            revision: snapshot.uirevision,
                        });
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
                reporting::emit(|sink| {
                    sink.record(
                        "integration.complete",
                        "",
                        "typed-mvc-wayland",
                        [
                            snapshot.frame.revision as f64,
                            presentation_revision as f64,
                            presentation_revision as f64,
                            crate::presentation_surface::retained_surface()
                                .and_then(|surface| surface.frame)
                                .map_or(0, |frame| frame.presentation_revision)
                                as f64,
                        ],
                    )
                });
                self.phase = Phase::Complete;
                self.report_phase_progress();
                Task::none()
            }
            _ => Task::none(),
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub(crate) struct ProbeFixture {
        pub(crate) controller: Controller,
        pub(crate) receiver: iced::futures::channel::mpsc::Receiver<Message>,
        surface: crate::presentation_surface::Surface,
        bounds: Rectangle,
    }

    impl ProbeFixture {
        pub(crate) fn new(scenario: &str) -> Self {
            initialize_reporting(true, true);
            let controller = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                "512".into(),
                scenario.into(),
            );
            let (sender, receiver) = iced::futures::channel::mpsc::channel(8);
            SURFACE_DRAW_OBSERVER.with(|observer| {
                observer.borrow_mut().output =
                    Some(ScenarioOutput::new(controller.generation, sender))
            });
            let (_, frame) = crate::view_model::test_support::explore_presentation();
            let surface = crate::view_model::test_support::physical_surface(frame);
            Self {
                controller,
                receiver,
                surface,
                bounds: Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0)),
            }
        }
    }

    impl Drop for ProbeFixture {
        fn drop(&mut self) {
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
            initialize_reporting(false, false);
        }
    }

    fn fps_settings() -> (ApplicationModel, crate::view::settings::SettingsModel) {
        let mut model = crate::view_model::test_support::bootstrapped();
        model
            .settings_snapshot
            .as_mut()
            .unwrap()
            .settingsstate
            .ui
            .showworkspaceperformance = true;
        let mut settings = crate::view::settings::SettingsModel::default();
        settings.install(model.settings_snapshot.as_ref().unwrap());
        (model, settings)
    }

    impl ProbeFixture {
        fn request_fps(
            &mut self,
            model: &ApplicationModel,
            settings: &crate::view::settings::SettingsModel,
        ) -> ScenarioOutput {
            record_probe_draw(
                EXPLORE_GALLERY,
                self.surface,
                self.bounds,
                self.bounds,
                self.bounds,
            );
            let (_, evidence) = reporting::fps_pixel_fixture(false, 1.0);
            let mut output = SURFACE_DRAW_OBSERVER.with(|observer| {
                let mut observer = observer.borrow_mut();
                observer.fps_draw = Some(evidence);
                observer.output_for(EXPLORE_GALLERY).unwrap().clone()
            });
            self.controller.phase = Phase::AwaitWorkspaceFps;
            output
                .try_send(Message::WorkspaceFpsDrawn(evidence))
                .unwrap();
            self.controller.update(self.receiver.try_recv().unwrap());
            drop(self.controller.advance(
                model,
                settings,
                1.0,
                &crate::view::router::Router::default(),
                FeatureId::Explore,
                None,
            ));
            assert_eq!(self.controller.phase, Phase::AwaitWorkspaceFpsPixels);
            self.controller.workspace_fps_probe.clone().unwrap()
        }
        pub(crate) fn prepare_app_fps(
            &mut self,
            model: &ApplicationModel,
            settings: &crate::view::settings::SettingsModel,
            baseline: bool,
            capture: bool,
        ) -> Option<Message> {
            self.controller.workspace_fps_baseline = baseline;
            if !capture {
                self.controller.phase = Phase::AwaitWorkspaceFps;
                return None;
            }
            let mut output = self.request_fps(model, settings);
            let (pixels, _) = reporting::fps_pixel_fixture(false, 1.0);
            output.send(Message::WorkspaceFpsPixels(FpsPixelOutcome::Captured(
                pixels,
            )));
            Some(self.receiver.try_recv().unwrap())
        }

        fn complete_fps(
            &mut self,
            output: &mut ScenarioOutput,
            outcome: FpsPixelOutcome,
            model: &ApplicationModel,
            settings: &crate::view::settings::SettingsModel,
        ) -> Task<RootMessage> {
            output
                .try_send(Message::WorkspaceFpsPixels(outcome))
                .unwrap();
            let message = self.receiver.try_recv().unwrap();
            if !self.controller.accepts_message(&message) {
                return Task::none();
            }
            self.controller.update(message);
            self.controller.advance(
                model,
                settings,
                1.0,
                &crate::view::router::Router::default(),
                FeatureId::Explore,
                None,
            )
        }

        fn assert_pending_fps(&self, output: &ScenarioOutput) {
            assert_eq!(self.controller.phase, Phase::AwaitWorkspaceFpsPixels);
            assert!(same_probe(
                &self.controller.workspace_fps_probe.as_ref().unwrap().probe,
                output.probe.as_ref()
            ));
        }
    }

    #[test]
    fn fps_capture_success_and_recoverable_failures_restore_both_canonical_baselines() {
        use iced::futures::StreamExt;
        for baseline in [false, true] {
            for result in 0..3 {
                let mut fixture = ProbeFixture::new("square");
                let (mut model, mut settings) = fps_settings();
                fixture.controller.workspace_fps_baseline = baseline;
                let mut output = fixture.request_fps(&model, &settings);
                let (mut pixels, _) = reporting::fps_pixel_fixture(false, 1.0);
                if result == 1 {
                    pixels.rgba.pop();
                }
                let task = fixture.complete_fps(
                    &mut output,
                    if result == 2 {
                        FpsPixelOutcome::Failed
                    } else {
                        FpsPixelOutcome::Captured(pixels)
                    },
                    &model,
                    &settings,
                );
                let driver = &mut fixture.controller;
                let router = crate::view::router::Router::default();
                assert_eq!(driver.phase, Phase::AwaitWorkspaceFpsRestored);
                assert_eq!(
                    driver.workspace_fps_failure,
                    (result != 0).then_some(WORKSPACE_FPS_PIXEL_FAILURE)
                );
                assert_eq!(driver.failure_line, 0);
                let mut actions = iced_runtime::task::into_stream(task).unwrap();
                let action = iced::futures::executor::block_on(actions.next()).unwrap();
                let iced_runtime::Action::Output(RootMessage::Settings(message)) = action else {
                    panic!("FPS restoration must use ordinary settings mutation");
                };
                assert!(
                    matches!(message, crate::view::settings::Message::PerformanceChanged(value) if value == baseline)
                );
                let outcome = crate::view::settings::update(&mut settings, message).unwrap();
                assert!(matches!(
                    outcome,
                    Some(crate::view::settings::Outcome::SettingsEdited(_))
                ));
                drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
                assert_eq!(driver.phase, Phase::AwaitWorkspaceFpsRestored);
                let restoration = settings
                    .take_request()
                    .expect("canonical restoration request for either baseline");
                assert_eq!(restoration.updates.len(), 1);
                assert_eq!(
                    restoration.updates[0],
                    crate::generated::update_uishowworkspaceperformance(baseline)
                );
                drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
                assert_eq!(driver.phase, Phase::AwaitWorkspaceFpsRestored);
                let pending = model
                    .begin_intent(crate::generated::ApplicationIntentEndpoint::SettingsUpdate)
                    .unwrap();
                let authoritative = model.settings_snapshot.as_mut().unwrap();
                authoritative.revision += 1;
                authoritative.settingsstate.ui.showworkspaceperformance = baseline;
                settings.settle_success(authoritative);
                drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
                assert_eq!(driver.phase, Phase::AwaitWorkspaceFpsRestored);
                assert_eq!(driver.failure_line, 0);
                model.abandon_intent(pending);
                drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
                assert_eq!(crate::workspace_fps::enabled(&settings), baseline);
                assert_eq!(
                    model
                        .settings_snapshot
                        .as_ref()
                        .unwrap()
                        .settingsstate
                        .ui
                        .showworkspaceperformance,
                    baseline
                );
                assert_eq!(driver.workspace_fps_verified, result == 0);
                assert_eq!(
                    driver.phase,
                    if result == 0 {
                        Phase::AwaitExploreReady
                    } else {
                        Phase::Failed
                    }
                );
                assert_eq!(driver.failure_line != 0, result != 0);
            }
        }
    }

    #[test]
    fn fps_capture_invalidation_rearms_and_obsolete_callbacks_cannot_finish_replacements() {
        for change in 0..4 {
            let mut fixture = ProbeFixture::new("square");
            let (model, settings) = fps_settings();
            let mut old = fixture.request_fps(&model, &settings);
            let (pixels, _) = reporting::fps_pixel_fixture(false, 1.0);
            let outcome = match change {
                0 => FpsPixelOutcome::Invalidated,
                1 => {
                    let moved = Rectangle {
                        x: 1.0,
                        ..fixture.bounds
                    };
                    record_probe_draw(EXPLORE_GALLERY, fixture.surface, moved, moved, moved);
                    FpsPixelOutcome::Captured(pixels.clone())
                }
                2 => {
                    SURFACE_DRAW_OBSERVER.with(|observer| {
                        observer.borrow_mut().fps_draw.as_mut().unwrap().bounds.x += 1.0
                    });
                    FpsPixelOutcome::Captured(pixels.clone())
                }
                _ => {
                    SURFACE_DRAW_OBSERVER.with(|observer| {
                        observer.borrow_mut().fps_draw.as_mut().unwrap().frames += 1
                    });
                    FpsPixelOutcome::Captured(pixels.clone())
                }
            };
            drop(fixture.complete_fps(&mut old, outcome, &model, &settings));
            assert_eq!(fixture.controller.phase, Phase::AwaitWorkspaceFps);
            assert!(!fixture.controller.workspace_fps_verified);
            assert!(fixture.controller.workspace_fps_failure.is_none());
            let mut replacement = fixture.request_fps(&model, &settings);
            drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Cancelled, &model, &settings));
            fixture.assert_pending_fps(&replacement);
            drop(fixture.complete_fps(
                &mut replacement,
                FpsPixelOutcome::Captured(pixels),
                &model,
                &settings,
            ));
            assert_eq!(fixture.controller.phase, Phase::AwaitWorkspaceFpsRestored);
            assert!(fixture.controller.workspace_fps_failure.is_none());
        }
    }

    #[test]
    fn fps_capture_from_a_prior_scenario_cannot_settle_the_replacement() {
        let mut fixture = ProbeFixture::new("square");
        let (model, settings) = fps_settings();
        let mut old = fixture.request_fps(&model, &settings);
        fixture.controller.phase = Phase::Complete;
        fixture
            .controller
            .reset_scenario(String::new(), String::new(), "512".into(), "square".into())
            .unwrap();
        let mut replacement = fixture.request_fps(&model, &settings);
        drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Cancelled, &model, &settings));
        fixture.assert_pending_fps(&replacement);
        let (pixels, _) = reporting::fps_pixel_fixture(false, 1.0);
        drop(fixture.complete_fps(
            &mut replacement,
            FpsPixelOutcome::Captured(pixels),
            &model,
            &settings,
        ));
        assert_eq!(fixture.controller.phase, Phase::AwaitWorkspaceFpsRestored);
        assert!(fixture.controller.workspace_fps_failure.is_none());
    }

    #[test]
    fn fps_capture_scale_change_rearms_without_accepting_the_old_result() {
        let mut fixture = ProbeFixture::new("square");
        let (model, settings) = fps_settings();
        let mut old = fixture.request_fps(&model, &settings);
        let router = crate::view::router::Router::default();
        drop(
            fixture
                .controller
                .advance(&model, &settings, 1.5, &router, FeatureId::Explore, None),
        );
        assert_eq!(fixture.controller.phase, Phase::AwaitWorkspaceFps);
        drop(fixture.complete_fps(&mut old, FpsPixelOutcome::Failed, &model, &settings));
        assert_eq!(fixture.controller.phase, Phase::AwaitWorkspaceFps);
    }

    #[test]
    fn numeric_seed_round_trip_reuses_one_distinct_exact_target_for_typing_and_paste() {
        for baseline in [0, 73, (1_u64 << 53) + 1, (1_u64 << 53) + 3, u64::MAX] {
            let mut fixture = ProbeFixture::new("square");
            let (mut model, settings) = fps_settings();
            model.connection = crate::view_model::ConnectionState::Connected;
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.ready = true;
            snapshot.busy = false;
            snapshot.revision = 17;
            snapshot.filter.shuffleseed = baseline;
            model
                .settings_snapshot
                .as_mut()
                .unwrap()
                .settingsstate
                .workflows
                .explore
                .shuffleseed = baseline;
            let driver = &mut fixture.controller;
            driver.phase = Phase::ExploreNumericStart(2);
            let router = crate::view::router::Router::default();
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
            let target = driver.explore_integer_target;
            assert_ne!(target, baseline);
            assert!(target > (1_u64 << 53));
            assert_eq!(target % 2, 1);
            assert_eq!(
                target,
                if baseline == (1_u64 << 53) + 1 {
                    (1_u64 << 53) + 3
                } else {
                    (1_u64 << 53) + 1
                }
            );
            assert_eq!(target.to_string().parse::<u64>(), Ok(target));
            driver.phase = Phase::AwaitExploreNumeric { index: 2, step: 5 };
            model.explore.snapshot.as_mut().unwrap().revision += 1;
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Explore, None));
            assert_eq!(driver.phase, Phase::AwaitExploreClipboard(18));
            assert_eq!(driver.explore_integer_target, target);
            assert_eq!(driver.explore_integer_baseline, baseline);
        }
    }

    #[test]
    fn clipboard_preparation_and_reads_belong_to_the_active_numeric_round_trip() {
        let mut fixture = ProbeFixture::new("square");
        let driver = &mut fixture.controller;
        driver.phase = Phase::AwaitExploreClipboard(17);
        let scoped = |generation, message| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(message),
        };
        driver.update(scoped(
            driver.generation.wrapping_add(1),
            Message::NumberClipboardPrepared {
                revision: 17,
                result: Ok(()),
            },
        ));
        assert_eq!(driver.phase, Phase::AwaitExploreClipboard(17));
        driver.update(scoped(
            driver.generation,
            Message::NumberClipboardPrepared {
                revision: 16,
                result: Ok(()),
            },
        ));
        assert_eq!(driver.phase, Phase::AwaitExploreClipboard(17));
        driver.update(scoped(
            driver.generation,
            Message::NumberClipboardPrepared {
                revision: 17,
                result: Ok(()),
            },
        ));
        assert_eq!(
            driver.phase,
            Phase::ExploreNumericControl { index: 2, step: 6 }
        );
        driver.phase = Phase::AwaitExploreNumeric { index: 2, step: 6 };
        driver.update(scoped(
            driver.generation.wrapping_add(1),
            Message::NumberPasteDelivered(false),
        ));
        driver.update(scoped(
            driver.generation,
            Message::NumberPasteDelivered(true),
        ));
        assert_eq!(
            driver.phase,
            Phase::AwaitExploreNumeric { index: 2, step: 6 }
        );
        driver.explore_integer_target = (1_u64 << 53) + 1;
        let contents = std::sync::Arc::new(iced::clipboard::Content::Text(
            driver.explore_integer_target.to_string(),
        ));
        driver.update(scoped(
            driver.generation,
            Message::NumberPasteRead {
                target: explore_integer_id(0),
                result: Ok(contents.clone()),
            },
        ));
        assert!(!driver.explore_paste_read);
        driver.update(scoped(
            driver.generation,
            Message::NumberPasteRead {
                target: explore_integer_id(2),
                result: Ok(contents),
            },
        ));
        assert!(driver.explore_paste_read);
        driver.update(scoped(
            driver.generation,
            Message::NumberPasteDelivered(false),
        ));
        assert!(driver.failure.contains("paste shortcut delivery failed"));
        driver.update(scoped(
            driver.generation,
            Message::NumberClipboardPrepared {
                revision: 17,
                result: Ok(()),
            },
        ));
        assert_eq!(driver.phase, Phase::Failed);
        assert!(!driver.explore_paste_read);
    }

    #[test]
    fn clipboard_rejection_preserves_the_underlying_error_and_fails_only_its_active_stage() {
        for writing in [true, false] {
            let mut fixture = ProbeFixture::new("square");
            let driver = &mut fixture.controller;
            driver.phase = if writing {
                Phase::AwaitExploreClipboard(17)
            } else {
                Phase::AwaitExploreNumeric { index: 2, step: 6 }
            };
            let error = iced::clipboard::Error::Unknown {
                description: std::sync::Arc::new("clipboard permission rejected".into()),
            };
            let message = if writing {
                Message::NumberClipboardPrepared {
                    revision: 17,
                    result: Err(error),
                }
            } else {
                Message::NumberPasteRead {
                    target: explore_integer_id(2),
                    result: Err(error),
                }
            };
            driver.update(Message::Scoped {
                generation: driver.generation,
                receipt: None,
                message: Box::new(message),
            });
            assert_eq!(driver.phase, Phase::Failed);
            assert!(driver.failure.contains("clipboard permission rejected"));
        }
    }

    #[test]
    fn delivered_gallery_mouse_advances_placeholder_selection_for_the_current_driver() {
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            String::new(),
        );
        driver.phase = Phase::AwaitVisibleReadHover(5, 9);
        driver.update(Message::Scoped {
            generation: driver.generation.wrapping_add(1),
            receipt: None,
            message: Box::new(Message::GalleryMouseDelivered),
        });
        assert_eq!(driver.phase, Phase::AwaitVisibleReadHover(5, 9));
        driver.update(Message::Scoped {
            generation: driver.generation,
            receipt: None,
            message: Box::new(Message::GalleryMouseDelivered),
        });
        assert_eq!(driver.phase, Phase::VisibleReadSelect(5, 9));
    }

    #[test]
    fn typed_capacity_commands_reject_duplicates_and_wrong_scenario() {
        use crate::generated::{IntegrationControlKind as Kind, IntegrationControlReceipt};
        for sequence in [1, 2] {
            let mut driver = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                "512".into(),
                "rapid".into(),
            );
            driver.phase = Phase::AwaitCapacityArm;
            let receipt = IntegrationControlReceipt {
                kind: Kind::CapacityArmed,
                sequence,
                progress: 0,
                failureline: 0,
                failure: String::new(),
                readgeneration: 0,
                compiledindex: 0,
            };
            let result = driver.receive_control(receipt.clone());
            assert_eq!(result.is_ok(), sequence == 1);
            if sequence == 1 {
                assert!(matches!(driver.phase, Phase::CapacityPublish));
                assert!(driver.receive_control(receipt).is_err());
            }
            assert!(matches!(driver.phase, Phase::Failed));
        }
    }

    #[test]
    fn cold_gallery_walk_waits_for_new_complete_viewports_and_exact_held_receipts() {
        use crate::generated::{IntegrationControlKind as Kind, IntegrationControlReceipt};
        initialize_reporting(false, false);
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            String::new(),
        );
        let (mut model, frame) = crate::view_model::test_support::explore_presentation();
        model.connection = ConnectionState::Connected;
        let settings = crate::view::settings::SettingsModel::default();
        let router = crate::view::router::Router::default();
        let drive = |driver: &mut Controller, model: &ApplicationModel| {
            drop(driver.advance(
                model,
                &settings,
                1.0,
                &router,
                FeatureId::Explore,
                Some(crate::view_model::test_support::physical_surface(frame)),
            ));
        };
        {
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.mode = crate::generated::ExploreMode::Gallery;
            snapshot.ready = true;
            snapshot.busy = false;
            snapshot.viewport.firstrow = 10;
            snapshot.viewport.rowcount = 2;
            snapshot.viewport.columns = 3;
            snapshot.gallery.layout.firstrow = 10;
            snapshot.gallery.layout.columns = 3;
            snapshot.gallery.layout.rowcount = 2;
            snapshot.gallery.generation = 4;
            snapshot.gallery.slots = vec![true; 6];
            snapshot.order.visibleindices = vec![17, 18, 19, 20, 21, 22];
            snapshot.order.matchingcount = 300;
        }
        driver.atlas_row_extent = 160.0;
        driver.phase = Phase::AwaitGalleryColdRead(10, 4);
        drive(&mut driver, &model);
        assert!(
            matches!(driver.phase, Phase::AwaitGalleryColdRead(10, 4)),
            "unchanged demand cannot advance"
        );
        model.explore.snapshot.as_mut().unwrap().gallery.generation = 5;
        model.explore.snapshot.as_mut().unwrap().gallery.slots[0] = false;
        drive(&mut driver, &model);
        assert!(
            matches!(driver.phase, Phase::AwaitGalleryColdRead(10, 4)),
            "pending read cannot advance"
        );
        model.explore.snapshot.as_mut().unwrap().gallery.slots[0] = true;
        drive(&mut driver, &model);
        assert!(matches!(driver.phase, Phase::GalleryColdRead(17)));
        drive(&mut driver, &model);
        assert!(
            matches!(driver.phase, Phase::GalleryColdRead(17)),
            "a sampleable publication must actually draw before a disjoint jump"
        );
        driver.gallery_drawn = Some((frame.presentation_revision, frame.content_sequence - 1));
        drive(&mut driver, &model);
        assert!(matches!(driver.phase, Phase::GalleryColdRead(17)));
        driver.gallery_drawn = Some((frame.presentation_revision, frame.content_sequence));
        drive(&mut driver, &model);
        assert!(matches!(driver.phase, Phase::AwaitGalleryColdRead(17, 5)));
        let receipt = IntegrationControlReceipt {
            kind: Kind::GalleryReadCompletionHeld,
            sequence: 1,
            progress: 0,
            failureline: 0,
            failure: String::new(),
            readgeneration: 6,
            compiledindex: 47,
        };
        driver.receive_control(receipt.clone()).unwrap();
        drive(&mut driver, &model);
        assert!(
            matches!(driver.phase, Phase::AwaitGalleryColdRead(17, 5)),
            "early receipt must retain its identity"
        );
        {
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.viewport.firstrow = 17;
            snapshot.gallery.layout.firstrow = 17;
            snapshot.gallery.generation = 6;
            snapshot.gallery.slots[0] = false;
            snapshot.order.visibleindices[0] = 47;
        }
        drive(&mut driver, &model);
        assert!(matches!(driver.phase, Phase::GallerySweep));
        assert_eq!(driver.gallery_completion_held, Some((6, 47)));
        assert!(driver.receive_control(receipt).is_err());
        assert!(matches!(driver.phase, Phase::Failed));

        for held in [(5, 47), (6, 99)] {
            let mut mismatch = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                "512".into(),
                String::new(),
            );
            mismatch.phase = Phase::AwaitGalleryColdRead(17, 5);
            mismatch.gallery_completion_held = Some(held);
            drive(&mut mismatch, &model);
            assert!(
                matches!(mismatch.phase, Phase::Failed),
                "stale generation or absent image cannot authorize the sweep"
            );
        }

        let viewport = &model.explore.snapshot.as_ref().unwrap().viewport;
        assert_eq!(
            cold_gallery_scroll_offset(17, viewport, 300, 160.0),
            Some(2760.0)
        );
        for extent in [0.0, -1.0, f32::NAN, f32::INFINITY] {
            assert!(cold_gallery_scroll_offset(17, viewport, 300, extent).is_none());
        }
        assert!(cold_gallery_scroll_offset(0, viewport, 300, 160.0).is_none());
        assert!(cold_gallery_scroll_offset(93, viewport, 300, 160.0).is_none());
        driver.phase = Phase::GalleryColdRead(93);
        drive(&mut driver, &model);
        assert!(
            matches!(driver.phase, Phase::Failed),
            "exhausted fixture is an explicit failure"
        );
    }

    #[test]
    fn gallery_completion_controls_reject_stale_scenarios_and_reset_with_the_driver() {
        use crate::generated::{IntegrationControlKind as Kind, IntegrationControlReceipt};
        for sequence in [0, 2] {
            let mut driver = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                "512".into(),
                String::new(),
            );
            assert!(
                driver
                    .receive_control(IntegrationControlReceipt {
                        kind: Kind::GalleryReadCompletionHeld,
                        sequence,
                        progress: 0,
                        failureline: 0,
                        failure: String::new(),
                        readgeneration: 7,
                        compiledindex: 47,
                    })
                    .is_err()
            );
        }
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            String::new(),
        );
        driver.phase = Phase::Complete;
        // A current physical read can settle after the UI's last step; it
        // belongs to this scenario until the native owner permits Advance.
        driver
            .receive_control(IntegrationControlReceipt {
                kind: Kind::GalleryReadCompletionHeld,
                sequence: 1,
                progress: 0,
                failureline: 0,
                failure: String::new(),
                readgeneration: 7,
                compiledindex: 47,
            })
            .unwrap();
        assert!(matches!(driver.phase, Phase::Complete));
        driver
            .reset_scenario(String::new(), String::new(), "512".into(), String::new())
            .unwrap();
        assert!(driver.gallery_completion_held.is_none());
    }

    fn advance_receipt(sequence: u64) -> crate::generated::IntegrationControlReceipt {
        crate::generated::IntegrationControlReceipt {
            kind: crate::generated::IntegrationControlKind::Advance,
            sequence,
            progress: 0,
            failureline: 0,
            failure: String::new(),
            readgeneration: 0,
            compiledindex: 0,
        }
    }

    #[test]
    fn destructive_profile_continues_viewer_completion_into_annotation() {
        initialize_reporting(false, false);
        for window_close in [false, true] {
            let mut driver = Controller::new(
                true,
                window_close,
                "source".into(),
                "compiled".into(),
                "512".into(),
                "terminal".into(),
            );
            driver.configure_session("terminal", String::new(), String::new());
            assert_eq!(driver.session.scenario(0), Some(("terminal", false)));
            assert_eq!(driver.session.scenario(1), None);
            assert!(driver.reuse_compiled);
            driver.phase = Phase::ViewerNoAspect;
            driver.viewer_drawn = Some((
                7,
                3,
                ViewerDraw {
                    crop: [0, 0, 512, 512],
                    container: Rectangle::default(),
                    image: Rectangle::default(),
                    fit_revision: 1,
                },
            ));
            driver.update(Message::Located {
                control: "explore.detail.aspect".into(),
                bounds: Rectangle::default(),
            });
            assert!(matches!(driver.phase, Phase::OpenAnnotation));
            assert!(driver.running());
            assert!(
                driver.receive_control(advance_receipt(2)).is_err(),
                "viewer evidence cannot settle a destructive Annotation workflow"
            );
            let (mut model, _) = crate::view_model::test_support::explore_presentation();
            let source = model.explore.snapshot.as_ref().unwrap().frame.clone();
            let snapshot = model.annotation.snapshot.as_mut().unwrap();
            snapshot.ready = true;
            snapshot.busy = false;
            snapshot.frame.revision = 23;
            snapshot.ui.documentrevision = 9;
            snapshot.inputdocumentepoch = 31;
            driver.phase = Phase::AwaitAnnotation;
            let settings = crate::view::settings::SettingsModel::default();
            let router = crate::view::router::Router::default();
            drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Annotate, None));
            assert_eq!(driver.phase, Phase::AwaitAnnotation);
            driver.observe_annotation_open(
                crate::generated::AnnotationOpen {
                    source,
                    originalcontent: true,
                },
                31,
            );
            for (epoch, busy, imported) in
                [(31, false, false), (32, true, false), (32, false, true)]
            {
                let snapshot = model.annotation.snapshot.as_mut().unwrap();
                snapshot.inputdocumentepoch = epoch;
                snapshot.busy = busy;
                drop(driver.advance(&model, &settings, 1.0, &router, FeatureId::Annotate, None));
                assert_eq!(driver.location_pending, imported);
                assert_eq!(
                    driver.phase,
                    if imported {
                        Phase::AnnotationTool {
                            revision: model
                                .annotation
                                .snapshot
                                .as_ref()
                                .unwrap()
                                .ui
                                .interactionrevision,
                            tool: crate::generated::AnnotationTool::Box,
                        }
                    } else {
                        Phase::AwaitAnnotation
                    }
                );
            }
        }
    }

    #[test]
    fn retained_workflows_require_the_unique_settled_control_owner() {
        initialize_reporting(false, false);
        let mut driver = Controller::new(
            true,
            false,
            "mixed-source".into(),
            "mixed-output".into(),
            "512".into(),
            "retained".into(),
        );
        driver.configure_session("retained", "square-source".into(), "square-output".into());
        assert_eq!(driver.viewer_scenario, "square");
        assert_eq!(driver.resolution, "384");
        assert_eq!(driver.dataset_source, "square-source");
        let generation = driver.generation;
        let advance = advance_receipt(2);
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
        // A retained paired Detail owns Close independently of logical delivery.
        crate::presentation_surface::reset_test_releases();
        let (mut model, frame) = crate::view_model::test_support::explore_presentation();
        model.connection = ConnectionState::Connected;
        model.window_width = 1200;
        model.window_height = 800;
        model
            .settings_snapshot
            .as_mut()
            .unwrap()
            .exploresource
            .available = true;
        let settings = crate::view::settings::SettingsModel::default();
        let mut router = crate::view::router::Router::default();
        router.explore_measure_gallery(
            896.0,
            896.0,
            crate::generated::VisualExtent {
                width: 1920,
                height: 1080,
            },
            3,
        );
        let drive = |driver: &mut Controller, model: &ApplicationModel| {
            drop(driver.advance(
                model,
                &settings,
                1.0,
                &router,
                FeatureId::Explore,
                Some(crate::view_model::test_support::physical_surface(frame)),
            ));
        };
        driver.desired_dark = None;
        driver.phase = Phase::AwaitExplore;
        assert!(model.explore_open_available());
        // Logical Detail may arrive before any actual composition.
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::AwaitExplore);
        assert!(!driver.location_pending);
        for phase in [
            Phase::AwaitDetail(0),
            Phase::AwaitNext(99),
            Phase::AwaitPrevious(99),
            Phase::AwaitDetailAgain,
        ] {
            driver.phase = phase.clone();
            drive(&mut driver, &model);
            assert_eq!(driver.phase, phase);
            assert!(!driver.location_pending);
        }
        driver.phase = Phase::AwaitExplore;
        assert!(crate::presentation_surface::accept_publication(frame));
        model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Gallery;
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::AwaitExplore);
        assert!(!driver.location_pending);
        model.explore.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Detail;
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::ExploreCloseDetail);
        assert!(driver.location_pending);
        // The packaged Wayland sequence exercises the browser click. This
        // native fixture starts at its receipt and checks native settlement.
        driver.phase = Phase::AwaitExploreGallery;
        driver.location_pending = false;
        for (mode, busy, expected) in [
            (
                crate::generated::ExploreMode::Detail,
                false,
                Phase::AwaitExploreGallery,
            ),
            (
                crate::generated::ExploreMode::Gallery,
                true,
                Phase::AwaitExploreGallery,
            ),
            (
                crate::generated::ExploreMode::Gallery,
                false,
                Phase::AwaitExplore,
            ),
        ] {
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.mode = mode;
            snapshot.busy = busy;
            if mode == crate::generated::ExploreMode::Gallery && !busy {
                // Logical Close settlement cannot uncover the old graphics overlay.
                drive(&mut driver, &model);
                assert_eq!(driver.phase, Phase::AwaitExploreGallery);
                let mut gallery = model.explore.snapshot.clone().unwrap();
                gallery.viewport.columns = 4;
                gallery.viewport.rowcount = 3;
                gallery.viewport.firstrow = 0;
                gallery.viewport.extent = gallery.frame.extent.clone();
                crate::view_model::test_support::gallery_layout(&mut gallery);
                crate::presentation_surface::metadata::retire(frame);
                crate::presentation_surface::metadata::install_explore(frame, &gallery);
                // A new Gallery composition can precede logical Close settlement too.
                model.explore.snapshot.as_mut().unwrap().mode =
                    crate::generated::ExploreMode::Detail;
                drive(&mut driver, &model);
                assert_eq!(driver.phase, Phase::AwaitExploreGallery);
                assert!(!driver.location_pending);
                model.explore.snapshot.as_mut().unwrap().mode = mode;
            }
            drive(&mut driver, &model);
            assert_eq!(driver.phase, expected);
            assert!(!driver.location_pending);
            assert!(driver.reporting.state_is_absent());
        }
        let revision = model.explore.snapshot.as_ref().unwrap().revision;
        model
            .explore
            .snapshot
            .as_mut()
            .unwrap()
            .detail
            .showoriginaldimensions = true;
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::AwaitExplorePreparation(revision));
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::AwaitExplorePreparation(revision));
        {
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.revision += 1;
            snapshot.detail.showoriginaldimensions = false;
            assert!(explore_scenario_preparation(snapshot).is_none());
        }
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::AwaitExplore);

        // The shared padding donors have no annotations. Missing or unready
        // donor slots cannot advance even when the published frame is drawn.
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.labels.clear();
        let revision = snapshot.revision;
        let source = snapshot.frame.revision;
        driver.gallery_drawn = Some((frame.presentation_revision, source));
        for (indices, slots, timeline, ready) in [
            (vec![0, 8], vec![true, true], 1, false),
            (vec![7, 8], vec![true, false], 1, false),
            (vec![7, 8], vec![true, true], 0, false),
            (vec![7, 8], vec![true, true], 1, true),
        ] {
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.order.visibleindices = indices;
            snapshot.gallery.slots = slots;
            crate::presentation_surface::metadata::retire(frame);
            if timeline != 0 {
                let mut product = snapshot.clone();
                product.mode = crate::generated::ExploreMode::Gallery;
                product.viewport.columns = 4;
                product.viewport.rowcount = 3;
                product.viewport.firstrow = 0;
                product.viewport.extent = product.frame.extent.clone();
                crate::view_model::test_support::gallery_layout(&mut product);
                crate::presentation_surface::metadata::install_explore(frame, &product);
            }
            driver.phase = Phase::AwaitExploreInitialPatch {
                revision,
                frame_revision: source,
            };
            drive(&mut driver, &model);
            assert_eq!(
                driver.phase,
                if ready {
                    Phase::AwaitExploreExactGrid(revision)
                } else {
                    Phase::AwaitExploreInitialPatch {
                        revision,
                        frame_revision: source,
                    }
                }
            );
            assert!(driver.reporting.state_is_absent());
        }
        // The empty message must reach a draw before restoring the filters.
        // Logical delivery, acquisition, and a preceding draw are insufficient.
        let current_draw = (frame.presentation_revision, source);
        for (logical_matches, displayed_matches, drawn, ready) in [
            (0, None, Some(current_draw), false),
            (0, Some(2), Some(current_draw), false),
            (2, Some(0), Some(current_draw), false),
            (0, Some(0), None, false),
            (
                0,
                Some(0),
                Some((frame.presentation_revision - 1, source)),
                false,
            ),
            (0, Some(0), Some(current_draw), true),
        ] {
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.order.matchingcount = logical_matches;
            crate::presentation_surface::metadata::retire(frame);
            if let Some(matching) = displayed_matches {
                let mut product = snapshot.clone();
                product.mode = crate::generated::ExploreMode::Gallery;
                product.order.matchingcount = matching;
                product.viewport.columns = 4;
                product.viewport.rowcount = 3;
                product.viewport.firstrow = 0;
                product.viewport.extent = product.frame.extent.clone();
                crate::view_model::test_support::gallery_layout(&mut product);
                crate::presentation_surface::metadata::install_explore(frame, &product);
            }
            driver.gallery_drawn = drawn;
            driver.phase = Phase::AwaitAtlasEmpty;
            drive(&mut driver, &model);
            assert_eq!(
                driver.phase,
                if ready {
                    Phase::AtlasEmpty
                } else {
                    Phase::AwaitAtlasEmpty
                }
            );
            assert_eq!(driver.location_pending, ready);
            driver.location_pending = false;
        }
        // The oversized measurement can resolve to the already committed
        // raster, so native viewport deduplication need not publish a revision.
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.viewport = crate::generated::ExploreViewport {
            extent: crate::generated::VisualExtent {
                width: 810,
                height: 810,
            },
            firstrow: 0,
            rowcount: 3,
            columns: 3,
        };
        snapshot.busy = false;
        driver.oversized_gallery = Some((
            iced::Size::new(896.0, 896.0),
            crate::generated::VisualExtent {
                width: 1920,
                height: 1080,
            },
        ));
        driver.phase = Phase::AwaitExploreExactGrid(revision);
        drive(&mut driver, &model);
        assert_eq!(
            driver.phase,
            Phase::AwaitExploreExactGridPatch {
                revision,
                frame_revision: source,
            }
        );
        let annotation = model.annotation.snapshot.as_mut().unwrap();
        annotation.ready = true;
        annotation.ui.documentrevision = 1;
        annotation.frame = crate::view_model::test_support::visual_frame(
            crate::generated::PresentationSourceKind::Annotation,
            1,
        );
        annotation.inputdocumentepoch = 1;
        driver.observe_annotation_open(
            crate::generated::AnnotationOpen {
                source: model.explore.snapshot.as_ref().unwrap().frame.clone(),
                originalcontent: true,
            },
            0,
        );
        driver.viewer_scenario = "terminal".into();
        driver.phase = Phase::AwaitAnnotation;
        drive(&mut driver, &model);
        assert_eq!(driver.phase, Phase::AwaitAnnotation);
        drop(driver.advance(
            &model,
            &settings,
            1.0,
            &router,
            FeatureId::Annotate,
            Some(crate::view_model::test_support::physical_surface(frame)),
        ));
        assert!(matches!(driver.phase, Phase::AnnotationTool { .. }));
        assert!(driver.reporting.state_is_absent());
        driver.phase = Phase::Complete;
        assert!(driver.receive_control(advance).is_err());
        assert!(matches!(driver.phase, Phase::Failed));

        let mut premature = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            "512".into(),
            "dpi".into(),
        );
        premature.configure_session("dpi", String::new(), String::new());
        premature.phase = Phase::Complete;
        assert!(
            premature.receive_control(advance_receipt(2)).is_err(),
            "local completion is insufficient before the typed receipt was admitted"
        );
    }

    #[test]
    fn first_native_gallery_draw_reports_placeholders_and_retains_pixels_in_enabled_and_quiet_modes()
     {
        use crate::presentation_surface as surface;
        use iced::advanced::{
            Layout, layout,
            renderer::{Headless, Renderer as _},
            widget,
        };
        for enabled in [false, true] {
            surface::reset_test_releases();
            surface::initialize_diagnostics(enabled, false);
            let mut fixture = ProbeFixture::new("atlas");
            initialize_reporting(enabled, false);
            while fixture.receiver.try_recv().is_ok() {}
            let frame = fixture.surface.frame.unwrap();
            let mut snapshot = crate::view_model::test_support::explore_snapshot();
            snapshot.mode = crate::generated::ExploreMode::Gallery;
            snapshot.frame = crate::view_model::test_support::visual_frame(
                crate::generated::PresentationSourceKind::Explore,
                frame.content_sequence,
            );
            snapshot.dataset.identity = 1;
            snapshot.viewport.columns = 4;
            snapshot.viewport.rowcount = 3;
            snapshot.viewport.extent = snapshot.frame.extent.clone();
            snapshot.order.matchingcount = 12;
            snapshot.order.visibleindices = (0..12).collect();
            snapshot.gallery.slots = vec![false; 12];
            crate::view_model::test_support::gallery_layout(&mut snapshot);
            surface::metadata::install_explore(frame, &snapshot);
            // Only the graphics receipt and its paired metadata reach the renderer.
            // No application snapshot is installed or reconciled before either draw.
            drop(snapshot);
            assert!(surface::accept_publication(frame));
            surface::authorize_draw(Some(frame));
            surface::complete_sample(frame);
            let received = surface::metadata::surface(frame).unwrap();
            let _renderer_cleanup = surface::TestRendererCleanup;
            let mut renderer = iced::futures::executor::block_on(
                <iced::Renderer as Headless>::new(Default::default(), Some("wgpu")),
            )
            .expect("first native gallery draw acceptance requires the container GPU backend");
            let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(128.0, 96.0));
            let viewport =
                iced::widget::shader::Viewport::with_physical_size(iced::Size::new(128, 96), 1.0);
            let node = layout::Node::new(bounds.size());
            let theme = crate::fluent_theme::app_theme(false);
            let style = iced::advanced::renderer::Style::default();
            let mut first_pixels = None;
            for requested in [received, received, surface::Surface::empty(), received] {
                let program = surface::Program::<()> {
                    show_fps: false,
                    input: None,
                    local: None,
                    publish: None,
                    surface: requested,
                    placement: if requested.valid() {
                        surface::Placement::GalleryGrid {
                            columns: 4,
                            rows: 3,
                            row_capacity: 3,
                            row_origin: 0,
                            first_row: 0,
                        }
                    } else {
                        surface::Placement::Contain
                    },
                    control_id: EXPLORE_GALLERY,
                };
                let mut element: crate::fluent_theme::Element<'_, ()> =
                    iced::widget::shader(program).width(128).height(96).into();
                let mut tree = widget::Tree::new(&element);
                tree.diff(element.as_widget_mut());
                renderer.reset(bounds);
                element.as_widget().draw(
                    &tree,
                    &mut renderer,
                    &theme,
                    &style,
                    Layout::new(&node),
                    iced::mouse::Cursor::Unavailable,
                    &bounds,
                );
                let pixels = renderer.screenshot(&viewport, iced::Color::WHITE);
                if requested.valid() {
                    assert_eq!(
                        &pixels[(16 * 128 + 16) * 4..(16 * 128 + 16) * 4 + 3],
                        &[0, 0, 0]
                    );
                    if let Some(first) = &first_pixels {
                        assert_eq!(&pixels, first);
                    } else {
                        first_pixels = Some(pixels);
                    }
                } else {
                    // An input-only workspace does not sample another component's
                    // retained image. Returning to its image preserves custody.
                    assert!(pixels.iter().all(|channel| *channel == 255));
                }
                let (displayed, metadata) = surface::gallery::displayed().unwrap();
                assert_eq!(displayed.frame, Some(frame));
                assert_eq!(metadata.gallery.slots, vec![false; 12]);
                assert!(surface::test_releases().is_empty());
            }
            let mut atlas_drawn = false;
            let mut gallery_drawn = false;
            while let Ok(message) = fixture.receiver.try_recv() {
                assert!(fixture.controller.accepts_message(&message));
                if let Message::Scoped { message, .. } = &message {
                    match message.as_ref() {
                        Message::AtlasDrawn { receipt, .. } => {
                            assert_eq!(receipt.surface.frame, Some(frame));
                            assert_eq!(receipt.snapshot.gallery.slots, vec![false; 12]);
                            assert_eq!(receipt.bounds, bounds);
                            atlas_drawn = true;
                        }
                        Message::GalleryDrawn {
                            presentation_revision,
                            source_revision,
                        } => {
                            assert_eq!(*presentation_revision, frame.presentation_revision);
                            assert_eq!(*source_revision, frame.content_sequence);
                            gallery_drawn = true;
                        }
                        _ => {}
                    }
                }
            }
            assert_eq!(atlas_drawn, enabled);
            assert_eq!(gallery_drawn, enabled);
            SURFACE_DRAW_OBSERVER.with(|observer| {
                let observer = observer.borrow();
                assert_eq!(observer.receipts.is_empty(), !enabled);
                assert_eq!(observer.atlas.is_some(), enabled);
            });
            surface::retire_samples();
            surface::retire_publication(frame);
            surface::discard_sample(frame);
            assert_eq!(surface::test_releases(), vec![frame]);
            surface::initialize_diagnostics(false, false);
        }
    }

    #[test]
    fn quiet_failure_receipts_preserve_ui_error_kind_and_bounded_utf8() {
        use crate::application_codec::FromApplicationValue;
        for detail in [
            "inconsistent frame revision".to_owned(),
            "λ".repeat(crate::generated::INTEGRATION_FAILURE_MAX_BYTES),
        ] {
            initialize_reporting(false, false);
            let mut driver = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                String::new(),
                "quiet".into(),
            );
            let mut model = crate::view_model::test_support::bootstrapped();
            model.error = Some(crate::view_model::UiError::protocol(detail));
            drop(driver.advance(
                &model,
                &crate::view::settings::SettingsModel::default(),
                1.0,
                &crate::view::router::Router::default(),
                FeatureId::Explore,
                None,
            ));
            assert_eq!(driver.phase, Phase::Failed);
            assert!(driver.failure.starts_with("Protocol: "));
            assert!(driver.failure.len() <= crate::generated::INTEGRATION_FAILURE_MAX_BYTES);
            assert!(driver.failure.is_char_boundary(driver.failure.len()));
            let (mut connection, _capture) =
                crate::transport_connection::Connection::test_channel();
            driver.publish_control(&mut connection);
            let mut wire = Vec::new();
            connection
                .flush(|bytes| {
                    wire.push(bytes.to_vec());
                    Ok(())
                })
                .unwrap();
            assert_eq!(wire.len(), 1);
            let envelope = crate::protocol::decode_envelope(&wire[0]).unwrap();
            let control =
                crate::generated::IntegrationControl::from_application_value(envelope.payload)
                    .unwrap();
            assert_eq!(control.receipt.failure, driver.failure);
            assert_eq!(
                control.receipt.kind,
                crate::generated::IntegrationControlKind::Failed
            );
            assert!(crate::generated::integration_receipt_valid(
                &control.receipt
            ));
            let mut invalid = control.clone();
            invalid
                .receipt
                .failure
                .push_str(&"x".repeat(crate::generated::INTEGRATION_FAILURE_MAX_BYTES));
            assert!(invalid.encode().is_err());
            invalid = control;
            invalid.receipt.kind = crate::generated::IntegrationControlKind::Progress;
            invalid.receipt.failureline = 0;
            assert!(invalid.encode().is_err());
            invalid.receipt.kind = crate::generated::IntegrationControlKind::Advance;
            invalid.receipt.progress = 0;
            assert!(driver.receive_control(invalid.receipt).is_err());
            assert!(driver.reporting.state_is_absent());
        }
        let mut disabled = Controller::new(
            false,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        );
        disabled.fail_detail(|| panic!("disabled driver evaluated failure data"));
        assert!(disabled.failure.is_empty());
    }

    #[test]
    fn quiet_driver_and_disabled_frontend_collect_no_probe_state() {
        initialize_reporting(false, false);
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        );
        assert!(driver.running());
        assert!(DRIVER_ENABLED.with(std::cell::Cell::get));
        assert!(!reporting_enabled());
        assert!(!pixel_fixture_enabled());
        assert!(driver.reporting.state_is_absent());
        let (mut model, frame) = crate::view_model::test_support::explore_presentation();
        let mut surface = crate::view_model::test_support::physical_surface(frame);
        surface.viewer_identity = model.explore.snapshot.as_ref().and_then(|snapshot| {
            snapshot
                .selectedimage
                .map(|image| (snapshot.dataset.identity, u64::from(image)))
        });
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        SURFACE_DRAW_OBSERVER.with(|observer| {
            let observer = observer.borrow();
            assert!(observer.receipts.is_empty());
            assert!(observer.output.is_none());
            assert!(observer.subscription.is_none());
            assert_eq!(observer.identity, (0, 0));
        });
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.dataset.imagewidth = frame.content_width;
        snapshot.dataset.imageheight = frame.content_height;
        snapshot.detail.showoriginaldimensions = false;
        driver.viewer_scenario = "quiet".into();
        driver.phase = Phase::AwaitDetail(0);
        let settings = crate::view::settings::SettingsModel::default();
        let router = crate::view::router::Router::default();
        assert!(
            sampleable_presentation(
                Some(frame),
                crate::generated::PresentationSourceKind::Explore,
                frame.content_sequence,
            )
            .is_some()
        );
        assert!(
            displayed_detail(Some(surface), model.explore.snapshot.as_ref().unwrap()).is_none()
        );
        assert!(crate::presentation_surface::accept_publication(frame));
        assert!(
            displayed_detail(Some(surface), model.explore.snapshot.as_ref().unwrap()).is_some()
        );
        crate::presentation_surface::clear_drawn_detail();
        let stale = crate::view_model::test_support::physical_surface(
            crate::presentation_surface::FrameReady {
                content_sequence: frame.content_sequence + 1,
                ..frame
            },
        );
        let crop = surface.content_region();
        for (draw, expected_ready) in [
            (None, false),
            (Some((stale, crop)), false),
            (
                Some((
                    surface,
                    [1, 0, frame.content_width - 1, frame.content_height],
                )),
                false,
            ),
            (Some((surface, crop)), true),
        ] {
            if let Some((surface, crop)) = draw {
                crate::presentation_surface::record_drawn_detail(surface, crop);
            }
            drop(driver.advance(
                &model,
                &settings,
                1.0,
                &router,
                FeatureId::Explore,
                Some(crate::view_model::test_support::physical_surface(frame)),
            ));
            assert_eq!(driver.location_pending, expected_ready);
            assert_eq!(
                driver.phase,
                if expected_ready {
                    Phase::OpenAnnotation
                } else {
                    Phase::AwaitDetail(0)
                }
            );
            assert!(model.error.is_none());
            assert!(driver.reporting.state_is_absent());
            SURFACE_DRAW_OBSERVER.with(|observer| assert!(observer.borrow().receipts.is_empty()));
        }
        crate::presentation_surface::clear_drawn_detail();
        driver.phase = Phase::Complete;
        driver
            .reset_scenario(String::new(), String::new(), String::new(), String::new())
            .unwrap();
        assert!(driver.running());
        assert!(DRIVER_ENABLED.with(std::cell::Cell::get));
        assert!(!reporting_enabled());
        assert!(driver.reporting.state_is_absent());
        let generation = SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation);
        let disabled = Controller::new(
            false,
            false,
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        );
        assert!(!disabled.running());
        assert!(!DRIVER_ENABLED.with(std::cell::Cell::get));
        notify_driver_draw(
            EXPLORE_GALLERY,
            frame.content_sequence,
            frame.presentation_revision,
        );
        assert_eq!(
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().generation),
            generation
        );
    }

    #[test]
    fn scenario_reset_isolates_queued_messages_geometry_and_obsolete_subscription_teardown() {
        initialize_reporting(true, true);
        let mut controller = Controller::new(
            true,
            false,
            "old-source".into(),
            "old-output".into(),
            "384".into(),
            "atlas".into(),
        );
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
        let surface = crate::view_model::test_support::physical_surface(frame);
        let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(640.0, 480.0));
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        let old_receipt = current_receipt(EXPLORE_GALLERY).unwrap();
        old_output
            .try_send(Message::Located {
                control: EXPLORE_GALLERY.into(),
                bounds,
            })
            .unwrap();
        record_probe_draw(
            crate::view::workspace::STABLE_ID,
            surface,
            bounds,
            bounds,
            bounds,
        );
        let viewer = ViewerDraw {
            crop: surface.content_region(),
            container: bounds,
            image: bounds,
            fit_revision: 0,
        };
        report_surface_draw(
            crate::view::workspace::STABLE_ID,
            5,
            1,
            false,
            640,
            480,
            1,
            viewer,
        );
        controller.location_pending = true;
        controller.atlas_baseline = Some((1, 5));
        controller
            .reporting
            .observe(|reporting| reporting.reported_style_bits = 7);
        controller.annotation_pixels_receipt = Some(old_receipt.clone());
        controller.upscale_pixel_pending = Some(old_receipt);
        assert!(
            controller
                .reset_scenario(
                    "new-source".into(),
                    "new-output".into(),
                    "512".into(),
                    "upscale".into()
                )
                .is_err()
        );
        controller.phase = Phase::Complete;
        controller
            .reset_scenario(
                "new-source".into(),
                "new-output".into(),
                "512".into(),
                "upscale".into(),
            )
            .unwrap();
        assert!(!controller.location_pending);
        assert!(controller.atlas_baseline.is_none());
        assert!(controller.upscale_pixel_pending.is_none());
        assert!(controller.annotation_pixels_receipt.is_none());
        controller
            .reporting
            .observe(|reporting| assert_eq!(reporting.reported_style_bits, 0));
        assert!(current_receipt(EXPLORE_GALLERY).is_none());
        assert_eq!(
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().identity),
            (0, 0)
        );
        controller.location_pending = true; // Same widget may already be armed in the replacement.
        while let Ok(message) = receiver.try_recv() {
            assert!(controller.update(message).is_none());
        }
        assert!(controller.location_pending);
        assert!(controller.annotation_drawn.is_none());
        // A late result retains the original sender/generation even after reset.
        old_output
            .try_send(Message::SurfaceDrawn {
                presentation_revision: 5,
                source_revision: 1,
                viewer: None,
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.annotation_drawn.is_none());
        for control in [
            EXPLORE_GALLERY,
            explore::DETAIL_WORKSPACE_ID,
            crate::view::workspace::STABLE_ID,
        ] {
            assert!(
                probe_output(control).is_none(),
                "no physical receipt cannot schedule a probe"
            );
            report_surface_draw(control, 5, 1, false, 640, 480, 1, viewer);
            assert!(
                receiver.try_recv().is_err(),
                "no physical receipt cannot enqueue a draw"
            );
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
                explore::DETAIL_WORKSPACE_ID => {
                    assert_eq!(controller.viewer_drawn.take(), Some((5, 1, viewer)))
                }
                _ => assert_eq!(controller.annotation_drawn.take(), Some((5, 1))),
            }
        }
        let snapshot = std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
            &crate::view_model::test_support::explore_snapshot(),
        ));
        let draw = AtlasDraw {
            surface,
            snapshot,
            bounds,
            image: bounds,
            clip: bounds,
        };
        SURFACE_DRAW_OBSERVER
            .with(|observer| observer.borrow_mut().receipts.remove(EXPLORE_GALLERY));
        report_atlas_draw(draw.clone(), true, 1.0);
        assert!(receiver.try_recv().is_err());
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        report_atlas_draw(draw.clone(), true, 1.0);
        let queued = receiver.try_recv().unwrap();
        let moved_draw = AtlasDraw {
            image: Rectangle { x: 17.0, ..bounds },
            ..draw
        };
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
        old_output
            .try_send(Message::SurfaceDrawn {
                presentation_revision: 5,
                source_revision: 1,
                viewer: None,
            })
            .unwrap();
        assert!(
            !controller.accepts_message(&receiver.try_recv().unwrap()),
            "physical messages cannot use a generation-only output"
        );
        let replacement_subscription = std::sync::Arc::new(());
        SURFACE_DRAW_OBSERVER.with(|observer| {
            observer.borrow_mut().subscription = Some(replacement_subscription.clone())
        });
        drop(SurfaceDrawSubscription(old_subscription));
        assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.is_some()));
        assert!(current_receipt(EXPLORE_GALLERY).is_some());
        drop(SurfaceDrawSubscription(replacement_subscription));
        assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().output.is_none()));
        controller.phase = Phase::Failed;
        assert!(
            controller
                .reset_scenario(String::new(), String::new(), String::new(), String::new())
                .is_err()
        );
        initialize_reporting(false, false);
    }

    #[test]
    fn atlas_scroll_uses_real_geometry_to_add_one_row_without_resizing_pixels() {
        for size in [
            iced::Size::new(896.0, 896.0),
            iced::Size::new(896.0, 896.6667),
            iced::Size::new(896.0, 895.3333),
            iced::Size::new(600.0, 712.5),
        ] {
            for columns in [3, 4, 5, 10] {
                let (rows, fraction) = atlas_scroll_window(size, columns);
                assert!(fraction > 0.0 && fraction < 1.0);
                let geometry = |fraction| {
                    crate::view::explore::state::gallery_geometry_at_fraction(
                        size.width,
                        size.height,
                        crate::generated::VisualExtent {
                            width: 1920,
                            height: 1080,
                        },
                        columns,
                        1_000,
                        0,
                        fraction,
                    )
                    .unwrap()
                };
                let aligned = geometry(0.0);
                let fractional = geometry(fraction);
                assert_eq!(aligned.viewport().rowcount, rows);
                assert_eq!(fractional.viewport().rowcount, rows + 1);
                assert_eq!(
                    aligned.viewport().extent.width,
                    fractional.viewport().extent.width
                );
            }
        }
    }

    #[test]
    fn atlas_invalidation_cannot_retire_a_replacement_request_on_the_same_draw() {
        let mut fixture = ProbeFixture::new("atlas");
        let surface = fixture.surface;
        let bounds = fixture.bounds;
        let (controller, receiver) = (&mut fixture.controller, &mut fixture.receiver);
        let draw = AtlasDraw {
            surface,
            bounds,
            image: bounds,
            clip: bounds,
            snapshot: std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
                &crate::view_model::test_support::explore_snapshot(),
            )),
        };
        record_probe_draw(EXPLORE_GALLERY, surface, bounds, bounds, bounds);
        report_atlas_draw(draw.clone(), false, 1.0);
        controller.update(receiver.try_recv().unwrap());
        let mut old_pixels = atlas_probe_output(false).unwrap();
        let mut old_composition = atlas_probe_output(true).unwrap();
        old_pixels
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Invalidated,
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().atlas.is_none()));
        // A normal draw rearms the same physical frame after CSS/backing settles.
        report_atlas_draw(draw.clone(), false, 1.0);
        controller.update(receiver.try_recv().unwrap());
        let mut pixels = atlas_probe_output(false).unwrap();
        let mut composition = atlas_probe_output(true).unwrap();
        for (output, message) in [
            (
                &mut old_pixels,
                Message::AtlasPixels {
                    receipt: draw.clone(),
                    outcome: ProbeOutcome::Invalidated,
                },
            ),
            (
                &mut old_composition,
                Message::AtlasComposition {
                    receipt: draw.clone(),
                    outcome: ProbeOutcome::Invalidated,
                },
            ),
        ] {
            output.try_send(message).unwrap();
            let stale = receiver.try_recv().unwrap();
            assert!(!controller.accepts_message(&stale));
            controller.update(stale);
        }
        assert_eq!(
            SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow().atlas.clone()),
            Some(draw.clone())
        );
        assert!(controller.atlas_pixels.is_none() && controller.atlas_composition.is_none());
        pixels
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 1),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        composition
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(13, 13),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.atlas_pixels, Some(draw.clone()));
        assert_eq!(controller.atlas_composition, Some(draw));
    }

    #[test]
    fn pixel_outcome_adapter_preserves_invalidated_observed_and_failed() {
        assert_eq!(
            ProbeOutcome::decode(Some("invalidated"), [Some(0.0); 2]),
            ProbeOutcome::Invalidated
        );
        assert_eq!(
            ProbeOutcome::decode(Some("observed"), [Some(1.0), Some(0.0)]),
            ProbeOutcome::Observed(1, 0)
        );
        for (status, values) in [
            (None, [Some(0.0); 2]),
            (Some("invalidated"), [Some(1.0), Some(0.0)]),
            (Some("observed"), [None, Some(0.0)]),
            (Some("observed"), [Some(f64::NAN), Some(0.0)]),
            (Some("observed"), [Some(f64::INFINITY), Some(0.0)]),
            (Some("observed"), [Some(-1.0), Some(0.0)]),
            (Some("observed"), [Some(0.5), Some(0.0)]),
            (
                Some("observed"),
                [Some(f64::from(u32::MAX) + 1.0), Some(0.0)],
            ),
            (Some("failed"), [Some(0.0); 2]),
        ] {
            assert_eq!(ProbeOutcome::decode(status, values), ProbeOutcome::Failed);
        }
    }

    #[test]
    fn annotation_and_upscale_consumers_retire_invalidations_without_pixel_evidence() {
        for consumer in 0..4 {
            let mut fixture = ProbeFixture::new("copy");
            let surface = fixture.surface;
            let bounds = fixture.bounds;
            let frame = surface.frame.unwrap();
            let (controller, receiver) = (&mut fixture.controller, &mut fixture.receiver);
            let control = if consumer == 3 {
                explore::DETAIL_WORKSPACE_ID
            } else {
                "workflow.visual.workspace"
            };
            record_probe_draw(control, surface, bounds, bounds, bounds);
            let arm = |controller: &mut Controller, image| match consumer {
                0 | 1 => {
                    controller.phase = if consumer == 0 {
                        Phase::CopyCapabilityWait
                    } else {
                        Phase::CopySwatchWait
                    };
                    assert!(controller.prepare_control_probe());
                    controller.control_probe.take().unwrap().output
                }
                2 => {
                    controller.phase = Phase::CopyProductWait;
                    assert!(controller.prepare_annotation_probe(
                        frame.content_sequence,
                        frame.presentation_revision,
                        [frame.content_width, frame.content_height],
                        vec![1.0; 7]
                    ));
                    controller.annotation_probe.take().unwrap().output
                }
                _ => {
                    controller.phase = Phase::AwaitExploreReady;
                    controller
                        .prepare_upscale_probe(
                            image,
                            frame.content_sequence,
                            frame.presentation_revision,
                        )
                        .unwrap()
                }
            };
            let message = |outcome| match consumer {
                0 => Message::AnnotationControlPixels { outcome },
                1 => Message::AnnotationPixels {
                    revision: 0,
                    outcome,
                },
                2 => Message::AnnotationPixels {
                    revision: frame.content_sequence,
                    outcome,
                },
                _ => Message::UpscalePixels {
                    source: frame.content_sequence,
                    presentation: frame.presentation_revision,
                    outcome,
                },
            };
            let mut old = arm(controller, bounds);
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
            assert!(
                controller.annotation_pixels_receipt.is_none()
                    && controller.upscale_pixels.is_none()
            );

            let mut same_frame = arm(controller, bounds);
            old.try_send(message(ProbeOutcome::Invalidated)).unwrap();
            let stale = receiver.try_recv().unwrap();
            assert!(
                !controller.accepts_message(&stale),
                "a new request can own the same physical receipt"
            );
            controller.update(stale);
            same_frame
                .try_send(message(ProbeOutcome::Invalidated))
                .unwrap();
            controller.update(receiver.try_recv().unwrap());
            assert_eq!(controller.phase, phase);

            let moved = Rectangle { x: 17.0, ..bounds };
            record_probe_draw(control, surface, bounds, moved, bounds);
            let mut replacement = arm(controller, moved);
            for outcome in [
                ProbeOutcome::Invalidated,
                ProbeOutcome::Failed,
                ProbeOutcome::Observed(1, 1),
            ] {
                old.try_send(message(outcome)).unwrap();
                let stale = receiver.try_recv().unwrap();
                assert!(!controller.accepts_message(&stale));
                controller.update(stale);
            }
            assert_eq!(controller.phase, phase);
            let pending = if consumer == 3 {
                &controller.upscale_pixel_pending
            } else if consumer == 2 {
                &controller.annotation_pixels_pending
            } else {
                &controller.control_probe_receipt
            };
            assert_eq!(*pending, replacement.receipt);
            replacement
                .try_send(message(ProbeOutcome::Observed(1, 1)))
                .unwrap();
            controller.update(receiver.try_recv().unwrap());
            match consumer {
                0 => assert!(controller.copy_capability_ready),
                1 => assert!(controller.copy_swatch_ready),
                2 => assert_eq!(controller.annotation_pixels_receipt, replacement.receipt),
                _ => assert_eq!(
                    controller.upscale_pixels,
                    Some((frame.content_sequence, frame.presentation_revision, 1, 1))
                ),
            }
            // A new current observation must retain both measured and adapter
            // failures. Upscale's wait phase owns checksum/color validation.
            if consumer == 2 {
                controller.annotation_pixels_pending = None;
            }
            let mut current = arm(controller, moved);
            current
                .try_send(message(ProbeOutcome::Observed(1, 0)))
                .unwrap();
            controller.update(receiver.try_recv().unwrap());
            if consumer == 3 {
                assert_eq!(
                    controller.upscale_pixels,
                    Some((frame.content_sequence, frame.presentation_revision, 1, 0))
                );
                let mut current = arm(controller, moved);
                current.try_send(message(ProbeOutcome::Failed)).unwrap();
                controller.update(receiver.try_recv().unwrap());
            }
            assert_eq!(controller.phase, Phase::Failed);
            let mut malformed = arm(controller, moved);
            malformed
                .try_send(message(ProbeOutcome::decode(
                    Some("observed"),
                    [None, Some(1.0)],
                )))
                .unwrap();
            controller.update(receiver.try_recv().unwrap());
            assert_eq!(controller.phase, Phase::Failed);
        }
    }

    #[test]
    fn probe_preparation_keeps_original_frame_through_widget_location() {
        let mut fixture = ProbeFixture::new("copy");
        let surface = fixture.surface;
        let bounds = fixture.bounds;
        let frame = surface.frame.unwrap();
        let controller = &mut fixture.controller;
        for swatch in [false, true] {
            record_probe_draw("workflow.visual.workspace", surface, bounds, bounds, bounds);
            controller.phase = if swatch {
                Phase::CopyCapability
            } else {
                Phase::CopyProductWait
            };
            if swatch {
                controller.copy_swatch_color = [48.0, 80.0, 112.0];
                controller.copy_capability_available = true;
                assert!(controller.prepare_control_probe());
                let prepared = controller.control_probe.as_ref().unwrap();
                assert_eq!(prepared.color, controller.copy_swatch_color);
                assert!(prepared.available);
            } else {
                for (source, presentation, extent) in [
                    (
                        frame.content_sequence + 1,
                        frame.presentation_revision,
                        [frame.content_width, frame.content_height],
                    ),
                    (
                        frame.content_sequence,
                        frame.presentation_revision + 1,
                        [frame.content_width, frame.content_height],
                    ),
                    (
                        frame.content_sequence,
                        frame.presentation_revision,
                        [frame.content_width + 1, frame.content_height],
                    ),
                ] {
                    assert!(!controller.prepare_annotation_probe(
                        source,
                        presentation,
                        extent,
                        vec![1.0; 7],
                    ));
                    assert!(controller.annotation_probe.is_none());
                    assert!(controller.annotation_pixels_pending.is_none());
                }
                assert!(controller.prepare_annotation_probe(
                    frame.content_sequence,
                    frame.presentation_revision,
                    [frame.content_width, frame.content_height],
                    vec![1.0; 7]
                ));
                let prepared = controller.annotation_probe.as_ref().unwrap();
                assert_eq!(prepared.source, frame.content_sequence);
                assert_eq!(prepared.presentation, frame.presentation_revision);
                assert_eq!(prepared.extent, [frame.content_width, frame.content_height]);
                assert_eq!(prepared.pixels, vec![1.0; 7]);
            }
            controller.location_pending = true;
            let moved = Rectangle { x: 17.0, ..bounds };
            record_probe_draw("workflow.visual.workspace", surface, bounds, moved, bounds);
            assert!(
                !controller.prepare_annotation_probe(999, 999, [1, 1], Vec::new()),
                "pending location cannot be overwritten"
            );
            assert!(!controller.prepare_control_probe());
            let phase = controller.phase.clone();
            // The obsolete location cannot validate bounds or stamp a new
            // output onto the saved old frame. Normal advance can now rearm.
            controller.update(Message::Located {
                control: ANNOTATION_SURFACE.into(),
                bounds: Rectangle::default(),
            });
            assert_eq!(controller.phase, phase);
            assert!(!controller.location_pending);
            assert!(controller.annotation_probe.is_none() && controller.control_probe.is_none());
            assert!(
                controller.annotation_pixels_pending.is_none()
                    && controller.control_probe_receipt.is_none()
            );
        }
    }

    #[test]
    fn atlas_composition_samples_bound_all_three_grid_lines_at_required_columns_and_dpi() {
        initialize_reporting(true, true);
        PIXEL_FIXTURE_ENABLED.with(|enabled| enabled.set(true));
        for columns in [4, 10] {
            for dpi in [1.0, 1.5] {
                let mut snapshot = crate::view_model::test_support::explore_snapshot();
                snapshot.viewport.columns = columns;
                snapshot.viewport.rowcount = columns;
                snapshot.gallery.layout.columns = columns;
                snapshot.gallery.layout.rowcount = columns;
                snapshot.gallery.layout.rowcapacity = columns;
                snapshot.gallery.layout.roworigin = 0;
                snapshot.gallery.layout.cardextent = 100;
                snapshot.augmentation.enabled = false;
                snapshot.overlay.showlabels = false;
                snapshot.overlay.showmasks = true;
                snapshot.overlay.showboxes = true;
                snapshot.labels.clear(); // Background row, no annotation or source padding.
                snapshot.gallery.slots = vec![true; (columns * columns) as usize];
                let (_, mut frame) = crate::view_model::test_support::explore_presentation();
                frame.content_width = columns * 100;
                frame.content_height = columns * 100;
                let mut surface = crate::view_model::test_support::physical_surface(frame);
                surface.width = frame.content_width;
                surface.height = frame.content_height;
                let width = 800.0 * dpi;
                let bounds = Rectangle::new(iced::Point::ORIGIN, iced::Size::new(width, width));
                let draw = AtlasDraw {
                    surface,
                    snapshot: std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(
                        &snapshot,
                    )),
                    bounds,
                    image: bounds,
                    clip: bounds,
                };
                let samples = atlas_composition_samples(&draw).unwrap();
                assert_eq!(samples.count, ATLAS_GRID_SAMPLES * 10);
                assert_eq!(samples.card_count, 0);
                assert_eq!(samples.cards.len(), 256);
                let cell = width / columns as f32;
                // Independent raster strips at known physical positions, not
                // the shader's rounding/distance algorithm.
                let positions = [
                    0.5,
                    1.5,
                    2.5,
                    3.5,
                    cell - 1.5,
                    cell - 0.5,
                    cell + 0.5,
                    cell + 1.5,
                    cell + 2.5,
                    0.5,
                    1.5,
                    2.5,
                    3.5,
                ];
                let clean_indices = [3, 4, 8, 12];
                let white_indices = [1, 6, 10];
                for (index, point) in samples.points[..samples.count].chunks_exact(10).enumerate() {
                    let expected_position = if index < 9 {
                        [positions[index], cell / 2.0]
                    } else {
                        [cell * 1.5, positions[index]]
                    };
                    assert_eq!(point[2..4], expected_position);
                    let expected = if clean_indices.contains(&index) {
                        [48.0, 80.0, 112.0]
                    } else if white_indices.contains(&index) {
                        [255.0; 3]
                    } else {
                        [0.0; 3]
                    };
                    assert_eq!(point[4..7], expected);
                    assert_eq!(point[7], 255.0);
                    assert_eq!(point[8], 4.0);
                }
                // The real scrollbar can cover the last five logical pixels.
                // None of the thirteen required samples enters that overlay.
                assert!(
                    samples.points[..samples.count]
                        .chunks_exact(10)
                        .all(|point| point[2] < width - 5.0 * dpi)
                );
                let mut clipped = draw.clone();
                clipped.clip.y = cell;
                clipped.clip.height -= cell;
                assert!(atlas_composition_samples(&clipped).is_none());
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
        for stage in 0..=3 {
            let first = Phase::CopyListSetup {
                stage,
                revision: 10,
            };
            let settled = Phase::CopyListSetup {
                stage,
                revision: 11,
            };
            assert_eq!(first.deadline_class(), "work");
            assert_eq!(settled.deadline_class(), "work");
            assert_ne!(first, settled);
            assert_eq!(
                settled,
                Phase::CopyListSetup {
                    stage,
                    revision: 11
                }
            );
        }
        assert_eq!(Phase::Complete.deadline_class(), "work");
        assert_eq!(Phase::AwaitSettings.deadline_class(), "interaction");
    }

    #[test]
    fn measured_reveal_handles_both_edges_visible_and_oversized_controls() {
        assert_eq!(reveal_axis(120.0, 40.0, 100.0, 200.0), 0.0);
        assert_eq!(reveal_axis(80.0, 40.0, 100.0, 200.0), -21.0);
        assert_eq!(reveal_axis(280.0, 40.0, 100.0, 200.0), 21.0);
        assert_eq!(reveal_axis(120.0, 400.0, 100.0, 200.0), 20.0);
        assert_eq!(reveal_axis(100.0, 200.0, 100.0, 200.0), 0.0);
        assert_eq!(reveal_axis(500.0, 0.0, 100.0, 200.0), 0.0);
        assert_eq!(reveal_axis(500.0, 40.0, 100.0, 0.0), 0.0);
        for start in [30.2, 2598.2] {
            let offset = reveal_axis(start, 46.4, 52.0, 771.3).round();
            assert!(start - offset >= 52.0);
            assert!(start - offset + 46.4 <= 823.3);
        }
    }

    #[test]
    fn annotation_reveal_proves_subregions_without_rescaling_full_geometry() {
        let full = Rectangle {
            x: -100.0,
            y: -200.0,
            width: 1000.0,
            height: 1600.0,
        };
        let viewport = Rectangle {
            x: 0.0,
            y: 50.0,
            width: 700.0,
            height: 500.0,
        };
        let measured = ControlBounds {
            target: full,
            page: viewport,
            horizontal: viewport,
        };
        let visible = measured.visible().unwrap();
        assert_eq!(visible, viewport);
        assert!(!contains_rectangle(
            visible,
            measured.requested(AnnotationReveal::Control).unwrap()
        ));
        let request = AnnotationReveal::Source {
            extent: [1000.0, 1600.0],
            region: Rectangle {
                x: 250.0,
                y: 300.0,
                width: 10.0,
                height: 20.0,
            },
            margin: 2.0,
        };
        let requested = measured.requested(request).unwrap();
        assert_eq!(
            requested,
            Rectangle {
                x: 148.0,
                y: 98.0,
                width: 14.0,
                height: 24.0
            }
        );
        assert!(contains_rectangle(visible, requested));
        assert_eq!(measured.target, full);
        let trailing = ControlBounds {
            target: Rectangle { y: 400.0, ..full },
            ..measured
        };
        assert!(!contains_rectangle(
            viewport,
            trailing.requested(request).unwrap()
        ));
        let clamped = ControlBounds {
            target: Rectangle {
                x: 650.0,
                width: 100.0,
                ..full
            },
            ..measured
        };
        assert!(!contains_rectangle(
            clamped.visible().unwrap(),
            clamped.target
        ));
        let missing = ControlBounds {
            page: Rectangle::default(),
            ..measured
        };
        assert!(missing.visible().is_none());
        assert!(missing.requested(request).is_none());
        let absent = ControlBounds {
            target: Rectangle::default(),
            ..measured
        };
        assert!(absent.visible().is_none());
        assert!(absent.requested(request).is_none());
    }

    #[test]
    fn annotation_layout_sequence_supports_initially_wide_and_narrow_scales() {
        let constraint = crate::generated::constraint_uiuiscale();
        let minimum = constraint.minimum.unwrap() as f32;
        let maximum = constraint.maximum.unwrap() as f32;
        assert_eq!(maximum, 1.75);
        for (original, initially_narrow) in [(1.0, false), (maximum, true)] {
            assert!((minimum..=maximum).contains(&original));
            let physical_width = 1500.0;
            let original_width = physical_width / original;
            assert_eq!(
                original_width < crate::view::PAGE_MIN_WIDTH,
                initially_narrow
            );
            let wide = annotation_layout_scale(original, original_width, false).unwrap();
            assert!(physical_width / wide >= crate::view::PAGE_MIN_WIDTH);
            let narrow = annotation_layout_scale(wide, physical_width / wide, true).unwrap();
            assert!(physical_width / narrow < crate::view::PAGE_MIN_WIDTH);
            let mut controller = Controller::new(
                false,
                false,
                String::new(),
                String::new(),
                "512".into(),
                "copy".into(),
            );
            controller.copy_original_scale = original;
            let model = ApplicationModel::default();
            drop(controller.copy_scale_transition(&model, narrow, CopyScaleStage::Restore));
            assert_eq!(controller.copy_requested_scale, original);
            assert_eq!(
                controller.phase,
                Phase::CopyAwaitScale(CopyScaleStage::Restore)
            );
        }
    }

    #[test]
    fn annotation_narrow_scale_obeys_native_bounds_at_packaged_dpi_widths() {
        let constraint = crate::generated::constraint_uiuiscale();
        for (unscaled_width, current_scale) in
            [(1500.0, 1.0), (1280.0, 1.5), (1500.0, 1.75), (1000.0, 1.25)]
        {
            let logical_width = unscaled_width / current_scale;
            let scale = annotation_layout_scale(current_scale, logical_width, true)
                .expect("packaged width reaches narrow layout");
            assert!(f64::from(scale) >= constraint.minimum.unwrap());
            assert!(f64::from(scale) <= constraint.maximum.unwrap());
            assert!(unscaled_width / scale < crate::view::PAGE_MIN_WIDTH);
        }
        let maximum = constraint.maximum.unwrap() as f32;
        assert!(annotation_layout_scale(maximum, 1920.0 / maximum, true).is_err());
        assert!(annotation_layout_scale(1.0, 1920.0, true).is_err());
        assert!(annotation_layout_scale(1.0, f32::NAN, true).is_err());
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
                width: 200,
                height: 200,
                frame: None,
                crop: None,
                viewer_identity: None,
                fit_revision: 0,
            },
            snapshot: std::sync::Arc::new(crate::generated::ExploreImageMetadata::from(&snapshot)),
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
        SURFACE_DRAW_OBSERVER.with(|observer| {
            observer.borrow_mut().output = Some(ScenarioOutput::new(controller.generation, sender))
        });
        record_probe_draw(
            EXPLORE_GALLERY,
            draw.surface,
            draw.bounds,
            draw.image,
            draw.clip,
        );
        atlas_probe_output(false)
            .unwrap()
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_pixels.is_none());
        atlas_probe_output(false)
            .unwrap()
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(0, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_pixels.is_none());
        atlas_probe_output(false)
            .unwrap()
            .try_send(Message::AtlasPixels {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 1),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.atlas_pixels, Some(draw.clone()));
        atlas_probe_output(true)
            .unwrap()
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_composition.is_none());
        atlas_probe_output(true)
            .unwrap()
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(0, 0),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert!(controller.atlas_composition.is_none());
        atlas_probe_output(true)
            .unwrap()
            .try_send(Message::AtlasComposition {
                receipt: draw.clone(),
                outcome: ProbeOutcome::Observed(1, 1),
            })
            .unwrap();
        controller.update(receiver.try_recv().unwrap());
        assert_eq!(controller.atlas_composition, Some(draw));
        SURFACE_DRAW_OBSERVER.with(|observer| observer.borrow_mut().output = None);
        initialize_reporting(false, false);
    }

    #[test]
    fn inactive_controllers_do_not_observe_snapshots_or_consume_callbacks() {
        initialize_reporting(false, false);
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
            assert!(controller.reporting.state_is_absent());

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
