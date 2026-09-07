use crate::generated::{
    ArtifactUiState, ComputeUiState, FeatureId, FileDialogFact, SettingsUiState, TrainingSnapshot,
};

#[derive(Debug, Clone)]
pub struct WorkflowModel {
    pub dataset: Option<ArtifactUiState>,
    pub training: Option<TrainingSnapshot>,
    pub validation: Option<ComputeUiState>,
    pub export: Option<ComputeUiState>,
    settings_revision: Option<u64>,
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
            validation: None,
            export: None,
            settings_revision: None,
        }
    }
}

impl WorkflowModel {
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
            ApplicationReply::TrainingStart(snapshot)
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

impl crate::generated::ValidationApplicationProjection<UiError> for ApplicationModel {
    fn project_validation_snapshot(
        &mut self,
        value: crate::generated::ComputeUiState,
    ) -> Result<(), UiError> {
        merge_compute_snapshot(&mut self.workflow.validation, value).map(|_| ())
    }

    fn project_validation_event(&mut self, event: ApplicationEvent) {
        let snapshot = match event {
            ApplicationEvent::ValidationComputeProgressEvent(value) => value.snapshot,
            ApplicationEvent::ValidationComputeChanged(value) => value.snapshot,
            _ => unreachable!("generated Validation dispatch supplied another system event"),
        };
        self.install_compute_snapshot(FeatureId::Validate, snapshot);
    }

    fn project_validation_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::ValidationStart(snapshot)
            | ApplicationReply::ValidationStop(snapshot) => snapshot,
            _ => unreachable!("generated Validation dispatch supplied another system reply"),
        };
        self.install_compute_snapshot(FeatureId::Validate, snapshot);
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
                if let Err(error) = self.install_predict_snapshot(value.snapshot) {
                    self.error = Some(error);
                }
            }
            ApplicationEvent::PredictPredictChanged(value) => {
                match self.install_predict_snapshot(value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if self.source_for(PresentationSourceKind::Predict).is_some() {
                            self.set_foreground_visual(Some(PresentationSourceKind::Predict));
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
            ApplicationReply::PredictStart(snapshot) | ApplicationReply::PredictStop(snapshot) => {
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
                        if self.presentation_model.foreground() == Some(PresentationSourceKind::Live) {
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
