use crate::generated::{
    ArtifactUiState, ComputeUiState, FeatureId, FileDialogFact, SettingsUiState, TrainingSnapshot,
};

#[derive(Debug, Clone)]
pub struct WorkflowModel {
    pub dataset: Option<ArtifactUiState>,
    pub training: Option<TrainingSnapshot>,
    pub train_continuation: Continuation,
    pub output: TrainingOutput,
    pub validation: Option<crate::generated::ValidationSnapshot>,
    pub validation_details: Option<crate::generated::EvaluationDetailPage>,
    pub export: Option<ComputeUiState>,
    settings_revision: Option<u64>,
    pub pending_start: Option<PendingStart>,
    pub start_status: Option<(FeatureId, String)>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ContinuationMode { Transfer, Resume }

#[derive(Debug, Clone, Default)]
pub enum CheckpointCapability {
    #[default]
    Idle,
    Pending { request: Option<u64>, generation: Option<u64> },
    Ready(crate::generated::TrainingCheckpoint),
    Failed(String),
}

#[derive(Debug, Clone)]
pub struct Continuation {
    pub mode: ContinuationMode,
    pub selection: Option<(crate::generated::ModelSelectionSource, String, String)>,
    pub capability: CheckpointCapability,
    pub mode_chosen: bool,
    pub cancel_needed: bool,
    observed: Option<crate::generated::TrainingCheckpointInspection>,
}
impl Default for Continuation {
    fn default() -> Self {
        Self { mode: ContinuationMode::Transfer, selection: None, capability: Default::default(),
            mode_chosen: false, cancel_needed: false, observed: None }
    }
}
impl Continuation {
    pub fn matches(&self, train: &crate::generated::TrainViewState) -> bool {
        self.selection.as_ref().is_some_and(|(source, path, preset)|
            *source == train.modelsource && *path == train.request.weightspath && *preset == train.request.presetname)
    }
    pub fn select(&mut self, train: &crate::generated::TrainViewState) {
        self.cancel_needed |= matches!(self.capability, CheckpointCapability::Pending { .. });
        self.selection = Some((train.modelsource, train.request.weightspath.clone(), train.request.presetname.clone()));
        self.capability = CheckpointCapability::Idle;
        self.mode = ContinuationMode::Transfer;
        self.mode_chosen = false;
        self.observed = None;
    }
    pub fn checkpoint(&self) -> Option<&crate::generated::TrainingCheckpoint> {
        if let CheckpointCapability::Ready(value) = &self.capability { Some(value) } else { None }
    }
    pub fn request(&self) -> Option<u64> {
        if let CheckpointCapability::Pending { request, .. } = self.capability { request } else { None }
    }
    pub fn begin(&mut self, request: u64) {
        self.cancel_needed = false;
        self.capability = CheckpointCapability::Pending { request: Some(request), generation: None };
    }
    pub fn fail(&mut self, correlation: u64, detail: String) -> bool {
        if self.request() != Some(correlation) { return false; }
        self.capability = CheckpointCapability::Failed(detail);
        true
    }
    pub fn reply(&mut self, correlation: u64, value: crate::generated::TrainingCheckpointInspection) {
        if self.request() != Some(correlation) { return; }
        self.capability = CheckpointCapability::Pending { request: None, generation: Some(value.generation) };
        let value = self.observed.take().filter(|observed| observed.generation == value.generation)
            .unwrap_or(value);
        self.observe(value);
    }
    pub fn observe(&mut self, value: crate::generated::TrainingCheckpointInspection) {
        if self.selection.as_ref().is_none_or(|(_, path, _)| *path != value.path) { return; }
        let CheckpointCapability::Pending { generation, .. } = self.capability else { return; };
        if generation.is_none() {
            if let Some(observed) = self.observed.as_mut() {
                if let Err(error) = merge_checkpoint_inspection(observed, value) {
                    self.capability = CheckpointCapability::Failed(error.detail);
                }
            } else { self.observed = Some(value); }
            return;
        }
        if generation != Some(value.generation) { return; }
        use crate::generated::TrainingInspectionStatus;
        match value.status {
            TrainingInspectionStatus::Ready => {
                if let Some(checkpoint) = value.checkpoint {
                    if !self.mode_chosen {
                        self.mode = if checkpoint.resumable { ContinuationMode::Resume } else { ContinuationMode::Transfer };
                    }
                    self.capability = CheckpointCapability::Ready(checkpoint);
                } else {
                    self.capability = CheckpointCapability::Failed("Checkpoint inspection returned no capability.".into());
                }
            }
            TrainingInspectionStatus::Failed => self.capability = CheckpointCapability::Failed(value.error),
            TrainingInspectionStatus::Cancelled => self.capability = CheckpointCapability::Failed("Checkpoint inspection cancelled.".into()),
            TrainingInspectionStatus::Idle | TrainingInspectionStatus::Running => {}
        }
    }
}

// Inspection has its own generation and terminal frontier. Training progress may
// overtake its critical event without changing which checkpoint was inspected.
pub(super) fn merge_checkpoint_inspection(
    current: &mut crate::generated::TrainingCheckpointInspection,
    value: crate::generated::TrainingCheckpointInspection,
) -> Result<(), UiError> {
    use crate::generated::TrainingInspectionStatus;
    if value.generation < current.generation { return Ok(()); }
    if value.generation == current.generation {
        if value == *current { return Ok(()); }
        if value.path != current.path { return Err(UiError::protocol("inconsistent checkpoint inspection path")); }
        if value.status == TrainingInspectionStatus::Running { return Ok(()); }
        if current.status != TrainingInspectionStatus::Running {
            return Err(UiError::protocol("inconsistent checkpoint inspection terminal"));
        }
    }
    *current = value;
    Ok(())
}

#[derive(Debug, Clone, Copy, Default)]
pub enum HistoryLoad {
    #[default]
    Idle,
    Opening(u64),
    Paging(u64),
    Failed,
}
#[derive(Debug, Clone)]
pub struct SavedTrainingOutput {
    pub directory: String,
    pub run: Option<crate::generated::TrainingOpenedRun>,
    pub page: Option<crate::generated::TrainingHistoryPage>,
    pub load: HistoryLoad,
}
#[derive(Debug, Clone, Default)]
pub enum TrainingOutputSource {
    #[default]
    Live,
    Saved(SavedTrainingOutput),
}
#[derive(Debug, Clone, Default)]
pub struct TrainingOutput {
    source: TrainingOutputSource,
    settings: Option<(bool, String)>,
    dialog_generation: u64,
}
impl TrainingOutput {
    pub fn saved(&self) -> Option<&SavedTrainingOutput> {
        if let TrainingOutputSource::Saved(value) = &self.source { Some(value) } else { None }
    }
    pub fn saved_mut(&mut self) -> Option<&mut SavedTrainingOutput> {
        if let TrainingOutputSource::Saved(value) = &mut self.source { Some(value) } else { None }
    }
    pub fn run(&self) -> Option<&crate::generated::TrainingOpenedRun> { self.saved().and_then(|saved| saved.run.as_ref()) }
    pub fn page(&self) -> Option<&crate::generated::TrainingHistoryPage> { self.saved().and_then(|saved| saved.page.as_ref()) }
    pub fn select_saved(&mut self, directory: String) {
        self.source = TrainingOutputSource::Saved(SavedTrainingOutput { directory, run: None, page: None, load: HistoryLoad::Idle });
    }
    pub fn live(&mut self) { self.source = TrainingOutputSource::Live; }
    pub fn start(&mut self, train: &crate::generated::TrainViewState, dialog_generation: Option<u64>) {
        self.settings = Some((train.autooutput, train.request.outputdir.clone()));
        if let Some(generation) = dialog_generation { self.dialog_generation = generation; }
        self.live();
    }
    pub fn fail(&mut self, correlation: u64) -> bool {
        let Some(saved) = self.saved_mut() else { return false; };
        if matches!(saved.load, HistoryLoad::Opening(value) | HistoryLoad::Paging(value) if value == correlation) {
            saved.load = HistoryLoad::Failed;
            true
        } else { false }
    }
    pub fn synchronize(&mut self, train: &crate::generated::TrainViewState, native: Option<&TrainingSnapshot>, browse: Option<u64>) -> bool {
        let changed = self.settings.as_ref().is_none_or(|(automatic, path)| *automatic != train.autooutput || *path != train.request.outputdir);
        let explicit = self.settings.is_some() && browse.is_some_and(|generation| generation != self.dialog_generation);
        if !changed && !explicit { return false; }
        let bootstrap = self.settings.is_none();
        self.settings = Some((train.autooutput, train.request.outputdir.clone()));
        if let Some(generation) = browse { self.dialog_generation = generation; }
        let native_live = native.is_some_and(|snapshot| snapshot.activity != crate::generated::TrainingActivity::Idle || !snapshot.outputdirectory.is_empty());
        if !train.autooutput && !train.request.outputdir.is_empty() && (explicit || !bootstrap || !native_live) {
            self.select_saved(train.request.outputdir.clone());
        } else { self.live(); }
        true
    }
}

#[derive(Debug, Clone)]
pub enum StartInputs {
    Train(crate::generated::TrainViewState),
    Validate {
        settings: crate::generated::ValidateViewState,
        inherited_source: Option<(bool, String)>,
    },
    Predict(crate::generated::PredictViewState),
}

impl StartInputs {
    pub fn capture(
        settings: &crate::generated::GuiSettingsState,
        feature: FeatureId,
    ) -> Option<Self> {
        match feature {
            FeatureId::Train => Some(Self::Train(settings.workflows.train.clone())),
            FeatureId::Validate => Some(Self::Validate {
                settings: settings.workflows.validate.clone(),
                inherited_source: validation_inherited_source(settings)
                    .map(|(inferred, path)| (inferred, path.to_owned())),
            }),
            FeatureId::Predict => Some(Self::Predict(settings.workflows.predict.clone())),
            _ => None,
        }
    }

