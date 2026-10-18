//! Rendered scratch initialization and source-independent saved-session Resume.
use super::{Step, completed, control, primary};
use crate::generated::*;
use crate::integration_control::pixel_checks::ProbeOutcome;
use crate::integration_control::{Driver, Phase, reporting, widget_ops};
use crate::message::Message as RootMessage;
use crate::view::settings::SettingsModel;
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Stage {
    Page,
    Pixels(u8, u8),
    AwaitPixels(u8, u8),
    Toggle(u8),
    Mode(u8),
    Prepare,
    Prepared,
    Start,
    Training,
    Stop,
    Stopped,
    AwaitMutation,
    SourceRemoved,
    Browse,
    Inspect,
    Resume,
    StartResume,
    Resuming,
}
#[derive(Default)]
pub(in crate::integration_control) struct State {
    checkpoint: String,
    generation: u64,
    pixels: u16,
}
fn phase(driver: &mut Driver, stage: Stage) -> Task<RootMessage> {
    driver.advance_to(Phase::Workflows(Step::Scratch(stage)))
}
fn pixel_control(index: u8) -> &'static str {
    match index {
        0 => crate::view::train::FROM_SCRATCH_ID,
        1 => crate::view::workflow::model_card::TRAIN_CUSTOM_ID,
        _ => crate::view::train::RESUME_ID,
    }
}
impl State {
    // CLEANUP-OFF: Only method-signature boilerplate matches; the independent workflow state machines have distinct parameters and behavior.
    pub(super) fn advance(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        stage: Stage,
        model: &ApplicationModel,
        settings: &SettingsModel,
        active: FeatureId,
    ) -> Task<RootMessage> {
        // CLEANUP-ON
        let Some(draft) = settings.draft.as_ref() else {
            return Task::none();
        };
        let train = &draft.workflows.train;
        let settled = !settings.has_local_edits();
        match stage {
            Stage::Page => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Train),
            ),
            Stage::Pixels(_, index) if active == FeatureId::Train && settled => {
                control(widgets, driver, pixel_control(index))
            }
            Stage::Toggle(_) if settled && model.settings_edit_available() => {
                control(widgets, driver, crate::view::train::FROM_SCRATCH_ID)
            }
            Stage::Mode(index) if settled && train.request.fromscratch == (index == 1) => {
                if train.modelsource != ModelSelectionSource::Canonical
                    || !train.request.weightspath.is_empty()
                    || !train.request.resumepath.is_empty()
                    || !train.request.classlayoutpath.is_empty()
                    || model.workflow.train_continuation.mode
                        != crate::view_model::ContinuationMode::Transfer
                {
                    driver.fail(
                        "Scratch checkbox retained a conflicting detector or Resume selection",
                    );
                    return Task::none();
                }
                phase(driver, Stage::Pixels(index, 0))
            }
            Stage::Prepare
                if settled && model.model_selection_available(draft, FeatureId::Train) =>
            {
                control(
                    widgets,
                    driver,
                    crate::view::workflow::model_card::TRAIN_ACTION_ID,
                )
            }
            Stage::Prepared
                if settled && model.model_selection_matches(draft, FeatureId::Train) =>
            {
                let selection = &model.model_snapshot.as_ref().unwrap().selection;
                let Some(lineage) = selection.encoderpretraining.as_ref() else {
                    driver.fail("Scratch preparation lost canonical encoder provenance");
                    return Task::none();
                };
                if !selection.key.fromscratch
                    || selection.classlayout.outputcount != 0
                    || selection.trainingconfig.as_ref().is_none_or(|config| {
                        config.numclasses != 0 || !config.keypointsperclass.is_empty()
                    })
                {
                    driver.fail("Base encoder was prepared as a pretrained detector");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.scratch.prepared",
                        &selection.artifact,
                        &lineage.sha256,
                        [
                            model.model_snapshot.as_ref().unwrap().generation as f64,
                            1.0,
                            selection.classlayout.outputcount as f64,
                            0.0,
                        ],
                    )
                });
                completed("scratch-prepared", [0.0; 4]);
                phase(driver, Stage::Start)
            }
            Stage::Start | Stage::StartResume
                if settled && model.compute_start_available(draft, FeatureId::Train) =>
            {
                control(widgets, driver, primary(FeatureId::Train))
            }
            Stage::Training => {
                let Some(snapshot) = model.workflow.training.as_ref() else {
                    return Task::none();
                };
                if let Some(record) = snapshot.metrics.as_ref()
                    && !record.progress.fullcheckpointpath.is_empty()
                    && record.progress.globaloptimizerstep > 0
                {
                    if !snapshot.local.active {
                        driver.fail("Scratch fixture finished before the bounded Stop checkpoint");
                        return Task::none();
                    }
                    self.checkpoint = record.progress.fullcheckpointpath.clone();
                    self.generation = snapshot.local.generationfrontier;
                    return phase(driver, Stage::Stop);
                }
                Task::none()
            }
            Stage::Stop if model.training_stop_available() => {
                control(widgets, driver, primary(FeatureId::Train))
            }
            Stage::Stopped
                if model.workflow.training.as_ref().is_some_and(|snapshot| {
                    !snapshot.local.active
                        && snapshot.local.generationfrontier == self.generation
                        && snapshot.local.terminal.outcome == ComputeOperationOutcome::Cancelled
                }) =>
            {
                let snapshot = model.workflow.training.as_ref().unwrap();
                reporting::emit(|sink| {
                    sink.record(
                        "integration.scratch.fresh-session",
                        &self.checkpoint,
                        &snapshot.local.output.directory,
                        [self.generation as f64, 1.0, 0.0, 0.0],
                    )
                });
                completed("scratch-stopped", [self.generation as f64, 0.0, 0.0, 0.0]);
                phase(driver, Stage::AwaitMutation)
            }
            Stage::SourceRemoved => phase(driver, Stage::Toggle(2)),
            Stage::Browse if settled && model.settings_edit_available() => control(
                widgets,
                driver,
                crate::view::workflow::model_card::TRAIN_CUSTOM_ID,
            ),
            Stage::Inspect
                if settled
                    && model.workflow.train_continuation.matches(train)
                    && model
                        .workflow
                        .train_continuation
                        .checkpoint()
                        .is_some_and(|checkpoint| checkpoint.resumable) =>
            {
                let checkpoint = model.workflow.train_continuation.checkpoint().unwrap();
                if train.request.fromscratch
                    || checkpoint.path != self.checkpoint
                    || checkpoint.encoderpretraining.is_none()
                {
                    driver.fail("Scratch-origin Resume lost its saved source or lineage");
                    return Task::none();
                }
                phase(driver, Stage::Pixels(3, 0))
            }
            Stage::Resume => control(widgets, driver, crate::view::train::RESUME_ID),
            Stage::Resuming
                if model.workflow.training.as_ref().is_some_and(|snapshot| {
                    !snapshot.local.active
                        && snapshot.local.generationfrontier > self.generation
                        && snapshot.local.terminal.outcome == ComputeOperationOutcome::Succeeded
                }) =>
            {
                let snapshot = model.workflow.training.as_ref().unwrap();
                let Some(record) = snapshot.metrics.as_ref() else {
                    driver.fail("Resume completed without session evidence");
                    return Task::none();
                };
                if train.request.fromscratch
                    || train.request.resumepath != self.checkpoint
                    || record.progress.fullcheckpointpath.is_empty()
                    || self.pixels != 0x0fff
                {
                    driver.fail("Resume lost restored inputs or rendered control evidence");
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.scratch.resumed-session",
                        &record.progress.fullcheckpointpath,
                        &snapshot.local.output.directory,
                        [
                            snapshot.local.generationfrontier as f64,
                            record.progress.globaloptimizerstep as f64,
                            0.0,
                            0.0,
                        ],
                    )
                });
                completed(
                    "scratch-resumed",
                    [snapshot.local.generationfrontier as f64, 0.0, 0.0, 0.0],
                );
                driver.advance_to(Phase::Complete)
            }
            _ => Task::none(),
        }
    }
    pub(super) fn located(
        &mut self,
        driver: &mut Driver,
        stage: Stage,
        _control: &str,
        bounds: Rectangle,
    ) {
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            driver.fail("Required scratch workflow control is absent");
            return;
        }
        let mut input = crate::presentation_surface::physical_bounds(bounds, driver.input_scale);
        if let Stage::Pixels(stage, index) = stage {
            driver.phase = Phase::Workflows(Step::Scratch(Stage::AwaitPixels(stage, index)));
            #[cfg(target_arch = "wasm32")]
            {
                let Some(mut output) = crate::integration_control::probe::scenario_output() else {
                    driver.fail("Scratch canvas observer is unavailable");
                    return;
                };
                output.receipt = None;
                output.probe = None;
                let callback = crate::integration_control::pixel_checks::pixel_result_callback(
                    move |outcome| {
                        output.send(crate::integration_control::Message::ScratchPixels {
                            stage,
                            index,
                            outcome,
                        });
                    },
                );
                crate::integration_control::scratch_pixels_js(
                    _control,
                    &[
                        input.x as f64,
                        input.y as f64,
                        input.width as f64,
                        input.height as f64,
                    ],
                    stage,
                    index,
                    &callback,
                );
            }
            return;
        }
        let next = match stage {
            Stage::Page => Stage::Pixels(0, 0),
            Stage::Toggle(index) => {
                input.width = input.height;
                Stage::Mode(index)
            }
            Stage::Prepare => Stage::Prepared,
            Stage::Start => Stage::Training,
            Stage::Stop => Stage::Stopped,
            Stage::Browse => Stage::Inspect,
            Stage::Resume => {
                input.width = input.height;
                Stage::StartResume
            }
            Stage::StartResume => Stage::Resuming,
            _ => {
                driver.fail("Unexpected scratch workflow input");
                return;
            }
        };
        driver.phase = Phase::Workflows(Step::Scratch(next));
        if !widget_ops::click(input) {
            driver.fail("Scratch workflow pointer dispatch failed");
        }
    }
    pub(in crate::integration_control) fn pixels(
        &mut self,
        driver: &mut Driver,
        stage: u8,
        index: u8,
        outcome: ProbeOutcome,
    ) {
        if driver.phase != Phase::Workflows(Step::Scratch(Stage::AwaitPixels(stage, index))) {
            return;
        }
        if !matches!(outcome, ProbeOutcome::Observed(sampled, visible) if sampled > 0 && visible > 0)
        {
            driver.fail("Scratch controls did not produce the required checkbox and disabled-control pixels");
            return;
        }
        self.pixels |= 1 << (stage * 3 + index);
        driver.phase = Phase::Workflows(Step::Scratch(if index < 2 {
            Stage::Pixels(stage, index + 1)
        } else {
            match stage {
                0 => Stage::Toggle(1),
                1 => Stage::Prepare,
                2 => Stage::Browse,
                _ => Stage::Resume,
            }
        }));
    }
}
