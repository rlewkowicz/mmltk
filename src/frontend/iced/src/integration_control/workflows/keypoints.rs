//! Keypoint products are driven by real pointer/key input and checked against
//! the immutable native metadata paired with completed canvas presentations.
#[cfg(target_arch = "wasm32")]
use super::Picture;
use super::{Step, completed, control};
use crate::generated::*;
use crate::integration_control::pixel_checks::ProbeOutcome;
use crate::integration_control::{Driver, Phase, probe, widget_ops};
use crate::message::Message as RootMessage;
use crate::presentation_surface::{self as surface, Surface};
use crate::view::settings::SettingsModel;
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Stage {
    SelectPage,
    Overwrite,
    OverwriteReady,
    Compile,
    Compiling,
    ExplorePage,
    Open,
    Gallery,
    Detail,
    Import(u8),
    Imported(u8),
    SelectObject(u8),
    SelectedObject(u8),
    SelectJoint(u8),
    SelectedJoint(u8),
    Occlude(u8),
    Occluded(u8),
    Undo(u8),
    Undone(u8),
    Redo(u8),
    Redone(u8),
    Connect(u8),
    Connected(u8),
    Move(u8),
    MoveReady(u8),
    Moved(u8),
    Save(u8),
    Saved(u8),
    Reload(u8),
    Reloaded(u8),
    TrainPage,
    PrepareTrain,
    TrainPrepared,
    StartTrain,
    Training,
    ValidatePage,
    PrepareValidate,
    ValidatePrepared,
    StartValidate,
    Validating,
    ValidateOks,
    ValidatePixels,
    ExportPage,
    PrepareExport,
    ExportPrepared,
    StartExport,
    Exporting,
    PredictPage,
    PreparePredict,
    PredictPrepared,
    StartPredict,
    Predicting,
    UpscalePredict,
    UpscaledPredict,
    LiveResize,
    LiveResizeReady,
    LivePage(u8),
    StartLive(u8),
    LiveFrame(u8),
    StopLive(u8),
    StoppedLive(u8),
    EnableAnalysis,
    AnalysisEnabled,
    ThresholdPage(bool),
    ThresholdEdit(bool),
    ThresholdReady(bool),
    Mutate(u32),
    AwaitMutation(u32),
    MutationReady(u32),
    FailedStart,
    Failed,
    ModelPage,
    SelectPe,
    PeSelected,
    PeSource,
    PeSourceReady,
    PreparePe,
    PePrepared,
    StartPe,
    PeRunning,
    PePixels,
    NextCatalog,
    PosePage,
    SelectPose,
    PoseSelected,
    CompilePose,
    PoseCompiled,
    PreparePose,
    PosePrepared,
    PoseSettings(u8),
    PoseSettingsReady(u8),
    StartPose,
    PoseTraining,
    LiveDerivedPage,
    UpscaleLive,
    UpscaledLive,
    ClearImported,
    ClearedImported,
    CleanImportReveal,
    CleanImportReady,
    RestoreImported,
    RestoredImported,
    IncompatiblePage,
    SelectIncompatible,
    IncompatibleSelected,
    RefuseStart,
    Refused,
    Pixels(u8),
    AwaitPixels(u8),
    Done,
}
#[derive(Default)]
pub(super) struct State {
    next_pixel: Option<Stage>,
    receipt: Option<probe::ProbeReceipt>,
    patches: Vec<f64>,
    pixel_name: String,
    pixel_stage: Option<Stage>,
    pixel_attempts: u8,
    empty: bool,
    retained_frame: Option<VisualFrame>,
    retained_document: Option<VisualDocumentFacts>,
    annotation: Option<AnnotationSceneContent>,
    annotation_epoch: u64,
    annotation_revision: u64,
    save_generation: u64,
    connection_before: bool,
    joint_before: Option<AnnotationPoint>,
    oks_bounds: Option<Rectangle>,
    live_revision: u64,
    live_completed_frames: u64,
    model_row: usize,
    catalog_prediction_index: usize,
    gallery_tile: Option<Rectangle>,
    analyzed_joint: Option<(f32, f32)>,
    catalog_selection: Option<ModelSelection>,
    model_generation: u64,
    operation_generation: u64,
    dataset_generation: u64,
    overwrite_revision: u64,
    catalog_frame: Option<VisualFrame>,
}
fn success(value: &ComputeUiState) -> bool {
    !value.active && value.terminal.outcome == ComputeOperationOutcome::Succeeded
}
fn annotation_current(metadata: &AnnotationImageMetadata, snapshot: &AnnotationSnapshot) -> bool {
    metadata.frame == snapshot.frame
        && metadata.diagnostics.as_ref().is_some_and(|rendered| {
            rendered.documentepoch == snapshot.inputdocumentepoch
                && rendered.scenerevision == snapshot.ui.scenerevision
        })
}
fn phase(driver: &mut Driver, stage: Stage) -> Task<RootMessage> {
    driver.advance_to(Phase::Workflows(Step::Keypoints(stage)))
}
fn same_scene(left: &AnnotationSceneContent, right: &AnnotationSceneContent) -> bool {
    let mut expected = right.clone();
    expected.document.revision = left.document.revision;
    *left == expected
}
fn schema_scene(scene: &AnnotationSceneContent) -> bool {
    scene
        .keypointschemas
        .iter()
        .any(|schema| schema.names.len() == 17)
        && scene
            .objects
            .iter()
            .any(|object| object.skeletonnodes.len() == 17 && object.skeletonedges.len() > 8)
}
fn catalog_prediction_rows() -> impl Iterator<Item = usize> {
    // Preserve the existing zero-decoder PE pixel case first, then exercise
    // every other detector/segmenter. The keypoint row owns real training below.
    let first = RFDETR_PRESET_CATALOG
        .iter()
        .position(|row| row.backbone.kind == BackboneKind::PeCoreTiny && row.decoderlayercount == 0)
        .unwrap();
    std::iter::once(first).chain(RFDETR_PRESET_CATALOG.iter().enumerate().filter_map(
        move |(index, row)| (index != first && row.task != ModelTask::Keypoints).then_some(index),
    ))
}
impl State {
    pub(super) fn advance(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        stage: Stage,
        model: &ApplicationModel,
        settings: &SettingsModel,
        active: FeatureId,
        requested: Option<Surface>,
    ) -> Task<RootMessage> {
        let settled = !settings.has_local_edits();
        let primary = |page| {
            crate::view::workflow::Composition::new(page, 0.0)
                .stable_id(crate::view::workflow::Region::PrimaryAction)
        };
        let can_prepare = |page| {
            settled
                && settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| model.model_selection_available(draft, page))
        };
        let prepared = |page| {
            settings
                .draft
                .as_ref()
                .is_some_and(|draft| model.model_selection_matches(draft, page))
        };
        let annotation = model
            .annotation
            .snapshot
            .as_ref()
            .filter(|snapshot| snapshot.ready && !snapshot.busy);
        let live = model.live_snapshot.as_ref();
        let overwrite = settled
            && settings
                .draft
                .as_ref()
                .is_some_and(|value| value.workflows.train.overwritecompileddataset)
            && model
                .settings_snapshot
                .as_ref()
                .is_some_and(|value| value.settingsstate.workflows.train.overwritecompileddataset);
        match stage {
            Stage::SelectPage | Stage::TrainPage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Train),
            ),
            Stage::Overwrite if active == FeatureId::Train && settled => {
                let Some(snapshot) = model.settings_snapshot.as_ref() else {
                    return Task::none();
                };
                if snapshot
                    .settingsstate
                    .workflows
                    .train
                    .overwritecompileddataset
                {
                    driver.fail("Keypoint fixture overwrite must start disabled");
                    return Task::none();
                }
                self.overwrite_revision = snapshot.revision;
                control(widgets, driver, "train.dataset.overwrite")
            }
            Stage::OverwriteReady
                if overwrite
                    && crate::integration_control::settled_settings_snapshot(
                        model,
                        settings,
                        self.overwrite_revision,
                    )
                    .is_some() =>
            {
                phase(driver, Stage::Compile)
            }
            Stage::Compile | Stage::CompilePose if active == FeatureId::Train && overwrite => {
                let train = &settings.draft.as_ref().unwrap().workflows.train;
                let resolution = if stage == Stage::Compile {
                    64
                } else {
                    RFDETR_PRESET_CATALOG[self.model_row].resolution as i32
                };
                if train.request.resolution != resolution
                    || (stage == Stage::CompilePose
                        && (train.modelsource != ModelSelectionSource::Canonical
                            || train.request.presetname
                                != RFDETR_PRESET_CATALOG[self.model_row].presetname.as_ref()))
                {
                    driver.fail("Keypoint Compile settings differ from the required fixture or selected catalog model");
                    return Task::none();
                }
                let Some(dataset) = model
                    .workflow
                    .dataset
                    .as_ref()
                    .filter(|value| !value.active)
                else {
                    return Task::none();
                };
                self.dataset_generation = dataset.generation;
                control(widgets, driver, crate::view::train::COMPILE_DATASET_ID)
            }
            Stage::Compiling | Stage::PoseCompiled => {
                let Some(dataset) =
                    model.workflow.dataset.as_ref().filter(|value| {
                        value.generation > self.dataset_generation && !value.active
                    })
                else {
                    return Task::none();
                };
                match dataset.terminal.outcome {
                    ArtifactTerminalOutcome::Succeeded if stage == Stage::Compiling => {
                        completed(
                            "keypoint-compile",
                            [
                                self.dataset_generation as f64,
                                dataset.generation as f64,
                                64.0,
                                0.0,
                            ],
                        );
                        phase(driver, Stage::ExplorePage)
                    }
                    ArtifactTerminalOutcome::Succeeded => phase(driver, Stage::PreparePose),
                    ArtifactTerminalOutcome::Failed
                    | ArtifactTerminalOutcome::Cancelled
                    | ArtifactTerminalOutcome::Refused => {
                        driver.fail(&format!(
                            "Keypoint Compile {:?}: {}",
                            dataset.terminal.outcome, dataset.terminal.detail
                        ));
                        Task::none()
                    }
                    ArtifactTerminalOutcome::Idle
                    | ArtifactTerminalOutcome::CancellationRequested => Task::none(),
                }
            }
            Stage::ExplorePage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Explore),
            ),
            Stage::Open
                if active == FeatureId::Explore && settled && model.explore_open_available() =>
            {
                control(widgets, driver, crate::view::explore::OPEN_ID)
            }
            Stage::Gallery => {
                let Some((display, content)) = surface::gallery::displayed() else {
                    return Task::none();
                };
                let Some(receipt) =
                    probe::current_receipt(crate::view::explore::GALLERY_WORKSPACE_ID)
                else {
                    return Task::none();
                };
                if receipt.surface.frame != display.frame {
                    return Task::none();
                }
                let draw = crate::integration_control::pixel_checks::AtlasDraw {
                    surface: display,
                    snapshot: content.metadata.clone(),
                    bounds: receipt.bounds,
                    image: receipt.image,
                    clip: receipt.clip,
                };
                let Some((_, tile)) = draw.ready_tiles().find(|(index, _)| *index == 1) else {
                    return Task::none();
                };
                self.gallery_tile = Some(tile);
                control(widgets, driver, crate::view::explore::GALLERY_WORKSPACE_ID)
            }
            Stage::Detail => {
                let Some(surface::ExploreDisplay::Detail(display, detail)) =
                    surface::explore_display(requested)
                else {
                    return Task::none();
                };
                if !schema_scene(detail.scene()) {
                    driver.fail("Explore lost complete class skeleton schema");
                    return Task::none();
                }
                self.scene_pixels(
                    driver,
                    display,
                    detail.scene(),
                    crate::view::explore::DETAIL_WORKSPACE_ID,
                    "keypoint-explore",
                    Stage::Import(0),
                )
            }
            Stage::Import(index) => {
                if surface::viewer_annotation_request().is_none() {
                    return Task::none();
                }
                self.annotation_epoch = annotation.map_or(0, |value| value.inputdocumentepoch);
                control(
                    widgets,
                    driver,
                    match index {
                        0 => crate::view::explore::DETAIL_ANNOTATE_ID,
                        1 => "predict.annotate",
                        _ => "live.annotate",
                    },
                )
            }
            Stage::Imported(index)
                if active == FeatureId::Annotate
                    && settled
                    && !model.has_pending(ApplicationIntentEndpoint::AnnotationOpen) =>
            {
                let Some(snapshot) =
                    annotation.filter(|value| value.inputdocumentepoch > self.annotation_epoch)
                else {
                    return Task::none();
                };
                if !schema_scene(&snapshot.ui.scene) {
                    driver.fail("Annotation import truncated skeleton or lost catalog schema");
                    return Task::none();
                }
                self.annotation_epoch = snapshot.inputdocumentepoch;
                self.annotation = Some(snapshot.ui.scene.clone());
                phase(
                    driver,
                    if index == 3 {
                        Stage::ClearImported
                    } else {
                        Stage::SelectObject(index)
                    },
                )
            }
            Stage::SelectObject(_) => control(widgets, driver, "annotation.object.0"),
            Stage::SelectedObject(index)
                if annotation.is_some_and(|value| value.ui.editor.selectedobject == Some(0)) =>
            {
                phase(driver, Stage::SelectJoint(index))
            }
            Stage::SelectJoint(_) => control(widgets, driver, "annotation.joint.0"),
            Stage::SelectedJoint(index)
                if annotation
                    .is_some_and(|value| value.ui.editor.selectedskeletonjoint == Some(0)) =>
            {
                phase(driver, Stage::Occlude(index))
            }
            Stage::Occlude(_) => control(widgets, driver, "annotation.command.SkeletonOccluded"),
            Stage::Occluded(index)
                if annotation.is_some_and(|value| {
                    value.ui.scene.objects[0].skeletonnodes[0].visibility
                        == KeypointVisibility::Occluded
                }) =>
            {
                phase(driver, Stage::Undo(index))
            }
            Stage::Undo(_) => control(widgets, driver, "annotation.undo"),
            Stage::Undone(index)
                if annotation.is_some_and(|value| {
                    self.annotation
                        .as_ref()
                        .is_some_and(|before| same_scene(&value.ui.scene, before))
                }) =>
            {
                phase(driver, Stage::Redo(index))
            }
            Stage::Redo(_) => control(widgets, driver, "annotation.redo"),
            Stage::Redone(index)
                if annotation.is_some_and(|value| {
                    value.ui.scene.objects[0].skeletonnodes[0].visibility
                        == KeypointVisibility::Occluded
                }) =>
            {
                phase(driver, Stage::Connect(index))
            }
            Stage::Connect(_) => {
                let Some(snapshot) = annotation else {
                    return Task::none();
                };
                self.connection_before =
                    snapshot.ui.scene.objects[0]
                        .skeletonedges
                        .iter()
                        .any(|edge| {
                            (edge.source == 0 && edge.target == 1)
                                || (edge.source == 1 && edge.target == 0)
                        });
                control(widgets, driver, "annotation.connection.0.1")
            }
            Stage::Connected(index)
                if annotation.is_some_and(|snapshot| {
                    snapshot.ui.scene.objects[0]
                        .skeletonedges
                        .iter()
                        .any(|edge| {
                            (edge.source == 0 && edge.target == 1)
                                || (edge.source == 1 && edge.target == 0)
                        })
                        != self.connection_before
                }) =>
            {
                phase(driver, Stage::Move(index))
            }
            Stage::Move(_) => control(widgets, driver, crate::view::workspace::STABLE_ID),
            Stage::MoveReady(_) => {
                let Some(snapshot) = annotation else {
                    return Task::none();
                };
                let Some((display, content)) = requested.and_then(surface::drawable_annotation)
                else {
                    return Task::none();
                };
                if !annotation_current(&content.metadata, snapshot) {
                    return Task::none();
                }
                let Some(receipt) = probe::current_receipt(crate::view::workspace::STABLE_ID)
                    .filter(|receipt| {
                        receipt.surface.frame == display.frame
                            // GPU scissor bounds round fractional image edges.
                            && widget_ops::contains_rectangle(receipt.clip.expand(0.5), receipt.image)
                    })
                else {
                    return Task::none();
                };
                self.receipt = Some(receipt);
                self.joint_before =
                    Some(snapshot.ui.scene.objects[0].skeletonnodes[0].point.clone());
                self.annotation = Some(snapshot.ui.scene.clone());
                control(widgets, driver, crate::view::workspace::STABLE_ID)
            }
            Stage::Moved(index)
                if annotation.is_some_and(|snapshot| {
                    snapshot.ui.scene.objects[0].skeletonnodes[0].point
                        != *self.joint_before.as_ref().unwrap()
                }) =>
            {
                let current = &annotation.unwrap().ui.scene;
                let before = self.annotation.as_ref().unwrap();
                if current.keypointschemas != before.keypointschemas
                    || current.objects[0].skeletonedges != before.objects[0].skeletonedges
                    || current.objects[0]
                        .skeletonnodes
                        .iter()
                        .zip(&before.objects[0].skeletonnodes)
                        .any(|(a, b)| a.key != b.key || a.visibility != b.visibility)
                {
                    driver.fail("Joint move changed schema, visibility or connections");
                    return Task::none();
                }
                phase(driver, Stage::Save(index))
            }
            Stage::Save(_) if settled && model.annotation_save_available() => {
                self.save_generation = annotation.map_or(0, |value| value.ui.savegeneration);
                control(widgets, driver, primary(FeatureId::Annotate))
            }
            Stage::Saved(index)
                if annotation.is_some_and(|value| {
                    value.ui.savestatus == AnnotationSaveStatus::Saved
                        && value.ui.savegeneration > self.save_generation
                }) && !model.annotation.save_active() =>
            {
                self.annotation = annotation.map(|value| value.ui.scene.clone());
                self.annotation_revision = annotation.unwrap().uirevision;
                phase(driver, Stage::Reload(index))
            }
            Stage::Reload(_) if settled && model.annotation_save_available() => {
                control(widgets, driver, "annotation.load")
            }
            Stage::Reloaded(index)
                if annotation.is_some_and(|value| {
                    value.inputdocumentepoch > self.annotation_epoch
                        && value.uirevision > self.annotation_revision
                }) =>
            {
                if !annotation
                    .zip(self.annotation.as_ref())
                    .is_some_and(|(value, before)| same_scene(&value.ui.scene, before))
                {
                    driver.fail("Saved skeleton reload changed schema, node identity, connections or visibility");
                    return Task::none();
                }
                completed(
                    match index {
                        0 => "keypoint-explore-edit-save",
                        1 => "keypoint-predict-edit-save",
                        2 => "keypoint-live-edit-save",
                        _ => "keypoint-live-upscale-edit-save",
                    },
                    [
                        self.annotation_epoch as f64,
                        annotation.unwrap().inputdocumentepoch as f64,
                        17.0,
                        0.0,
                    ],
                );
                phase(
                    driver,
                    match index {
                        0 => Stage::TrainPage,
                        1 => Stage::LiveResize,
                        2 => Stage::LiveDerivedPage,
                        _ => Stage::ThresholdPage(true),
                    },
                )
            }
            Stage::PrepareTrain if active == FeatureId::Train && can_prepare(FeatureId::Train) => {
                control(widgets, driver, "train.model.action")
            }
            Stage::TrainPrepared if prepared(FeatureId::Train) => phase(driver, Stage::StartTrain),
            Stage::StartTrain => control(widgets, driver, primary(FeatureId::Train)),
            Stage::Training
                if model
                    .workflow
                    .training
                    .as_ref()
                    .is_some_and(|value| success(&value.local)) =>
            {
                let Some(record) = model
                    .workflow
                    .training
                    .as_ref()
                    .and_then(|value| value.metrics.as_ref())
                else {
                    driver.fail("Keypoint training has no metrics");
                    return Task::none();
                };
                let progress = &record.progress;
                if progress.globaloptimizerstep == 0
                    || !progress
                        .val
                        .as_ref()
                        .and_then(|value| value.keypoint.as_ref())
                        .is_some_and(|value| value.available)
                    || [
                        progress.scalars.keypointl1,
                        progress.scalars.keypointfindable,
                        progress.scalars.keypointvisible,
                        progress.scalars.keypointnll,
                    ]
                    .iter()
                    .any(Option::is_none)
                {
                    driver.fail("Bounded Keypoint training lost task losses or OKS");
                    return Task::none();
                }
                completed(
                    "keypoint-train-oks",
                    [
                        record.sequence as f64,
                        progress.globaloptimizerstep as f64,
                        progress.val.as_ref().unwrap().keypoint.as_ref().unwrap().ap,
                        1.0,
                    ],
                );
                phase(driver, Stage::ValidatePage)
            }
            Stage::ValidatePage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Validate),
            ),
            Stage::PrepareValidate
                if active == FeatureId::Validate && can_prepare(FeatureId::Validate) =>
            {
                control(widgets, driver, "validate.model.action")
            }
            Stage::ValidatePrepared if prepared(FeatureId::Validate) => {
                phase(driver, Stage::StartValidate)
            }
            Stage::StartValidate => control(widgets, driver, primary(FeatureId::Validate)),
            Stage::Validating
                if model
                    .workflow
                    .validation
                    .as_ref()
                    .is_some_and(|value| success(&value.operation)) =>
            {
                let Some(metrics) = model
                    .workflow
                    .validation
                    .as_ref()
                    .and_then(|value| value.metrics.as_ref())
                else {
                    return Task::none();
                };
                if !metrics
                    .keypoint
                    .as_ref()
                    .is_some_and(|value| value.available)
                {
                    driver.fail("Validation lost OKS");
                    return Task::none();
                }
                completed(
                    "keypoint-validate-oks",
                    [metrics.keypoint.as_ref().unwrap().ap, 0.0, 0.0, 0.0],
                );
                phase(driver, Stage::ValidateOks)
            }
            Stage::ValidateOks => control(widgets, driver, "validate.results.oks.ap"),
            Stage::ValidatePixels => self.validation_pixels(driver, requested),
            Stage::ExportPage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Export),
            ),
            Stage::PrepareExport
                if active == FeatureId::Export && can_prepare(FeatureId::Export) =>
            {
                control(widgets, driver, "export.model.action")
            }
            Stage::ExportPrepared if prepared(FeatureId::Export) => {
                phase(driver, Stage::StartExport)
            }
            Stage::StartExport => control(widgets, driver, primary(FeatureId::Export)),
            Stage::Exporting if model.workflow.export.as_ref().is_some_and(success) => {
                completed("keypoint-export", [1.0, 0.0, 0.0, 0.0]);
                phase(driver, Stage::PredictPage)
            }
            Stage::PredictPage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Predict),
            ),
            Stage::PreparePredict
                if active == FeatureId::Predict && can_prepare(FeatureId::Predict) =>
            {
                control(widgets, driver, "predict.model.action")
            }
            Stage::PredictPrepared if prepared(FeatureId::Predict) => {
                phase(driver, Stage::StartPredict)
            }
            Stage::StartPredict => control(widgets, driver, primary(FeatureId::Predict)),
            Stage::Predicting
                if model
                    .predict_snapshot
                    .as_ref()
                    .is_some_and(|value| success(&value.operation)) =>
            {
                self.object_pixels(
                    driver,
                    requested,
                    "keypoint-predict",
                    false,
                    Stage::UpscalePredict,
                )
            }
            Stage::UpscalePredict => control(widgets, driver, "predict.upscale.basic"),
            Stage::UpscaledPredict
                if model
                    .upscale_snapshot
                    .as_ref()
                    .is_some_and(|value| value.ready && !value.busy) =>
            {
                let Some((_, content)) = requested.and_then(surface::drawable_prediction) else {
                    return Task::none();
                };
                if content.frame().source.kind != PresentationSourceKind::Upscale {
                    return Task::none();
                }
                let derived = model.upscale_snapshot.as_ref().unwrap();
                if !schema_scene(&derived.scene) || derived.input != *content.input_frame() {
                    driver.fail("Upscale lost paired skeleton source geometry");
                    return Task::none();
                }
                self.object_pixels(
                    driver,
                    requested,
                    "keypoint-predict-upscale",
                    false,
                    Stage::Import(1),
                )
            }
            Stage::LiveResize => {
                driver.phase = Phase::Workflows(Step::Keypoints(Stage::LiveResizeReady));
                // Live currently requests the canvas extent. Keep this fixture
                // within its native capture envelope before starting a run.
                #[cfg(target_arch = "wasm32")]
                if !crate::integration_control::canvas_size_js(1200.0, 1000.0) {
                    driver.fail("Cannot resize the Live acceptance canvas");
                }
                Task::none()
            }
            Stage::LiveResizeReady => {
                if model.window_width != 1200 || model.window_height != 1000 {
                    return Task::none();
                }
                #[cfg(target_arch = "wasm32")]
                if !crate::integration_control::canvas_size_settled_js(1200.0, 1000.0) {
                    return Task::none();
                }
                phase(driver, Stage::LivePage(0))
            }
            Stage::LivePage(_) => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Live),
            ),
            Stage::StartLive(_) if active == FeatureId::Live && model.live_start_available() => {
                self.live_revision = live.map_or(0, |value| value.revision);
                self.live_completed_frames = live.map_or(0, |value| value.completedframes);
                control(widgets, driver, primary(FeatureId::Live))
            }
            Stage::LiveFrame(index) => {
                let Some(snapshot) = live.filter(|value| {
                    value.running
                        && value.revision > self.live_revision
                        && value.completedframes > self.live_completed_frames
                }) else {
                    return Task::none();
                };
                let Some((_, content)) = requested.and_then(surface::drawable_prediction) else {
                    return Task::none();
                };
                if content.frame() != &snapshot.frame {
                    return Task::none();
                }
                if index != 0 && content.analyzed() == Some(false) {
                    return Task::none();
                }
                if content.analyzed() != Some(index != 0)
                    || (index == 2 && !content.objects().is_empty())
                    || (index == 1
                        && !content
                            .objects()
                            .iter()
                            .any(|object| object.skeleton.joints.len() == 17))
                {
                    driver.fail("Production Live metadata disagrees with admitted analysis");
                    return Task::none();
                }
                let joint = content
                    .objects()
                    .iter()
                    .find_map(|object| object.skeleton.joints.first())
                    .map(|joint| (joint.point.x, joint.point.y));
                if index == 1 {
                    self.analyzed_joint = joint;
                }
                if index == 3 && (joint.is_none() || joint == self.analyzed_joint) {
                    driver.fail("New Live run reused prior model outputs");
                    return Task::none();
                }
                phase(driver, Stage::StopLive(index))
            }
            Stage::StopLive(_) => control(widgets, driver, primary(FeatureId::Live)),
            Stage::StoppedLive(index)
                if live.is_some_and(|value| !value.running && !value.cancellationrequested) =>
            {
                self.retained_frame = live.map(|value| value.frame.clone());
                self.retained_document = live.map(|value| value.document.clone());
                self.object_pixels(
                    driver,
                    requested,
                    match index {
                        0 => "live-capture-only",
                        1 => "live-production-analysis",
                        2 => "live-zero-detections",
                        _ => "live-new-model-restart",
                    },
                    index != 1 && index != 3,
                    match index {
                        0 => Stage::EnableAnalysis,
                        1 => Stage::Import(2),
                        2 => Stage::ThresholdPage(false),
                        _ => Stage::ModelPage,
                    },
                )
            }
            Stage::EnableAnalysis => control(widgets, driver, "live.analysis.enabled"),
            Stage::AnalysisEnabled
                if settled
                    && settings
                        .draft
                        .as_ref()
                        .is_some_and(|value| value.workflows.predict.liveanalysisenabled) =>
            {
                phase(driver, Stage::StartLive(1))
            }
            Stage::ThresholdPage(_) => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Predict),
            ),
            Stage::ThresholdEdit(_) if active == FeatureId::Predict => control(
                widgets,
                driver,
                constraint_workflowspredictrequestthreshold()
                    .stable_field_id
                    .to_string(),
            ),
            Stage::ThresholdReady(zero)
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        value.workflows.predict.request.threshold == if zero { 1.0 } else { 0.0 }
                    }) =>
            {
                phase(
                    driver,
                    if zero {
                        Stage::LivePage(2)
                    } else {
                        Stage::Mutate(0)
                    },
                )
            }
            Stage::Mutate(index) => phase(driver, Stage::AwaitMutation(index)),
            Stage::MutationReady(0) => phase(driver, Stage::LivePage(4)),
            Stage::MutationReady(1) => phase(driver, Stage::Mutate(2)),
            Stage::MutationReady(2) => phase(driver, Stage::LivePage(3)),
            Stage::FailedStart
                if active == FeatureId::Live && settled && model.live_start_available() =>
            {
                self.live_revision = live.map_or(0, |value| value.revision);
                control(widgets, driver, primary(FeatureId::Live))
            }
            Stage::Failed
                if live.is_some_and(|value| {
                    value.revision > self.live_revision
                        && !value.running
                        && !value.cancellationrequested
                }) =>
            {
                if live.map(|value| &value.frame) != self.retained_frame.as_ref()
                    || live.map(|value| &value.document) != self.retained_document.as_ref()
                {
                    driver.fail("Failed Live start replaced retained pixels or document");
                    return Task::none();
                }
                completed(
                    "live-provider-start-failed",
                    [
                        self.live_revision as f64,
                        live.unwrap().revision as f64,
                        0.0,
                        0.0,
                    ],
                );
                self.object_pixels(
                    driver,
                    requested,
                    "live-failure-retained",
                    true,
                    Stage::Mutate(1),
                )
            }
            Stage::LiveDerivedPage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Live),
            ),
            Stage::UpscaleLive if active == FeatureId::Live => {
                control(widgets, driver, "live.upscale.basic")
            }
            Stage::UpscaledLive
                if model
                    .upscale_snapshot
                    .as_ref()
                    .is_some_and(|value| value.ready && !value.busy) =>
            {
                let Some((_, content)) = requested.and_then(surface::drawable_prediction) else {
                    return Task::none();
                };
                let derived = model.upscale_snapshot.as_ref().unwrap();
                if content.frame().source.kind != PresentationSourceKind::Upscale {
                    return Task::none();
                }
                if self.retained_frame.as_ref() != Some(&derived.input)
                    || derived.input != *content.input_frame()
                    || !schema_scene(&derived.scene)
                    || self.retained_document.as_ref().is_none_or(|source| {
                        derived.scene.document.storage != source.resource.storage
                            || derived.scene.document.size != source.resource.size
                    })
                {
                    driver.fail(
                        "Live Upscale lost exact retained frame, source resource or joint meaning",
                    );
                    return Task::none();
                }
                self.object_pixels(driver, requested, "live-upscale", false, Stage::Import(3))
            }
            Stage::ClearImported => control(widgets, driver, "annotation.scene.clear"),
            Stage::ClearedImported
                if annotation.is_some_and(|value| value.ui.scene.objects.is_empty()) =>
            {
                phase(driver, Stage::CleanImportReveal)
            }
            Stage::CleanImportReveal => control(widgets, driver, crate::view::workspace::STABLE_ID),
            Stage::CleanImportReady
                if annotation.is_some_and(|value| value.ui.scene.objects.is_empty()) =>
            {
                self.clean_import_pixels(driver, requested, annotation.unwrap())
            }
            Stage::RestoreImported => control(widgets, driver, "annotation.undo"),
            Stage::RestoredImported
                if annotation
                    .zip(self.annotation.as_ref())
                    .is_some_and(|(current, before)| same_scene(&current.ui.scene, before)) =>
            {
                phase(driver, Stage::SelectObject(3))
            }
            Stage::ModelPage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Predict),
            ),
            Stage::PosePage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Train),
            ),
            Stage::SelectPe | Stage::SelectPose
                if active
                    == if stage == Stage::SelectPe {
                        FeatureId::Predict
                    } else {
                        FeatureId::Train
                    }
                    && settled
                    && model.settings_edit_available() =>
            {
                if RFDETR_PRESET_CATALOG.len() != 16 {
                    driver.fail("Model selector omitted a canonical model");
                    return Task::none();
                }
                self.model_row = if stage == Stage::SelectPe {
                    catalog_prediction_rows()
                        .nth(self.catalog_prediction_index)
                        .unwrap()
                } else {
                    RFDETR_PRESET_CATALOG
                        .iter()
                        .position(|preset| preset.task == ModelTask::Keypoints)
                        .unwrap()
                };
                control(
                    widgets,
                    driver,
                    if stage == Stage::SelectPe {
                        "predict.model.presets"
                    } else {
                        "train.model.presets"
                    },
                )
            }
            Stage::PeSelected | Stage::PoseSelected
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        let (name, source) = if stage == Stage::PeSelected {
                            (
                                &value.workflows.predict.request.presetname,
                                value.workflows.predict.modelsource,
                            )
                        } else {
                            (
                                &value.workflows.train.request.presetname,
                                value.workflows.train.modelsource,
                            )
                        };
                        name == RFDETR_PRESET_CATALOG[self.model_row].presetname.as_ref()
                            && source == ModelSelectionSource::Canonical
                    }) =>
            {
                if stage == Stage::PoseSelected {
                    let request = &settings.draft.as_ref().unwrap().workflows.train.request;
                    let defaults = &RFDETR_PRESET_CATALOG[self.model_row].trainingdefaults;
                    if request.recipe.optimizer != defaults.recipe.optimizer
                        || request.recipe.lr != defaults.recipe.lr
                        || request.recipe.lrencoder != defaults.recipe.lrencoder
                        || request.recipe.weightdecay != defaults.recipe.weightdecay
                        || request.epochs != defaults.epochs
                        || request.batchsize != defaults.batchsize
                        || request.gradaccumsteps != defaults.gradaccumsteps
                        || request.valbatchsize != defaults.valbatchsize
                        || request.useema != defaults.useema
                    {
                        driver.fail("Rendered catalog selection did not install the model training defaults");
                        return Task::none();
                    }
                }
                completed(
                    if stage != Stage::PeSelected {
                        "model-keypoint-selection"
                    } else if self.catalog_prediction_index == 0 {
                        "model-pe-selection"
                    } else {
                        "model-catalog-selection"
                    },
                    [
                        self.model_row as f64,
                        RFDETR_PRESET_CATALOG.len() as f64,
                        0.0,
                        0.0,
                    ],
                );
                phase(
                    driver,
                    if stage == Stage::PeSelected {
                        Stage::PeSource
                    } else {
                        Stage::CompilePose
                    },
                )
            }
            Stage::PeSource => {
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        value.workflows.predict.source.kind == SourceKind::SingleImage
                    })
                {
                    phase(driver, Stage::PreparePe)
                } else {
                    control(widgets, driver, "predict.source.image")
                }
            }
            Stage::PeSourceReady
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        value.workflows.predict.source.kind == SourceKind::SingleImage
                    }) =>
            {
                phase(driver, Stage::PreparePe)
            }
            Stage::PreparePe if can_prepare(FeatureId::Predict) => {
                control(widgets, driver, "predict.model.action")
            }
            Stage::PePrepared if prepared(FeatureId::Predict) => {
                if !self.capture_catalog_selection(driver, model, FeatureId::Predict) {
                    return Task::none();
                }
                phase(driver, Stage::StartPe)
            }
            Stage::StartPe => {
                self.operation_generation = model
                    .predict_snapshot
                    .as_ref()
                    .map_or(0, |value| value.operation.generationfrontier);
                self.catalog_frame = model
                    .predict_snapshot
                    .as_ref()
                    .map(|value| value.frame.clone());
                control(widgets, driver, primary(FeatureId::Predict))
            }
            Stage::PeRunning
                if model.predict_snapshot.as_ref().is_some_and(|value| {
                    success(&value.operation)
                        && value.operation.generationfrontier > self.operation_generation
                }) =>
            {
                let value = model.predict_snapshot.as_ref().unwrap();
                if !self.catalog_selection_current(model)
                    || value.execution.operationgeneration != value.operation.generationfrontier
                    || value.execution.admittedcapacity == 0
                    || self.catalog_frame.as_ref() == Some(&value.frame)
                    || value.frame.extent.width == 0
                {
                    driver.fail(
                        "Catalog completion is not the prepared operation and new native product",
                    );
                    return Task::none();
                }
                phase(driver, Stage::PePixels)
            }
            Stage::PePixels => {
                let Some((display, content)) = requested.and_then(surface::drawable_prediction)
                else {
                    return Task::none();
                };
                if model
                    .predict_snapshot
                    .as_ref()
                    .is_none_or(|value| value.frame != *content.frame())
                {
                    return Task::none();
                }
                if probe::current_receipt("workflow.visual.workspace")
                    .is_none_or(|value| value.surface.frame != display.frame)
                {
                    return Task::none();
                }
                let value = model.predict_snapshot.as_ref().unwrap();
                if self.pixel_attempts == 0 {
                    self.report_catalog("completed", value.operation.generationfrontier);
                    completed(
                        if self.catalog_prediction_index == 0 {
                            "catalog-pe-predict"
                        } else {
                            "catalog-predict"
                        },
                        [
                            self.model_generation as f64,
                            value.operation.generationfrontier as f64,
                            value.execution.admittedcapacity as f64,
                            self.model_row as f64,
                        ],
                    );
                }
                if self.catalog_prediction_index != 0 {
                    return phase(driver, Stage::NextCatalog);
                }
                self.patches.clear();
                self.begin_pixels(
                    driver,
                    display,
                    "workflow.visual.workspace",
                    "catalog-pe-image",
                    false,
                    Stage::NextCatalog,
                )
            }
            Stage::NextCatalog => {
                self.catalog_prediction_index += 1;
                phase(
                    driver,
                    if catalog_prediction_rows()
                        .nth(self.catalog_prediction_index)
                        .is_some()
                    {
                        Stage::SelectPe
                    } else {
                        Stage::PosePage
                    },
                )
            }
            Stage::PreparePose if can_prepare(FeatureId::Train) => {
                control(widgets, driver, "train.model.action")
            }
            Stage::PosePrepared if prepared(FeatureId::Train) => {
                if !self.capture_catalog_selection(driver, model, FeatureId::Train) {
                    return Task::none();
                }
                phase(driver, Stage::PoseSettings(0))
            }
            Stage::PoseSettings(index) | Stage::PoseSettingsReady(index) => {
                if !settled {
                    return Task::none();
                }
                let request = &settings.draft.as_ref().unwrap().workflows.train.request;
                let (ready, field) = match index {
                    0 => (
                        request.epochs == 1,
                        constraint_workflowstrainrequestepochs().stable_field_id,
                    ),
                    1 => (
                        request.batchsize == 1,
                        constraint_workflowstrainrequestbatchsize().stable_field_id,
                    ),
                    2 => (
                        request.freezeencoder,
                        constraint_workflowstrainrequestfreezeencoder().stable_field_id,
                    ),
                    _ => {
                        driver.fail("Unknown catalog training fixture setting");
                        return Task::none();
                    }
                };
                if ready {
                    phase(
                        driver,
                        if index == 2 {
                            Stage::StartPose
                        } else {
                            Stage::PoseSettings(index + 1)
                        },
                    )
                } else if matches!(stage, Stage::PoseSettings(_)) {
                    control(widgets, driver, field.to_string())
                } else {
                    Task::none()
                }
            }
            Stage::StartPose => {
                self.operation_generation = model
                    .workflow
                    .training
                    .as_ref()
                    .map_or(0, |value| value.local.generationfrontier);
                control(widgets, driver, primary(FeatureId::Train))
            }
            Stage::PoseTraining
                if model.workflow.training.as_ref().is_some_and(|value| {
                    success(&value.local)
                        && value.local.generationfrontier > self.operation_generation
                }) =>
            {
                let value = model.workflow.training.as_ref().unwrap();
                let Some(record) = value.metrics.as_ref() else {
                    driver.fail("Catalog Keypoint run has no native metrics");
                    return Task::none();
                };
                let progress = &record.progress;
                let oks = progress
                    .val
                    .as_ref()
                    .and_then(|value| value.keypoint.as_ref())
                    .filter(|value| value.available);
                if !self.catalog_selection_current(model)
                    || progress.globaloptimizerstep == 0
                    || progress.totalepochs != 1
                    || oks.is_none()
                    || [
                        progress.scalars.keypointl1,
                        progress.scalars.keypointfindable,
                        progress.scalars.keypointvisible,
                        progress.scalars.keypointnll,
                    ]
                    .iter()
                    .any(Option::is_none)
                    || progress.checkpointpath.is_empty()
                    || value.local.output.directory.is_empty()
                {
                    driver.fail("Actual catalog Keypoint training lost admitted model, bounded work, task losses, OKS or checkpoint");
                    return Task::none();
                }
                self.report_catalog("completed", value.local.generationfrontier);
                crate::integration_control::reporting::emit(|sink| {
                    for (event, path) in [
                        (
                            "integration.keypoint.training-artifact",
                            &progress.checkpointpath,
                        ),
                        (
                            "integration.keypoint.training-output",
                            &value.local.output.directory,
                        ),
                    ] {
                        sink.record(
                            event,
                            path,
                            &record.runid,
                            [
                                self.model_generation as f64,
                                value.local.generationfrontier as f64,
                                progress.globaloptimizerstep as f64,
                                oks.unwrap().ap,
                            ],
                        );
                    }
                });
                completed(
                    "catalog-keypoint-train-oks",
                    [
                        self.model_generation as f64,
                        value.local.generationfrontier as f64,
                        progress.globaloptimizerstep as f64,
                        oks.unwrap().ap,
                    ],
                );
                phase(driver, Stage::IncompatiblePage)
            }
            Stage::IncompatiblePage => control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Live),
            ),
            Stage::SelectIncompatible
                if active == FeatureId::Live && settled && model.settings_edit_available() =>
            {
                self.model_row = RFDETR_PRESET_CATALOG
                    .iter()
                    .position(|preset| preset.task == ModelTask::Keypoints)
                    .unwrap();
                control(widgets, driver, "predict.model.presets")
            }
            Stage::IncompatibleSelected
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        value.workflows.predict.modelsource == ModelSelectionSource::Canonical
                            && value.workflows.predict.request.presetname
                                == RFDETR_PRESET_CATALOG[self.model_row].presetname.as_ref()
                    }) =>
            {
                if model.live_start_available() {
                    driver.fail("Incompatible Live artifact remained startable");
                    return Task::none();
                }
                self.live_revision = live.map_or(0, |value| value.revision);
                phase(driver, Stage::RefuseStart)
            }
            Stage::RefuseStart => control(widgets, driver, primary(FeatureId::Live)),
            Stage::Refused => {
                if live.is_some_and(|value| value.running || value.revision != self.live_revision) {
                    driver.fail("Disabled Live admission started capture");
                    return Task::none();
                }
                completed(
                    "live-incompatible-refused",
                    [self.live_revision as f64, 0.0, 0.0, 0.0],
                );
                phase(driver, Stage::Done)
            }
            Stage::Pixels(_) => {
                let Some(receipt) = &self.receipt else {
                    return Task::none();
                };
                control(widgets, driver, receipt.control)
            }
            Stage::Done => driver.advance_to(Phase::Complete),
            _ => Task::none(),
        }
    }
    fn capture_catalog_selection(
        &mut self,
        driver: &mut Driver,
        model: &ApplicationModel,
        workflow: FeatureId,
    ) -> bool {
        let Some(admitted) = model.model_snapshot.as_ref().filter(|value| {
            !value.active && value.terminal.outcome == ModelSelectionOutcome::Accepted
        }) else {
            return false;
        };
        let selection = &admitted.selection;
        let row = &RFDETR_PRESET_CATALOG[self.model_row];
        let Some(config) = selection.trainingconfig.as_ref() else {
            driver.fail("Prepared catalog model has no admitted native configuration");
            return false;
        };
        if selection.key.workflow != workflow
            || selection.key.source != ModelSelectionSource::Canonical
            || selection.key.input != ModelArtifactInputKind::Weights
            || selection.key.preset != row.presetname.as_ref()
            || config.presetname != row.presetname.as_ref()
            || selection.key.resolution != row.resolution
            || config.resolution != row.resolution as i32
            || selection.task != row.task
            || config.task != row.task
            || config.backbone != row.backbone
            || config.projector != row.projector
            || config.keypointhead != row.keypointhead
            || config.declayers != row.decoderlayercount as i32
            || config.hiddendim != row.hiddendimension as i32
            || config.groupdetr != row.groupcount as i32
        {
            driver.fail("Prepared model configuration differs from selected catalog architecture");
            return false;
        }
        self.catalog_selection = Some(selection.clone());
        self.model_generation = admitted.generation;
        self.report_catalog("prepared", 0);
        true
    }
    fn catalog_selection_current(&self, model: &ApplicationModel) -> bool {
        model.model_snapshot.as_ref().is_some_and(|value| {
            value.generation == self.model_generation
                && self.catalog_selection.as_ref() == Some(&value.selection)
        })
    }
    fn report_catalog(&self, stage: &str, operation: u64) {
        let row = &RFDETR_PRESET_CATALOG[self.model_row];
        crate::integration_control::reporting::emit(|sink| {
            sink.record(
                "integration.keypoint.catalog",
                row.presetname.as_ref(),
                stage,
                [
                    self.model_generation as f64,
                    operation as f64,
                    row.resolution as f64,
                    self.model_row as f64,
                ],
            )
        });
    }
    fn clean_import_pixels(
        &mut self,
        driver: &mut Driver,
        requested: Option<Surface>,
        snapshot: &AnnotationSnapshot,
    ) -> Task<RootMessage> {
        let Some((display, content)) = requested.and_then(surface::drawable_annotation) else {
            return Task::none();
        };
        if !annotation_current(&content.metadata, snapshot) {
            return Task::none();
        }
        let Some(receipt) = probe::current_receipt(crate::view::workspace::STABLE_ID)
            .filter(|value| value.surface.frame == display.frame)
        else {
            return Task::none();
        };
        let scene = self.annotation.as_ref().unwrap().clone();
        self.patches.clear();
        for object in &scene.objects {
            for joint in object
                .skeletonnodes
                .iter()
                .filter(|joint| joint.visibility != KeypointVisibility::Unannotated)
            {
                self.patch_rgb(
                    &receipt,
                    joint.point.x,
                    joint.point.y,
                    scene.framewidth as f32,
                    scene.frameheight as f32,
                    [16.0, 24.0, 72.0],
                );
            }
            // Include the former analysis box edge, not just its skeleton.
            self.patch_rgb(
                &receipt,
                object.box_.first.x,
                (object.box_.first.y + object.box_.second.y) / 2.0,
                scene.framewidth as f32,
                scene.frameheight as f32,
                [16.0, 24.0, 72.0],
            );
        }
        if self.patches.is_empty() {
            driver.fail("Live imported scene has no former overlay locations");
            return Task::none();
        }
        self.begin_pixels(
            driver,
            display,
            crate::view::workspace::STABLE_ID,
            "live-import-clean",
            false,
            Stage::RestoreImported,
        )
    }
    fn begin_pixels(
        &mut self,
        driver: &mut Driver,
        display: Surface,
        control: &'static str,
        name: &str,
        empty: bool,
        next: Stage,
    ) -> Task<RootMessage> {
        let Some(receipt) =
            probe::current_receipt(control).filter(|value| value.surface.frame == display.frame)
        else {
            return Task::none();
        };
        self.receipt = Some(receipt);
        self.pixel_name = name.into();
        self.empty = empty;
        self.next_pixel = Some(next);
        if let Phase::Workflows(Step::Keypoints(stage)) = driver.phase {
            self.pixel_stage = Some(stage);
        }
        phase(driver, Stage::Pixels(self.pixel_attempts))
    }
    fn scene_pixels(
        &mut self,
        driver: &mut Driver,
        display: Surface,
        scene: &AnnotationSceneContent,
        control: &'static str,
        name: &str,
        next: Stage,
    ) -> Task<RootMessage> {
        let Some(receipt) =
            probe::current_receipt(control).filter(|value| value.surface.frame == display.frame)
        else {
            return Task::none();
        };
        let Some(frame) = display.frame else {
            return Task::none();
        };
        self.patches.clear();
        for object in &scene.objects {
            // The fixture deliberately overlays its two-joint class on the
            // first person joints. Probe the remaining visible person joints;
            // complete schema identity is checked separately above.
            if !object.enabled || object.skeletonnodes.len() != 17 {
                continue;
            }
            let Some(color) = scene.palette.get(object.category as usize) else {
                continue;
            };
            for joint in object
                .skeletonnodes
                .iter()
                .skip(6)
                .filter(|joint| joint.visibility != KeypointVisibility::Unannotated)
            {
                self.patch(
                    &receipt,
                    joint.point.x,
                    joint.point.y,
                    frame.content_width as f32,
                    frame.content_height as f32,
                    color,
                );
            }
        }
        if self.patches.is_empty() {
            driver.fail("Explore produced no labeled joints to sample");
            return Task::none();
        }
        self.begin_pixels(driver, display, control, name, false, next)
    }
    fn validation_pixels(
        &mut self,
        driver: &mut Driver,
        requested: Option<Surface>,
    ) -> Task<RootMessage> {
        let Some((display, content)) = requested.and_then(surface::drawable_validation) else {
            return Task::none();
        };
        let Some(receipt) = probe::current_receipt("validate.samples.atlas")
            .filter(|value| value.surface.frame == display.frame)
        else {
            return Task::none();
        };
        self.patches.clear();
        let metadata = &content.metadata;
        for sample in metadata.samples.iter().filter(|sample| sample.available) {
            for label in sample.labels.iter().filter(|label| label.groundtruth) {
                if label.skeleton.schema.names.len() != label.skeleton.joints.len() {
                    driver.fail("Validation skeleton schema differs from paired joints");
                    return Task::none();
                }
                if label.skeleton.joints.len() != 17 {
                    continue;
                }
                for joint in label.skeleton.joints.iter().skip(6).filter(|joint| {
                    joint
                        .visibility
                        .is_some_and(|visibility| visibility != KeypointVisibility::Unannotated)
                }) {
                    let x = sample.crop.x as f32
                        + joint.point.x * sample.crop.width as f32
                            / sample.pixelextent.width as f32;
                    let y = sample.crop.y as f32
                        + joint.point.y * sample.crop.height as f32
                            / sample.pixelextent.height as f32;
                    self.patch_rgb(
                        &receipt,
                        x,
                        y,
                        metadata.frame.extent.width as f32,
                        metadata.frame.extent.height as f32,
                        label.rgb.0.map(f64::from),
                    );
                }
            }
        }
        if self.patches.is_empty() {
            driver.fail("Validation has no paired labeled skeleton pixels");
            return Task::none();
        }
        self.begin_pixels(
            driver,
            display,
            "validate.samples.atlas",
            "keypoint-validate",
            false,
            Stage::ExportPage,
        )
    }
    fn object_pixels(
        &mut self,
        driver: &mut Driver,
        requested: Option<Surface>,
        name: &str,
        empty: bool,
        next: Stage,
    ) -> Task<RootMessage> {
        let Some((display, content)) = requested.and_then(surface::drawable_prediction) else {
            return Task::none();
        };
        if content.analyzed().is_some()
            && self.retained_frame.as_ref() != Some(content.input_frame())
        {
            return Task::none();
        }
        let Some(receipt) = probe::current_receipt("workflow.visual.workspace")
            .filter(|value| value.surface.frame == display.frame)
        else {
            return Task::none();
        };
        // Full-color joint evidence needs a visible raster interior. Live's
        // canvas-sized source can otherwise minify its three-pixel joint radius
        // below two screen pixels. Exercise the normal viewer zoom first.
        let input = content.input_frame();
        let display_scale = (receipt.image.width / input.extent.width as f32)
            .min(receipt.image.height / input.extent.height as f32);
        if !empty && input.source.kind == PresentationSourceKind::Live && display_scale < 0.7 {
            if self.receipt.as_ref() != Some(&receipt) {
                self.receipt = Some(receipt.clone());
                #[cfg(target_arch = "wasm32")]
                if crate::integration_control::wheel_js(
                    receipt.bounds.center_x() as f64,
                    receipt.bounds.center_y() as f64,
                    -400.0,
                    false,
                    true,
                ) != 1
                {
                    driver.fail("Cannot zoom the retained Live joint pixels");
                }
            }
            return Task::none();
        }
        self.patches.clear();
        let preview = content.preview();
        let mut visible = 0;
        for object in content.objects() {
            if object.skeleton.schema.names.len() != object.skeleton.joints.len() {
                driver.fail("Paired prediction skeleton lost joint identities");
                return Task::none();
            }
        }
        let mut sampled_locations = std::collections::HashSet::new();
        // Skeletons draw in metadata order. Coincident raster points expose
        // only the last object's color, while every semantic joint is retained.
        for object in content.objects().iter().rev() {
            if preview.is_some_and(|value| {
                !value.keypoints || object.confidence < value.confidencethreshold
            }) {
                continue;
            }
            visible += 1;
            for joint in object
                .skeleton
                .joints
                .iter()
                .filter(|joint| joint.confidence >= 0.5)
            {
                let location = [joint.point.x.round() as i32, joint.point.y.round() as i32];
                if !sampled_locations.insert(location) {
                    continue;
                }
                self.patch(
                    &receipt,
                    location[0] as f32,
                    location[1] as f32,
                    content.input_frame().extent.width as f32,
                    content.input_frame().extent.height as f32,
                    &object.color,
                );
            }
        }
        crate::integration_control::reporting::emit(|sink| {
            sink.record(
                "integration.keypoint.display_policy",
                name,
                "paired prediction skeletons",
                [
                    content.objects().len() as f64,
                    visible as f64,
                    preview.map_or(0.0, |value| value.confidencethreshold as f64),
                    (self.patches.len() / 7) as f64,
                ],
            )
        });
        if empty != content.objects().is_empty() || (!empty && self.patches.is_empty()) {
            driver.fail("Completed Live/Predict product does not match its detection evidence");
            return Task::none();
        }
        self.begin_pixels(
            driver,
            display,
            "workflow.visual.workspace",
            name,
            empty,
            next,
        )
    }
    fn patch(
        &mut self,
        receipt: &probe::ProbeReceipt,
        x: f32,
        y: f32,
        width: f32,
        height: f32,
        color: &AnnotationColor,
    ) {
        let color = surface::labels::class_color(color);
        self.patch_rgb(
            receipt,
            x,
            y,
            width,
            height,
            [
                color.r as f64 * 255.0,
                color.g as f64 * 255.0,
                color.b as f64 * 255.0,
            ],
        );
    }
    fn patch_rgb(
        &mut self,
        receipt: &probe::ProbeReceipt,
        x: f32,
        y: f32,
        width: f32,
        height: f32,
        color: [f64; 3],
    ) {
        let x = receipt.image.x + x / width * receipt.image.width;
        let y = receipt.image.y + y / height * receipt.image.height;
        if receipt.clip.contains(iced::Point::new(x, y)) {
            self.patches.extend([
                x as f64,
                y as f64,
                color[0],
                color[1],
                color[2],
                32.0,
                4.0 * receipt.scale as f64,
            ]);
        }
    }
    pub(super) fn located(
        &mut self,
        driver: &mut Driver,
        stage: Stage,
        control: &str,
        bounds: Rectangle,
    ) {
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            driver.fail("Required Keypoint control is absent");
            return;
        }
        let mut input = surface::physical_bounds(bounds, driver.input_scale);
        if let Stage::Pixels(attempt) = stage {
            let Some(receipt) = self.receipt.as_ref() else {
                driver.fail("Keypoint pixel request lost its paired receipt");
                return;
            };
            if probe::current_receipt(control).as_ref() != Some(receipt) {
                self.retry_pixels(driver, attempt);
                return;
            }
            driver.phase = Phase::Workflows(Step::Keypoints(Stage::AwaitPixels(attempt)));
            #[cfg(target_arch = "wasm32")]
            {
                let Some(mut output) = probe::probe_output(control) else {
                    driver.fail("Missing Keypoint canvas owner");
                    return;
                };
                let canvas = output.canvas_probe.clone();
                output.receipt = None;
                // The workflow owns this once completion. Browser receipt
                // validation must deliver invalidation after a new image draw.
                output.probe = None;
                let callback = crate::integration_control::pixel_checks::pixel_result_callback(
                    move |outcome| {
                        output.send(crate::integration_control::Message::WorkflowPixels {
                            picture: Picture::Keypoints,
                            index: attempt,
                            outcome,
                        });
                    },
                );
                let frame = receipt.surface.frame.unwrap();
                crate::integration_control::keypoint_pixels_js(
                    &canvas,
                    frame.content_sequence as f64,
                    frame.presentation_revision as f64,
                    &self.patches,
                    &[
                        receipt.image.x as f64,
                        receipt.image.y as f64,
                        receipt.image.width as f64,
                        receipt.image.height as f64,
                    ],
                    self.empty,
                    &self.pixel_name,
                    &self
                        .oks_bounds
                        .filter(|_| self.pixel_name == "keypoint-validate")
                        .map_or_else(Vec::new, |bounds| {
                            vec![
                                bounds.x as f64,
                                bounds.y as f64,
                                bounds.width as f64,
                                bounds.height as f64,
                            ]
                        }),
                    &callback,
                );
            }
            return;
        }
        let next = match stage {
            Stage::SelectPage => Stage::Overwrite,
            Stage::Overwrite => {
                let padding = crate::view::workflow::FIELD_SPACING * driver.input_scale;
                input.y += padding;
                input.height -= padding;
                input.width = input.width.min(input.height);
                Stage::OverwriteReady
            }
            Stage::Compile => Stage::Compiling,
            Stage::ExplorePage => Stage::Open,
            Stage::Open => Stage::Gallery,
            Stage::Gallery => {
                input = self.gallery_tile.take().unwrap();
                Stage::Detail
            }
            Stage::Import(index) => {
                if surface::viewer_annotation_request().is_none() {
                    return;
                }
                Stage::Imported(index)
            }
            Stage::SelectObject(index) => Stage::SelectedObject(index),
            Stage::SelectJoint(index) => Stage::SelectedJoint(index),
            Stage::Occlude(index) => Stage::Occluded(index),
            Stage::Undo(index) => Stage::Undone(index),
            Stage::Redo(index) => Stage::Redone(index),
            Stage::Connect(index) => Stage::Connected(index),
            Stage::Save(index) => Stage::Saved(index),
            Stage::Reload(index) => Stage::Reloaded(index),
            Stage::TrainPage => Stage::PrepareTrain,
            Stage::PrepareTrain => Stage::TrainPrepared,
            Stage::StartTrain => Stage::Training,
            Stage::ValidatePage => Stage::PrepareValidate,
            Stage::PrepareValidate => Stage::ValidatePrepared,
            Stage::StartValidate => Stage::Validating,
            Stage::ValidateOks => {
                self.oks_bounds = Some(input);
                driver.phase = Phase::Workflows(Step::Keypoints(Stage::ValidatePixels));
                return;
            }
            Stage::Move(index) => {
                driver.phase = Phase::Workflows(Step::Keypoints(Stage::MoveReady(index)));
                return;
            }
            Stage::CleanImportReveal => {
                driver.phase = Phase::Workflows(Step::Keypoints(Stage::CleanImportReady));
                return;
            }
            Stage::MoveReady(index) => {
                let Some(_receipt) = probe::current_receipt(control) else {
                    return;
                };
                if self.receipt.as_ref() != Some(&_receipt) {
                    return;
                }
                let scene = self.annotation.as_ref().unwrap();
                let point = self.joint_before.as_ref().unwrap();
                let _x = point.x as f64 / scene.framewidth as f64;
                let _y = point.y as f64 / scene.frameheight as f64;
                driver.phase = Phase::Workflows(Step::Keypoints(Stage::Moved(index)));
                #[cfg(target_arch = "wasm32")]
                if crate::integration_control::annotation_pointer_js(
                    _receipt.image.x as f64,
                    _receipt.image.y as f64,
                    _receipt.image.width as f64,
                    _receipt.image.height as f64,
                    _x,
                    _y,
                    (_x + 0.08).min(0.95),
                    (_y + 0.05).min(0.95),
                    false,
                    1,
                ) != 1
                {
                    driver.fail("Joint pointer move failed");
                }
                return;
            }
            Stage::ExportPage => Stage::PrepareExport,
            Stage::PrepareExport => Stage::ExportPrepared,
            Stage::StartExport => Stage::Exporting,
            Stage::PredictPage => Stage::PreparePredict,
            Stage::PreparePredict => Stage::PredictPrepared,
            Stage::StartPredict => Stage::Predicting,
            Stage::UpscalePredict => Stage::UpscaledPredict,
            Stage::LivePage(index) => {
                if index == 4 {
                    Stage::FailedStart
                } else {
                    Stage::StartLive(index)
                }
            }
            Stage::StartLive(index) => Stage::LiveFrame(index),
            Stage::StopLive(index) => Stage::StoppedLive(index),
            Stage::EnableAnalysis => {
                input.width = input.width.min(input.height);
                Stage::AnalysisEnabled
            }
            Stage::ThresholdPage(zero) => Stage::ThresholdEdit(zero),
            Stage::ThresholdEdit(zero) => {
                driver.phase = Phase::Workflows(Step::Keypoints(Stage::ThresholdReady(zero)));
                if !widget_ops::replace_number_input(input, if zero { "1" } else { "0" }, 32) {
                    driver.fail("Live threshold keyboard input failed");
                }
                return;
            }
            Stage::FailedStart => Stage::Failed,
            Stage::ModelPage => Stage::SelectPe,
            Stage::PosePage => Stage::SelectPose,
            Stage::PeSource => Stage::PeSourceReady,
            Stage::PreparePe => Stage::PePrepared,
            Stage::StartPe => Stage::PeRunning,
            Stage::CompilePose => Stage::PoseCompiled,
            Stage::PreparePose => Stage::PosePrepared,
            Stage::StartPose => Stage::PoseTraining,
            Stage::PoseSettings(index) => {
                if index < 2 {
                    driver.phase =
                        Phase::Workflows(Step::Keypoints(Stage::PoseSettingsReady(index)));
                    if !widget_ops::replace_number_input(input, "1", 32) {
                        driver.fail("Catalog training fixture keyboard input failed");
                    }
                    return;
                }
                input.width = input.width.min(input.height);
                Stage::PoseSettingsReady(index)
            }
            Stage::LiveDerivedPage => Stage::UpscaleLive,
            Stage::UpscaleLive => Stage::UpscaledLive,
            Stage::ClearImported => Stage::ClearedImported,
            Stage::RestoreImported => Stage::RestoredImported,
            Stage::IncompatiblePage => Stage::SelectIncompatible,
            Stage::RefuseStart => Stage::Refused,
            Stage::SelectPe | Stage::SelectPose | Stage::SelectIncompatible => {
                input.y += (1.0 + self.model_row as f32 * 22.0) * driver.input_scale;
                input.height = 22.0 * driver.input_scale;
                if stage == Stage::SelectPe {
                    Stage::PeSelected
                } else if stage == Stage::SelectPose {
                    Stage::PoseSelected
                } else {
                    Stage::IncompatibleSelected
                }
            }
            _ => {
                driver.fail("Unexpected Keypoint input continuation");
                return;
            }
        };
        driver.phase = Phase::Workflows(Step::Keypoints(next));
        if !widget_ops::click(input) {
            driver.fail("Keypoint pointer input failed");
        }
    }
    pub(super) fn pixels(&mut self, driver: &mut Driver, outcome: ProbeOutcome) {
        let Phase::Workflows(Step::Keypoints(Stage::AwaitPixels(attempt))) = driver.phase else {
            return;
        };
        match outcome {
            ProbeOutcome::Observed(samples, matched) if samples > 0 && matched > 0 => {
                self.pixel_attempts = 0;
                self.pixel_stage = None;
                completed(
                    &self.pixel_name,
                    [
                        samples as f64,
                        matched as f64,
                        self.patches.len() as f64 / 7.0,
                        self.empty as u8 as f64,
                    ],
                );
                driver.phase = Phase::Workflows(Step::Keypoints(self.next_pixel.take().unwrap()));
            }
            ProbeOutcome::Invalidated => self.retry_pixels(driver, attempt),
            _ => driver.fail("Completed Keypoint canvas did not contain its native joint pixels"),
        }
    }
    fn retry_pixels(&mut self, driver: &mut Driver, attempt: u8) {
        if attempt < 8
            && let Some(stage) = self.pixel_stage.take()
        {
            self.pixel_attempts = attempt + 1;
            self.receipt = None;
            self.patches.clear();
            driver.phase = Phase::Workflows(Step::Keypoints(stage));
        } else {
            driver.fail("Keypoint image changed before its pixel evidence settled");
        }
    }
}

