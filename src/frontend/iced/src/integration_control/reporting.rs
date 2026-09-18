//! Effect-only integration reporting. Payloads enter here before collection.
use super::lifecycle::{
    BENCHMARK_OVERRIDE, COMPILE_DATASET, DATASET_BROWSE, TRAIN_MODEL_CARD, advanced_layout_field,
};
use super::retained::EXPLORE_OPEN;
use crate::generated::FeatureId;
#[cfg(test)]
use crate::integration_control::initialize_reporting;
use crate::integration_control::pixel_checks::sampleable_presentation;
use crate::integration_control::probe::current_receipt;
use crate::integration_control::{
    Driver, EXPLORE_GALLERY, EXPLORE_UPSCALE_ACTIONS, Phase, probe, reporting_enabled, retained,
};
use crate::view::explore;
use crate::view_model::ApplicationModel;
use iced::Rectangle;
use std::cell::RefCell;

/// Only the opt-in acceptance observer sees presentation facts. It cannot
/// change execution, widget identity, redraw scheduling, or GPU resource custody.
pub(crate) fn primary_action_draw(
    control: &str,
    label: &str,
    active: bool,
    dark: bool,
    phase: f32,
    bounds: Rectangle,
    viewport: Rectangle,
    inset: f32,
    scale: f32,
    blue: iced::Color,
) {
    if !reporting_enabled() {
        return;
    }
    #[cfg(target_arch = "wasm32")]
    {
        let token = primary_action_js(
            control,
            label,
            active,
            dark,
            &[
                f64::from(bounds.x),
                f64::from(bounds.y),
                f64::from(bounds.width),
                f64::from(bounds.height),
                f64::from(viewport.x),
                f64::from(viewport.y),
                f64::from(viewport.width),
                f64::from(viewport.height),
                f64::from(scale),
                f64::from(phase),
                f64::from(blue.r),
                f64::from(blue.g),
                f64::from(blue.b),
                f64::from(inset),
            ],
        );
        if token != 0
            && let Some(mut output) = probe::scenario_output()
        {
            output.receipt = None;
            output.send(super::Message::PrimaryActionMeasure {
                control: control.to_owned(),
                token,
            });
        }
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (
        control, label, active, dark, phase, bounds, viewport, inset, scale, blue,
    );
}

pub(super) fn primary_action_measured(control: String, token: u32, bounds: [Rectangle; 3]) {
    if !reporting_enabled() {
        return;
    }
    #[cfg(target_arch = "wasm32")]
    if let Some(mut output) = probe::scenario_output() {
        output.receipt = None;
        let completed_control = control.clone();
        let completed =
            wasm_bindgen::closure::Closure::once_into_js(move |outcome: String, active: bool| {
                match outcome.as_str() {
                    "measure" => output.send(super::Message::PrimaryActionMeasure {
                        control: completed_control,
                        token,
                    }),
                    "observed" => output.send(super::Message::PrimaryActionPixels {
                        control: completed_control,
                        active,
                        token,
                    }),
                    _ => {}
                }
            });
        let facts = bounds.map(|r| {
            [
                f64::from(r.x),
                f64::from(r.y),
                f64::from(r.width),
                f64::from(r.height),
            ]
        });
        primary_action_measured_js(&control, token, facts.as_flattened(), &completed);
    }
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (control, token, bounds);
}

pub(super) fn primary_page(page: FeatureId, scale: f32) {
    if !reporting_enabled() {
        return;
    }
    #[cfg(target_arch = "wasm32")]
    primary_page_js(crate::view::navigation::stable_id(page), f64::from(scale));
    #[cfg(not(target_arch = "wasm32"))]
    let _ = (page, scale);
}

pub(super) fn primary_action_current(control: &str, token: u32) -> bool {
    if !reporting_enabled() {
        return false;
    }
    #[cfg(target_arch = "wasm32")]
    {
        primary_action_current_js(control, token)
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        let _ = (control, token);
        true
    }
}

pub(super) fn chart_view(stage: &str, view: &crate::view::metrics::ChartView) {
    emit(|sink| {
        sink.record(
            "integration.chart_view",
            "train.metrics.chart.Loss",
            stage,
            [
                view.ranges[0][0],
                view.ranges[0][1],
                view.ranges[1][0],
                view.ranges[1][1],
            ],
        )
    });
}

pub(crate) fn metric_projection(label: &str, positions: &[[f64; 2]]) {
    emit(|sink| {
        let finite = |point: &[f64; 2]| point.iter().all(|value| value.is_finite());
        let points = positions.iter().filter(|point| finite(point)).count();
        let segments = positions
            .windows(2)
            .filter(|pair| finite(&pair[0]) && finite(&pair[1]) && pair[0] != pair[1])
            .count();
        if let (Some(first), Some(last)) = (
            positions.iter().find(|p| finite(p)),
            positions.iter().rfind(|p| finite(p)),
        ) {
            sink.record(
                "integration.metric_values",
                "train.metrics.plot",
                label,
                [first[0], first[1], last[0], last[1]],
            );
        }
        sink.record(
            "integration.metric_projection",
            "train.metrics.plot",
            label,
            [points as f64, segments as f64, positions.len() as f64, 0.0],
        );
    });
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FpsEvidence {
    pub bounds: Rectangle,
    pub clip: Rectangle,
    pub dark: bool,
    pub frames: u64,
    pub seconds: f64,
}

pub(super) fn workspace_fps(
    control: &'static str,
    meter: &crate::workspace_fps::Meter,
    evidence: FpsEvidence,
) {
    emit(|sink| {
        sink.record(
            "integration.workspace_fps",
            control,
            meter.text(),
            [
                evidence.frames as f64,
                evidence.seconds,
                f64::from(
                    evidence.clip.x + evidence.clip.width
                        - evidence.bounds.x
                        - evidence.bounds.width,
                ),
                f64::from(evidence.bounds.y - evidence.clip.y),
            ],
        );
    });
}

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

    pub(super) fn measure_primary(&self, control: String, token: u32) {
        self.observe(|state| {
            if let Some(slot) = state
                .primary_measurements
                .iter_mut()
                .find(|slot| slot.as_ref().is_some_and(|(id, _)| id == &control))
            {
                *slot = Some((control, token));
            } else if let Some(slot) = state
                .primary_measurements
                .iter_mut()
                .find(|slot| slot.is_none())
            {
                *slot = Some((control, token));
            }
        });
    }

    pub(super) fn primary_measurements(
        &self,
        generation: u64,
    ) -> Option<iced::Task<crate::message::Message>> {
        if !reporting_enabled() {
            return None;
        }
        let mut state = self.state.borrow_mut();
        let state = state.as_mut()?;
        if state.primary_measurements.iter().all(Option::is_none) {
            return None;
        }
        Some(iced::Task::batch(
            state
                .primary_measurements
                .iter_mut()
                .filter_map(Option::take)
                .map(|(control, token)| {
                    super::widget_ops::measure_control(control.clone()).map(move |bounds| {
                        crate::message::Message::Integration(super::Message::Scoped {
                            generation,
                            receipt: None,
                            message: Box::new(super::Message::PrimaryActionMeasured {
                                control: control.clone(),
                                token,
                                bounds: [bounds.target, bounds.page, bounds.horizontal],
                            }),
                        })
                    })
                }),
        ))
    }

    #[cfg(test)]
    pub(super) fn state_is_absent(&self) -> bool {
        self.state.borrow().is_none()
    }
}

#[derive(Default)]
pub(crate) struct State {
    reported_phase: Option<Phase>,
    primary_measurements: [Option<(String, u32)>; 6],
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

pub(super) fn upscale_settlement(
    driver: &Driver,
    retained: &retained::State,
    probes: &probe::Requests,
    model: &ApplicationModel,
    frame: Option<crate::presentation_surface::FrameReady>,
    blocker: &'static str,
) {
    emit(|sink| {
        let Phase::AwaitUpscale { kernel, .. } = driver.phase else {
            return;
        };
        let native = model.upscale_snapshot.as_ref();
        let observed = retained.upscale_observation();
        // Read only real frontiers here. The controller reports the branch that
        // blocked; reporting neither repeats its predicates nor settles work.
        sink.record(
            "integration.upscale_settlement",
            EXPLORE_UPSCALE_ACTIONS[kernel],
            &format!(
                "blocked={blocker}; phase={:?}; foreground={:?}; viewed_frame={:?}; native(revision,busy,ready,kernel,frame)={:?}; presentation(completed,publication,ready,browser_sample)={:?}; received={frame:?}; displayed_kernel={:?}; drawn={:?}; probe_current={:?}; probe_pending={:?}; probe_pixels={:?}; button={:?}; repeat(revision,observed)={:?}; requested={:?}; native_input={:?}; native_pending={:?}; native_method={:?}; explore(mode,selected,requested_selection,frame,document)={:?}",
                driver.phase,
                model.foreground_visual(),
                model.viewed_explore_frame(),
                native.map(|state| (
                    state.revision,
                    state.busy,
                    state.ready,
                    state.kernel,
                    &state.frame,
                )),
                frame.and_then(|frame| crate::presentation_surface::metadata::product(frame)
                    .map(|product| (product, frame.presentation_revision))),
                model.displayed_upscale_kernel(),
                probes.draws().viewer,
                current_receipt(explore::DETAIL_WORKSPACE_ID),
                (*probes.upscale_pending()),
                observed.pixels,
                observed.button,
                observed.repeat,
                model.requested_upscale,
                native.map(|state| &state.input),
                native.and_then(|state| state.pending.as_ref()),
                native.and_then(|state| state.methods.get(kernel)),
                model.explore.snapshot.as_ref().map(|state| (
                    state.mode,
                    state.selectedimage,
                    model.explore.requested_selection,
                    &state.frame,
                    &state.document,
                )),
            ),
            [
                native.map_or(0.0, |state| state.revision as f64),
                native.map_or(0.0, |state| state.frame.revision as f64),
                crate::presentation_surface::retained_surface().and_then(|surface| surface.frame)
                    .map_or(0.0, |frame| frame.presentation_revision as f64),
                frame.map_or(0.0, |value| value.presentation_revision as f64),
            ],
        );
    });
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
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationPrimaryAction)]
    fn primary_action_js(
        control: &str,
        label: &str,
        active: bool,
        dark: bool,
        facts: &[f64],
    ) -> u32;
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationPrimaryActionMeasured)]
    fn primary_action_measured_js(
        control: &str,
        token: u32,
        bounds: &[f64],
        completed: &wasm_bindgen::JsValue,
    );

    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationPrimaryPage)]
    fn primary_page_js(control: &str, scale: f64);
    #[wasm_bindgen::prelude::wasm_bindgen(js_name = mmltkIntegrationPrimaryActionCurrent)]
    fn primary_action_current_js(control: &str, token: u32) -> bool;
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
                0.0,
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
                        "current",
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
            Phase::AdvancedLayout(index) => {
                report_advanced_field(control, &advanced_layout_field(index).1, bounds)
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
        frame: Option<crate::presentation_surface::FrameReady>,
        snapshot: &crate::generated::ExploreSnapshot,
        revision: u64,
        frame_revision: u64,
        selection_grid_present: bool,
        gallery_drawn: Option<(u64, u64)>,
    ) {
        if snapshot.revision > self.reopen_wait_revision {
            let sampleable = sampleable_presentation(
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
                    || frame.is_none_or(|frame| frame.presentation_revision != presentation)
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
    use super::{Capture, STYLES};
    use crate::generated::FeatureId;
    use crate::generated::{ExploreMode, ExploreOrder, IntegrationControlKind};
    use crate::integration_control::lifecycle::{BENCHMARK_OVERRIDE, TRAIN_MODEL_CARD};
    use crate::integration_control::retained::EXPLORE_OPEN;
    use crate::integration_control::{
        COMPLETION_WITHOUT_INPUT, Controller, EXPLORE_GALLERY, Message, Phase,
    };
    use crate::view::explore;
    use iced::Rectangle;
    use std::cell::Cell;

    #[test]
    fn snapshot_conflict_reporting_projects_fields_and_retains_installed_state() {
        let capture = Capture::new(true);
        let mut model = crate::view_model::test_support::bootstrapped();
        let installed = model.explore.snapshot.as_mut().unwrap();
        installed.revision = 11;
        let installed = installed.clone();
        let mut incoming = installed.clone();
        incoming.renderpending = !incoming.renderpending;
        for multiple_fields in [false, true] {
            if multiple_fields {
                incoming.busy = !incoming.busy;
                incoming.selectedimage = Some(7);
                incoming.order.visibleindices.push(7);
                incoming.labels.push(crate::generated::ExploreLabel {
                    box_: crate::view_model::test_support::annotation_object(2).box_,
                    category: 2,
                    compiledindex: 7,
                });
            }
            model.reduce_event(crate::generated::ApplicationEvent::ExploreExploreChanged(
                crate::generated::ExploreChanged {
                    snapshot: incoming.clone(),
                },
            ));
            assert_eq!(model.explore.snapshot.as_ref(), Some(&installed));
            assert_eq!(
                model.error.as_ref().unwrap().detail,
                "inconsistent Explore snapshot observation revision"
            );
            assert_eq!(
                capture.records(),
                vec![(
                    "integration.snapshot_conflict".into(),
                    "Explore".into(),
                    if multiple_fields {
                        "busy,renderpending,order,selectedimage,labels"
                    } else {
                        "renderpending"
                    }
                    .into(),
                    [11.0, 0.0, 0.0, 0.0],
                )]
            );
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
            let surface = crate::view_model::test_support::physical_surface(frame);
            model.window_width = 1280;
            model.window_height = 720;
            let snapshot = model.explore.snapshot.as_mut().unwrap();
            snapshot.mode = ExploreMode::Gallery;
            snapshot.revision = 11;
            snapshot.augmentation.enabled = true;
            snapshot.augmentation.seed = 73;
            snapshot.detail.showoriginaldimensions = false;
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
                Some(surface),
            ));
            assert_eq!(driver.driver.phase, Phase::SettingsOpen);
            assert_eq!(driver.driver.input_scale, 1.5);
            assert!(driver.widgets.location_pending());
            driver.driver.report_phase_progress();
            driver.driver.report_phase_progress();
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
                            "current".into(),
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
                        driver.driver.phase.deadline_class().into(),
                        "SettingsOpen".into(),
                        [0.0; 4]
                    )
                );
                assert_eq!(records.len(), count + 7);
            } else {
                assert!(records.is_empty());
                assert!(driver.driver.reporting.state_is_absent());
            }

            // The same and older revisions do not report or rescan. A newer
            // detail snapshot retains the two frame records and emits no slots.
            drop(driver.advance(
                &model,
                &settings,
                1.5,
                &router,
                FeatureId::Explore,
                Some(surface),
            ));
            assert!(capture.records().is_empty());
            model.explore.snapshot.as_mut().unwrap().revision = 10;
            drop(driver.advance(
                &model,
                &settings,
                1.5,
                &router,
                FeatureId::Explore,
                Some(surface),
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
                Some(surface),
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
                driver.driver.phase = Phase::ExploreOpen;
                driver.widgets.begin_location();
                driver.update(Message::Located {
                    control: EXPLORE_OPEN.into(),
                    bounds,
                });
                // The native fixture has no Firefox click adapter. Its existing
                // failure behavior still follows the real located-style route.
                assert_eq!(driver.driver.phase, Phase::Failed);
                assert!(!driver.widgets.location_pending());
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
            driver.driver.phase = Phase::Complete;
            driver
                .reset_scenario(
                    "source".into(),
                    "compiled".into(),
                    "512".into(),
                    "quiet".into(),
                )
                .unwrap();
            assert_eq!(driver.driver.phase, Phase::AwaitBootstrap);
            assert_eq!(driver.driver.reporting.state_is_absent(), !enabled);
            driver.driver.reporting.observe(|state| {
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
                Some(surface),
            ));
            assert_eq!(driver.driver.phase, Phase::TrainNavigation);
            if !enabled {
                assert!(capture.records().is_empty());
            }
            for dark in [false, true] {
                driver.driver.phase = Phase::Complete;
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
                    Some(surface),
                ));
                assert_eq!(driver.driver.phase, Phase::TrainNavigation);
                STYLES.with(|styles| styles.borrow_mut().as_mut().unwrap().clear());
                driver.driver.reporting.observe(|state| {
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
                    assert!(driver.driver.reporting.state_is_absent());
                }
            }
        }
    }

    #[test]
    fn passive_reporting_and_failure_receipts_have_independent_activation() {
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
                state.reopen_wait(Some(frame), snapshot, 0, 0, false, None);
                state.reopen_wait(Some(frame), snapshot, 0, 0, false, None);
                state.located(&Phase::AdvancedField(2), "field", Rectangle::default());
            });
            driver.driver.fail_detail(|| {
                query_count.set(query_count.get() + 1);
                format!("failure {}", 7).into()
            });
            assert_eq!(driver.driver.phase, Phase::Failed);
            driver.driver.report_phase_progress();
            driver.driver.report_phase_progress();
            driver.observe_reporting(|_| panic!("failed controller ran passive reporting"));
            assert_eq!(query_count.get(), if enabled { 3 } else { 1 });
            let records = capture.records();
            if !enabled {
                assert!(records.is_empty());
                assert!(driver.driver.reporting.state_is_absent());
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
            for (index, detail, matching) in [
                (3, "accepted", 1.0),
                (4, "surface-missing", 0.0),
                (5, "rejected", 0.0),
            ] {
                assert_eq!(records[index].2, detail);
                assert_eq!(
                    records[index].3,
                    [
                        frame.content_sequence as f64,
                        frame.presentation_revision as f64,
                        0.0,
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
        let expected = |kind, progress, failureline, failure: &str| {
            crate::generated::IntegrationControl {
                protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                receipt: crate::generated::IntegrationControlReceipt {
                    kind,
                    sequence: 1,
                    progress,
                    failureline,
                    failure: failure.into(),
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
        assert_eq!(
            wire,
            vec![expected(IntegrationControlKind::Progress, 5, 0, "")]
        );
        wire.clear();
        let sample_count = 129;
        for index in 0..sample_count {
            let mut mouse = crate::workspace_input::record(
                crate::generated::WorkspaceMouseKind::Motion,
                Some(crate::generated::WorkspacePoint {
                    x: index as f32,
                    y: 1.0,
                }),
            );
            mouse.source = crate::generated::PresentationSourceKind::Annotation;
            mouse.documentepoch = 1;
            connection.send_workspace_mouse(mouse).unwrap();
        }
        connection
            .flush(|bytes| {
                wire.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(wire.len(), sample_count + 1);
        assert_eq!(
            wire.last().unwrap(),
            &expected(IntegrationControlKind::PressureEntered, 0, 0, "")
        );
        driver.driver.phase = Phase::Complete;
        assert!(connection.integration_pressure_settled());
        wire.clear();
        driver.publish_control(&mut connection);
        connection
            .flush(|bytes| {
                wire.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(
            wire,
            vec![expected(IntegrationControlKind::Settled, 10, 0, "")]
        );
        let failure_detail = "Protocol: Invalid snapshot: inconsistent frame revision";
        let failure_line = line!() + 1;
        driver.driver.fail(failure_detail);
        assert_eq!(driver.driver.failure_line, failure_line);
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
            vec![expected(
                IntegrationControlKind::Failed,
                15,
                failure_line,
                failure_detail
            )]
        );
        let forwarded_line = line!() + 1;
        driver.driver.fail("static quiet failure");
        assert_eq!(driver.driver.failure_line, forwarded_line);
        assert!(
            driver
                .reset_scenario(String::new(), String::new(), String::new(), "quiet".into())
                .is_err()
        );
        driver.driver.phase = Phase::Complete;
        driver
            .reset_scenario(String::new(), String::new(), String::new(), "quiet".into())
            .unwrap();
        assert_eq!(driver.driver.control_progress, 0);
        assert_eq!(driver.driver.failure_line, 0);
        assert_eq!(driver.driver.control_phase, None);
        assert!(driver.driver.reporting.state_is_absent());
        assert!(capture.records().is_empty());
    }
}
// Captures exist only within these fixtures; ordinary enabled execution
// retains neither records nor instrumentation counters in Rust.
#[cfg(test)]
pub(super) struct Capture;
#[cfg(test)]
impl Capture {
    pub(super) fn new(enabled: bool) -> Self {
        initialize_reporting(enabled, false);
        RECORDS.with(|records| *records.borrow_mut() = Some(Vec::new()));
        STYLES.with(|styles| *styles.borrow_mut() = Some(Vec::new()));
        Self
    }

    pub(super) fn records(&self) -> Vec<(String, String, String, [f64; 4])> {
        RECORDS.with(|records| std::mem::take(records.borrow_mut().as_mut().unwrap()))
    }
}
#[cfg(test)]
impl Drop for Capture {
    fn drop(&mut self) {
        RECORDS.with(|records| *records.borrow_mut() = None);
        STYLES.with(|styles| *styles.borrow_mut() = None);
        initialize_reporting(false, false);
    }
}
