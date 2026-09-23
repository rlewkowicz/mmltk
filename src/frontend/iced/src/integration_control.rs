use crate::generated::FeatureId;
use crate::integration_control::probe::{reset_observer, surface_draw_stream};
use crate::integration_control::widget_ops::click;
use crate::message::Message as RootMessage;
use crate::view::{annotation, explore, train};
use crate::view_model::{ApplicationModel, ConnectionState};
use iced::{Rectangle, Task};
mod lifecycle;
mod dataset_presentation;
pub(crate) use dataset_presentation::{observe_text as observe_dataset_text, TextKind as DatasetTextKind};
mod pixel_checks;
mod probe;
mod retained;
mod widget_ops;
pub use pixel_checks::{AtlasDraw, FpsPixelOutcome, ProbeOutcome};
pub(crate) use pixel_checks::{report_atlas_draw, sample_boundary_pixels};
#[cfg(test)]
pub(crate) use probe::tests::ProbeFixture;
pub use probe::{ProbeReceipt, ViewerDraw};
pub(crate) use probe::{record_probe_draw, report_surface_draw, report_workspace_fps};
mod annotation_checks;
mod annotation_product;
mod reporting;
pub(crate) use reporting::metric_projection as report_metric_projection;
pub(crate) use reporting::primary_action_draw;
mod workflows;

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
        let output = probe::scenario_output();
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

pub const EXPLORE_DATASET_PANE: &str = explore::DATASET_PANE_ID;
pub const EXPLORE_DETAILS_PANE: &str = explore::DETAILS_PANE_ID;
pub const EXPLORE_DETAIL_CLOSE: &str = explore::DETAIL_CLOSE_ID;

const EXPLORE_GALLERY: &str = explore::GALLERY_WORKSPACE_ID;

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

// Fixture demand windows are disjoint and separated beyond the native four
// neighbour rows. This does not change the producer's admission/cache policy.

const ANNOTATION_SURFACE: &str = annotation::WORKSPACE_ID;

#[derive(Debug, Clone)]
pub enum Message {
    DatasetDrawn(dataset_presentation::Frame),
    DatasetPixels(u16, bool),
    DatasetDisclosureToggle,
    DatasetInputDelivered(u8, bool),
    PrimaryActionPixels {
        control: String,
        active: bool,
        token: u32,
    },
    PrimaryActionMeasure {
        control: String,
        token: u32,
    },
    PrimaryActionMeasured {
        control: String,
        token: u32,
        bounds: [Rectangle; 3],
    },
    #[cfg(any(target_arch = "wasm32", test))]
    ConfidenceInputDelivered(u8, bool),
    WorkflowPixels {
        picture: workflows::Picture,
        index: u8,
        outcome: ProbeOutcome,
    },
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
    ChartInputDelivered(bool),
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
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationExpectInitialAtlas)]
    fn expect_initial_atlas_js();
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
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationWorkflowPixels)]
    fn workflow_pixels_js(
        control: &str,
        css_bounds: &[f64],
        chart: bool,
        progress: bool,
        source: f64,
        presentation: f64,
        caption_stage: i32,
        caption_case: u32,
        caption_patches: &[f64],
        completed: &wasm_bindgen::JsValue,
        gallery_tile: &[f64],
        confidence: &[f64],
    );
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
    fn wheel_js(x: f64, y: f64, delta: f64, control: bool, pixels: bool) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationChartInput)]
    fn chart_input_js(
        x: f64,
        y: f64,
        width: f64,
        height: f64,
        pan: bool,
        completed: &wasm_bindgen::JsValue,
    ) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationChartSettled)]
    fn chart_settled_js(completed: &wasm_bindgen::JsValue);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationSliderDrag)]
    fn slider_drag_js(x: f64, y: f64, width: f64, height: f64) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationReplaceNumber)]
    fn replace_number_js(x: f64, y: f64, value: &str, selection_length: u32) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationConfidenceInput)]
    fn confidence_input_js(
        x: f64,
        y: f64,
        action: u8,
        value: &str,
        completed: &wasm_bindgen::JsValue,
    ) -> u32;
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
enum CopyScaleStage {
    Wide,
    Narrow,
    Restore,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Phase {
    Workflows(workflows::Step),
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
    PagePrimary(FeatureId),
    AwaitPagePrimary(FeatureId),
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
    AdvancedLayout(usize),
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
    BenchmarkChoice(usize),
    AwaitBenchmarkChoice(usize),
    DatasetSource,
    AwaitDatasetSource,
    CompiledDirectory,
    AwaitCompiledDirectory,
    PerceptualControl(usize),
    AwaitPerceptualControl(usize),
    CompileDimensions,
    AwaitCompileDimensions,
    CompileResizeMode,
    AwaitCompileResizeMode,
    CompileResolution,
    AwaitCompileResolution,
    AwaitDatasetSettings(u64),
    Compile,
    AwaitCompileProgress,
    CompileProgress,
    CompileActionWithProgress,
    AwaitCompileCompletion,
    AwaitCompileCancelled,
    DatasetFixture(u8),
    DatasetDisclosure(u8),
    DatasetInput(u8),
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
            Self::Workflows(_)
            | Self::AwaitCompileProgress
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
            | Self::ViewerSquareBasic
            | Self::AwaitUpscale { .. }
            | Self::Complete => "work",
            _ => "interaction",
        }
    }
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
    driver: Driver,
    retained: retained::State,
    annotation_scenario: annotation_checks::State,
    workflows: workflows::State,
    lifecycle: lifecycle::State,
    pixel_checks: pixel_checks::State,
    probes: probe::Requests,
    widgets: widget_ops::RevealState,
}

