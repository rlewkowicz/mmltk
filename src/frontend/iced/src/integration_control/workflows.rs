//! Real model workflows driven through the packaged Iced interface.
use super::annotation_checks::annotation_layout_scale;
use crate::generated::FeatureId;
use crate::generated::{ComputeOperationOutcome, SourceKind};
use crate::integration_control::pixel_checks::ProbeOutcome;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::pixel_checks::pixel_result_callback;
use crate::integration_control::widget_ops::click;
use crate::integration_control::widget_ops::{AnnotationReveal, locate, reveal_control};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::workflow_pixels_js;
use crate::integration_control::{Driver, Phase, reporting, widget_ops};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{Message, probe::scenario_output};
use crate::message::Message as RootMessage;
use crate::view::{settings, workflow};
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Picture {
    Progress,
    Train,
    Validation,
    Detail,
    Compiled,
    Image,
    Video,
    Stop,
    Theme,
    Narrow,
}
impl Picture {
    fn name(self) -> &'static str {
        match self {
            Self::Progress => "progress",
            Self::Train => "train",
            Self::Validation => "validation",
            Self::Detail => "detail",
            Self::Compiled => "compiled",
            Self::Image => "image",
            Self::Video => "video",
            Self::Stop => "stop",
            Self::Theme => "theme",
            Self::Narrow => "narrow",
        }
    }
    fn chart(self) -> bool {
        matches!(self, Self::Train | Self::Theme | Self::Narrow)
    }
    fn control(self, _index: u8) -> String {
        match self {
            Self::Progress => "train.progress.bar".into(),
            Self::Train | Self::Theme | Self::Narrow => "train.metrics.plot".into(),
            Self::Validation => crate::view::validate::samples::ATLAS_ID.into(),
            Self::Detail => "validate.detail.image".into(),
            _ => "workflow.visual.workspace".into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Retention {
    Expanded,
    Back,
    Hidden,
    Revealed,
    Navigation,
}
impl Retention {
    fn name(self) -> &'static str {
        match self {
            Self::Expanded => "expanded",
            Self::Back => "back",
            Self::Hidden => "hidden",
            Self::Revealed => "revealed",
            Self::Navigation => "navigation",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Step {
    Train,
    StartTrain,
    Training,
    LeaveTrain,
    HiddenTrain,
    ReturnTrain,
    Trained,
    NoImageWorkspace,
    TrainAspect,
    ChartBounds,
    ChartTile(u8),
    ChartLegend,
    ChartLegendPending,
    ChartLegendChanged,
    ChartPan,
    ChartPanPending,
    ChartPanned,
    ChartSettle(Retention),
    ChartSettlePending(Retention),
    ChartRetained(Retention),
    ChartSelector,
    ChartHide,
    ChartAbsent,
    ChartShow,
    ChartCloseSelector,
    ChartLeave,
    ChartAway,
    ChartReturn,
    ExpandChart,
    ExpandedBounds,
    ExpandedChart,
    ChartWheel(u8, bool),
    ChartWheelPending(u8, bool),
    ChartWheelSettle(u8, bool),
    ChartWheelSettling(u8, bool),
    ChartScrolled(u8, bool),
    BackToCharts,
    ChartAspect(u8),
    ChartAspectReady(u8),
    Validate,
    StartValidate,
    Validating,
    NoValidationAspect,
    OpenSample,
    Sample,
    HideBoxes,
    HiddenBoxes,
    ValidationLayer(bool, u8),
    ValidationLayerReady(bool, u8),
    CloseSample,
    ClosedSample,
    Predict,
    Source(u8),
    SourceReady(u8),
    StartPredict(u8),
    Predicting(u8),
    Pause,
    Paused,
    Resume,
    VideoEnd,
    Restart,
    Restarted,
    Stop,
    Stopped,
    Export,
    PrepareExport,
    StartExport,
    Exporting,
    StopExport,
    ExportStopped,
    ExportReturn,
    Theme,
    Dark,
    DarkReady,
    Narrow,
    NarrowReady,
    Pixels(Picture, u8),
    AwaitPixels(Picture, u8),
}

#[derive(Default)]
pub(super) struct State {
    chart_bounds: Rectangle,
    chart_view: Option<crate::view::metrics::ChartView>,
    chart_sequence: Option<u64>,
    wheel_y: f32,
    hidden_sequence: u64,
    generation: u64,
    video_index: u64,
    narrow_scale: f32,
    pixel_source: u64,
    pixel_presentation: u64,
    pixel_attempts: u8,
    progress_epoch: Option<u64>,
    validation_layer: u8,
    export_pixels: bool,
    primary_pixels: [bool; 4],
    primary_reveal: [Option<(u64, u64)>; 4],
    work_progress: Option<(FeatureId, u64, u64)>,
    export_narrow: bool,
}

fn atlas_cell(bounds: Rectangle, index: u8) -> Rectangle {
    Rectangle {
        x: bounds.x + f32::from(index % 2) * bounds.width / 2.0,
        y: bounds.y + f32::from(index / 2) * bounds.height / 3.0,
        width: bounds.width / 2.0,
        height: bounds.height / 3.0,
    }
}
fn layer_selection(index: u8) -> (bool, bool) {
    match index {
        1 => (false, true),
        2 => (false, false),
        3 => (true, false),
        _ => (true, true),
    }
}

fn source(index: u8) -> SourceKind {
    match index {
        0 => SourceKind::CompiledDataset,
        1 => SourceKind::SingleImage,
        _ => SourceKind::VideoFile,
    }
}
fn primary(feature: FeatureId) -> &'static str {
    workflow::Composition::new(feature, 0.0).stable_id(workflow::Region::PrimaryAction)
}
fn completed(stage: &str, facts: [f64; 4]) {
    reporting::emit(|sink| sink.record("integration.workflow.completed", "", stage, facts));
}

#[cfg(test)]
mod tests {
    use super::{Picture, Step};
    use crate::generated::FeatureId;
    use crate::integration_control::pixel_checks::ProbeOutcome;
    use crate::integration_control::{Controller, Message, Phase};

    fn advance_workflow(
        controller: &mut Controller,
        model: &crate::view_model::ApplicationModel,
        step: Step,
        feature: FeatureId,
    ) -> usize {
        controller
            .workflows
            .advance_workflows(
                &mut controller.widgets,
                &mut controller.driver,
                step,
                model,
                &crate::view::settings::SettingsModel::default(),
                feature,
                None,
                &crate::view::router::Router::default(),
            )
            .units()
    }

    #[test]
    fn validation_controls_wait_for_the_sample_selection_reply() {
        for step in [
            Step::OpenSample,
            Step::HideBoxes,
            Step::ValidationLayer(true, 1),
            Step::CloseSample,
        ] {
            let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
            let controller = &mut fixture.controller;
            controller.driver.phase = Phase::Workflows(step);
            let mut model = crate::view_model::test_support::bootstrapped();
            model.workflow.validation.as_mut().unwrap().detail = true;
            let pending = model
                .begin_intent(crate::generated::ApplicationIntentEndpoint::ValidationSelectSample)
                .unwrap();
            assert_eq!(
                advance_workflow(controller, &model, step, FeatureId::Validate),
                0
            );
            assert!(!controller.widgets.location_pending());
            model.abandon_intent(pending);
            assert!(advance_workflow(controller, &model, step, FeatureId::Validate) > 0);
        }
    }

    #[test]
    fn active_primary_reveal_yields_to_rendering_until_native_progress_changes() {
        for (feature, step) in [
            (FeatureId::Train, Step::Training),
            (FeatureId::Export, Step::Exporting),
        ] {
            let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
            let controller = &mut fixture.controller;
            controller.driver.phase = Phase::Workflows(step);
            let mut model = crate::view_model::test_support::bootstrapped();
            let operation = match feature {
                FeatureId::Train => &mut model.workflow.training.as_mut().unwrap().local,
                _ => model.workflow.export.as_mut().unwrap(),
            };
            operation.active = true;
            assert!(advance_workflow(controller, &model, step, feature) > 0);
            controller.widgets.location_completed();
            assert_eq!(advance_workflow(controller, &model, step, feature), 0);
            assert_eq!(advance_workflow(controller, &model, step, feature), 0);
            let operation = match feature {
                FeatureId::Train => &mut model.workflow.training.as_mut().unwrap().local,
                _ => model.workflow.export.as_mut().unwrap(),
            };
            operation.progress.sequence += 1;
            assert!(advance_workflow(controller, &model, step, feature) > 0);
        }
    }

    #[test]
    fn page_navigation_waits_for_verified_idle_primary_pixels() {
        let mut fixture = crate::integration_control::ProbeFixture::new("");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::PagePrimary(FeatureId::Validate);
        let model = crate::view_model::test_support::bootstrapped();
        let settings = crate::view::settings::SettingsModel::default();
        let advance = |controller: &mut Controller| {
            controller
                .lifecycle
                .advance_lifecycle(
                    &mut controller.driver,
                    &mut controller.widgets,
                    &model,
                    &settings,
                    1.0,
                    FeatureId::Validate,
                )
                .units()
        };
        assert!(advance(controller) > 0);
        let _ = controller.update_location(
            "validate.primary".into(),
            iced::Rectangle {
                x: 10.0,
                y: 10.0,
                width: 200.0,
                height: 48.0,
            },
        );
        assert_eq!(
            controller.driver.phase,
            Phase::AwaitPagePrimary(FeatureId::Validate)
        );
        assert_eq!(advance(controller), 0);
        let completion = |control: &str, active| Message::Scoped {
            generation: controller.driver.generation,
            receipt: None,
            message: Box::new(Message::PrimaryActionPixels {
                control: control.into(),
                active,
                token: 1,
            }),
        };
        let active = completion("validate.primary", true);
        let other = completion("train.primary", false);
        let idle = completion("validate.primary", false);
        let _ = controller.update(active);
        let _ = controller.update(other);
        assert_eq!(advance(controller), 0);
        let _ = controller.update(idle);
        let _ = advance(controller);
        assert_eq!(
            controller.driver.phase,
            Phase::PageNavigation(FeatureId::Predict)
        );
    }

    #[test]
    fn export_stop_requires_current_active_primary_canvas_completion() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::Exporting);
        let generation = controller.driver.generation;
        let completion = |generation, active| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::PrimaryActionPixels {
                control: "export.primary".into(),
                active,
                token: 1,
            }),
        };
        let _ = controller.update(completion(generation.wrapping_sub(1), true));
        assert!(!controller.workflows.export_pixels);
        let _ = controller.update(completion(generation, false));
        assert!(!controller.workflows.export_pixels);
        let _ = controller.update(completion(generation, true));
        assert!(controller.workflows.export_pixels);
    }

    #[test]
    fn primary_measurement_tasks_belong_to_the_current_enabled_scenario() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::Exporting);
        let generation = controller.driver.generation;
        let request = |generation| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::PrimaryActionMeasure {
                control: "export.primary".into(),
                token: 1,
            }),
        };
        let _ = controller.update(request(generation.wrapping_sub(1)));
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_none()
        );
        let _ = controller.update(request(generation));
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_some()
        );
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_none()
        );
        crate::integration_control::initialize_reporting(false, false);
        let _ = controller.update(request(generation));
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_none()
        );
        assert!(
            !crate::integration_control::reporting::primary_action_current("export.primary", 1)
        );
    }

    #[test]
    fn chart_input_completion_requires_current_scenario_and_successful_settlement() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::ChartPanPending);
        let completion = |generation, delivered| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::ChartInputDelivered(delivered)),
        };
        let generation = controller.driver.generation;
        let _ = controller.update(completion(generation.wrapping_sub(1), true));
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::ChartPanPending)
        );
        let _ = controller.update(completion(generation, true));
        assert_eq!(controller.driver.phase, Phase::Workflows(Step::ChartPanned));
        controller.driver.phase = Phase::Workflows(Step::ChartWheelSettling(1, true));
        let _ = controller.update(completion(generation, true));
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::ChartScrolled(1, true))
        );
        controller.driver.phase = Phase::Workflows(Step::ChartLegendPending);
        let _ = controller.update(completion(generation, false));
        assert_eq!(controller.driver.phase, Phase::Failed);
    }

    #[test]
    fn workflow_completion_requires_current_nonempty_canvas_evidence() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::AwaitPixels(Picture::Narrow, 0));
        let capture = |generation, outcome| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::WorkflowPixels {
                picture: Picture::Narrow,
                index: 0,
                outcome,
            }),
        };
        let _ = controller.update(capture(
            controller.driver.generation.wrapping_sub(1),
            ProbeOutcome::Observed(256, 256),
        ));
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::AwaitPixels(Picture::Narrow, 0))
        );
        let _ = controller.update(capture(
            controller.driver.generation,
            ProbeOutcome::Observed(256, 0),
        ));
        assert_eq!(controller.driver.phase, Phase::Failed);
    }

    #[test]
    fn progress_capture_rejects_a_training_phase_change_without_accepting_blank_pixels() {
        for changed in [false, true] {
            let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
            let controller = &mut fixture.controller;
            controller.driver.phase = Phase::Workflows(Step::AwaitPixels(Picture::Progress, 0));
            let mut model = crate::view_model::test_support::bootstrapped();
            let train = model.workflow.training.as_mut().unwrap();
            train.local.active = true;
            train.metrics = Some(crate::view::metrics::tests::record());
            let progress = &mut train.metrics.as_mut().unwrap().progress;
            controller.workflows.progress_epoch = Some(progress.epoch as u64);
            if changed {
                progress.phase = crate::generated::TrainingPhase::Validate;
            }
            let _ = controller.workflows.advance_workflows(
                &mut controller.widgets,
                &mut controller.driver,
                Step::AwaitPixels(Picture::Progress, 0),
                &model,
                &crate::view::settings::SettingsModel::default(),
                FeatureId::Train,
                None,
                &crate::view::router::Router::default(),
            );
            controller.workflows.workflow_pixels(
                &mut controller.driver,
                Picture::Progress,
                0,
                ProbeOutcome::Observed(256, 0),
            );
            assert_eq!(
                controller.driver.phase,
                if changed {
                    Phase::Workflows(Step::Training)
                } else {
                    Phase::Failed
                }
            );
        }
    }
}