    pub fn matches(&self, settings: &crate::generated::GuiSettingsState) -> bool {
        match self {
            Self::Train(value) => value == &settings.workflows.train,
            Self::Validate { settings: value, inherited_source } => {
                value == &settings.workflows.validate
                    && inherited_source.as_ref().map(|(inferred, path)| (*inferred, path.as_str()))
                        == validation_inherited_source(settings)
            },
            Self::Predict(value) => value == &settings.workflows.predict,
        }
    }
}

// Track the configured source rather than the normalized request path: native
// settlement may materialize inferred paths without changing the user's intent.
fn validation_inherited_source(settings: &crate::generated::GuiSettingsState) -> Option<(bool, &str)> {
    if !settings.workflows.validate.request.compiledpath.is_empty() {
        return None;
    }
    let train = &settings.workflows.train;
    Some((train.usecompileddirectorydefaults, if train.usecompileddirectorydefaults {
        train.compileddatasetdir.as_str()
    } else {
        train.request.valcompiledpath.as_str()
    }))
}

#[derive(Debug, Clone)]
pub struct PendingStart {
    pub feature: FeatureId,
    pub inputs: StartInputs,
    pub preparation: StartPreparation,
    pub resume_checkpoint: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StartPreparation {
    Waiting,
    ResumeQueued,
    Restoring { correlation: u64, cancelled: bool },
    Restored,
    Selecting { correlation: u64, cancelled: bool },
    Active { generation: u64, cancelled: bool },
    Stopping { generation: u64, correlation: u64 },
}

impl StartPreparation {
    pub fn cancelled(self) -> bool {
        matches!(
            self,
            Self::Restoring { cancelled: true, .. } | Self::Selecting {
                cancelled: true,
                ..
            } | Self::Active {
                cancelled: true,
                ..
            } | Self::Stopping { .. }
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ModelSelectionReceipt {
    pub workflow: FeatureId,
    pub settings_revision: u64,
}

impl Default for WorkflowModel {
    fn default() -> Self {
        Self {
            dataset: None,
            training: None,
            train_continuation: Default::default(),
            output: Default::default(),
            validation: None,
            validation_details: None,
            export: None,
            settings_revision: None,
            pending_start: None,
            start_status: None,
        }
    }
}

impl WorkflowModel {
    pub fn cancel_start(&mut self, detail: &str) {
        let Some(pending) = self.pending_start.as_mut() else {
            return;
        };
        self.start_status = Some((pending.feature, detail.to_owned()));
        match &mut pending.preparation {
            StartPreparation::Waiting | StartPreparation::ResumeQueued | StartPreparation::Restored => self.pending_start = None,
            StartPreparation::Restoring { cancelled, .. } => *cancelled = true,
            StartPreparation::Selecting { cancelled, .. }
            | StartPreparation::Active { cancelled, .. } => *cancelled = true,
            StartPreparation::Stopping { .. } => {}
        }
    }

    pub fn settle_start_model_reply(
        &mut self,
        correlation: u64,
        reply: &Result<crate::generated::ApplicationReply, crate::protocol::ApplicationError>,
    ) {
        let Some(pending) = self.pending_start.as_mut() else {
            return;
        };
        match pending.preparation {
            StartPreparation::Restoring { correlation: owned, cancelled } if correlation == owned => {
                match reply {
                    Ok(crate::generated::ApplicationReply::TrainingPrepareResume(checkpoint)) if !cancelled => {
                        pending.resume_checkpoint = Some(checkpoint.path.clone());
                        pending.preparation = StartPreparation::Restored;
                    }
                    _ => {
                        if let Err(error) = reply {
                            self.start_status = Some((pending.feature, error.detail.clone()));
                        }
                        self.pending_start = None;
                    }
                }
            }
            StartPreparation::Selecting {
                correlation: owned,
                cancelled,
            } if correlation == owned => match reply {
                Ok(crate::generated::ApplicationReply::ModelSelect(snapshot)) => {
                    pending.preparation = StartPreparation::Active {
                        generation: snapshot.generation,
                        cancelled,
                    };
                }
                Err(error) => {
                    self.start_status = Some((pending.feature, error.detail.clone()));
                    self.pending_start = None;
                }
                _ => {
                    self.start_status = Some((
                        pending.feature,
                        "Model selection reply did not match its request.".to_owned(),
                    ));
                    self.pending_start = None;
                }
            },
            StartPreparation::Stopping {
                correlation: owned, ..
            } if correlation == owned => {
                if let Err(error) = reply {
                    self.start_status = Some((pending.feature, error.detail.clone()));
                    self.pending_start = None;
                }
            }
            _ => {}
        }
    }

    pub fn start_detail(&self, feature: FeatureId) -> &str {
        self.start_status
            .as_ref()
            .filter(|(owner, _)| *owner == feature)
            .map_or("", |(_, detail)| detail.as_str())
    }

    pub fn install_settings(&mut self, snapshot: &SettingsUiState) {
        self.settings_revision = Some(snapshot.revision);
    }

    pub fn model_selection_receipt(&self, workflow: FeatureId) -> Option<ModelSelectionReceipt> {
        Some(ModelSelectionReceipt {
            workflow,
            settings_revision: self.settings_revision?,
        })
    }

    pub fn dialogs(&self, feature: FeatureId) -> impl Iterator<Item = &'static FileDialogFact> {
        crate::generated::FILE_DIALOGS
            .iter()
            .filter(move |fact| fact.workflows.contains(&feature))
    }
}

use super::{
    ApplicationEvent, ApplicationModel, ApplicationReply, Observation, PresentationSourceKind,
    UiError, merge_compute_snapshot, merge_live_snapshot,
};

impl crate::generated::DatasetApplicationProjection<UiError> for ApplicationModel {
    fn project_dataset_snapshot(
        &mut self,
        value: crate::generated::ArtifactUiState,
    ) -> Result<(), UiError> {
        self.install_dataset_snapshot(value).map(|_| ())
    }

    fn project_dataset_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::DatasetDatasetProgress(value) => {
                if let Err(error) = self.install_dataset_progress(
                    value.generation,
                    value.active,
                    value.terminal,
                    value.progress,
                ) {
                    self.error = Some(error);
                }
            }
            ApplicationEvent::DatasetDatasetChanged(value) => {
                let failed = value.snapshot.terminal.outcome
                    == crate::generated::ArtifactTerminalOutcome::Failed;
                let detail = value.snapshot.terminal.detail.clone();
                match self.install_dataset_snapshot(value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) if failed => self.failed(detail),
                    Ok(Observation::Installed | Observation::Current | Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated Dataset dispatch supplied another system event"),
        }
    }

    fn project_dataset_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::DatasetCompile(snapshot)
            | ApplicationReply::DatasetStop(snapshot) => snapshot,
            _ => unreachable!("generated Dataset dispatch supplied another system reply"),
        };
        let failed = snapshot.terminal.outcome == crate::generated::ArtifactTerminalOutcome::Failed;
        let detail = snapshot.terminal.detail.clone();
        match self.install_dataset_snapshot(snapshot) {
            Err(error) => self.error = Some(error),
            Ok(Observation::Installed) if failed => self.failed(detail),
            Ok(Observation::Installed | Observation::Current | Observation::Stale) => {}
        }
    }
}

impl crate::generated::TrainingApplicationProjection<UiError> for ApplicationModel {
    fn project_training_snapshot(
        &mut self,
        value: crate::generated::TrainingSnapshot,
    ) -> Result<(), UiError> {
        self.install_training_snapshot(value).map(|_| ())
    }

