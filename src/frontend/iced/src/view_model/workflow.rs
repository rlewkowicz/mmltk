use crate::generated::{
    ArtifactUiState, ComputeUiState, FeatureId, FileDialogFact, SettingsUiState, TrainingSnapshot,
};

#[derive(Debug, Clone)]
pub struct WorkflowModel {
    pub dataset: Option<ArtifactUiState>,
    pub training: Option<TrainingSnapshot>,
    pub training_run: Option<crate::generated::TrainingOpenedRun>,
    pub training_history: Option<crate::generated::TrainingHistoryPage>,
    pub training_checkpoint: Option<crate::generated::TrainingCheckpoint>,
    pub resume_ready: Option<crate::generated::TrainingCheckpoint>,
    pub validation: Option<crate::generated::ValidationSnapshot>,
    pub validation_details: Option<crate::generated::EvaluationDetailPage>,
    pub export: Option<ComputeUiState>,
    settings_revision: Option<u64>,
    pub pending_start: Option<PendingStart>,
    pub start_status: Option<(FeatureId, String)>,
}

#[derive(Debug, Clone)]
pub enum StartInputs {
    Train(crate::generated::TrainViewState),
    Validate(crate::generated::ValidateViewState),
    Predict(crate::generated::PredictViewState),
}

impl StartInputs {
    pub fn capture(settings: &crate::generated::GuiSettingsState, feature: FeatureId) -> Option<Self> {
        match feature {
            FeatureId::Train => Some(Self::Train(settings.workflows.train.clone())),
            FeatureId::Validate => Some(Self::Validate(settings.workflows.validate.clone())),
            FeatureId::Predict => Some(Self::Predict(settings.workflows.predict.clone())),
            _ => None,
        }
    }

    pub fn matches(&self, settings: &crate::generated::GuiSettingsState) -> bool {
        match self {
            Self::Train(value) => value == &settings.workflows.train,
            Self::Validate(value) => value == &settings.workflows.validate,
            Self::Predict(value) => value == &settings.workflows.predict,
        }
    }
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
    Selecting { correlation: u64, cancelled: bool },
    Active { generation: u64, cancelled: bool },
    Stopping { generation: u64, correlation: u64 },
}

impl StartPreparation {
    pub fn cancelled(self) -> bool {
        matches!(self, Self::Selecting { cancelled: true, .. }
            | Self::Active { cancelled: true, .. } | Self::Stopping { .. })
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
            training_run: None,
            training_history: None,
            training_checkpoint: None,
            resume_ready: None,
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
        let Some(pending) = self.pending_start.as_mut() else { return; };
        self.start_status = Some((pending.feature, detail.to_owned()));
        match &mut pending.preparation {
            StartPreparation::Waiting => self.pending_start = None,
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
        let Some(pending) = self.pending_start.as_mut() else { return; };
        match pending.preparation {
            StartPreparation::Selecting { correlation: owned, cancelled } if correlation == owned => {
                match reply {
                    Ok(crate::generated::ApplicationReply::ModelSelect(snapshot)) => {
                        pending.preparation = StartPreparation::Active { generation: snapshot.generation, cancelled };
                    }
                    Err(error) => {
                        self.start_status = Some((pending.feature, error.detail.clone()));
                        self.pending_start = None;
                    }
                    _ => {
                        self.start_status = Some((pending.feature, "Model selection reply did not match its request.".to_owned()));
                        self.pending_start = None;
                    }
                }
            }
            StartPreparation::Stopping { correlation: owned, .. } if correlation == owned => {
                if let Err(error) = reply {
                    self.start_status = Some((pending.feature, error.detail.clone()));
                    self.pending_start = None;
                }
            }
            _ => {}
        }
    }

    pub fn start_detail(&self, feature: FeatureId) -> &str {
        self.start_status.as_ref().filter(|(owner, _)| *owner == feature)
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
            _ => unreachable!("generated Training dispatch supplied another system event"),
        }
    }