impl State {
    pub(super) fn wheel_delivered(&mut self, driver: &mut Driver) {
        if let Phase::Workflows(Step::ChartWheelPending(index, expanded)) = driver.phase {
            driver.phase = Phase::Workflows(Step::ChartWheelSettle(index, expanded));
        }
    }

    pub(super) fn chart_input_delivered(&mut self, driver: &mut Driver) {
        let Phase::Workflows(step) = driver.phase else {
            return;
        };
        let next = match step {
            Step::ChartLegendPending => Step::ChartLegendChanged,
            Step::ChartPanPending => Step::ChartPanned,
            Step::ChartSettlePending(retention) => Step::ChartRetained(retention),
            Step::ChartWheelSettling(index, expanded) => Step::ChartScrolled(index, expanded),
            _ => return,
        };
        driver.phase = Phase::Workflows(next);
    }

    fn retained(
        &self,
        driver: &mut Driver,
        view: &crate::view::metrics::ChartView,
        stage: &str,
    ) -> bool {
        let Some(expected) = &self.chart_view else {
            driver.fail("Missing chart view baseline");
            return false;
        };
        if view.ranges != expected.ranges || view.legend_collapsed != expected.legend_collapsed {
            driver.fail(&format!("Chart camera or legend changed during {stage}"));
            return false;
        }
        reporting::chart_view(stage, view);
        true
    }