struct Driver {
    reporting: reporting::Owner,
    generation: u64,
    phase: Phase,
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
    desired_dark: Option<bool>,
    reuse_compiled: bool,
    input_scale: f32,
}

impl Controller {
    pub(crate) fn view<'a>(&'a self, content: crate::fluent_theme::Element<'a, RootMessage>, model: &'a ApplicationModel)
        -> crate::fluent_theme::Element<'a, RootMessage> {
        if !reporting_enabled() { return content; }
        let generation = self.driver.generation;
        let content = if matches!(self.driver.phase, Phase::DatasetDisclosure(..)) {
            iced::widget::stack![content, iced::widget::container(
                crate::view::diagnostics::view(model, &self.lifecycle.presentation.diagnostics)
                    .map(move |_| RootMessage::Integration(Message::Scoped { generation, receipt: None,
                        message: Box::new(Message::DatasetDisclosureToggle) })))
                .id("integration.dataset.fixture").width(360).style(crate::fluent_theme::container_shell)].into()
        } else { content };
        let (key, fixture, width) = match self.driver.phase {
            Phase::DatasetInput(index) => (dataset_presentation::input_key(index), None, 0.0),
            Phase::DatasetDisclosure(index) => (50 + u16::from(index), None, 0.0),
            Phase::DatasetFixture(index) => (200 + u16::from(index), self.lifecycle.presentation.fixture.as_ref(),
                if index % 18 < 9 { 224.0 } else { 360.0 }),
            Phase::CompileDimensions => (40, None, 0.0),
            Phase::AwaitCompileDimensions => (41, None, 0.0),
            Phase::BenchmarkChoice(index) => (index as u16 + 20, None, 0.0),
            Phase::AwaitBenchmarkChoice(index) => (index as u16 + 1, None, 0.0),
            Phase::CompileProgress | Phase::CompileActionWithProgress => (100, None, 0.0),
            Phase::AwaitCompileCancelled => (101, None, 0.0),
            Phase::AwaitCompileCompletion => (102, None, 0.0),
            _ => (0, None, 0.0),
        };
        dataset_presentation::wrap(content, key, fixture, width, self.driver.input_scale, model.workflow.dataset.as_ref())
    }
    pub fn subscription(&self) -> iced::Subscription<Message> {
        if self.driver.running() {
            iced::Subscription::batch([
                if reporting_enabled()
                    || matches!(
                        self.driver.phase,
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
                .with(self.driver.generation)
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

    pub fn observe_workspace_message<'a>(
        &self,
        payload: impl FnOnce() -> (&'a crate::view::router::Message, FeatureId),
    ) {
        self.observe_reporting(|reporting| {
            let (message, active) = payload();
            reporting.observe_workspace_message(message, active, &self.driver.phase);
        });
    }

    pub(crate) fn observe_reporting(&self, observe: impl FnOnce(&mut reporting::State)) {
        if self.driver.running() {
            self.driver.reporting.observe(observe);
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
            retained: retained::State::default(),
            annotation_scenario: annotation_checks::State::default(),
            workflows: workflows::State::default(),
            lifecycle: lifecycle::State::default(),
            pixel_checks: pixel_checks::State::default(),
            probes: probe::Requests::default(),
            widgets: widget_ops::RevealState::default(),
            driver: Driver {
                reporting: reporting::Owner::new(),
                generation,
                phase: if enabled {
                    Phase::AwaitBootstrap
                } else {
                    Phase::Disabled
                },
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
                desired_dark: None,
                reuse_compiled: false,
                input_scale: 1.0,
            },
        }
    }

    pub(crate) fn reset_scenario(
        &mut self,
        dataset_source: String,
        compiled_directory: String,
        resolution: String,
        viewer_scenario: String,
    ) -> Result<(), &'static str> {
        if self.driver.running() || matches!(self.driver.phase, Phase::Failed) {
            return Err("scenario reset requires successful settlement");
        }
        let enabled = self.driver.generation != 0;
        *self = Self::new(
            enabled,
            self.driver.window_close,
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
        self.driver.session = SessionInputs {
            profile: profile.into(),
            source: self.driver.dataset_source.clone(),
            compiled: self.driver.compiled_directory.clone(),
        };
        if let Some((scenario, dark)) = self.driver.session.scenario(0) {
            self.driver.viewer_scenario = scenario.into();
            self.driver.desired_dark = Some(dark);
            self.driver.reuse_compiled = true;
            if scenario == "square" {
                self.driver.dataset_source = square_source;
                self.driver.compiled_directory = square_compiled;
                self.driver.resolution = "384".into();
            }
        } else {
            self.driver.reuse_compiled = profile != "compile";
        }
    }

    pub(crate) fn receive_control_transition(
        &mut self,
        receipt: crate::generated::IntegrationControlReceipt,
    ) -> Result<(), &'static str> {
        use crate::generated::IntegrationControlKind as Kind;
        if !crate::generated::integration_receipt_valid(&receipt)
            || !crate::generated::integration_server_command(receipt.kind)
        {
            self.driver.fail("invalid integration control policy");
            return Err("invalid integration control policy");
        }
        if receipt.kind != Kind::Advance {
            if receipt.sequence != self.driver.control_sequence
                || receipt.failureline != 0
                || receipt.progress != 0
            {
                self.driver.fail("invalid capacity control identity");
                return Err("invalid capacity control identity");
            }
            if receipt.kind == Kind::GalleryReadCompletionHeld {
                return self
                    .retained
                    .hold_gallery_completion(&mut self.driver, &receipt);
            }
            self.driver.phase = match (receipt.kind, &self.driver.phase) {
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
                    self.driver
                        .fail("duplicate, stale, or reordered capacity control");
                    return Err("duplicate, stale, or reordered capacity control");
                }
            };
            return Ok(());
        }
        if receipt.kind != crate::generated::IntegrationControlKind::Advance
            || receipt.failureline != 0
            || self.driver.control_sequence.checked_add(1) != Some(receipt.sequence)
            || !matches!(self.driver.phase, Phase::Complete)
            || self.driver.control_phase.as_ref() != Some(&Phase::Complete)
        {
            self.driver
                .fail("premature, duplicate or stale scenario advance");
            return Err("premature, duplicate or stale scenario advance");
        }
        let index = usize::try_from(receipt.sequence - 1).map_err(|_| "scenario index overflow")?;
        let Some((scenario, dark)) = self.driver.session.scenario(index) else {
            if self.driver.window_close {
                #[cfg(target_arch = "wasm32")]
                if window_close_js() != 1 {
                    self.driver.fail("Firefox window close dispatch failed");
                    return Err("Firefox window close dispatch failed");
                }
                self.driver.control_sequence = receipt.sequence;
                return Ok(());
            }
            self.driver.fail("advance beyond the configured workflow");
            return Err("advance beyond the configured workflow");
        };
        let session = self.driver.session.clone();
        self.reset_scenario(
            session.source.clone(),
            session.compiled.clone(),
            "512".into(),
            scenario.into(),
        )?;
        self.driver.session = session;
        self.driver.control_sequence = receipt.sequence;
        self.driver.desired_dark = Some(dark);
        self.driver.reuse_compiled = !scenario.is_empty();
        Ok(())
    }

    pub(crate) fn observe_annotation_open(
        &mut self,
        request: crate::generated::AnnotationOpen,
        document_epoch: u64,
    ) {
        if self.driver.running() {
            self.annotation_scenario
                .observe_open(request, document_epoch);
        }
    }

    #[cfg(test)]
    pub(crate) fn annotation_open_for_test(
        &self,
    ) -> Option<(&crate::generated::AnnotationOpen, u64)> {
        self.annotation_scenario.open_for_test()
    }

    pub(crate) fn publish_control(
        &mut self,
        connection: &mut crate::transport_connection::Connection,
    ) {
        if self.driver.generation == 0
            || self.driver.control_phase.as_ref() == Some(&self.driver.phase)
        {
            return;
        }
        if self.driver.viewer_scenario == "quiet"
            && matches!(self.driver.phase, Phase::Complete)
            && !connection.integration_pressure_settled()
        {
            return;
        }
        use crate::generated::IntegrationControlKind as Kind;
        let kind = match self.driver.phase {
            Phase::Disabled => return,
            Phase::Complete => Kind::Settled,
            Phase::Failed => Kind::Failed,
            Phase::AwaitCapacityArm => Kind::CapacityArmRequested,
            Phase::AwaitVisibleReadArm(_) => Kind::VisibleReadArmRequested,
            Phase::VisibleReadRelease(_, _) => Kind::VisibleReadReleaseRequested,
            _ => Kind::Progress,
        };
        let class = match self.driver.phase.deadline_class() {
            "startup" => 1,
            "work" => 2,
            _ => 3,
        };
        let Some(progress) = self
            .driver
            .control_progress
            .checked_add(4)
            .and_then(|value| value.checked_add(class))
        else {
            self.driver.fail("integration progress identity exhausted");
            return;
        };
        match connection.send_integration_control(crate::generated::IntegrationControlReceipt {
            kind,
            sequence: self.driver.control_sequence,
            progress,
            readgeneration: match self.driver.phase {
                Phase::VisibleReadRelease(_, generation) => generation,
                _ => 0,
            },
            compiledindex: match self.driver.phase {
                Phase::AwaitVisibleReadArm(index) | Phase::VisibleReadRelease(index, _) => index,
                _ => 0,
            },
            failure: self.driver.failure.clone(),
            failureline: if matches!(self.driver.phase, Phase::Failed) {
                self.driver.failure_line
            } else {
                0
            },
        }) {
            Ok(_) => {
                self.driver.control_phase = Some(self.driver.phase.clone());
                self.driver.control_progress = progress & !3;
            }
            Err(crate::transport_connection::OutboundSendError::Capacity) => {}
            Err(_) => self.driver.fail("integration control delivery failed"),
        }
    }

    pub(crate) fn observe_upscale_request(&mut self, kernel: crate::generated::UpscaleKernel) {
        self.retained.observe_upscale_request(&self.driver, kernel);
    }

    pub(crate) fn hold_initial_upscale(&self) -> bool {
        // The semantics fixture must draw the original scene before testing
        // all derived methods. Other scenarios exercise automatic Basic.
        self.driver.viewer_scenario == "semantics"
            && matches!(
                self.driver.phase,
                Phase::AwaitDetail(_)
                    | Phase::DetailOriginal { .. }
                    | Phase::AwaitDetailOriginal { .. }
                    | Phase::DetailFit
                    | Phase::AwaitDetailFit
                    | Phase::ViewerOverlay(_)
                    | Phase::AwaitViewerOverlay(_)
            )
    }

    pub(super) fn update_location(
        &mut self,
        control: String,
        bounds: Rectangle,
    ) -> Option<train::Message> {
        self.widgets.location_completed();
        if matches!(self.driver.phase, Phase::Workflows(_)) {
            self.workflows
                .workflow_located(&mut self.driver, &control, bounds);
            return None;
        }
        if !self.probes.accept_location() {
            return None;
        }
        if matches!(self.driver.phase, Phase::ViewerNoAspect) {
            self.retained
                .viewer_selector_located(&mut self.driver, &self.probes, bounds);
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
            self.driver
                .fail("Iced widget operation could not locate the stable identity");
            return None;
        }
        self.driver
            .reporting
            .observe(|reporting| reporting.located(&self.driver.phase, &control, bounds));
        let expected = self
            .lifecycle
            .expected_lifecycle(&self.driver)
            .or_else(|| self.retained.expected_retained(&self.driver))
            .or_else(|| {
                self.annotation_scenario
                    .expected_annotation_checks(&self.driver)
            });
        let Some(expected) = expected else {
            self.driver.fail("unexpected Iced widget operation result");
            return None;
        };
        if control != expected {
            self.driver
                .fail("Iced widget operation returned the wrong stable identity");
            return None;
        }
        self.driver
            .reporting
            .observe(|reporting| reporting.style(&control, bounds));
        let input_bounds =
            crate::presentation_surface::physical_bounds(bounds, self.driver.input_scale);
        match self.driver.phase {
            Phase::SettingsOpen
            | Phase::SettingsModal
            | Phase::SettingsGroup(..)
            | Phase::SettingsScaleDrag
            | Phase::SettingsShowFps
            | Phase::SettingsRestoreShowFps
            | Phase::SettingsNumeric { .. }
            | Phase::SettingsFooter
            | Phase::SettingsReset
            | Phase::SettingsClose
            | Phase::TrainNavigation
            | Phase::PageNavigation(..)
            | Phase::PageRegion { .. }
            | Phase::PagePrimary(..)
            | Phase::TrainModelCard
            | Phase::TrainModelPart(..)
            | Phase::TrainModelProgress
            | Phase::ReturnTrain
            | Phase::AdvancedField(..)
            | Phase::AdvancedSpinnerEdge { .. }
            | Phase::AdvancedSpinnerWheel(..)
            | Phase::AdvancedNumericEdit(..)
            | Phase::AdvancedAssignment
            | Phase::AdvancedMatchFree(..)
            | Phase::AdvancedDenoisingToggle
            | Phase::AdvancedDenoising(..)
            | Phase::AdvancedLayout(..)
            | Phase::ErrorModal
            | Phase::ErrorCopy
            | Phase::ErrorDismiss
            | Phase::TrainCard
            | Phase::DatasetBrowse
            | Phase::BenchmarkOverride
            | Phase::BenchmarkRestore
            | Phase::BenchmarkChoice(..)
            | Phase::DatasetSource
            | Phase::CompiledDirectory
            | Phase::PerceptualControl(..)
            | Phase::CompileDimensions
            | Phase::CompileResizeMode
            | Phase::CompileResolution
            | Phase::Compile
            | Phase::CompileActionWithProgress
            | Phase::CompileProgress
            | Phase::DatasetStatus => self.lifecycle.located_lifecycle(
                &mut self.driver,
                &mut self.widgets,
                bounds,
                input_bounds,
            ),
            Phase::ExploreNavigation
            | Phase::ExploreOpen
            | Phase::ExploreCloseDetail
            | Phase::ExploreDatasetPane
            | Phase::ExploreDetailsPane
            | Phase::ExploreNumericControl { .. }
            | Phase::ExploreNumericReveal { .. }
            | Phase::ExplorePolicyOrder(..)
            | Phase::ExplorePolicyRange(..)
            | Phase::ExplorePolicyRangeVisible(..)
            | Phase::ExplorePolicyOverlay(..)
            | Phase::ExplorePolicyOverlayVisible(..)
            | Phase::ExploreAugmentationToggle { .. }
            | Phase::ExploreAugmentationReroll { .. }
            | Phase::ExploreReshuffle { .. }
            | Phase::ExploreCard { .. }
            | Phase::GallerySweep
            | Phase::GalleryLater(..)
            | Phase::GalleryImage(..)
            | Phase::DetailOriginal { .. }
            | Phase::DetailFit
            | Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(..)
            | Phase::AtlasAwaySelect(..)
            | Phase::AtlasCapacity
            | Phase::AtlasEmpty
            | Phase::AtlasOverlay(..)
            | Phase::ViewerOverlay(..)
            | Phase::ViewerNoAspect
            | Phase::StartUpscale { .. }
            | Phase::DetailNext(..)
            | Phase::DetailPrevious(..)
            | Phase::DetailCloseEvidence
            | Phase::ExploreDatasetReopen { .. }
            | Phase::GalleryReselect => self.retained.located_retained(
                &mut self.driver,
                &mut self.probes,
                &mut self.widgets,
                control,
                bounds,
                input_bounds,
            ),
            Phase::CopyUndo { .. }
            | Phase::CopyRedo { .. }
            | Phase::CopySave
            | Phase::CopyCapability
            | Phase::CopyLayout(..)
            | Phase::OpenAnnotation
            | Phase::AnnotationSidebar { .. }
            | Phase::AnnotationTimeline { .. }
            | Phase::AnnotationOperation { .. }
            | Phase::AnnotationStop { .. }
            | Phase::AnnotationBrush { .. }
            | Phase::AnnotationTool { .. }
            | Phase::AnnotationSurface(..)
            | Phase::AnnotationPointer(..)
            | Phase::AwaitPointer(..)
            | Phase::CopyProductWait => self.annotation_scenario.located_annotation_checks(
                &mut self.driver,
                &mut self.probes,
                control,
                bounds,
                input_bounds,
            ),
            _ => self.driver.click_located(input_bounds),
        }
    }

    pub(super) fn update_probe_message(&mut self, message: Message) -> Option<train::Message> {
        if !self
            .probes
            .accepts_message(&self.driver, &self.pixel_checks, &message)
        {
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
            Message::DatasetInputDelivered(next, valid) => {
                self.lifecycle.presentation.input_pending = false;
                if valid { self.driver.phase = Phase::DatasetInput(next); }
                else { self.driver.fail("Dataset input delivery was invalidated"); }
                return None;
            }
            Message::DatasetDisclosureToggle => {
                self.lifecycle.presentation.diagnostics.update(crate::view::diagnostics::Message::Toggled);
                return None;
            }
            Message::DatasetDrawn(frame) => {
                let bounded = !(100..=102).contains(&frame.key);
                self.lifecycle.presentation.observe(frame);
                if bounded && self.lifecycle.presentation.frames > 180 { self.driver.fail("Dataset observation exceeded its frame budget"); }
                return None;
            }
            Message::DatasetPixels(key, valid) => {
                if valid { self.lifecycle.presentation.pixels = Some(key); }
                else { self.driver.fail("Dataset rendered pixel observation failed"); }
                return None;
            }
            Message::PrimaryActionMeasure { control, token } => {
                self.driver.reporting.measure_primary(control, token);
                return None;
            }
            Message::PrimaryActionMeasured {
                control,
                token,
                bounds,
            } => {
                reporting::primary_action_measured(control, token, bounds);
                return None;
            }
            Message::PrimaryActionPixels {
                control,
                active,
                token,
            } => {
                if reporting::primary_action_current(&control, token) {
                    self.lifecycle.primary_action_pixels(&control, active);
                    self.workflows
                        .primary_action_pixels(&self.driver, &control, active);
                }
                return None;
            }
            #[cfg(any(target_arch = "wasm32", test))]
            Message::ConfidenceInputDelivered(stage, delivered) => {
                self.workflows
                    .confidence_input_delivered(&mut self.driver, stage, delivered);
                return None;
            }
            Message::WorkflowPixels {
                picture,
                index,
                outcome,
            } => {
                self.workflows
                    .workflow_pixels(&mut self.driver, picture, index, outcome);
                return None;
            }
            Message::ReportingDisabled => {
                if matches!(
                    self.driver.phase,
                    Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
                ) {
                    self.pixel_checks.cancel_workspace_fps(&mut self.driver);
                }
                return None;
            }
            Message::Scoped { .. } | Message::ProbeCompleted { .. } | Message::Advance => {
                return None;
            }
            Message::ChartInputDelivered(delivered) => {
                if delivered {
                    self.workflows.chart_input_delivered(&mut self.driver);
                } else {
                    self.driver
                        .fail("Chart input/render callback was invalidated");
                }
                return None;
            }
            Message::NumberWheelDelivered => {
                self.retained.callback(
                    &mut self.driver,
                    &mut self.probes,
                    Message::NumberWheelDelivered,
                    request_receipt,
                );
                self.lifecycle.wheel_delivered(&mut self.driver);
                self.workflows.wheel_delivered(&mut self.driver);
                return None;
            }

            Message::Located { control, bounds } => (control, bounds),
            message @ Message::GalleryDrawn { .. } => {
                self.retained.callback(
                    &mut self.driver,
                    &mut self.probes,
                    message,
                    request_receipt,
                );
                return None;
            }
            message @ Message::SurfaceDrawn { .. } => {
                self.retained.callback(
                    &mut self.driver,
                    &mut self.probes,
                    message,
                    request_receipt,
                );
                return None;
            }
            message @ (Message::AtlasComposition { .. }
            | Message::AtlasDrawn { .. }
            | Message::AtlasPixels { .. }
            | Message::GalleryMouseDelivered
            | Message::NumberClipboardPrepared { .. }
            | Message::NumberInvalidDelivered
            | Message::NumberPasteDelivered(_)
            | Message::NumberPasteRead { .. }
            | Message::UpscalePixels { .. }) => {
                self.retained.callback(
                    &mut self.driver,
                    &mut self.probes,
                    message,
                    request_receipt,
                );
                return None;
            }
            message @ (Message::AnnotationControlPixels { .. }
            | Message::AnnotationPixels { .. }) => {
                self.annotation_scenario.callback(
                    &mut self.driver,
                    &mut self.probes,
                    message,
                    request_receipt,
                );
                return None;
            }
            message @ (Message::WorkspaceFpsDrawn(_) | Message::WorkspaceFpsPixels(_)) => {
                self.pixel_checks
                    .callback(&mut self.driver, message, request_receipt);
                return None;
            }
        };
        self.update_location(control, bounds)
    }
    pub(crate) fn accepts_message(&self, message: &Message) -> bool {
        self.probes
            .accepts_message(&self.driver, &self.pixel_checks, message)
    }

    fn advance_transition(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
        router: &crate::view::router::Router,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
    ) -> Task<RootMessage> {
        let frame = surface.and_then(|surface| surface.frame);
        self.driver.report_phase_progress();
        if !self.driver.running() {
            return Task::none();
        }
        if let Some(dark) = self.driver.desired_dark {
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
            self.driver.desired_dark = None;
        }
        if !reporting_enabled()
            && matches!(
                self.driver.phase,
                Phase::AwaitWorkspaceFps | Phase::AwaitWorkspaceFpsPixels
            )
        {
            self.pixel_checks.cancel_workspace_fps(&mut self.driver);
        }
        self.driver.input_scale = applied_scale;
        self.probes
            .observe_presentation(&mut self.driver, model, frame);
        self.driver
            .reporting
            .observe(|reporting| reporting.explore_snapshot(model, settings));
        if let Some(error) = model.error.as_ref()
            && !matches!(
                self.driver.phase,
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
            self.driver.fail_detail(|| {
                format!("{:?}: {}: {}", error.kind, error.title, error.detail).into()
            });
            return Task::none();
        }
        match self.driver.phase.clone() {
            Phase::AwaitBootstrap => self.driver.bootstrap(&mut self.widgets, model, settings),
            Phase::Workflows(step) => self.workflows.advance_workflows(
                &mut self.widgets,
                &mut self.driver,
                step,
                model,
                settings,
                active,
                surface,
                router,
            ),
            Phase::AwaitWorkspaceFps
            | Phase::AwaitWorkspaceFpsPixels
            | Phase::RestoreWorkspaceFps
            | Phase::AwaitWorkspaceFpsRestored => self.pixel_checks.advance_pixel_checks(
                &mut self.driver,
                model,
                settings,
                applied_scale,
            ),
            Phase::AtlasPixelColumns(..)
            | Phase::AtlasPixelRestore
            | Phase::ViewerConfirmSettings
            | Phase::ViewerRestoreSettings
            | Phase::ViewerDepart
            | Phase::ViewerReenter
            | Phase::ViewerAwaitDisconnect
            | Phase::ViewerReconnect
            | Phase::Disabled
            | Phase::Complete
            | Phase::Failed
            | Phase::ExploreNavigation
            | Phase::AwaitExplore
            | Phase::ExploreCloseDetail
            | Phase::AwaitExploreGallery
            | Phase::AwaitExplorePreparation(..)
            | Phase::ExploreOpen
            | Phase::AwaitExploreReady
            | Phase::AwaitExploreInitialPatch { .. }
            | Phase::AwaitExploreExactGridPatch { .. }
            | Phase::AwaitExploreExactGrid(..)
            | Phase::ExploreDatasetPane
            | Phase::ExploreDetailsPane
            | Phase::ExploreNumericStart(..)
            | Phase::ExploreNumericControl { .. }
            | Phase::ExploreNumericReveal { .. }
            | Phase::AwaitExploreNumeric { .. }
            | Phase::ExplorePolicyOrderReady
            | Phase::ExplorePolicyOrder(..)
            | Phase::AwaitExplorePolicyOrder(..)
            | Phase::ExplorePolicyRangeReady
            | Phase::ExplorePolicyRange(..)
            | Phase::ExplorePolicyRangeVisible(..)
            | Phase::AwaitExplorePolicyRange(..)
            | Phase::ExplorePolicyOverlayReady
            | Phase::ExplorePolicyOverlay(..)
            | Phase::ExplorePolicyOverlayVisible(..)
            | Phase::AwaitExplorePolicyOverlay(..)
            | Phase::AwaitExploreOverlayAll(..)
            | Phase::AwaitExploreOverlaySubset(..)
            | Phase::AwaitExploreOverlayRestored(..)
            | Phase::ExploreAugmentationToggle { .. }
            | Phase::AwaitExploreAugmentationToggle { .. }
            | Phase::ExploreAugmentationReroll { .. }
            | Phase::AwaitExploreAugmentationReroll { .. }
            | Phase::ExploreReshuffle { .. }
            | Phase::AwaitExploreReshuffle { .. }
            | Phase::ExploreCard { .. }
            | Phase::AwaitGalleryPatch { .. }
            | Phase::GalleryColdRead(..)
            | Phase::AwaitGalleryColdRead(..)
            | Phase::GallerySweep
            | Phase::AwaitGallerySweep
            | Phase::GalleryLaterReady
            | Phase::GalleryLater(..)
            | Phase::AwaitGalleryScroll(..)
            | Phase::GalleryImage(..)
            | Phase::AwaitDetail(..)
            | Phase::DetailOriginal { .. }
            | Phase::AwaitDetailOriginal { .. }
            | Phase::DetailFit
            | Phase::ViewerSelect
            | Phase::AtlasReturnSelect
            | Phase::AtlasResizeSelect(..)
            | Phase::AtlasAwaySelect(..)
            | Phase::AwaitAtlasCapacity
            | Phase::AtlasCapacity
            | Phase::AtlasRestoreColumns
            | Phase::AwaitAtlasColumns
            | Phase::AwaitAtlasEmpty
            | Phase::AtlasEmpty
            | Phase::AtlasRestoreFilter
            | Phase::AwaitAtlasRestored
            | Phase::AwaitAtlasWindow(..)
            | Phase::VisibleReadScroll(..)
            | Phase::AwaitVisibleReadHover(..)
            | Phase::AwaitVisibleReadPixels(..)
            | Phase::VisibleReadSelect(..)
            | Phase::AwaitVisibleReadSelection(..)
            | Phase::AwaitVisibleReadReturn(..)
            | Phase::AwaitVisibleReadOscillation(..)
            | Phase::VisibleReadRelease(..)
            | Phase::AwaitVisibleReadComplete(..)
            | Phase::AwaitAtlasAwayFilter(..)
            | Phase::AwaitAtlasAwayDetail(..)
            | Phase::AwaitAtlasAwayReturn
            | Phase::AwaitAtlasAwayRestore
            | Phase::AwaitCapacitySlots(..)
            | Phase::CapacityPublish
            | Phase::AwaitCapacityRetry
            | Phase::AtlasResizeStart
            | Phase::AwaitAtlasResizeGallery(..)
            | Phase::AwaitAtlasResizeDetail(..)
            | Phase::AwaitAtlasResizeMeasurement(..)
            | Phase::AwaitAtlasReturnReady
            | Phase::AwaitAtlasReturnDetail
            | Phase::AwaitAtlasOscillation(..)
            | Phase::AwaitAtlasScroll(..)
            | Phase::AtlasOverlay(..)
            | Phase::AwaitAtlasOverlay(..)
            | Phase::ViewerOverlay(..)
            | Phase::AwaitViewerOverlay(..)
            | Phase::ViewerSquareBasic
            | Phase::ViewerNoAspect
            | Phase::ViewerRapidGallery
            | Phase::ViewerRapidSelection(..)
            | Phase::AwaitDetailFit
            | Phase::StartUpscale { .. }
            | Phase::AwaitUpscale { .. }
            | Phase::DetailNext(..)
            | Phase::AwaitNext(..)
            | Phase::DetailPrevious(..)
            | Phase::AwaitPrevious(..)
            | Phase::DetailCloseEvidence
            | Phase::AwaitDetailClose
            | Phase::ExploreDatasetReopen { .. }
            | Phase::AwaitExploreDatasetReopen { .. }
            | Phase::GalleryReselect
            | Phase::AwaitDetailAgain => self.retained.advance_retained(
                &mut self.driver,
                &mut self.pixel_checks,
                &mut self.probes,
                &mut self.widgets,
                model,
                settings,
                router,
                active,
                surface,
            ),
            Phase::SettingsOpen
            | Phase::AwaitSettings
            | Phase::SettingsModal
            | Phase::SettingsGroup(..)
            | Phase::SettingsScaleDrag
            | Phase::AwaitSettingsScaleDrag
            | Phase::AwaitSettingsScaleRelease
            | Phase::AwaitSettingsScaleSnapshot
            | Phase::AwaitSettingsScaleRestoreDraft
            | Phase::AwaitSettingsScaleRestored
            | Phase::SettingsShowFps
            | Phase::AwaitSettingsShowFps
            | Phase::AwaitSettingsShowFpsChangedSnapshot
            | Phase::SettingsRestoreShowFps
            | Phase::AwaitSettingsShowFpsRestored
            | Phase::AwaitSettingsShowFpsSnapshot
            | Phase::SettingsNumeric { .. }
            | Phase::SettingsFooter
            | Phase::SettingsReset
            | Phase::SettingsClose
            | Phase::AwaitSettingsClosed
            | Phase::TrainNavigation
            | Phase::AwaitTrain
            | Phase::PageNavigation(..)
            | Phase::AwaitPage(..)
            | Phase::PageRegion { .. }
            | Phase::PagePrimary(..)
            | Phase::AwaitPagePrimary(..)
            | Phase::TrainModelCard
            | Phase::TrainModelPart(..)
            | Phase::TrainModelProgress
            | Phase::ReturnTrain
            | Phase::AwaitReturnTrain
            | Phase::AdvancedField(..)
            | Phase::AdvancedSpinnerEdge { .. }
            | Phase::AdvancedSpinnerWheel(..)
            | Phase::AdvancedSpinnerVerify { .. }
            | Phase::AdvancedSpinnerWheelVerify(..)
            | Phase::AdvancedNumericEdit(..)
            | Phase::AwaitAdvancedNumericDraft(..)
            | Phase::AwaitAdvancedNumericSnapshot(..)
            | Phase::AdvancedAssignment
            | Phase::AwaitAdvancedAssignmentDraft
            | Phase::AwaitAdvancedAssignmentSnapshot
            | Phase::AdvancedMatchFree(..)
            | Phase::AdvancedDenoisingToggle
            | Phase::AwaitAdvancedDenoisingDraft
            | Phase::AwaitAdvancedDenoisingSnapshot
            | Phase::AdvancedDenoising(..)
            | Phase::AdvancedLayout(..)
            | Phase::TriggerError
            | Phase::AwaitErrorModal
            | Phase::ErrorModal
            | Phase::ErrorCopy
            | Phase::AwaitErrorCopy
            | Phase::ErrorDismiss
            | Phase::AwaitErrorDismissed
            | Phase::TrainCard
            | Phase::DatasetBrowse
            | Phase::BenchmarkOverride
            | Phase::AwaitBenchmarkOverride
            | Phase::AwaitBenchmarkChangedSnapshot
            | Phase::BenchmarkRestore
            | Phase::BenchmarkChoice(..)
            | Phase::AwaitBenchmarkRestored
            | Phase::AwaitBenchmarkSnapshot
            | Phase::AwaitBenchmarkChoice(..)
            | Phase::PerceptualControl(..)
            | Phase::AwaitPerceptualControl(..)
            | Phase::DatasetSource
            | Phase::AwaitDatasetSource
            | Phase::CompiledDirectory
            | Phase::AwaitCompiledDirectory
            | Phase::CompileDimensions
            | Phase::AwaitCompileDimensions
            | Phase::CompileResizeMode
            | Phase::AwaitCompileResizeMode
            | Phase::CompileResolution
            | Phase::AwaitCompileResolution
            | Phase::AwaitDatasetSettings(..)
            | Phase::Compile
            | Phase::AwaitCompileProgress
            | Phase::AwaitCompileCompletion
            | Phase::AwaitCompileCancelled
            | Phase::DatasetFixture(..)
            | Phase::DatasetDisclosure(..)
            | Phase::DatasetInput(..)
            | Phase::CompileProgress
            | Phase::CompileActionWithProgress
            | Phase::DatasetStatus => self.lifecycle.advance_lifecycle(
                &mut self.driver,
                &mut self.widgets,
                model,
                settings,
                applied_scale,
                active,
            ),
            Phase::OpenAnnotation
            | Phase::CopyAwaitObject { .. }
            | Phase::CopyUndo { .. }
            | Phase::CopyRedo { .. }
            | Phase::CopyAwaitUndo { .. }
            | Phase::CopyAwaitRedo { .. }
            | Phase::CopyAwaitClass { .. }
            | Phase::CopyListSetup { .. }
            | Phase::CopyLayout(..)
            | Phase::CopySwatchWait
            | Phase::CopyCapability
            | Phase::CopyCapabilityWait
            | Phase::CopyAwaitScale(..)
            | Phase::CopyProductStart
            | Phase::CopyAwaitOutput
            | Phase::CopySave
            | Phase::CopyAwaitSave
            | Phase::AwaitAnnotation
            | Phase::AnnotationSidebar { .. }
            | Phase::AnnotationTimeline { .. }
            | Phase::AnnotationOperation { .. }
            | Phase::AnnotationStop { .. }
            | Phase::AnnotationBrush { .. }
            | Phase::AnnotationTool { .. }
            | Phase::AwaitTool { .. }
            | Phase::AnnotationSurface(..)
            | Phase::AwaitAnnotationFrame(..)
            | Phase::AnnotationPointer(..)
            | Phase::AwaitPointer(..)
            | Phase::CopyProductWait => self.annotation_scenario.advance_annotation_checks(
                &mut self.widgets,
                &mut self.driver,
                &mut self.probes,
                model,
                settings,
                applied_scale,
                active,
                surface,
            ),
            _ => Task::none(),
        }
    }
}

#[cfg(test)]
pub(crate) mod tests;

impl Driver {
    pub(super) fn running(&self) -> bool {
        !matches!(
            self.phase,
            Phase::Disabled | Phase::Complete | Phase::Failed
        )
    }
    #[track_caller]
    pub(super) fn fail(&mut self, detail: &str) {
        self.fail_detail(|| std::borrow::Cow::Borrowed(detail));
    }
    #[track_caller]
    pub(super) fn fail_detail<'a>(&mut self, detail: impl FnOnce() -> std::borrow::Cow<'a, str>) {
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
    }
    pub(super) fn click_located(&mut self, input_bounds: Rectangle) -> Option<train::Message> {
        if !click(input_bounds) {
            self.fail("Firefox click dispatch failed");
        }
        None
    }
    pub(super) fn advance_to(&mut self, phase: Phase) -> Task<RootMessage> {
        reporting::emit(|sink| {
            sink.record(
                "integration.phase_advanced",
                "",
                &format!("{phase:?}"),
                [0.0; 4],
            )
        });
        self.phase = phase;
        // A completed local step has no pending native event to wake its
        // successor. Queue one continuation without requiring another draw.
        let continuation = Task::done(RootMessage::Integration(Message::Scoped {
            generation: self.generation,
            receipt: None,
            message: Box::new(Message::Advance),
        }));
        continuation
    }
    pub(super) fn report_phase_progress(&self) {
        self.reporting
            .observe(|reporting| reporting.phase_progress(&self.phase));
    }
}

impl Driver {
    fn bootstrap(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
    ) -> Task<RootMessage> {
        match self.phase.clone() {
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
                if self.viewer_scenario == "workflows" {
                    return self.advance_to(Phase::Workflows(workflows::Step::Train));
                }
                if !self.viewer_scenario.is_empty() {
                    self.phase = Phase::TrainNavigation;
                    return widgets.arm(self, crate::view::navigation::stable_id(FeatureId::Train));
                }
                self.phase = Phase::SettingsOpen;
                widgets.arm(self, "navigation.settings")
            }
            _ => Task::none(),
        }
    }
}