    fn project_training_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::TrainingOpenRun(value) => {
                if self.workflow.training_run.as_ref().is_none_or(|current| value.generation >= current.generation) {
                    self.workflow.training_run = Some(value);
                    self.workflow.training_history = None;
                }
                return;
            }
            ApplicationReply::TrainingHistory(value) => {
                if self.workflow.training_run.as_ref().is_some_and(|run| run.generation == value.generation) {
                    self.workflow.training_history = Some(value);
                }
                return;
            }
            ApplicationReply::TrainingInspectCheckpoint(value) => {
                self.workflow.training_checkpoint = Some(value);
                return;
            }
            ApplicationReply::TrainingPrepareResume(value) => {
                self.workflow.resume_ready = Some(value);
                return;
            }
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
    fn install_training_reply_snapshot(&mut self, snapshot: crate::generated::TrainingSnapshot) {
        match self.install_training_snapshot(snapshot) {
            Err(error) => self.error = Some(error),
            Ok((Observation::Installed, Some(detail))) => self.failed(detail),
            Ok((Observation::Installed | Observation::Current | Observation::Stale, _)) => {}
        }
    }
}

impl ApplicationModel {
    fn install_validation_snapshot(&mut self, mut value: crate::generated::ValidationSnapshot) -> Result<(), UiError> {
        let failed = value.operation.terminal.outcome == crate::generated::ComputeOperationOutcome::Failed;
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
            let outcome = super::reduction::merge_compute_state(&mut operation, value.operation.clone())?;
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
        if self.workflow.validation_details.as_ref().is_some_and(|page| page.generation != value.operation.generationfrontier) {
            self.workflow.validation_details = None;
        }
        self.workflow.validation = Some(value);
        if installed && failed { self.failed(detail); }
        Ok(())
    }
}
impl crate::generated::ValidationApplicationProjection<UiError> for ApplicationModel {
    fn project_validation_snapshot(&mut self, value: crate::generated::ValidationSnapshot) -> Result<(), UiError> {
        self.install_validation_snapshot(value)
    }
    fn project_validation_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::ValidationValidationProgress(value) => self.install_compute_snapshot(FeatureId::Validate, value.operation),
            ApplicationEvent::ValidationValidationChanged(value) => {
                if let Err(error) = self.install_validation_snapshot(value.snapshot) { self.error = Some(error); }
            }
            _ => unreachable!("generated Validation dispatch supplied another system event"),
        }
    }
    fn project_validation_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::ValidationStart(snapshot) | ApplicationReply::ValidationStop(snapshot)
            | ApplicationReply::ValidationSelectSample(snapshot) | ApplicationReply::ValidationCloseDetail(snapshot)
            | ApplicationReply::ValidationSetOverlays(snapshot) => snapshot,
            ApplicationReply::ValidationDetails(page) => {
                if self.workflow.validation.as_ref().is_some_and(|snapshot| snapshot.operation.generationfrontier == page.generation) {
                    self.workflow.validation_details = Some(page);
                }
                return;
            }
            _ => unreachable!("generated Validation dispatch supplied another system reply"),
        };
        if let Err(error) = self.install_validation_snapshot(snapshot) { self.error = Some(error); }
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
                if let Err(error) = super::reduction::merge_predict_progress(&mut self.predict_snapshot, value.snapshot) {
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
            ApplicationReply::PredictStart(snapshot) | ApplicationReply::PredictStop(snapshot) | ApplicationReply::PredictPause(snapshot) => {
                snapshot
            }
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
    use crate::generated::{ValidationApplicationProjection, ValidationChanged, ValidationProgress};
    #[test]
    fn validation_keeps_physical_generation_independent_and_rejects_old_detail_pages() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut current = model.workflow.validation.clone().unwrap();
        current.operation.generationfrontier = 2;
        current.operation.progress.sequence = 1;
        current.frame = crate::view_model::test_support::visual_frame(PresentationSourceKind::Validation, 7);
        current.contentidentity = 11;
        current.sampleidentities[0].generation = 2;
        current.sampleavailable[0] = true;
        model.project_validation_event(ApplicationEvent::ValidationValidationChanged(ValidationChanged { snapshot: current.clone() }));
        let mut old = current.clone();
        old.operation.generationfrontier = 1;
        old.frame.revision = 8;
        model.project_validation_event(ApplicationEvent::ValidationValidationChanged(ValidationChanged { snapshot: old }));
        let installed = model.workflow.validation.as_ref().unwrap();
        assert_eq!(installed.operation.generationfrontier, 2);
        assert_eq!(installed.frame.revision, 8);
        let mut progress = current.operation.clone();
        progress.progress.sequence = 2;
        model.project_validation_event(ApplicationEvent::ValidationValidationProgress(ValidationProgress { operation: progress }));
        assert_eq!(model.workflow.validation.as_ref().unwrap().contentidentity, 11);
        model.project_validation_reply(1, ApplicationReply::ValidationDetails(crate::generated::EvaluationDetailPage {
            generation: 1, total: 0, offset: 0, rows: vec![],
        }));
        assert!(model.workflow.validation_details.is_none());
        model.project_validation_reply(2, ApplicationReply::ValidationDetails(crate::generated::EvaluationDetailPage {
            generation: 2, total: 0, offset: 0, rows: vec![],
        }));
        assert!(model.workflow.validation_details.is_some());
        let retained = model.workflow.validation_details.clone();
        let mut settings = crate::generated::application_snapshot_defaults().unwrap().into_iter()
            .find_map(|fact| match fact.value { crate::generated::ApplicationSnapshot::Settings(value) => Some(value), _ => None }).unwrap();
        settings.revision += 1;
        settings.settingsstate.workflows.validate.request.compiledpath = "/later/input.bin".into();
        model.install_settings(&settings);
        assert_eq!(model.workflow.validation_details, retained);
    }
    #[test]
    fn validation_equal_physical_revisions_require_identical_product_facts() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut baseline = model.workflow.validation.clone().unwrap();
        baseline.frame = crate::view_model::test_support::visual_frame(PresentationSourceKind::Validation, 9);
        baseline.operation.generationfrontier = 3;
        baseline.contentidentity = 17;
        model.install_validation_snapshot(baseline.clone()).unwrap();
        let mut progress = baseline.clone();
        progress.operation.progress.sequence = 1;
        model.install_validation_snapshot(progress.clone()).unwrap();
        assert_eq!(model.workflow.validation.as_ref().unwrap().operation.progress.sequence, 1);
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
        assert_eq!(model.workflow.validation.as_ref().unwrap().overlays, newer.overlays);
        assert_eq!(model.workflow.validation.as_ref().unwrap().operation.generationfrontier, 3);
    }

}

