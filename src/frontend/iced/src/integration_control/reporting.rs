//! Effect-only integration reporting. Payloads enter here before collection.
use super::*;
use std::cell::RefCell;

pub(super) struct Owner {
    state: RefCell<Option<State>>,
}

impl Owner {
    pub(super) fn new() -> Self {
        Self {
            state: RefCell::new(reporting_enabled().then(State::default)),
        }
    }

    pub(super) fn observe(&self, observe: impl FnOnce(&mut State)) {
        emit(|_| {
            if let Some(state) = self.state.borrow_mut().as_mut() {
                observe(state);
            }
        });
    }

    #[cfg(test)]
    pub(super) fn state_is_absent(&self) -> bool {
        self.state.borrow().is_none()
    }
}

#[derive(Default)]
pub(crate) struct State {
    reported_phase: Option<Phase>,
    explore_snapshot_revision: u64,
    reopen_wait_revision: u64,
    scroll_placeholder_reported: bool,
    resolved_primary: [f64; 4],
    resolved_benchmark: [f64; 4],
    pub(super) reported_style_bits: u8,
}

// No payload can acquire a sink until reporting is active. Stateless draw
// reports share this boundary without acquiring controller/probe ownership.
pub(super) fn emit(payload: impl FnOnce(&Sink)) {
    if reporting_enabled() {
        payload(&SINK);
    }
}

pub(super) struct Sink {
    _private: (),
}
const SINK: Sink = Sink { _private: () };
impl Sink {
    pub(super) fn record(&self, event: &str, control: &str, detail: &str, values: [f64; 4]) {
        #[cfg(target_arch = "wasm32")]
        report_js(
            event, control, detail, values[0], values[1], values[2], values[3],
        );
        #[cfg(test)]
        RECORDS.with(|records| {
            if let Some(records) = records.borrow_mut().as_mut() {
                assert!(records.len() < 4096, "bounded test record capture");
                records.push((event.into(), control.into(), detail.into(), values));
            }
        });
        #[cfg(all(not(target_arch = "wasm32"), not(test)))]
        let _ = (event, control, detail, values);
    }
}

#[cfg(test)]
thread_local! {
    static STYLES: RefCell<Option<Vec<(String, String, [f64; 4], Rectangle)>>> = const { RefCell::new(None) };
    static RECORDS: RefCell<Option<Vec<(String, String, String, [f64; 4])>>> = const { RefCell::new(None) };
}

#[cfg(target_arch = "wasm32")]
#[wasm_bindgen::prelude::wasm_bindgen(module = "/src/integration_control/browser.mjs")]
extern "C" {
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationReport)]
    fn report_js(event: &str, control: &str, detail: &str, a: f64, b: f64, c: f64, d: f64);
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
}

fn report_rendered_control_style(
    control: &str,
    semantic: &str,
    color: [f64; 4],
    bounds: Rectangle,
) {
    #[cfg(target_arch = "wasm32")]
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
    #[cfg(test)]
    STYLES.with(|styles| {
        if let Some(styles) = styles.borrow_mut().as_mut() {
            assert!(styles.len() < 64, "bounded test style capture");
            styles.push((control.into(), semantic.into(), color, bounds));
        }
    });
    #[cfg(all(not(target_arch = "wasm32"), not(test)))]
    let _ = (control, semantic, color, bounds);
}