    fn project_training_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::TrainingTrainingProgress(value) => {
                if let Err(error) = self.install_training_progress(value) {
                    self.error = Some(error);
                }
            }
            ApplicationEvent::TrainingTrainingChanged(value) => {
                self.install_training_reply_snapshot(value.snapshot);
            }
            ApplicationEvent::TrainingTrainingInspectionChanged(value) => {
                if let Err(error) = self.install_checkpoint_inspection(value.inspection) { self.error = Some(error); }
            }
            _ => unreachable!("generated Training dispatch supplied another system event"),
        }
    }

    fn project_training_reply(&mut self, correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::TrainingOpenRun(value) => {
                if let Some(saved) = self.workflow.output.saved_mut()
                    && matches!(saved.load, HistoryLoad::Opening(owned) if owned == correlation) {
                    saved.load = HistoryLoad::Idle;
                    saved.run = Some(value);
                    saved.page = None;
                }
                return;
            }
            ApplicationReply::TrainingHistory(value) => {
                if let Some(saved) = self.workflow.output.saved_mut()
                    && matches!(saved.load, HistoryLoad::Paging(owned) if owned == correlation)
                    && saved.run.as_ref().is_some_and(|run| run.generation == value.generation) {
                    saved.load = HistoryLoad::Idle;
                    saved.page = Some(value);
                }
                return;
            }
            ApplicationReply::TrainingInspectCheckpoint(value) => {
                if let Err(error) = self.install_checkpoint_inspection(value.clone()) { self.error = Some(error); }
                self.workflow.train_continuation.reply(correlation, value);
                return;
            }
            ApplicationReply::TrainingCancelCheckpointInspection(value) => {
                if let Err(error) = self.install_checkpoint_inspection(value) { self.error = Some(error); }
                return;
            }
            ApplicationReply::TrainingPrepareResume(_) => return,
            ApplicationReply::TrainingResume(snapshot)
            | ApplicationReply::TrainingStart(snapshot)
            | ApplicationReply::TrainingStop(snapshot)
            | ApplicationReply::TrainingQuery(snapshot)
            | ApplicationReply::TrainingSelect(snapshot)
            | ApplicationReply::TrainingClear(snapshot)
            | ApplicationReply::TrainingStartRemote(snapshot)
            | ApplicationReply::TrainingStopRemote(snapshot)
            | ApplicationReply::TrainingRetryReconciliation(snapshot) => snapshot,
            _ => unreachable!("generated Training dispatch supplied another system reply"),
        };
        self.install_training_reply_snapshot(snapshot);
    }
}

