//! Real model workflows driven through the packaged Iced interface.
use crate::integration_control::widget_ops::click;
use crate::generated::FeatureId;
use crate::integration_control::{Driver, Phase, reporting, widget_ops};
use crate::integration_control::pixel_checks::ProbeOutcome;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{Message, probe::scenario_output};
use crate::integration_control::widget_ops::{AnnotationReveal, locate, reveal_control};
use crate::message::Message as RootMessage;
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::workflow_pixels_js;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::pixel_checks::pixel_result_callback;
use super::annotation_checks::annotation_layout_scale;
use crate::generated::{ComputeOperationOutcome, SourceKind};
use crate::view::{settings, workflow};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Picture {
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
    fn control(self, index: u8) -> String {
        match self {
            Self::Train | Self::Theme | Self::Narrow => "train.metrics.plot".into(),
            Self::Validation => format!("validate.sample.{index}"),
            Self::Detail => "validate.detail.image".into(),
            _ => "workflow.visual.workspace".into(),
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
    NoTrainAspect,
    Validate,
    StartValidate,
    Validating,
    NoValidationAspect,
    OpenSample,
    Sample,
    HideBoxes,
    HiddenBoxes,
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
    hidden_sequence: u64,
    generation: u64,
    video_index: u64,
    narrow_scale: f32,
    pixel_source: u64,
    pixel_presentation: u64,
    pixel_attempts: u8,
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
    use crate::integration_control::{Message, Phase};
    use crate::integration_control::pixel_checks::ProbeOutcome;

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
}

impl State {
    pub(super) fn workflow_step(&mut self, driver: &mut Driver, step: Step) -> Task<RootMessage> {
        driver.advance_to(Phase::Workflows(step))
    }
    pub(super) fn workflow_control(&mut self, widgets: &mut widget_ops::RevealState, driver: &mut Driver, control: impl Into<String>) -> Task<RootMessage> {
        if !widgets.begin_location() { return Task::none(); }
        let control = control.into();
        if matches!(
            driver.phase,
            Phase::Workflows(
                Step::Train
                    | Step::LeaveTrain
                    | Step::ReturnTrain
                    | Step::Validate
                    | Step::Predict
                    | Step::Theme
                    | Step::NoImageWorkspace
                    | Step::NoTrainAspect
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
    pub(super) fn workflow_pixels(&mut self, driver: &mut Driver, picture: Picture, index: u8, outcome: ProbeOutcome) {
        if driver.phase != Phase::Workflows(Step::AwaitPixels(picture, index)) {
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
                    Picture::Train => Step::NoImageWorkspace,
                    Picture::Validation if index < 5 => Step::Pixels(picture, index + 1),
                    Picture::Validation => Step::OpenSample,
                    Picture::Detail => Step::CloseSample,
                    Picture::Compiled => Step::Source(1),
                    Picture::Image => Step::Source(2),
                    Picture::Video => Step::Restart,
                    Picture::Stop => Step::Theme,
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
                "Workflow {picture:?} canvas did not contain its completed image or plot curves: {outcome:?}"
            )),
        }
    }
    pub(super) fn workflow_located(&mut self, driver: &mut Driver, control: &str, bounds: Rectangle) {
        let Phase::Workflows(step) = driver.phase else {
            return;
        };
        if matches!(
            step,
            Step::NoImageWorkspace | Step::NoTrainAspect | Step::NoValidationAspect
        ) {
            if bounds.width > 0.0 || bounds.height > 0.0 {
                driver.fail("Workflow still exposes a removed image workspace or aspect selector");
                return;
            }
            driver.phase = Phase::Workflows(match step {
                Step::NoImageWorkspace => Step::NoTrainAspect,
                Step::NoTrainAspect => Step::Validate,
                _ => Step::Pixels(Picture::Validation, 0),
            });
            return;
        }
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            driver.fail(&format!(
                "Workflow control {control} is missing from the rendered Iced tree in {step:?}"
            ));
            return;
        }
        driver.reporting
            .observe(|reporting| reporting.located(&driver.phase, control, bounds));
        let input = crate::presentation_surface::physical_bounds(bounds, driver.input_scale);
        if let Step::Pixels(picture, index) = step {
            driver.phase = Phase::Workflows(Step::AwaitPixels(picture, index));
            #[cfg(not(target_arch = "wasm32"))]
            let _ = (input, picture.chart());
            #[cfg(target_arch = "wasm32")]
            {
                let output =
                    scenario_output();
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
                    self.pixel_source as f64,
                    self.pixel_presentation as f64,
                    &callback,
                );
            }
            return;
        }
        driver.phase = Phase::Workflows(match step {
            Step::Train => Step::StartTrain,
            Step::StartTrain => Step::Training,
            Step::LeaveTrain => Step::HiddenTrain,
            Step::ReturnTrain => Step::Trained,
            Step::Validate => Step::StartValidate,
            Step::StartValidate => Step::Validating,
            Step::OpenSample => Step::Sample,
            Step::HideBoxes => Step::HiddenBoxes,
            Step::CloseSample => Step::ClosedSample,
            Step::Predict => Step::Source(0),
            Step::Source(index) => Step::SourceReady(index),
            Step::StartPredict(index) => Step::Predicting(index),
            Step::Pause => Step::Paused,
            Step::Resume => Step::VideoEnd,
            Step::Restart => Step::Restarted,
            Step::Stop => Step::Stopped,
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
        &mut self, widgets: &mut widget_ops::RevealState, driver: &mut Driver,
        step: Step,
        model: &ApplicationModel,
        settings: &settings::SettingsModel,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
    ) -> Task<RootMessage> {
        let settled = !settings.has_local_edits();
        let train = model.workflow.training.as_ref();
        let validation = model.workflow.validation.as_ref();
        let prediction = model.predict_snapshot.as_ref();
        let record = train.and_then(|value| value.metrics.as_ref());
        let success = |value: &crate::generated::ComputeUiState| {
            !value.active && value.terminal.outcome == ComputeOperationOutcome::Succeeded
        };
        match step {
            Step::Train | Step::ReturnTrain | Step::Theme => {
                self.workflow_control(widgets, driver, crate::view::navigation::stable_id(FeatureId::Train))
            }
            Step::StartTrain if active == FeatureId::Train && settled => {
                self.workflow_control(widgets, driver, primary(FeatureId::Train))
            }
            Step::Training
                if record.is_some_and(|value| value.progress.globaloptimizerstep > 0) =>
            {
                self.hidden_sequence = record.unwrap().sequence;
                self.workflow_step(driver, Step::LeaveTrain)
            }
            Step::LeaveTrain | Step::Validate => {
                self.workflow_control(widgets, driver, crate::view::navigation::stable_id(FeatureId::Validate))
            }
            Step::HiddenTrain
                if active == FeatureId::Validate
                    && record
                        .is_some_and(|value| value.sequence > self.hidden_sequence) =>
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
            Step::NoImageWorkspace => widgets.arm(driver, "workflow.visual.workspace"),
            Step::NoTrainAspect | Step::NoValidationAspect => widgets.arm(driver, "workflow.workspace.aspect"),
            Step::StartValidate if active == FeatureId::Validate && settled => {
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
            Step::OpenSample => self.workflow_control(widgets, driver, "validate.sample.0"),
            Step::Sample if validation.is_some_and(|value| value.detail) => {
                self.workflow_step(driver, Step::HideBoxes)
            }
            Step::HideBoxes => self.workflow_control(widgets, driver, "validate.pred.boxes"),
            Step::HiddenBoxes
                if validation
                    .is_some_and(|value| value.detail && !value.overlays.predictionboxes) =>
            {
                self.workflow_step(driver, Step::Pixels(Picture::Detail, 0))
            }
            Step::CloseSample => self.workflow_control(widgets, driver, "validate.detail.close"),
            Step::ClosedSample if validation.is_some_and(|value| !value.detail) => {
                self.workflow_step(driver, Step::Predict)
            }
            Step::Predict => {
                self.workflow_control(widgets, driver, crate::view::navigation::stable_id(FeatureId::Predict))
            }
            Step::Source(index) if active == FeatureId::Predict && settled => self.workflow_control(widgets, driver,
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
                driver.generation =
                    prediction.map_or(0, |value| value.operation.generationfrontier);
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
                if prediction.is_some_and(|value| {
                    value.operation.generationfrontier > driver.generation
                }) =>
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
                driver.generation = snapshot.operation.generationfrontier;
                self.workflow_step(driver, Step::Pixels(Picture::Video, 0))
            }
            Step::Restarted
                if prediction.is_some_and(|value| {
                    value.operation.generationfrontier > driver.generation
                        && value.operation.active
                        && value.operation.progress.completed > 0
                }) =>
            {
                self.workflow_step(driver, Step::Stop)
            }
            Step::Stop if model.compute_stop_available(FeatureId::Predict) => {
                self.workflow_control(widgets, driver, "predict.stop")
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
                self.workflow_step(driver, Step::Pixels(Picture::Narrow, 0))
            }
            Step::Pixels(picture, index) => {
                if picture.chart() {
                    self.pixel_source = 0;
                    self.pixel_presentation = 0;
                } else {
                    let Some(surface) = surface else {
                        return Task::none();
                    };
                    let drawn = if matches!(picture, Picture::Validation | Picture::Detail) {
                        crate::presentation_surface::drawable_validation(surface)
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