impl Controller {
    pub fn update(&mut self, message: Message) -> Option<train::Message> {
        if !self.driver.running() {
            return None;
        }
        let result = self.update_probe_message(message);
        self.finish_transition();
        result
    }
}

impl Controller {
    pub(crate) fn receive_control(
        &mut self,
        receipt: crate::generated::IntegrationControlReceipt,
    ) -> Result<(), &'static str> {
        let result = self.receive_control_transition(receipt);
        self.finish_transition();
        result
    }
}

impl Controller {
    pub fn advance(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        applied_scale: f32,
        router: &crate::view::router::Router,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
    ) -> Task<RootMessage> {
        let running = self.driver.running();
        if running {
            reporting::primary_page(active, applied_scale);
        }
        let measurements = if running {
            self.driver
                .reporting
                .primary_measurements(self.driver.generation)
        } else {
            None
        };
        let result =
            self.advance_transition(model, settings, applied_scale, router, active, surface);
        if running {
            self.finish_transition();
        }
        match measurements {
            Some(measurements) => Task::batch([measurements, result]),
            None => result,
        }
    }
    fn finish_transition(&mut self) {
        if matches!(self.driver.phase, Phase::Failed) {
            self.widgets.location_completed();
            self.retained.cancel_input();
        }
    }
}