impl ApplicationModel {
    fn install_checkpoint_inspection(&mut self, value: crate::generated::TrainingCheckpointInspection) -> Result<(), UiError> {
        if let Some(snapshot) = self.workflow.training.as_mut() {
            merge_checkpoint_inspection(&mut snapshot.inspection, value.clone())?;
        }
        self.workflow.train_continuation.observe(value);
        Ok(())
    }

    fn install_training_reply_snapshot(&mut self, snapshot: crate::generated::TrainingSnapshot) {
        match self.install_training_snapshot(snapshot) {
            Err(error) => self.error = Some(error),
            Ok((Observation::Installed, Some(detail))) => self.failed(detail),
            Ok((Observation::Installed | Observation::Current | Observation::Stale, _)) => {}
        }
    }
}

impl ApplicationModel {
    fn install_validation_snapshot(
        &mut self,
        mut value: crate::generated::ValidationSnapshot,
    ) -> Result<(), UiError> {
        let failed =
            value.operation.terminal.outcome == crate::generated::ComputeOperationOutcome::Failed;
        let detail = value.operation.terminal.detail.clone();
        let mut installed = true;
        if let Some(current) = self.workflow.validation.as_ref() {
            // Neutralize only the independent logical facts: equality then covers
            // every physical field in the canonical generated snapshot, including
            // future additions, without a second physical member inventory.
            if value.frame.revision == current.frame.revision {
                let mut physical = value.clone();
                physical.operation = current.operation.clone();
                physical.metrics = current.metrics.clone();
                physical.detailrows = current.detailrows;
                if physical != *current {
                    return Err(UiError::protocol("inconsistent Validation frame revision"));
                }
            }
            let mut operation = current.operation.clone();
            let outcome =
                super::reduction::merge_compute_state(&mut operation, value.operation.clone())?;
            installed = outcome == Observation::Installed;
            if outcome == Observation::Stale {
                value.metrics = current.metrics.clone();
                value.detailrows = current.detailrows;
            }
            if value.frame.revision < current.frame.revision {
                let metrics = value.metrics.take();
                let detail_rows = value.detailrows;
                value = current.clone();
                value.metrics = metrics;
                value.detailrows = detail_rows;
            }
            value.operation = operation;
        }
        if self
            .workflow
            .validation_details
            .as_ref()
            .is_some_and(|page| page.generation != value.operation.generationfrontier)
        {
            self.workflow.validation_details = None;
        }
        self.workflow.validation = Some(value);
        if installed && failed {
            self.failed(detail);
        }
        Ok(())
    }
}
impl crate::generated::ValidationApplicationProjection<UiError> for ApplicationModel {
    fn project_validation_snapshot(
        &mut self,
        value: crate::generated::ValidationSnapshot,
    ) -> Result<(), UiError> {
        self.install_validation_snapshot(value)
    }
    fn project_validation_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::ValidationValidationProgress(value) => {
                self.install_compute_snapshot(FeatureId::Validate, value.operation)
            }
            ApplicationEvent::ValidationValidationChanged(value) => {
                if let Err(error) = self.install_validation_snapshot(value.snapshot) {
                    self.error = Some(error);
                }
            }
            _ => unreachable!("generated Validation dispatch supplied another system event"),
        }
    }
    fn project_validation_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::ValidationStart(snapshot)
            | ApplicationReply::ValidationStop(snapshot)
            | ApplicationReply::ValidationSelectSample(snapshot)
            | ApplicationReply::ValidationCloseDetail(snapshot)
            | ApplicationReply::ValidationSetOverlays(snapshot) => snapshot,
            ApplicationReply::ValidationDetails(page) => {
                if self.workflow.validation.as_ref().is_some_and(|snapshot| {
                    snapshot.operation.generationfrontier == page.generation
                }) {
                    self.workflow.validation_details = Some(page);
                }
                return;
            }
            _ => unreachable!("generated Validation dispatch supplied another system reply"),
        };
        if let Err(error) = self.install_validation_snapshot(snapshot) {
            self.error = Some(error);
        }
    }
}