#[cfg(test)]
mod catalog_cases {
    use super::*;

    #[test]
    fn ordinary_workflow_exercises_each_generated_catalog_row_once() {
        let rows: Vec<_> = catalog_prediction_rows().collect();
        let first = &RFDETR_PRESET_CATALOG[rows[0]];
        assert_eq!(first.backbone.kind, BackboneKind::PeCoreTiny);
        assert_eq!(first.decoderlayercount, 0);
        let mut executions = rows;
        executions.extend(
            RFDETR_PRESET_CATALOG
                .iter()
                .enumerate()
                .filter_map(|(index, row)| (row.task == ModelTask::Keypoints).then_some(index)),
        );
        executions.sort_unstable();
        assert_eq!(
            executions,
            (0..RFDETR_PRESET_CATALOG.len()).collect::<Vec<_>>()
        );
        assert_eq!(executions.len(), 16);
    }

    #[test]
    fn each_catalog_model_uses_the_ordinary_selector_on_every_model_workflow() {
        use crate::view::workflow::model_card;
        for workflow in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ] {
            let mut component = model_card::Component::new(workflow);
            let mut settings = crate::view::settings::installed_settings_model();
            for (index, row) in RFDETR_PRESET_CATALOG.iter().enumerate() {
                assert!(matches!(
                    component
                        .update(
                            model_card::Message::PresetSelected(index),
                            None,
                            &mut settings
                        )
                        .unwrap(),
                    Some(model_card::Outcome::SettingsEdited(_))
                ));
                let selected = crate::view_model::model_settings_projection(
                    settings.draft.as_ref().unwrap(),
                    workflow,
                )
                .unwrap();
                assert_eq!(selected.selection.key.preset, row.presetname.as_ref());
                assert_eq!(selected.selection.key.resolution, row.resolution);
                assert_eq!(
                    selected.selection.key.source,
                    ModelSelectionSource::Canonical
                );
                assert_eq!(
                    selected.selection.key.input,
                    ModelArtifactInputKind::Weights
                );
                assert!(selected.selection.key.classlayoutpath.is_empty());
            }
        }
    }
}