fn report_advanced_field(control: &str, detail: &str, bounds: Rectangle) {
    SINK.record(
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

impl State {
    pub(super) fn observe_workspace_message(
        &self,
        message: &crate::view::router::Message,
        active: FeatureId,
        phase: &Phase,
    ) {
        if let crate::view::router::Message::Navigation(
            crate::view::navigation::Message::PageSelected(feature),
        ) = message
        {
            SINK.record(
                "integration.navigation_message",
                crate::view::navigation::stable_id(*feature),
                crate::view::navigation::label(active),
                [0.0; 4],
            );
        }
        if matches!(
            phase,
            Phase::AwaitAdvancedNumericDraft(_) | Phase::AwaitAdvancedNumericSnapshot(_)
        ) && matches!(
            message,
            crate::view::router::Message::Train(crate::view::train::Message::Advanced(_))
        ) {
            SINK.record(
                "integration.advanced_message",
                "",
                &format!("{message:?}"),
                [0.0; 4],
            );
        }
        if let crate::view::router::Message::Explore(message) = message {
            SINK.record(
                "integration.explore_message",
                "",
                &format!("{message:?}"),
                [0.0; 4],
            );
        }
    }
    pub fn observe_navigation_outcome(&self, selected: FeatureId, active: FeatureId) {
        SINK.record(
            "integration.navigation_outcome",
            crate::view::navigation::stable_id(selected),
            crate::view::navigation::label(active),
            [0.0; 4],
        );
    }
    pub fn observe_authoritative_route(
        &self,
        source: &'static str,
        authoritative: FeatureId,
        active: FeatureId,
    ) {
        SINK.record(
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
    pub fn observe_native_frame(
        &self,
        frame: crate::presentation_surface::FrameReady,
        surface: Option<crate::presentation_surface::Surface>,
    ) {
        let matching_surface = surface.is_some_and(|candidate| {
            candidate.high == frame.high
                && candidate.low == frame.low
                && frame.content_width <= candidate.width
                && frame.content_height <= candidate.height
        });
        SINK.record(
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
        SINK.record(
            "integration.explore_open_submission",
            EXPLORE_OPEN,
            if submitted { "submitted" } else { "rejected" },
            [0.0; 4],
        );
    }
    pub fn observe_explore_open_layout(
        &self,
        measured: bool,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        columns: u32,
    ) {
        SINK.record(
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
    pub fn observe_explore_filter_request(
        &self,
        request: &crate::generated::ExploreFilterUpdate,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        local_edits: bool,
        mutation_available: bool,
    ) {
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
        SINK.record(
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
        SINK.record(
            "integration.explore_filter_submission",
            "explore.UpdateFilter",
            if submitted { "submitted" } else { "rejected" },
            [0.0; 4],
        );
    }
    pub fn observe_explore_filter_settlement(
        &self,
        source: &'static str,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        mutation_available: bool,
    ) {
        let Some(snapshot) = snapshot else {
            SINK.record(
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
        SINK.record(
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
        SINK.record(
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
    pub(super) fn phase_progress(&mut self, phase: &Phase) {
        if matches!(*phase, Phase::Disabled) {
            return;
        }
        if self.reported_phase.as_ref() == Some(phase) {
            return;
        }
        SINK.record(
            "integration.phase_progress",
            phase.deadline_class(),
            &format!("{phase:?}"),
            [0.0; 4],
        );
        self.reported_phase = Some(phase.clone());
    }
    pub(super) fn explore_snapshot(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
    ) {
        if let Some(snapshot) = model.explore.snapshot.as_ref()
            && snapshot.revision > self.explore_snapshot_revision
        {
            self.explore_snapshot_revision = snapshot.revision;
            let aspect = settings.draft.as_ref().map_or(
                crate::generated::WorkspaceAspectRatio::Widescreen,
                |draft| draft.ui.workspaceaspectratio,
            );
            SINK.record(
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
            SINK.record(
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
                    SINK.record(
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
    }
    pub(super) fn located(&self, phase: &Phase, control: &str, bounds: Rectangle) {
        SINK.record(
            "integration.control_bounds",
            control,
            "",
            [
                f64::from(bounds.x),
                f64::from(bounds.y),
                f64::from(bounds.width),
                f64::from(bounds.height),
            ],
        );
        match *phase {
            Phase::AdvancedField(index) => {
                report_advanced_field(control, &format!("fixed-{index}"), bounds)
            }
            Phase::AdvancedAssignment => report_advanced_field(control, "assignment", bounds),
            Phase::AdvancedMatchFree(index) => {
                report_advanced_field(control, &format!("match-free-{index}"), bounds)
            }
            Phase::AdvancedDenoisingToggle => report_advanced_field(control, "dn-toggle", bounds),
            Phase::AdvancedDenoising(index) => {
                report_advanced_field(control, &format!("dn-{index}"), bounds)
            }
            _ => {}
        }
    }
    pub(super) fn style(&mut self, control: &str, bounds: Rectangle) {
        let resolved_style = if control == BENCHMARK_OVERRIDE {
            Some(("benchmark-purple", self.resolved_benchmark))
        } else if matches!(
            control,
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
            let bit = match control {
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
                report_rendered_control_style(control, semantic, color, bounds);
            }
        }
    }
    pub(super) fn bootstrap(
        &mut self,
        model: &ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
    ) {
        let dark_mode = settings
            .draft
            .as_ref()
            .is_some_and(|draft| draft.ui.darkmode);
        let fluent = crate::fluent_theme::conformance(&crate::fluent_theme::app_theme(dark_mode));
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
        SINK.record(
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
        let status =
            crate::view::workflow::model_card::status_presentation(model.model_snapshot.as_ref());
        SINK.record(
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
    }
    pub(super) fn reset_scroll(&mut self) {
        self.scroll_placeholder_reported = false;
    }
    pub(super) fn scroll_placeholder(&mut self, snapshot: &crate::generated::ExploreSnapshot) {
        if !self.scroll_placeholder_reported {
            self.scroll_placeholder_reported = true;
            SINK.record(
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
    }
    pub(super) fn reopen_wait(
        &mut self,
        model: &ApplicationModel,
        frame: Option<crate::presentation_surface::FrameReady>,
        snapshot: &crate::generated::ExploreSnapshot,
        revision: u64,
        frame_revision: u64,
        selection_grid_present: bool,
        gallery_drawn: Option<(u64, u64)>,
    ) {
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
            } else if !selection_grid_present {
                "selection-grid"
            } else if snapshot.viewport.columns == 0 || snapshot.viewport.rowcount == 0 {
                "viewport"
            } else if sampleable.is_none() {
                "sampleable-presentation"
            } else if gallery_drawn.is_none_or(|(presentation, source)| {
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
            SINK.record(
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
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::generated::{ExploreMode, ExploreOrder, IntegrationControlKind};
    use std::cell::Cell;

    // Captures exist only within these fixtures; ordinary enabled execution
    // retains neither records nor instrumentation counters in Rust.
    struct Capture;
    impl Capture {
        fn new(enabled: bool) -> Self {
            initialize_reporting(enabled, false);
            RECORDS.with(|records| *records.borrow_mut() = Some(Vec::new()));
            STYLES.with(|styles| *styles.borrow_mut() = Some(Vec::new()));
            Self
        }

        fn records(&self) -> Vec<(String, String, String, [f64; 4])> {
            RECORDS.with(|records| std::mem::take(records.borrow_mut().as_mut().unwrap()))
        }
    }
    impl Drop for Capture {
        fn drop(&mut self) {
            RECORDS.with(|records| *records.borrow_mut() = None);
            STYLES.with(|styles| *styles.borrow_mut() = None);
            initialize_reporting(false, false);
        }
    }

    #[test]
    fn controller_reporting_is_lazy_and_preserves_snapshot_phase_and_style_records() {
        for enabled in [false, true] {
            let capture = Capture::new(enabled);
            let mut driver = Controller::new(
                true,
                false,
                "source".into(),
                "compiled".into(),
                "512".into(),
                String::new(),
            );
            let (mut model, frame) = crate::view_model::test_support::explore_presentation();
            model.window_width = 1280;
            model.window_height = 720;
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.mode = ExploreMode::Gallery;
            snapshot.revision = 11;
            snapshot.augmentation.enabled = true;
            snapshot.augmentation.seed = 73;
            snapshot.detail.showoriginaldimensions = false;
            snapshot.focusedimage = Some(3);
            snapshot.order.visibleindices =
                (0..crate::generated::EXPLORE_VISIBLE_ITEM_CAPACITY).collect();
            snapshot.gallery.slots = vec![true; snapshot.order.visibleindices.len()];
            let settings = crate::view::settings::SettingsModel::default();
            let router = crate::view::router::Router::default();
            drop(driver.advance(
                &model,
                &settings,
                1.5,
                &router,
                FeatureId::Explore,
                Some(frame),
            ));
            assert_eq!(driver.phase, Phase::SettingsOpen);
            assert_eq!(driver.input_scale, 1.5);
            assert!(driver.location_pending);
            driver.report_phase_progress();
            driver.report_phase_progress();
            let records = capture.records();
            if enabled {
                assert_eq!(
                    records[0],
                    (
                        "integration.phase_progress".into(),
                        "startup".into(),
                        "AwaitBootstrap".into(),
                        [0.0; 4]
                    )
                );
                assert_eq!(
                    records[1],
                    (
                        "integration.explore_state".into(),
                        EXPLORE_GALLERY.into(),
                        "augmentation-enabled".into(),
                        [
                            11.0,
                            73.0,
                            0.0,
                            f64::from(crate::view::aspect_ratio::height_factor(
                                crate::generated::WorkspaceAspectRatio::Widescreen
                            ))
                        ]
                    )
                );
                assert_eq!(
                    records[2],
                    (
                        "integration.explore_frame".into(),
                        explore::DETAIL_WORKSPACE_ID.into(),
                        "padded".into(),
                        [1.0, 640.0, 480.0, 0.0]
                    )
                );
                let count = crate::generated::EXPLORE_VISIBLE_ITEM_CAPACITY as usize;
                for slot in 0..count {
                    assert_eq!(
                        records[3 + slot],
                        (
                            "integration.explore_slot".into(),
                            EXPLORE_GALLERY.into(),
                            "current-focused".into(),
                            [11.0, 1.0, slot as f64, slot as f64]
                        )
                    );
                }
                assert_eq!(
                    records[count + 3],
                    (
                        "integration.bootstrap".into(),
                        "".into(),
                        "typed-bootstrap".into(),
                        [1280.0, 720.0, 0.0, 0.0]
                    )
                );
                let fluent =
                    crate::fluent_theme::conformance(&crate::fluent_theme::app_theme(false));
                assert_eq!(
                    records[count + 4],
                    (
                        "integration.fluent_shell".into(),
                        "navigation".into(),
                        "light".into(),
                        [
                            f64::from(fluent.navigation_border),
                            f64::from(fluent.card_border),
                            f64::from(fluent.primary_border),
                            f64::from(u8::from(fluent.resolved))
                        ]
                    )
                );
                let status = crate::view::workflow::model_card::status_presentation(
                    model.model_snapshot.as_ref(),
                );
                assert_eq!(
                    records[count + 5],
                    (
                        "integration.model_copy".into(),
                        TRAIN_MODEL_CARD.into(),
                        crate::view::workflow::model_card::card_title(FeatureId::Train).into(),
                        [f64::from(u8::from(!status.label.is_empty())), 0.0, 0.0, 0.0]
                    )
                );
                assert_eq!(
                    records[count + 6],
                    (
                        "integration.phase_progress".into(),
                        driver.phase.deadline_class().into(),
                        "SettingsOpen".into(),
                        [0.0; 4]
                    )
                );
                assert_eq!(records.len(), count + 7);
            } else {
                assert!(records.is_empty());
                assert!(driver.reporting.state_is_absent());
            }

            // The same and older revisions do not report or rescan. A newer
            // detail snapshot retains the two frame records and emits no slots.
            drop(driver.advance(
                &model,
                &settings,
                1.5,
                &router,
                FeatureId::Explore,
                Some(frame),
            ));
            assert!(capture.records().is_empty());
            model.explore.snapshot.as_mut().unwrap().revision = 10;
            drop(driver.advance(
                &model,
                &settings,
                1.5,
                &router,
                FeatureId::Explore,
                Some(frame),
            ));
            assert!(capture.records().is_empty());
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.revision = 12;
            snapshot.mode = ExploreMode::Detail;
            drop(driver.advance(
                &model,
                &settings,
                1.5,
                &router,
                FeatureId::Explore,
                Some(frame),
            ));
            let records = capture.records();
            assert_eq!(records.len(), if enabled { 2 } else { 0 });
            if enabled {
                assert_eq!(records[0].0, "integration.explore_state");
                assert_eq!(records[0].1, explore::DETAIL_WORKSPACE_ID);
                assert_eq!(records[1].0, "integration.explore_frame");
            }

            let bounds = Rectangle::new(iced::Point::new(12.0, 24.0), iced::Size::new(96.0, 32.0));
            for _ in 0..2 {
                driver.phase = Phase::ExploreOpen;
                driver.location_pending = true;
                driver.update(Message::Located {
                    control: EXPLORE_OPEN.into(),
                    bounds,
                });
                // The native fixture has no Firefox click adapter. Its existing
                // failure behavior still follows the real located-style route.
                assert_eq!(driver.phase, Phase::Failed);
                assert!(!driver.location_pending);
                assert!(COMPLETION_WITHOUT_INPUT.with(Cell::get));
                let records = capture.records();
                if enabled {
                    assert_eq!(
                        records,
                        vec![
                            (
                                "integration.control_bounds".into(),
                                EXPLORE_OPEN.into(),
                                "".into(),
                                [12.0, 24.0, 96.0, 32.0]
                            ),
                            (
                                "integration.failed".into(),
                                "".into(),
                                "Firefox click dispatch failed".into(),
                                [0.0; 4]
                            ),
                        ]
                    );
                } else {
                    assert!(records.is_empty());
                }
            }
            STYLES.with(|styles| {
                let styles = styles.borrow();
                let styles = styles.as_ref().unwrap();
                assert_eq!(styles.len(), usize::from(enabled));
                if enabled {
                    let fluent =
                        crate::fluent_theme::conformance(&crate::fluent_theme::app_theme(false));
                    let color = fluent.primary_color;
                    assert_eq!(
                        styles[0],
                        (
                            EXPLORE_OPEN.into(),
                            "shared-primary".into(),
                            [color.r, color.g, color.b, color.a].map(f64::from),
                            bounds
                        )
                    );
                }
            });
            driver.phase = Phase::Complete;
            driver
                .reset_scenario(
                    "source".into(),
                    "compiled".into(),
                    "512".into(),
                    "quiet".into(),
                )
                .unwrap();
            assert_eq!(driver.phase, Phase::AwaitBootstrap);
            assert_eq!(driver.reporting.state_is_absent(), !enabled);
            driver.reporting.observe(|state| {
                assert_eq!(state.reported_phase, None);
                assert_eq!(state.explore_snapshot_revision, 0);
                assert_eq!(state.reopen_wait_revision, 0);
                assert_eq!(state.reported_style_bits, 0);
                assert!(!state.scroll_placeholder_reported);
                assert_eq!(state.resolved_primary, [0.0; 4]);
                assert_eq!(state.resolved_benchmark, [0.0; 4]);
            });
            drop(driver.advance(
                &model,
                &settings,
                1.0,
                &router,
                FeatureId::Explore,
                Some(frame),
            ));
            assert_eq!(driver.phase, Phase::TrainNavigation);
            if !enabled {
                assert!(capture.records().is_empty());
            }
            for dark in [false, true] {
                driver.phase = Phase::Complete;
                driver
                    .reset_scenario(
                        "source".into(),
                        "compiled".into(),
                        "512".into(),
                        "terminal".into(),
                    )
                    .unwrap();
                let mut settings = crate::view::settings::SettingsModel::default();
                settings.install(model.settings_snapshot.as_ref().unwrap());
                settings.draft.as_mut().unwrap().ui.darkmode = dark;
                drop(driver.advance(
                    &model,
                    &settings,
                    1.0,
                    &router,
                    FeatureId::Explore,
                    Some(frame),
                ));
                assert_eq!(driver.phase, Phase::TrainNavigation);
                STYLES.with(|styles| styles.borrow_mut().as_mut().unwrap().clear());
                driver.reporting.observe(|state| {
                    state.style(EXPLORE_OPEN, bounds);
                    state.style(EXPLORE_OPEN, bounds);
                    state.style(BENCHMARK_OVERRIDE, bounds);
                    state.style(BENCHMARK_OVERRIDE, bounds);
                });
                STYLES.with(|styles| {
                    let styles = styles.borrow();
                    let styles = styles.as_ref().unwrap();
                    assert_eq!(styles.len(), if enabled { 2 } else { 0 });
                    if enabled {
                        let fluent =
                            crate::fluent_theme::conformance(&crate::fluent_theme::app_theme(dark));
                        for (record, color) in styles
                            .iter()
                            .zip([fluent.primary_color, fluent.benchmark_color])
                        {
                            assert_eq!(
                                record.2,
                                [color.r, color.g, color.b, color.a].map(f64::from)
                            );
                            assert_eq!(record.2[3], 1.0);
                        }
                    }
                });
                if !enabled {
                    assert!(capture.records().is_empty());
                    assert!(driver.reporting.state_is_absent());
                }
            }
        }
    }

    #[test]
    fn passive_queries_failure_detail_and_reporting_dedupe_share_the_lazy_boundary() {
        for enabled in [false, true] {
            let capture = Capture::new(enabled);
            let mut driver = Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                String::new(),
                "quiet".into(),
            );
            let (model, frame) = crate::view_model::test_support::explore_presentation();
            let surface = crate::view_model::test_support::physical_surface(frame);
            let snapshot = model.explore.snapshot.as_ref().unwrap();
            let query_count = Cell::new(0);
            let message = crate::view::router::Message::Navigation(
                crate::view::navigation::Message::PageSelected(FeatureId::Explore),
            );
            driver.observe_workspace_message(|| {
                query_count.set(query_count.get() + 1);
                (&message, FeatureId::Train)
            });
            driver.observe_reporting(|state| {
                query_count.set(query_count.get() + 1);
                state.observe_navigation_outcome(FeatureId::Explore, FeatureId::Train);
                state.observe_authoritative_route(
                    "settings.event",
                    FeatureId::Explore,
                    FeatureId::Explore,
                );
                for candidate in [
                    Some(surface),
                    None,
                    Some(crate::presentation_surface::Surface { high: 9, ..surface }),
                ] {
                    state.observe_native_frame(frame, candidate);
                }
                state.observe_explore_open_request(false, true);
                state.observe_explore_open_layout(false, Some(snapshot), 4);
                state.observe_explore_open_submission(true);
                let request = crate::generated::ExploreFilterUpdate {
                    filter: snapshot.filter.clone(),
                    overlay: snapshot.overlay.clone(),
                };
                state.observe_explore_filter_request(&request, Some(snapshot), true, false);
                state.observe_explore_filter_submission(false);
                state.observe_explore_filter_settlement("intent-reply", None, false);
                state.observe_explore_filter_settlement("system-event", Some(snapshot), true);
                state.scroll_placeholder(snapshot);
                state.scroll_placeholder(snapshot);
                state.reset_scroll();
                state.scroll_placeholder(snapshot);
                state.reopen_wait(&model, Some(frame), snapshot, 0, 0, false, None);
                state.reopen_wait(&model, Some(frame), snapshot, 0, 0, false, None);
                state.located(&Phase::AdvancedField(2), "field", Rectangle::default());
            });
            driver.fail_detail(|| {
                query_count.set(query_count.get() + 1);
                format!("failure {}", 7).into()
            });
            assert_eq!(driver.phase, Phase::Failed);
            driver.report_phase_progress();
            driver.report_phase_progress();
            driver.observe_reporting(|_| panic!("failed controller ran passive reporting"));
            assert_eq!(query_count.get(), if enabled { 3 } else { 0 });
            let records = capture.records();
            if !enabled {
                assert!(records.is_empty());
                assert!(driver.reporting.state_is_absent());
                STYLES.with(|styles| assert!(styles.borrow().as_ref().unwrap().is_empty()));
                continue;
            }
            assert_eq!(
                records
                    .iter()
                    .map(|record| record.0.as_str())
                    .collect::<Vec<_>>(),
                [
                    "integration.navigation_message",
                    "integration.navigation_outcome",
                    "integration.route_state",
                    "integration.native_frame",
                    "integration.native_frame",
                    "integration.native_frame",
                    "integration.explore_open_request",
                    "integration.explore_open_layout",
                    "integration.explore_open_submission",
                    "integration.explore_filter_request",
                    "integration.explore_filter_submission",
                    "integration.explore_filter_settlement",
                    "integration.explore_filter_settlement",
                    "integration.explore_scroll_placeholder",
                    "integration.explore_scroll_placeholder",
                    "integration.explore_reopen_wait",
                    "integration.control_bounds",
                    "integration.advanced_field",
                    "integration.failed",
                    "integration.phase_progress",
                ]
            );
            assert_eq!(
                records[0].1,
                crate::view::navigation::stable_id(FeatureId::Explore)
            );
            assert_eq!(
                records[0].2,
                crate::view::navigation::label(FeatureId::Train)
            );
            assert_eq!(records[2].3, [1.0, 0.0, 0.0, 0.0]);
            for (index, detail, generation, matching) in [
                (3, "accepted", 1.0, 1.0),
                (4, "surface-missing", 0.0, 0.0),
                (5, "rejected", 1.0, 0.0),
            ] {
                assert_eq!(records[index].2, detail);
                assert_eq!(
                    records[index].3,
                    [
                        frame.content_sequence as f64,
                        frame.presentation_revision as f64,
                        generation,
                        matching
                    ]
                );
            }
            assert_eq!(records[6].3, [0.0, 1.0, 0.0, 0.0]);
            assert_eq!(records[7].2, "waiting-for-measurement");
            assert_eq!(
                records[7].3,
                [
                    snapshot.dataset.identity as f64,
                    snapshot.revision as f64,
                    4.0,
                    snapshot.order.matchingcount as f64
                ]
            );
            assert_eq!(records[8].2, "submitted");
            assert_eq!(records[9].2, "rejected-local-edits");
            assert_eq!(records[10].2, "rejected");
            assert_eq!(records[11].2, "snapshot-unavailable");
            assert_eq!(
                records[12].2,
                if snapshot.filter.order == ExploreOrder::Shuffled {
                    "shuffled"
                } else {
                    "sequential"
                }
            );
            assert_eq!(records[17].2, "fixed-2");
            assert_eq!(records[18].2, "failure 7");
            assert_eq!(records[19].2, "Failed");
        }
    }

    #[test]
    fn quiet_reporting_preserves_pressure_and_typed_settlement_receipts() {
        let capture = Capture::new(false);
        let mut driver = Controller::new(
            true,
            false,
            String::new(),
            String::new(),
            String::new(),
            "quiet".into(),
        );
        let (mut connection, _transport_capture) =
            crate::transport_connection::Connection::test_channel();
        connection.observe_integration_pressure(1);
        let expected = |kind, progress, failureline| {
            crate::generated::IntegrationControl {
                protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                receipt: crate::generated::IntegrationControlReceipt {
                    kind,
                    sequence: 1,
                    progress,
                    failureline,
                    readgeneration: 0,
                    compiledindex: 0,
                },
            }
            .encode()
            .unwrap()
        };
        let mut wire = Vec::new();
        driver.publish_control(&mut connection);
        connection
            .flush(|bytes| {
                wire.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(wire, vec![expected(IntegrationControlKind::Progress, 5, 0)]);
        wire.clear();
        let sample_count = crate::generated::ANNOTATION_INPUT_BATCH_CAPACITY
            * crate::generated::ANNOTATION_INPUT_ADMISSION_SLOTS
            + 1;
        for index in 0..sample_count {
            connection
                .send_annotation_pointer(
                    crate::generated::AnnotationPointer {
                        phase: if index == 0 {
                            crate::generated::AnnotationPointerPhase::Begin
                        } else if index + 1 == sample_count {
                            crate::generated::AnnotationPointerPhase::End
                        } else {
                            crate::generated::AnnotationPointerPhase::Update
                        },
                        interactionid: 1,
                        sequence: index as u64 + 1,
                        identity: crate::generated::AnnotationTargetIdentity { object: 0, element: 0 },
                        target: crate::generated::AnnotationPointerTarget {
                            object: None,
                            element: None,
                            role: None,
                        },
                        point: crate::generated::AnnotationPoint {
                            x: index as f32,
                            y: 1.0,
                        },
                        brushradius: crate::generated::default_uiannotationbrushradius().unwrap()
                            as u16,
                    },
                    1,
                )
                .unwrap();
        }
        connection
            .flush(|bytes| {
                wire.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(
            wire.len(),
            crate::generated::ANNOTATION_INPUT_ADMISSION_SLOTS + 1
        );
        assert_eq!(
            wire.last().unwrap(),
            &expected(IntegrationControlKind::PressureEntered, 0, 0)
        );
        driver.phase = Phase::Complete;
        driver.publish_control(&mut connection);
        assert_eq!(driver.control_phase, Some(Phase::AwaitBootstrap));
        assert!(!connection.integration_pressure_settled());
        for consumed in [
            crate::generated::ANNOTATION_INPUT_ADMISSION_SLOTS,
            crate::generated::ANNOTATION_INPUT_ADMISSION_SLOTS + 1,
        ] {
            connection
                .observe(&crate::protocol::ServerRecord::InputProgress(
                    crate::generated::InputProgress {
                        protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                        progress: crate::generated::AnnotationInputProgress {
                            epoch: 1,
                            consumedsequence: consumed as u64,
                            rejection: None,
                        },
                        error: None,
                    },
                ))
                .unwrap();
            connection.flush(|_| Ok(())).unwrap();
        }
        assert!(connection.integration_pressure_settled());
        wire.clear();
        driver.publish_control(&mut connection);
        connection
            .flush(|bytes| {
                wire.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(wire, vec![expected(IntegrationControlKind::Settled, 10, 0)]);
        let failure_line = line!() + 1;
        driver.fail_detail(|| panic!("quiet failure formatting ran"));
        assert_eq!(driver.failure_line, failure_line);
        driver.publish_control(&mut connection);
        wire.clear();
        connection
            .flush(|bytes| {
                wire.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(
            wire,
            vec![expected(IntegrationControlKind::Failed, 15, failure_line)]
        );
        let forwarded_line = line!() + 1;
        driver.fail("static quiet failure");
        assert_eq!(driver.failure_line, forwarded_line);
        assert!(
            driver
                .reset_scenario(String::new(), String::new(), String::new(), "quiet".into())
                .is_err()
        );
        driver.phase = Phase::Complete;
        driver
            .reset_scenario(String::new(), String::new(), String::new(), "quiet".into())
            .unwrap();
        assert_eq!(driver.control_progress, 0);
        assert_eq!(driver.failure_line, 0);
        assert_eq!(driver.control_phase, None);
        assert!(driver.reporting.state_is_absent());
        assert!(capture.records().is_empty());
    }
}