impl crate::generated::ExportSystemApplicationProjection<UiError> for ApplicationModel {
    fn project_exportsystem_snapshot(
        &mut self,
        value: crate::generated::ComputeUiState,
    ) -> Result<(), UiError> {
        merge_compute_snapshot(&mut self.workflow.export, value).map(|_| ())
    }

    fn project_exportsystem_event(&mut self, event: ApplicationEvent) {
        let snapshot = match event {
            ApplicationEvent::ExportSystemComputeProgressEvent(value) => value.snapshot,
            ApplicationEvent::ExportSystemComputeChanged(value) => value.snapshot,
            _ => unreachable!("generated Export dispatch supplied another system event"),
        };
        self.install_compute_snapshot(FeatureId::Export, snapshot);
    }

    fn project_exportsystem_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::ExportSystemStart(snapshot)
            | ApplicationReply::ExportSystemStop(snapshot) => snapshot,
            _ => unreachable!("generated Export dispatch supplied another system reply"),
        };
        self.install_compute_snapshot(FeatureId::Export, snapshot);
    }
}

impl crate::generated::PredictApplicationProjection<UiError> for ApplicationModel {
    fn project_predict_snapshot(
        &mut self,
        value: crate::generated::PredictSnapshot,
    ) -> Result<(), UiError> {
        self.install_predict_snapshot(value).map(|_| ())
    }

    fn project_predict_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::PredictPredictProgress(value) => {
                if let Err(error) = super::reduction::merge_predict_progress(
                    &mut self.predict_snapshot,
                    value.snapshot,
                ) {
                    self.error = Some(error);
                }
            }
            ApplicationEvent::PredictPredictChanged(value) => {
                match self.install_predict_snapshot(value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if self.source_for(PresentationSourceKind::Predict).is_some() {
                            self.set_foreground_visual(Some(PresentationSourceKind::Predict));
                        }
                    }
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            ApplicationEvent::PredictPredictFailed(value) => {
                match self.install_predict_snapshot(value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => self.failed(value.detail),
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated Predict dispatch supplied another system event"),
        }
    }

    fn project_predict_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::PredictStart(snapshot)
            | ApplicationReply::PredictStop(snapshot)
            | ApplicationReply::PredictPause(snapshot) => snapshot,
            _ => unreachable!("generated Predict dispatch supplied another system reply"),
        };
        if let Err(error) = self.install_predict_snapshot(snapshot) {
            self.error = Some(error);
        }
    }
}