    pub(super) fn primary_action_pixels(&mut self, driver: &Driver, control: &str, active: bool) {
        if !active {
            return;
        }
        for (index, feature) in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ]
        .into_iter()
        .enumerate()
        {
            if control == primary(feature) {
                self.primary_pixels[index] = true;
            }
        }
        if driver.phase == Phase::Workflows(Step::Exporting)
            && control == primary(FeatureId::Export)
        {
            self.export_pixels = true;
        }
    }

    pub(super) fn workflow_step(&mut self, driver: &mut Driver, step: Step) -> Task<RootMessage> {
        driver.advance_to(Phase::Workflows(step))
    }
    pub(super) fn workflow_control(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        control: impl Into<String>,
    ) -> Task<RootMessage> {
        if !widgets.begin_location() {
            return Task::none();
        }
        let control = control.into();
        if matches!(
            driver.phase,
            Phase::Workflows(
                Step::Train
                    | Step::LeaveTrain
                    | Step::ReturnTrain
                    | Step::ChartLeave
                    | Step::ChartReturn
                    | Step::Validate
                    | Step::Predict
                    | Step::Export
                    | Step::ExportReturn
                    | Step::Theme
                    | Step::NoImageWorkspace
                    | Step::TrainAspect
                    | Step::NoValidationAspect
            )
        ) {
            // Navigation sits above the page scroller; absent-control checks
            // also need the unmodified tree result, without scroll clipping.
            locate(control, driver.generation)
        } else {
            reveal_control(control, driver.generation, AnnotationReveal::Control)
        }
    }
    pub(super) fn workflow_pixels(
        &mut self,
        driver: &mut Driver,
        picture: Picture,
        index: u8,
        outcome: ProbeOutcome,
    ) {
        if driver.phase != Phase::Workflows(Step::AwaitPixels(picture, index)) {
            return;
        }
        if picture == Picture::Progress && self.progress_epoch.is_none() {
            self.retry_progress_capture(driver);
            return;
        }
        match outcome {
            ProbeOutcome::Invalidated if self.pixel_attempts < 32 => {
                self.pixel_attempts += 1;
                driver.phase = Phase::Workflows(Step::Pixels(picture, index));
            }
            ProbeOutcome::Observed(sampled, visible) if sampled > 0 && visible >= 12 => {
                self.pixel_attempts = 0;
                reporting::emit(|sink| {
                    sink.record(
                        "integration.workflow.pixels",
                        &picture.control(index),
                        picture.name(),
                        [
                            self.pixel_source as f64,
                            self.pixel_presentation as f64,
                            sampled as f64,
                            visible as f64,
                        ],
                    )
                });
                driver.phase = Phase::Workflows(match picture {
                    Picture::Progress => Step::LeaveTrain,
                    Picture::Train => Step::NoImageWorkspace,
                    Picture::Validation if index < 5 => Step::Pixels(picture, index + 1),
                    Picture::Validation if self.validation_layer < 4 => Step::ValidationLayer(false, self.validation_layer + 1),
                    Picture::Validation => { self.validation_layer = 0; Step::OpenSample },
                    Picture::Detail if self.validation_layer < 4 => Step::ValidationLayer(true, self.validation_layer + 1),
                    Picture::Detail => Step::CloseSample,
                    Picture::Compiled => Step::Source(1),
                    Picture::Image => Step::Source(2),
                    Picture::Video => Step::Restart,
                    Picture::Stop => Step::Export,
                    Picture::Theme => Step::Narrow,
                    Picture::Narrow => {
                        completed(
                            "narrow",
                            [f64::from(self.narrow_scale), 0.0, 0.0, 0.0],
                        );
                        driver.phase = Phase::Complete;
                        return;
                    }
                });
            }
            _ => driver.fail(&format!(
                "Workflow {picture:?} canvas did not contain its expected rendered content: {outcome:?}"
            )),
        }
    }
    fn retry_progress_capture(&mut self, driver: &mut Driver) {
        if self.pixel_attempts < 32 {
            self.pixel_attempts += 1;
            driver.phase = Phase::Workflows(Step::Training);
        } else {
            driver.fail("Training progress capture remained invalidated");
        }
    }
    pub(super) fn workflow_located(
        &mut self,
        driver: &mut Driver,
        control: &str,
        bounds: Rectangle,
    ) {
        let Phase::Workflows(step) = driver.phase else {
            return;
        };
        if matches!(step, Step::Pixels(Picture::Progress, _)) && self.progress_epoch.is_none() {
            self.retry_progress_capture(driver);
            return;
        }
        if matches!(step, Step::NoImageWorkspace | Step::NoValidationAspect) {
            if bounds.width > 0.0 || bounds.height > 0.0 {
                driver.fail("Workflow still exposes a removed image workspace or aspect selector");
                return;
            }
            driver.phase = Phase::Workflows(match step {
                Step::NoImageWorkspace => Step::TrainAspect,
                _ => Step::Pixels(Picture::Validation, 0),
            });
            return;
        }
        if step == Step::ChartAbsent {
            if bounds.width > 0.0 || bounds.height > 0.0 {
                driver.fail("Hidden chart remains mounted in the rendered dashboard");
            } else {
                driver.phase = Phase::Workflows(Step::ChartShow);
            }
            return;
        }
        if matches!(step, Step::TrainAspect) {
            if bounds.width <= 0.0 || bounds.height <= 0.0 {
                driver.fail("Train workspace aspect selector is missing");
            } else {
                driver.phase = Phase::Workflows(Step::ChartBounds);
            }
            return;
        }
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            driver.fail(&format!(
                "Workflow control {control} is missing from the rendered Iced tree in {step:?}"
            ));
            return;
        }
        match step {
            Step::Training | Step::Validating | Step::Predicting(_) | Step::Exporting => {
                driver
                    .reporting
                    .observe(|reporting| reporting.located(&driver.phase, control, bounds));
                return;
            }
            Step::ChartLegend | Step::ChartPan => {
                driver.phase = Phase::Workflows(if step == Step::ChartLegend {
                    Step::ChartLegendPending
                } else {
                    Step::ChartPanPending
                });
                let input =
                    crate::presentation_surface::physical_bounds(bounds, driver.input_scale);
                if !widget_ops::chart_input(Some(input), step == Step::ChartPan) {
                    driver.fail("Chart pointer dispatch failed");
                }
                return;
            }
            Step::ChartBounds | Step::ExpandedBounds => {
                self.chart_bounds = bounds;
                driver.phase = Phase::Workflows(if step == Step::ExpandedBounds {
                    Step::ExpandedChart
                } else {
                    Step::ChartTile(0)
                });
                return;
            }
            Step::ChartWheel(index, expanded) => {
                self.wheel_y = bounds.y;
                driver.phase = Phase::Workflows(Step::ChartWheelPending(index, expanded));
                if !widget_ops::chart_wheel(
                    crate::presentation_surface::physical_bounds(bounds, driver.input_scale),
                    index,
                    driver.input_scale,
                ) {
                    driver.fail("Chart wheel dispatch failed");
                }
                return;
            }
            Step::ChartScrolled(index, expanded) => {
                if bounds.y >= self.wheel_y - 0.1 {
                    driver.fail("Wheel over chart did not scroll the enclosing page");
                    return;
                }
                completed(
                    &format!(
                        "chart_wheel_{}_{}",
                        if expanded { "expanded" } else { "grid" },
                        index
                    ),
                    [self.wheel_y as f64, bounds.y as f64, index as f64, 0.0],
                );
                driver.phase = Phase::Workflows(if index < 5 {
                    Step::ChartWheel(index + 1, expanded)
                } else if expanded {
                    Step::BackToCharts
                } else {
                    Step::ExpandChart
                });
                return;
            }
            Step::ChartTile(_) | Step::ExpandedChart => {
                let index = if let Step::ChartTile(index) = step {
                    index
                } else {
                    0
                };
                let region = self.chart_bounds;
                if bounds.x < region.x - 1.0
                    || bounds.y < region.y - 1.0
                    || bounds.x + bounds.width > region.x + region.width + 1.0
                    || bounds.y + bounds.height > region.y + region.height + 1.0
                {
                    driver.fail("Training chart extends outside its workspace");
                    return;
                }
                if step == Step::ExpandedChart {
                    if (bounds.width - region.width).abs() > 2.0
                        || (bounds.height - region.height).abs() > 2.0
                    {
                        driver.fail("Expanded chart does not fill its workspace");
                        return;
                    }
                    completed(
                        "chart_expanded",
                        [bounds.width as f64, bounds.height as f64, 0.0, 0.0],
                    );
                    driver.phase = Phase::Workflows(Step::ChartSettle(Retention::Expanded));
                } else {
                    completed(
                        &format!("chart_tile_{index}"),
                        [index as f64, bounds.width as f64, bounds.height as f64, 0.0],
                    );
                    driver.phase = Phase::Workflows(if index < 5 {
                        Step::ChartTile(index + 1)
                    } else {
                        Step::ChartLegend
                    });
                }
                return;
            }
            _ => {}
        }
        driver
            .reporting
            .observe(|reporting| reporting.located(&driver.phase, control, bounds));
        // Repeated clicks on selectable checkbox text select a word. Target
        // the checkbox square so hide/show exercises the toggle itself.
        let bounds = if matches!(step, Step::ChartHide | Step::ChartShow) {
            Rectangle {
                width: bounds.width.min(bounds.height),
                ..bounds
            }
        } else {
            bounds
        };
        let bounds = match step {
            Step::Pixels(Picture::Validation, index) => atlas_cell(bounds, index),
            Step::OpenSample => atlas_cell(bounds, 0),
            Step::ValidationLayer(..) => Rectangle {
                width: bounds.width.min(bounds.height),
                ..bounds
            },
            _ => bounds,
        };
        let input = crate::presentation_surface::physical_bounds(bounds, driver.input_scale);
        if let Step::Pixels(picture, index) = step {
            driver.phase = Phase::Workflows(Step::AwaitPixels(picture, index));
            #[cfg(not(target_arch = "wasm32"))]
            let _ = (input, picture.chart());
            #[cfg(target_arch = "wasm32")]
            {
                let output = scenario_output();
                let Some(mut output) = output else {
                    driver.fail("Workflow pixel callback has no live scenario owner");
                    return;
                };
                // This completion belongs to the workflow request. The browser
                // verifies its exact frame/geometry and must deliver invalidation
                // even if the observer's last generic surface receipt changed.
                output.receipt = None;
                let callback = pixel_result_callback(move |outcome| {
                    output.send(Message::WorkflowPixels {
                        picture,
                        index,
                        outcome,
                    });
                });
                workflow_pixels_js(
                    control,
                    &[
                        f64::from(input.x),
                        f64::from(input.y),
                        f64::from(input.width),
                        f64::from(input.height),
                    ],
                    picture.chart(),
                    picture == Picture::Progress,
                    self.pixel_source as f64,
                    self.pixel_presentation as f64,
                    &callback,
                );
            }
            return;
        }
        driver.phase = Phase::Workflows(match step {
            Step::ExpandChart => Step::ExpandedBounds,
            Step::BackToCharts => Step::ChartSettle(Retention::Back),
            Step::ChartSelector => Step::ChartHide,
            Step::ChartHide => Step::ChartSettle(Retention::Hidden),
            Step::ChartShow => Step::ChartCloseSelector,
            Step::ChartCloseSelector => Step::ChartSettle(Retention::Revealed),
            Step::ChartLeave => Step::ChartAway,
            Step::ChartReturn => Step::ChartSettle(Retention::Navigation),
            Step::ChartAspect(index) => Step::ChartAspectReady(index),
            Step::Train => Step::StartTrain,
            Step::StartTrain => Step::Training,
            Step::LeaveTrain => Step::HiddenTrain,
            Step::ReturnTrain => Step::Trained,
            Step::Validate => Step::StartValidate,
            Step::StartValidate => Step::Validating,
            Step::OpenSample => Step::Sample,
            Step::HideBoxes => Step::HiddenBoxes,
            Step::ValidationLayer(detail, index) => Step::ValidationLayerReady(detail, index),
            Step::CloseSample => Step::ClosedSample,
            Step::Predict => Step::Source(0),
            Step::Source(index) => Step::SourceReady(index),
            Step::StartPredict(index) => Step::Predicting(index),
            Step::Pause => Step::Paused,
            Step::Resume => Step::VideoEnd,
            Step::Restart => Step::Restarted,
            Step::Stop => Step::Stopped,
            Step::Export => Step::PrepareExport,
            Step::StartExport => {
                self.export_pixels = false;
                self.primary_pixels[3] = false;
                self.primary_reveal[3] = None;
                Step::Exporting
            }
            Step::StopExport => Step::ExportStopped,
            Step::ExportReturn => Step::Pixels(Picture::Narrow, 0),
            Step::Theme => Step::Dark,
            _ => {
                driver.fail("Unexpected workflow click continuation");
                return;
            }
        });
        if !click(input) {
            driver.fail("Workflow pointer dispatch failed");
        }
    }
    pub(super) fn advance_workflows(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        step: Step,
        model: &ApplicationModel,
        settings: &settings::SettingsModel,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
        router: &crate::view::router::Router,
    ) -> Task<RootMessage> {
        let settled = !settings.has_local_edits();
        let train = model.workflow.training.as_ref();
        let validation = model.workflow.validation.as_ref();
        let prediction = model.predict_snapshot.as_ref();
        let record = train.and_then(|value| value.metrics.as_ref());
        if crate::integration_control::reporting_enabled() {
            let work = match step {
                Step::Training | Step::HiddenTrain | Step::Trained => {
                    train.map(|value| (FeatureId::Train, &value.local))
                }
                Step::Validating => validation.map(|value| (FeatureId::Validate, &value.operation)),
                Step::Predicting(_) | Step::VideoEnd | Step::Restarted | Step::Stopped => {
                    prediction.map(|value| (FeatureId::Predict, &value.operation))
                }
                Step::Exporting | Step::ExportStopped => model
                    .workflow
                    .export
                    .as_ref()
                    .map(|value| (FeatureId::Export, value)),
                _ => None,
            };
            if let Some((feature, operation)) =
                work.filter(|(_, operation)| operation.active && operation.progress.sequence > 0)
            {
                let current = (
                    feature,
                    operation.generationfrontier,
                    operation.progress.sequence,
                );
                if self.work_progress != Some(current) {
                    self.work_progress = Some(current);
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.workflow.operation_progress",
                            primary(feature),
                            &format!("{step:?}"),
                            [current.1 as f64, current.2 as f64, 0.0, 0.0],
                        )
                    });
                }
            }
        }
        if matches!(
            step,
            Step::Pixels(Picture::Progress, _) | Step::AwaitPixels(Picture::Progress, _)
        ) && self.progress_epoch.is_some()
            && !(train.is_some_and(|value| value.local.active)
                && record.is_some_and(|value| {
                    value.progress.phase == crate::generated::TrainingPhase::Train
                        && Some(value.progress.epoch as u64) == self.progress_epoch
                }))
        {
            // A native phase transition can remove the bar between widget
            // measurement and the asynchronous canvas read. Retire that request;
            // never interpret pixels from its old rectangle as current evidence.
            reporting::emit(|sink| {
                sink.record(
                    "integration.workflow.progress_invalidated",
                    "train.progress.bar",
                    "native phase changed before canvas completion",
                    [
                        self.progress_epoch.unwrap() as f64,
                        record.map_or(-1.0, |value| value.progress.epoch as f64),
                        0.0,
                        0.0,
                    ],
                )
            });
            self.progress_epoch = None;
        }
        let success = |value: &crate::generated::ComputeUiState| {
            !value.active && value.terminal.outcome == ComputeOperationOutcome::Succeeded
        };
        // Observe the primary through normal scrolling before progressing to
        // another card. Native progress may increase the setup column height.
        let primary_observation = match step {
            Step::Training => train.map(|value| (0, FeatureId::Train, &value.local)),
            Step::Validating => validation.map(|value| (1, FeatureId::Validate, &value.operation)),
            Step::Predicting(_) => {
                prediction.map(|value| (2, FeatureId::Predict, &value.operation))
            }
            Step::Exporting => model
                .workflow
                .export
                .as_ref()
                .map(|value| (3, FeatureId::Export, value)),
            _ => None,
        };
        if let Some((index, feature, operation)) = primary_observation
            && !self.primary_pixels[index]
            && model.primary_action_active(feature)
        {
            // A location completion immediately re-enters advance. Reveal once
            // per native progress change, then let rendering and transport run.
            let revision = (operation.generationfrontier, operation.progress.sequence);
            if !widgets.location_pending() && self.primary_reveal[index] != Some(revision) {
                self.primary_reveal[index] = Some(revision);
                return self.workflow_control(widgets, driver, primary(feature));
            }
            return Task::none();
        }
        match step {
            Step::Train | Step::ReturnTrain | Step::Theme | Step::ExportReturn => self
                .workflow_control(
                    widgets,
                    driver,
                    crate::view::navigation::stable_id(FeatureId::Train),
                ),
            Step::StartTrain
                if active == FeatureId::Train
                    && settled
                    && settings.draft.as_ref().is_some_and(|draft| {
                        model.compute_start_available(draft, FeatureId::Train)
                    }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Train))
            }
            Step::Training
                if train.is_some_and(|value| value.local.active)
                    && record.is_some_and(|value| {
                        value.progress.globaloptimizerstep > 0
                            && value.progress.phase == crate::generated::TrainingPhase::Train
                            && value.progress.completedimages > 0
                            && value.progress.completedimages < value.progress.totalimages
                    }) =>
            {
                let progress = &record.unwrap().progress;
                self.progress_epoch = Some(progress.epoch as u64);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.workflow.progress",
                        "train.progress.bar",
                        "native-image-counts",
                        [
                            progress.completedimages as f64,
                            progress.totalimages as f64,
                            progress.epoch as f64,
                            progress.totalepochs as f64,
                        ],
                    )
                });
                self.workflow_step(driver, Step::Pixels(Picture::Progress, 0))
            }
            Step::LeaveTrain | Step::Validate => {
                if step == Step::LeaveTrain {
                    self.hidden_sequence = record.unwrap().sequence;
                }
                self.workflow_control(
                    widgets,
                    driver,
                    crate::view::navigation::stable_id(FeatureId::Validate),
                )
            }
            Step::HiddenTrain
                if active == FeatureId::Validate
                    && record.is_some_and(|value| value.sequence > self.hidden_sequence) =>
            {
                self.workflow_step(driver, Step::ReturnTrain)
            }
            Step::Trained
                if active == FeatureId::Train
                    && train.is_some_and(|value| success(&value.local)) =>
            {
                let Some(record) = record else {
                    driver.fail("Completed training has no metric history");
                    return Task::none();
                };
                if record.progress.globaloptimizerstep <= 1 || record.progress.val.is_none() {
                    driver.fail("Training did not publish loss and validation metrics");
                    return Task::none();
                }
                completed(
                    "train",
                    [
                        record.sequence as f64,
                        record.progress.globaloptimizerstep as f64,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(driver, Step::Pixels(Picture::Train, 0))
            }
            Step::ChartLegend
            | Step::ChartPan
            | Step::ChartLegendChanged
            | Step::ChartPanned
            | Step::ChartRetained(_)
            | Step::ChartScrolled(_, _) => {
                // Observation is effect-only and never collected in ordinary quiet runs.
                if !crate::integration_control::reporting_enabled() {
                    driver.fail("Chart retained-view acceptance requires reporting");
                    return Task::none();
                }
                let Some(view) = router.train_chart_view(crate::view::metrics::Chart::Loss) else {
                    return Task::none();
                };
                let sequence = record.map(|record| record.sequence);
                if self.chart_sequence.is_some() && self.chart_sequence != sequence {
                    driver.fail("Chart retention input overlapped a training data update");
                    return Task::none();
                }
                match step {
                    Step::ChartLegend => {
                        self.chart_sequence = sequence;
                        self.chart_view = Some(view.clone());
                        self.workflow_control(widgets, driver, view.legend_control)
                    }
                    Step::ChartLegendChanged => {
                        let previous = self.chart_view.as_ref().unwrap();
                        if previous.legend_collapsed == view.legend_collapsed
                            || previous.ranges != view.ranges
                        {
                            driver.fail("Ordinary legend input did not change only the legend");
                            return Task::none();
                        }
                        self.chart_view = Some(view);
                        completed("chart_legend", [1.0, 0.0, 0.0, 0.0]);
                        self.workflow_step(driver, Step::ChartPan)
                    }
                    Step::ChartPan => {
                        self.workflow_control(widgets, driver, "train.metrics.chart.Loss")
                    }
                    Step::ChartPanned => {
                        let previous = self.chart_view.as_ref().unwrap();
                        if previous.ranges == view.ranges
                            || previous.legend_collapsed != view.legend_collapsed
                        {
                            driver.fail("Ordinary chart pan did not change the camera");
                            return Task::none();
                        }
                        reporting::chart_view("panned", &view);
                        self.chart_view = Some(view);
                        completed("chart_pan", [1.0, 0.0, 0.0, 0.0]);
                        self.workflow_step(driver, Step::ChartWheel(0, false))
                    }
                    Step::ChartRetained(retention) => {
                        if !self.retained(driver, &view, retention.name()) {
                            return Task::none();
                        }
                        if view.visible == (retention == Retention::Hidden) {
                            driver
                                .fail("Chart visibility input did not change dashboard membership");
                            return Task::none();
                        }
                        if retention == Retention::Expanded
                            && view.plot_bounds.width
                                <= self.chart_view.as_ref().unwrap().plot_bounds.width
                        {
                            driver.fail("Expanded chart did not publish its larger plot rectangle");
                            return Task::none();
                        }
                        if retention == Retention::Navigation && active != FeatureId::Train {
                            return Task::none();
                        }
                        completed(
                            &format!("chart_retained_{}", retention.name()),
                            [
                                view.plot_bounds.width as f64,
                                view.plot_bounds.height as f64,
                                0.0,
                                0.0,
                            ],
                        );
                        let next = match retention {
                            Retention::Expanded => Step::ChartWheel(0, true),
                            Retention::Back => Step::ChartSelector,
                            Retention::Hidden => Step::ChartAbsent,
                            Retention::Revealed => Step::ChartLeave,
                            Retention::Navigation => {
                                self.chart_sequence = None;
                                Step::ChartAspect(0)
                            }
                        };
                        self.workflow_step(driver, next)
                    }
                    Step::ChartScrolled(_, _) => {
                        if !self.retained(driver, &view, "wheel") {
                            return Task::none();
                        }
                        if widgets.begin_location() {
                            locate("train.metrics.chart.Loss".into(), driver.generation)
                        } else {
                            Task::none()
                        }
                    }
                    _ => unreachable!(),
                }
            }
            Step::ChartSettle(retention) => {
                driver.phase = Phase::Workflows(Step::ChartSettlePending(retention));
                if !widget_ops::chart_input(None, false) {
                    driver.fail("Chart render settlement failed");
                }
                Task::none()
            }
            Step::ChartWheelSettle(index, expanded) => {
                driver.phase = Phase::Workflows(Step::ChartWheelSettling(index, expanded));
                if !widget_ops::chart_input(None, false) {
                    driver.fail("Chart wheel settlement failed");
                }
                Task::none()
            }
            Step::ChartWheel(_, _) => {
                self.workflow_control(widgets, driver, "train.metrics.chart.Loss")
            }
            Step::ChartSelector | Step::ChartCloseSelector => {
                self.workflow_control(widgets, driver, "train.metrics.charts")
            }
            Step::ChartAbsent => {
                if widgets.begin_location() {
                    locate("train.metrics.chart.Loss".into(), driver.generation)
                } else {
                    Task::none()
                }
            }
            Step::ChartHide | Step::ChartShow => {
                self.workflow_control(widgets, driver, "train.metrics.visible.Loss")
            }
            Step::ChartLeave => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Validate),
            ),
            Step::ChartAway if active == FeatureId::Validate => {
                self.workflow_step(driver, Step::ChartReturn)
            }
            Step::ChartReturn => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Train),
            ),
            Step::ChartBounds | Step::ExpandedBounds => {
                self.workflow_control(widgets, driver, "train.metrics.plot")
            }
            Step::ChartTile(index) => {
                let names = [
                    "Loss",
                    "Ap50",
                    "Ap",
                    "AverageRecall",
                    "Confidence",
                    "LearningRate",
                ];
                self.workflow_control(
                    widgets,
                    driver,
                    format!("train.metrics.chart.{}", names[index as usize]),
                )
            }
            Step::ExpandChart => {
                self.workflow_control(widgets, driver, "train.metrics.expand.Loss")
            }
            Step::ExpandedChart => {
                self.workflow_control(widgets, driver, "train.metrics.chart.Loss")
            }
            Step::BackToCharts => self.workflow_control(widgets, driver, "train.metrics.back"),
            Step::ChartAspect(index) => {
                let aspect = crate::generated::WORKSPACE_ASPECT_RATIO_VALUES[index as usize];
                self.workflow_control(
                    widgets,
                    driver,
                    crate::view::aspect_ratio::option_id(aspect),
                )
            }
            Step::ChartAspectReady(index) if settled => {
                let aspect = crate::generated::WORKSPACE_ASPECT_RATIO_VALUES[index as usize];
                if settings
                    .draft
                    .as_ref()
                    .is_none_or(|s| s.ui.workspaceaspectratio != aspect)
                {
                    // Pointer dispatch is asynchronous: settled old settings
                    // can precede the click. Wait for the requested value;
                    // the existing phase deadline bounds missing delivery.
                    return Task::none();
                }
                completed(
                    &format!("chart_aspect_{index}"),
                    [
                        index as f64,
                        crate::view::aspect_ratio::height_factor(aspect) as f64,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(
                    driver,
                    if index as usize + 1 < crate::generated::WORKSPACE_ASPECT_RATIO_VALUES.len() {
                        Step::ChartAspect(index + 1)
                    } else {
                        Step::Validate
                    },
                )
            }
            Step::NoImageWorkspace => widgets.arm(driver, "workflow.visual.workspace"),
            Step::TrainAspect | Step::NoValidationAspect => {
                widgets.arm(driver, "workflow.workspace.aspect")
            }
            Step::StartValidate
                if active == FeatureId::Validate
                    && settled
                    && settings.draft.as_ref().is_some_and(|draft| {
                        model.compute_start_available(draft, FeatureId::Validate)
                    }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Validate))
            }
            Step::Validating if validation.is_some_and(|value| success(&value.operation)) => {
                let snapshot = validation.unwrap();
                let mut identities = std::collections::BTreeSet::new();
                if snapshot.metrics.is_none()
                    || !snapshot.sampleavailable.iter().all(|value| *value)
                    || !snapshot
                        .sampleidentities
                        .iter()
                        .all(|id| identities.insert((id.generation, id.datasetindex)))
                {
                    driver.fail(
                        "Validation did not produce metrics and six distinct retained samples",
                    );
                    return Task::none();
                }
                completed(
                    "validation",
                    [
                        snapshot.operation.terminal.completed as f64,
                        identities.len() as f64,
                        snapshot.detailrows as f64,
                        0.0,
                    ],
                );
                self.workflow_step(driver, Step::NoValidationAspect)
            }
            Step::OpenSample if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, crate::view::validate::samples::ATLAS_ID)
            }
            Step::Sample if validation.is_some_and(|value| value.detail) => {
                self.workflow_step(driver, Step::HideBoxes)
            }
            Step::HideBoxes if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, "validate.pred.boxes")
            }
            Step::HiddenBoxes
                if validation
                    .is_some_and(|value| value.detail && !value.overlays.predictionboxes) =>
            {
                self.workflow_step(driver, Step::Pixels(Picture::Detail, 0))
            }
            Step::ValidationLayer(_, index) if model.validation_navigation_available() => self
                .workflow_control(
                    widgets,
                    driver,
                    if index == 1 || index == 3 {
                        "validate.gt.layer"
                    } else {
                        "validate.pred.layer"
                    },
                ),
            Step::ValidationLayerReady(detail, index)
                if validation.is_some_and(|snapshot| {
                    snapshot.detail == detail
                        && snapshot.overlayselection.value == snapshot.overlays
                        && (
                            snapshot.overlays.groundtruthlayer,
                            snapshot.overlays.predictionlayer,
                        ) == layer_selection(index)
                }) =>
            {
                self.validation_layer = index;
                self.workflow_step(
                    driver,
                    Step::Pixels(
                        if detail {
                            Picture::Detail
                        } else {
                            Picture::Validation
                        },
                        0,
                    ),
                )
            }
            Step::CloseSample if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, "validate.detail.close")
            }
            Step::ClosedSample if validation.is_some_and(|value| !value.detail) => {
                self.workflow_step(driver, Step::Predict)
            }
            Step::Predict => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Predict),
            ),
            Step::Source(index) if active == FeatureId::Predict && settled => self
                .workflow_control(
                    widgets,
                    driver,
                    [
                        "predict.source.compiled",
                        "predict.source.image",
                        "predict.source.video",
                    ][usize::from(index)],
                ),
            Step::SourceReady(index)
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        value.workflows.predict.source.kind == source(index)
                    }) =>
            {
                self.generation = prediction.map_or(0, |value| value.operation.generationfrontier);
                self.workflow_step(driver, Step::StartPredict(index))
            }
            Step::StartPredict(_) | Step::Restart
                if settings.draft.as_ref().is_some_and(|draft| {
                    model.compute_start_available(draft, FeatureId::Predict)
                }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Predict))
            }
            Step::Predicting(index)
                if prediction
                    .is_some_and(|value| value.operation.generationfrontier > self.generation) =>
            {
                let snapshot = prediction.unwrap();
                if index == 2 {
                    if snapshot.operation.active && snapshot.operation.progress.completed >= 2 {
                        self.video_index = snapshot.operation.progress.completed;
                        return self.workflow_step(driver, Step::Pause);
                    }
                } else if success(&snapshot.operation) {
                    let picture = if index == 0 {
                        Picture::Compiled
                    } else {
                        Picture::Image
                    };
                    if snapshot.labels.is_empty() {
                        driver.fail("Prediction has no class labels or confidence values");
                        return Task::none();
                    }
                    completed(
                        picture.name(),
                        [
                            snapshot.operation.terminal.completed as f64,
                            snapshot.labels.len() as f64,
                            0.0,
                            0.0,
                        ],
                    );
                    return self.workflow_step(driver, Step::Pixels(picture, 0));
                }
                Task::none()
            }
            Step::Pause | Step::Resume if model.predict_pause_available() => {
                self.workflow_control(widgets, driver, "predict.pause")
            }
            Step::Paused
                if prediction.is_some_and(|value| value.paused && value.operation.active) =>
            {
                self.workflow_step(driver, Step::Resume)
            }
            Step::VideoEnd if prediction.is_some_and(|value| success(&value.operation)) => {
                let snapshot = prediction.unwrap();
                if snapshot.operation.terminal.completed <= self.video_index {
                    driver.fail("Video did not continue through EOF after resuming");
                    return Task::none();
                }
                completed(
                    "video",
                    [snapshot.operation.terminal.completed as f64, 0.0, 0.0, 0.0],
                );
                self.generation = snapshot.operation.generationfrontier;
                self.workflow_step(driver, Step::Pixels(Picture::Video, 0))
            }
            Step::Restarted
                if prediction.is_some_and(|value| {
                    value.operation.generationfrontier > self.generation
                        && value.operation.active
                        && value.operation.progress.completed > 0
                }) =>
            {
                self.workflow_step(driver, Step::Stop)
            }
            Step::Stop if model.compute_stop_available(FeatureId::Predict) => {
                self.workflow_control(widgets, driver, primary(FeatureId::Predict))
            }
            Step::Stopped
                if prediction.is_some_and(|value| {
                    !value.operation.active
                        && value.operation.terminal.outcome == ComputeOperationOutcome::Cancelled
                }) =>
            {
                completed(
                    "stop",
                    [
                        prediction.unwrap().operation.terminal.completed as f64,
                        0.0,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(driver, Step::Pixels(Picture::Stop, 0))
            }
            Step::Export => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Export),
            ),
            Step::PrepareExport
                if active == FeatureId::Export
                    && settled
                    && settings.draft.as_ref().is_some_and(|draft| {
                        model.model_selection_available(draft, FeatureId::Export)
                    }) =>
            {
                self.generation = model
                    .workflow
                    .export
                    .as_ref()
                    .map_or(0, |operation| operation.generationfrontier);
                driver.phase = Phase::Workflows(Step::StartExport);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Export(crate::view::export::Message::Model(
                        crate::view::workflow::model_card::Message::PrepareRequested,
                    )),
                ))
            }
            Step::StartExport
                if settings.draft.as_ref().is_some_and(|draft| {
                    model.compute_start_available(draft, FeatureId::Export)
                }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Export))
            }
            Step::Exporting
                if model.workflow.export.as_ref().is_some_and(|operation| {
                    operation.generationfrontier > self.generation && operation.active
                }) =>
            {
                if self.export_pixels {
                    self.workflow_step(driver, Step::StopExport)
                } else {
                    self.workflow_control(widgets, driver, primary(FeatureId::Export))
                }
            }
            Step::StopExport if model.compute_stop_available(FeatureId::Export) => {
                self.workflow_control(widgets, driver, primary(FeatureId::Export))
            }
            Step::ExportStopped
                if model.workflow.export.as_ref().is_some_and(|operation| {
                    !operation.active
                        && operation.terminal.outcome == ComputeOperationOutcome::Cancelled
                }) =>
            {
                completed(
                    if self.export_narrow {
                        "export_stop_narrow_dark"
                    } else {
                        "export_stop"
                    },
                    [
                        model.workflow.export.as_ref().unwrap().generationfrontier as f64,
                        0.0,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(
                    driver,
                    if self.export_narrow {
                        Step::ExportReturn
                    } else {
                        Step::Theme
                    },
                )
            }
            Step::Dark if active == FeatureId::Train && settled => {
                driver.phase = Phase::Workflows(Step::DarkReady);
                Task::done(RootMessage::Settings(settings::Message::DarkModeChanged(
                    true,
                )))
            }
            Step::DarkReady
                if settled
                    && settings
                        .draft
                        .as_ref()
                        .is_some_and(|value| value.ui.darkmode) =>
            {
                completed("theme", [1.0, 0.0, 0.0, 0.0]);
                self.workflow_step(driver, Step::Pixels(Picture::Theme, 0))
            }
            Step::Narrow => {
                match annotation_layout_scale(driver.input_scale, model.window_width as f32, true) {
                    Ok(scale) => {
                        self.narrow_scale = scale;
                        driver.phase = Phase::Workflows(Step::NarrowReady);
                        Task::done(RootMessage::Settings(settings::Message::UiScaleChanged(
                            scale,
                        )))
                        .chain(Task::done(RootMessage::Settings(
                            settings::Message::UiScaleReleased,
                        )))
                    }
                    Err(_) => {
                        driver.fail("Workflow cannot reach its minimum-width layout");
                        Task::none()
                    }
                }
            }
            Step::NarrowReady
                if settled && (driver.input_scale - self.narrow_scale).abs() < 0.001 =>
            {
                self.export_narrow = true;
                self.workflow_step(driver, Step::Export)
            }
            Step::Pixels(picture, index) => {
                if picture.chart() || picture == Picture::Progress {
                    self.pixel_source = 0;
                    self.pixel_presentation = 0;
                } else {
                    let Some(surface) = surface else {
                        return Task::none();
                    };
                    let drawn = if matches!(picture, Picture::Validation | Picture::Detail) {
                        crate::presentation_surface::drawable_validation(surface)
                            .filter(|(_, content)| {
                                // Native completion can precede the graphics handoff.
                                // Measure the view only when its paired image has
                                // the requested atlas/detail shape and overlay state.
                                content.metadata.detail == (picture == Picture::Detail)
                                    && (
                                        content.metadata.overlays.groundtruthlayer,
                                        content.metadata.overlays.predictionlayer,
                                    ) == layer_selection(self.validation_layer)
                                    && (picture != Picture::Detail
                                        || !content.metadata.overlays.predictionboxes)
                            })
                            .map(|(surface, _)| surface)
                    } else {
                        crate::presentation_surface::drawable_prediction(surface)
                            .map(|(surface, _)| surface)
                    };
                    let Some(frame) = drawn.and_then(|value| value.frame) else {
                        return Task::none();
                    };
                    self.pixel_source = frame.content_sequence;
                    self.pixel_presentation = frame.presentation_revision;
                }
                self.workflow_control(widgets, driver, picture.control(index))
            }
            _ => Task::none(),
        }
    }
}