#[cfg(test)]
mod training_history_tests {
    use super::*;
    use crate::generated::TrainingApplicationProjection;

    #[test]
    fn browsing_checkpoint_and_unopened_history_does_not_start_training() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let before = model.workflow.training.clone();
        let checkpoint = crate::generated::TrainingCheckpoint {
            path: "/run/checkpoint.pt".into(), attemptid: "saved-attempt".into(),
            originalweights: "/original/weights.pt".into(), originalclassdescriptor: String::new(),
            resumable: true, epoch: 4, configuration: None, classlayout: None,
            evaluatedweights: crate::generated::EvaluatedWeights::Ema,
        };
        model.project_training_reply(1, ApplicationReply::TrainingInspectCheckpoint(checkpoint.clone()));
        assert_eq!(model.workflow.training, before);
        assert_eq!(model.workflow.training_checkpoint, Some(checkpoint.clone()));
        assert!(model.workflow.resume_ready.is_none());
        model.project_training_reply(2, ApplicationReply::TrainingHistory(crate::generated::TrainingHistoryPage {
            generation: 7, nextcursor: 4096, more: true, records: vec![],
        }));
        assert!(model.workflow.training_history.is_none());
        model.project_training_reply(3, ApplicationReply::TrainingPrepareResume(checkpoint.clone()));
        assert_eq!(model.workflow.resume_ready, Some(checkpoint));
        assert_eq!(model.workflow.training, before);
    }
}