impl crate::generated::LiveApplicationProjection<UiError> for ApplicationModel {
    fn project_live_snapshot(
        &mut self,
        value: crate::generated::LiveSnapshot,
    ) -> Result<(), UiError> {
        merge_live_snapshot(&mut self.live_snapshot, value).map(|_| ())
    }

    fn project_live_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::LiveLiveFrameCompleted(crate::generated::LiveFrameCompleted {
                snapshot,
            })
            | ApplicationEvent::LiveLiveChanged(crate::generated::LiveChanged { snapshot }) => {
                match merge_live_snapshot(&mut self.live_snapshot, snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if self.presentation_model.foreground()
                            == Some(PresentationSourceKind::Live)
                        {
                            self.set_foreground_visual(Some(PresentationSourceKind::Live));
                        }
                    }
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            ApplicationEvent::LiveLiveFailed(value) => {
                match merge_live_snapshot(&mut self.live_snapshot, value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed | Observation::Current) => {
                        self.failed(value.detail);
                    }
                    Ok(Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated Live dispatch supplied another system event"),
        }
    }

    fn project_live_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::LiveStart(snapshot) | ApplicationReply::LiveStop(snapshot) => {
                snapshot
            }
            _ => unreachable!("generated Live dispatch supplied another system reply"),
        };
        if let Err(error) = merge_live_snapshot(&mut self.live_snapshot, snapshot) {
            self.error = Some(error);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validation_start_tracks_only_its_effective_dataset_source() {
        let mut settings = crate::view::settings::installed_settings_model().draft.unwrap();
        settings.workflows.validate.request.compiledpath.clear();
        let inherited = StartInputs::capture(&settings, FeatureId::Validate).unwrap();
        settings.workflows.train.request.valcompiledpath = "/normalized/val.bin".into();
        assert!(inherited.matches(&settings));
        settings.workflows.train.compileddatasetdir = "/new-source".into();
        assert!(!inherited.matches(&settings));
        settings.workflows.train.usecompileddirectorydefaults = false;
        let manual = StartInputs::capture(&settings, FeatureId::Validate).unwrap();
        settings.workflows.train.request.valcompiledpath = "/another.bin".into();
        assert!(!manual.matches(&settings));
        settings.workflows.validate.request.compiledpath = "/independent.bin".into();
        let explicit = StartInputs::capture(&settings, FeatureId::Validate).unwrap();
        settings.workflows.train.usecompileddirectorydefaults = true;
        settings.workflows.train.compileddatasetdir = "/unrelated".into();
        assert!(explicit.matches(&settings));
        settings.workflows.validate.request.compiledpath.clear();
        assert!(!explicit.matches(&settings));
    }

    #[test]
    fn generated_catalog_and_dialog_facts_drive_workflow_availability() {
        assert!(!crate::generated::RFDETR_PRESET_CATALOG.is_empty());
        let mut model = WorkflowModel::default();
        assert!(
            model
                .dialogs(crate::generated::FeatureId::Train)
                .any(|dialog| dialog.stable_field_id != 0)
        );
        assert_eq!(
            crate::generated::FEATURE_ID_VALUES.len(),
            crate::view::navigation::ORDER.len()
        );

        let settings = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Settings(value) => Some(value),
                _ => None,
            })
            .unwrap();
        model.install_settings(&settings);
        assert_eq!(model.settings_revision, Some(settings.revision));
    }

    #[test]
    fn model_selection_receipt_names_the_workflow_and_current_settings_revision() {
        let mut model = WorkflowModel::default();
        let settings = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Settings(value) => Some(value),
                _ => None,
            })
            .unwrap();
        model.install_settings(&settings);
        let receipt = model.model_selection_receipt(FeatureId::Predict).unwrap();
        assert_eq!(receipt.workflow, FeatureId::Predict);
        assert_eq!(receipt.settings_revision, settings.revision);
    }
}

#[cfg(test)]
mod validation_tests {
    use super::*;
    use crate::generated::{
        ValidationApplicationProjection, ValidationChanged, ValidationProgress,
    };
    #[test]
    fn validation_keeps_physical_generation_independent_and_rejects_old_detail_pages() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut current = model.workflow.validation.clone().unwrap();
        current.operation.generationfrontier = 2;
        current.operation.progress.sequence = 1;
        current.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Validation, 7);
        current.contentidentity = 11;
        current.sampleidentities[0].generation = 2;
        current.sampleavailable[0] = true;
        model.project_validation_event(ApplicationEvent::ValidationValidationChanged(
            ValidationChanged {
                snapshot: current.clone(),
            },
        ));
        let mut old = current.clone();
        old.operation.generationfrontier = 1;
        old.frame.revision = 8;
        model.project_validation_event(ApplicationEvent::ValidationValidationChanged(
            ValidationChanged { snapshot: old },
        ));
        let installed = model.workflow.validation.as_ref().unwrap();
        assert_eq!(installed.operation.generationfrontier, 2);
        assert_eq!(installed.frame.revision, 8);
        let mut progress = current.operation.clone();
        progress.progress.sequence = 2;
        model.project_validation_event(ApplicationEvent::ValidationValidationProgress(
            ValidationProgress {
                operation: progress,
            },
        ));
        assert_eq!(
            model.workflow.validation.as_ref().unwrap().contentidentity,
            11
        );
        model.project_validation_reply(
            1,
            ApplicationReply::ValidationDetails(crate::generated::EvaluationDetailPage {
                generation: 1,
                total: 0,
                offset: 0,
                rows: vec![],
            }),
        );
        assert!(model.workflow.validation_details.is_none());
        model.project_validation_reply(
            2,
            ApplicationReply::ValidationDetails(crate::generated::EvaluationDetailPage {
                generation: 2,
                total: 0,
                offset: 0,
                rows: vec![],
            }),
        );
        assert!(model.workflow.validation_details.is_some());
        let retained = model.workflow.validation_details.clone();
        let mut settings = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Settings(value) => Some(value),
                _ => None,
            })
            .unwrap();
        settings.revision += 1;
        settings
            .settingsstate
            .workflows
            .validate
            .request
            .compiledpath = "/later/input.bin".into();
        model.install_settings_snapshot(settings).unwrap();
        assert_eq!(model.workflow.validation_details, retained);
    }
    #[test]
    fn validation_equal_physical_revisions_require_identical_product_facts() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut baseline = model.workflow.validation.clone().unwrap();
        baseline.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Validation, 9);
        baseline.operation.generationfrontier = 3;
        baseline.contentidentity = 17;
        model.install_validation_snapshot(baseline.clone()).unwrap();
        let mut progress = baseline.clone();
        progress.operation.progress.sequence = 1;
        model.install_validation_snapshot(progress.clone()).unwrap();
        assert_eq!(
            model
                .workflow
                .validation
                .as_ref()
                .unwrap()
                .operation
                .progress
                .sequence,
            1
        );
        for mutation in 0..6 {
            let mut conflict = progress.clone();
            match mutation {
                0 => conflict.frame.extent.width += 1,
                1 => conflict.contentidentity += 1,
                2 => conflict.detail = !conflict.detail,
                3 => conflict.sampleidentities[0].datasetindex += 1,
                4 => conflict.sampleavailable[0] = !conflict.sampleavailable[0],
                _ => conflict.overlays.groundtruthmasks = !conflict.overlays.groundtruthmasks,
            }
            assert!(model.install_validation_snapshot(conflict).is_err());
            assert_eq!(model.workflow.validation.as_ref().unwrap(), &progress);
        }
        let mut older = progress.clone();
        older.frame.revision -= 1;
        older.contentidentity += 1;
        older.overlays.predictionboxes = !older.overlays.predictionboxes;
        older.operation.progress.sequence = 2;
        model.install_validation_snapshot(older).unwrap();
        let installed = model.workflow.validation.as_ref().unwrap();
        assert_eq!(installed.frame, baseline.frame);
        assert_eq!(installed.contentidentity, baseline.contentidentity);
        assert_eq!(installed.overlays, baseline.overlays);
        assert_eq!(installed.operation.progress.sequence, 2);
        let mut newer = baseline.clone();
        newer.frame.revision += 1;
        newer.operation.generationfrontier -= 1;
        newer.overlays.predictionboxes = !newer.overlays.predictionboxes;
        model.install_validation_snapshot(newer.clone()).unwrap();
        assert_eq!(
            model.workflow.validation.as_ref().unwrap().overlays,
            newer.overlays
        );
        assert_eq!(
            model
                .workflow
                .validation
                .as_ref()
                .unwrap()
                .operation
                .generationfrontier,
            3
        );
    }
}

#[cfg(test)]
mod training_history_tests {
    use super::*;
    use crate::generated::{TrainingApplicationProjection, TrainingCheckpointInspection, TrainingInspectionStatus};

    fn inspect(path: &str, generation: u64, resumable: bool) -> TrainingCheckpointInspection {
        TrainingCheckpointInspection {
            generation, path: path.into(), status: TrainingInspectionStatus::Ready, error: String::new(),
            checkpoint: Some(crate::generated::TrainingCheckpoint {
                path: path.into(), attemptid: "saved-attempt".into(), originalweights: "/original/weights.pt".into(),
                originalclassdescriptor: String::new(), resumable, epoch: 4, configuration: None,
                classlayout: None, evaluatedweights: crate::generated::EvaluatedWeights::Ema,
            }),
        }
    }

    #[test]
    fn checkpoint_capability_waits_for_matching_receipt_and_native_completion() {
        for resumable in [false, true] {
            for event_first in [false, true] {
                let mut model = crate::view_model::test_support::bootstrapped();
                let mut before = model.workflow.training.clone();
                let mut train = model.settings_snapshot.as_ref().unwrap().settingsstate.workflows.train.clone();
                train.modelsource = crate::generated::ModelSelectionSource::Custom;
                train.request.weightspath = "/run/checkpoint.pt".into();
                model.workflow.train_continuation.select(&train);
                model.workflow.train_continuation.begin(1);
                let ready = inspect(&train.request.weightspath, 9, resumable);
                let mut running = ready.clone();
                running.status = TrainingInspectionStatus::Running;
                running.checkpoint = None;
                if event_first {
                    model.project_training_event(ApplicationEvent::TrainingTrainingInspectionChanged(
                        crate::generated::TrainingInspectionChanged { inspection: ready.clone() }));
                }
                assert!(model.workflow.train_continuation.checkpoint().is_none());
                model.project_training_reply(1, ApplicationReply::TrainingInspectCheckpoint(running));
                if !event_first {
                    assert!(model.workflow.train_continuation.checkpoint().is_none());
                    model.project_training_event(ApplicationEvent::TrainingTrainingInspectionChanged(
                        crate::generated::TrainingInspectionChanged { inspection: ready.clone() }));
                }
                assert_eq!(model.workflow.train_continuation.checkpoint(), ready.checkpoint.as_ref());
                assert_eq!(model.workflow.train_continuation.mode, if resumable { ContinuationMode::Resume } else { ContinuationMode::Transfer });
                model.project_training_reply(2, ApplicationReply::TrainingPrepareResume(ready.checkpoint.clone().unwrap()));
                assert!(model.workflow.pending_start.is_none());
                before.as_mut().unwrap().inspection = ready;
                assert_eq!(model.workflow.training, before);
                assert!(model.error.is_none());
            }
        }
    }

    #[test]
    fn training_progress_and_snapshot_do_not_roll_back_independent_inspection() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut training = model.workflow.training.clone().unwrap();
        training.revision += 1;
        model.project_training_snapshot(training.clone()).unwrap();
        let ready = inspect("/run/checkpoint.pt", 3, true);
        model.project_training_event(ApplicationEvent::TrainingTrainingInspectionChanged(
            crate::generated::TrainingInspectionChanged { inspection: ready.clone() }));
        // The runtime snapshot may have been captured before the inspection event.
        model.project_training_snapshot(training.clone()).unwrap();
        training.inspection = ready.clone();
        model.project_training_snapshot(training).unwrap();
        assert_eq!(model.workflow.training.as_ref().unwrap().inspection, ready);
        assert!(model.error.is_none());
    }

    #[test]
    fn replaced_inspection_failure_and_user_mode_are_settled_explicitly() {
        let mut selection = Continuation::default();
        let mut train = crate::view_model::test_support::bootstrapped().settings_snapshot.unwrap().settingsstate.workflows.train;
        train.modelsource = crate::generated::ModelSelectionSource::Custom;
        train.request.weightspath = "/one.pt".into();
        selection.select(&train);
        selection.begin(1);
        train.request.weightspath = "/two.pt".into();
        selection.select(&train);
        selection.begin(2);
        selection.reply(1, inspect("/one.pt", 1, true));
        selection.observe(inspect("/one.pt", 1, true));
        assert!(selection.checkpoint().is_none());
        assert!(!selection.fail(1, "stale".into()));
        assert!(selection.fail(2, "unavailable weights".into()));
        assert!(matches!(selection.capability, CheckpointCapability::Failed(_)));
        assert!(selection.request().is_none());
        assert!(selection.checkpoint().is_none());
        selection.begin(3);
        selection.mode = ContinuationMode::Transfer;
        selection.mode_chosen = true;
        selection.reply(3, inspect("/two.pt", 3, true));
        assert_eq!(selection.mode, ContinuationMode::Transfer);
    }

    #[test]
    fn bootstrap_prefers_active_or_retained_live_output_and_explicit_browse_selects_saved() {
        let model = crate::view_model::test_support::bootstrapped();
        let mut train = model.settings_snapshot.unwrap().settingsstate.workflows.train;
        train.autooutput = false;
        train.request.outputdir = "/configured".into();
        for active in [false, true] {
            let mut snapshot = model.workflow.training.clone().unwrap();
            snapshot.outputdirectory = "/configured/run-0002".into();
            if active { snapshot.activity = crate::generated::TrainingActivity::Local; }
            let mut output = TrainingOutput::default();
            // An old accepted dialog in bootstrap is not a new user selection.
            assert!(output.synchronize(&train, Some(&snapshot), Some(4)));
            assert!(output.saved().is_none());
            assert!(!output.synchronize(&train, Some(&snapshot), Some(4)));
            assert!(output.synchronize(&train, Some(&snapshot), Some(5)));
            assert_eq!(output.saved().unwrap().directory, "/configured");
            assert!(!output.synchronize(&train, Some(&snapshot), Some(5)));
        }
        let mut output = TrainingOutput::default();
        output.synchronize(&train, model.workflow.training.as_ref(), None);
        assert_eq!(output.saved().unwrap().directory, "/configured");
    }

    #[test]
    fn live_selection_discards_saved_correlations_and_stale_pages() {
        let mut model = crate::view_model::test_support::bootstrapped();
        model.workflow.output.select_saved("/saved".into());
        model.workflow.output.saved_mut().unwrap().load = HistoryLoad::Opening(2);
        model.workflow.output.live();
        model.project_training_reply(2, ApplicationReply::TrainingOpenRun(crate::generated::TrainingOpenedRun {
            generation: 7, directory: "/saved".into(), run: None,
        }));
        model.project_training_reply(3, ApplicationReply::TrainingHistory(crate::generated::TrainingHistoryPage {
            generation: 7, nextcursor: 4096, more: true, records: vec![],
        }));
        assert!(model.workflow.output.saved().is_none());
    }
}
