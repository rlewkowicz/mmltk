use super::*;
use crate::generated::{ModelArtifactDialogFact, ModelArtifactInputKind, SettingsFieldValue};

#[derive(Clone)]
pub(crate) struct ModelSettingsProjection {
    pub(crate) fields: &'static ModelArtifactDialogFact,
    pub(crate) artifact_field: Option<&'static ModelArtifactDialogFact>,
    pub(crate) source: ModelSelectionSource,
    pub(crate) input: ModelArtifactInputKind,
    pub(crate) export_build_tensorrt: bool,
    pub(crate) preset: String,
    pub(crate) resolution: i32,
    pub(crate) artifact: String,
}

fn compatibility_for_dialog(
    dialog: &ModelArtifactDialogFact,
) -> Option<&'static crate::generated::ModelSelectionCompatibility> {
    crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
        .iter()
        .find(|row| {
            row.workflow == dialog.target.workflow
                && row.input == dialog.target.input
                && row.artifactfieldpath == dialog.field_path
        })
}

fn read_field(
    settings: &GuiSettingsState,
    stable_id: u64,
) -> Option<crate::generated::SettingsFieldValue> {
    crate::generated::read_settings_field(settings, stable_id).ok()
}

fn predicate_matches(settings: &GuiSettingsState, dialog: &ModelArtifactDialogFact) -> bool {
    match (
        dialog.predicate_field_id,
        compatibility_for_dialog(dialog).and_then(|row| row.requiredexportbuildtensorrt),
    ) {
        (None, None) => true,
        (Some(stable_id), Some(required)) => {
            read_field(settings, stable_id) == Some(SettingsFieldValue::Bool(required))
        }
        _ => false,
    }
}

pub(crate) fn model_settings_projection(
    settings: &GuiSettingsState,
    workflow: FeatureId,
) -> Option<ModelSettingsProjection> {
    let fields = crate::generated::MODEL_ARTIFACT_DIALOGS
        .iter()
        .find(|dialog| dialog.target.workflow == workflow && predicate_matches(settings, dialog))?;
    let SettingsFieldValue::ModelSelectionSource(source) =
        read_field(settings, fields.source_field_id)?
    else {
        return None;
    };
    let SettingsFieldValue::ModelArtifactInputKind(input) =
        read_field(settings, fields.input_field_id)?
    else {
        return None;
    };
    let SettingsFieldValue::String(preset) = read_field(settings, fields.preset_field_id)? else {
        return None;
    };
    let SettingsFieldValue::I32(resolution) = read_field(settings, fields.resolution_field_id)?
    else {
        return None;
    };
    let export_build_tensorrt = match fields.predicate_field_id {
        Some(stable_id) => {
            let SettingsFieldValue::Bool(value) = read_field(settings, stable_id)? else {
                return None;
            };
            value
        }
        None => false,
    };
    let artifact_field = crate::generated::MODEL_ARTIFACT_DIALOGS
        .iter()
        .find(|dialog| {
            dialog.target.workflow == workflow
                && dialog.target.input == input
                && predicate_matches(settings, dialog)
        });
    let artifact = artifact_field
        .and_then(|dialog| read_field(settings, dialog.stable_field_id))
        .and_then(|value| match value {
            SettingsFieldValue::String(path) => Some(path),
            _ => None,
        })
        .unwrap_or_default();
    Some(ModelSettingsProjection {
        fields,
        artifact_field,
        source,
        input,
        export_build_tensorrt,
        preset,
        resolution,
        artifact,
    })
}

#[derive(Clone)]
pub(super) struct EffectiveModelSelection {
    pub(super) workflow: FeatureId,
    pub(super) source: ModelSelectionSource,
    pub(super) input: ModelArtifactInputKind,
    pub(super) export_build_tensorrt: bool,
    pub(super) preset: String,
    pub(super) resolution: u32,
    pub(super) artifact: String,
}

impl EffectiveModelSelection {
    pub(super) fn can_prepare(self) -> bool {
        crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
            .iter()
            .any(|row| {
                row.workflow == self.workflow
                    && row.input == self.input
                    && row
                        .requiredexportbuildtensorrt
                        .is_none_or(|required| required == self.export_build_tensorrt)
                    && match self.source {
                        ModelSelectionSource::Canonical => row.canonicalallowed,
                        ModelSelectionSource::Custom => {
                            row.customallowed && !self.artifact.is_empty()
                        }
                    }
            })
    }

    pub(super) fn matches(self, selection: &ModelSelection) -> bool {
        selection.key.workflow == self.workflow
            && selection.key.source == self.source
            && selection.key.input == self.input
            && selection.key.preset == self.preset
            && selection.key.resolution == self.resolution
            && (self.source == ModelSelectionSource::Canonical
                || selection.artifact == self.artifact)
    }
}

pub(super) fn effective_model_selection(
    settings: &GuiSettingsState,
    page: FeatureId,
) -> Option<EffectiveModelSelection> {
    let projection = model_settings_projection(settings, page)?;
    Some(EffectiveModelSelection {
        workflow: page,
        source: projection.source,
        input: projection.input,
        export_build_tensorrt: projection.export_build_tensorrt,
        preset: projection.preset,
        resolution: projection.resolution as u32,
        artifact: projection.artifact,
    })
}

impl ApplicationModel {
    pub fn model_selection_matches(&self, settings: &GuiSettingsState, page: FeatureId) -> bool {
        let Some(selection) = self
            .model_snapshot
            .as_ref()
            .filter(|value| {
                !value.active && value.terminal.outcome == ModelSelectionOutcome::Accepted
            })
            .map(|value| &value.selection)
        else {
            return false;
        };
        effective_model_selection(settings, page)
            .is_some_and(|effective| effective.matches(selection))
    }
}

impl crate::generated::ModelApplicationProjection<UiError> for ApplicationModel {
    fn project_model_snapshot(&mut self, value: ModelUiState) -> Result<(), UiError> {
        merge_model_snapshot(&mut self.model_snapshot, value).map(|_| ())
    }

    fn project_model_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::ModelModelProgressChanged(value) => {
                if let Some(snapshot) = self.model_snapshot.as_mut() {
                    if value.active
                        && (value.generation > snapshot.generation
                            || (value.generation == snapshot.generation && snapshot.active))
                    {
                        snapshot.generation = value.generation;
                        snapshot.active = true;
                        snapshot.progress = value.progress;
                        snapshot.terminal = value.terminal;
                    }
                }
            }
            ApplicationEvent::ModelModelChanged(value) => {
                let rejected = value.snapshot.terminal.outcome == ModelSelectionOutcome::Rejected;
                let detail = value.snapshot.terminal.detail.clone();
                match merge_model_snapshot(&mut self.model_snapshot, value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if rejected {
                            self.failed(detail);
                        }
                    }
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated Model dispatch supplied another system event"),
        }
    }

    fn project_model_reply(&mut self, correlation: u64, reply: ApplicationReply) {
        match reply {
            ApplicationReply::ModelSelect(snapshot) => {
                let Some(PendingDetail::ModelSelect(receipt)) =
                    self.pending_detail(correlation).cloned()
                else {
                    self.error = Some(UiError::protocol(
                        "Model Select reply did not match its pending detail",
                    ));
                    return;
                };
                let reply_generation = snapshot.generation;
                let outcome = snapshot.terminal.outcome;
                let detail = snapshot.terminal.detail.clone();
                match merge_model_snapshot(&mut self.model_snapshot, snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Stale)
                        if self
                            .model_snapshot
                            .as_ref()
                            .is_some_and(|installed| installed.generation == reply_generation) => {}
                    Ok(Observation::Stale) => {
                        self.error = Some(UiError::invalid(
                            "Model selection is stale because its inputs changed.",
                        ));
                    }
                    Ok(observation @ (Observation::Installed | Observation::Current)) => {
                        if self.settings_snapshot.as_ref().map(|value| value.revision)
                            != Some(receipt.settings_revision)
                            || (outcome == ModelSelectionOutcome::Accepted
                                && self.model_snapshot.as_ref().is_some_and(|value| {
                                    value.selection.key.workflow != receipt.workflow
                                }))
                        {
                            self.error = Some(UiError::invalid(
                                "Model selection is stale because its inputs changed.",
                            ));
                        } else if outcome == ModelSelectionOutcome::Rejected
                            && observation == Observation::Installed
                        {
                            self.failed(detail);
                        }
                    }
                }
            }
            ApplicationReply::ModelStop(snapshot) => {
                if !matches!(
                    self.pending_detail(correlation),
                    Some(PendingDetail::ModelStop(_))
                ) {
                    self.error = Some(UiError::protocol(
                        "Model Stop reply did not match its pending detail",
                    ));
                    return;
                }
                if let Err(error) = merge_model_snapshot(&mut self.model_snapshot, snapshot) {
                    self.error = Some(error);
                }
            }
            _ => unreachable!("generated Model dispatch supplied another system reply"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::*;

    fn write_field(settings: &mut GuiSettingsState, stable_id: u64, value: SettingsFieldValue) {
        crate::generated::apply_settings_field(settings, stable_id, value).unwrap();
    }

    #[test]
    fn exact_model_projection_rejects_none_and_covers_generated_rows() {
        let mut settings = bootstrapped()
            .settings_snapshot
            .expect("Settings snapshot")
            .settingsstate;
        assert!(
            !effective_model_selection(&settings, FeatureId::Train)
                .expect("Train selection")
                .can_prepare()
        );

        for dialog in crate::generated::MODEL_ARTIFACT_DIALOGS {
            let row = compatibility_for_dialog(dialog).unwrap();
            let build = row.requiredexportbuildtensorrt.unwrap_or(false);
            if let Some(predicate) = dialog.predicate_field_id {
                write_field(&mut settings, predicate, SettingsFieldValue::Bool(build));
            }
            write_field(
                &mut settings,
                dialog.input_field_id,
                SettingsFieldValue::ModelArtifactInputKind(row.input),
            );
            write_field(
                &mut settings,
                dialog.stable_field_id,
                SettingsFieldValue::String("/tmp/model".into()),
            );
            for (source, allowed) in [
                (ModelSelectionSource::Canonical, row.canonicalallowed),
                (ModelSelectionSource::Custom, row.customallowed),
            ] {
                write_field(
                    &mut settings,
                    dialog.source_field_id,
                    SettingsFieldValue::ModelSelectionSource(source),
                );
                assert_eq!(
                    effective_model_selection(&settings, row.workflow)
                        .expect("generated model workflow")
                        .can_prepare(),
                    allowed
                );
            }
            if let Some(predicate) = dialog.predicate_field_id {
                write_field(&mut settings, predicate, SettingsFieldValue::Bool(!build));
                assert!(
                    !effective_model_selection(&settings, FeatureId::Export)
                        .expect("Export selection")
                        .can_prepare()
                );
            }
        }
    }

    #[test]
    fn model_event_before_reply_and_stale_reply_keep_newest_selection() {
        let mut model = bootstrapped();
        let settings = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .clone();
        let receipt = model
            .workflow
            .model_selection_receipt(FeatureId::Train)
            .unwrap();
        let correlation = model.begin_model_select_intent(receipt).unwrap();
        let accepted = accepted_train_model(&model);
        let mut newer = accepted.clone();
        newer.generation += 1;
        model.reduce_event(ApplicationEvent::ModelModelChanged(
            crate::generated::ModelChanged {
                snapshot: newer.clone(),
            },
        ));
        assert!(model.model_selection_matches(&settings, FeatureId::Train));
        model.reduce_reply(correlation, Ok(ApplicationReply::ModelSelect(accepted)));
        assert_eq!(model.model_snapshot, Some(newer));
        assert!(model.model_selection_matches(&settings, FeatureId::Train));
        assert_eq!(
            model.error.as_ref().unwrap().kind,
            UiErrorKind::InvalidIntent
        );
    }

    #[test]
    fn rejected_model_observation_preserves_native_failure() {
        let mut model = bootstrapped();
        let settings = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .clone();
        let mut rejected = model.model_snapshot.clone().unwrap();
        rejected.generation += 1;
        rejected.terminal.outcome = ModelSelectionOutcome::Rejected;
        rejected.terminal.detail = "native model failure".into();
        model.reduce_event(ApplicationEvent::ModelModelChanged(
            crate::generated::ModelChanged {
                snapshot: rejected.clone(),
            },
        ));
        assert_eq!(model.model_snapshot, Some(rejected));
        assert!(!model.model_selection_matches(&settings, FeatureId::Train));
        assert_eq!(model.error.as_ref().unwrap().detail, "native model failure");
    }

    #[test]
    fn custom_artifact_change_and_catalog_normalization_invalidate_selection() {
        let mut model = bootstrapped();
        let mut settings = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .clone();
        let train = crate::generated::MODEL_ARTIFACT_DIALOGS
            .iter()
            .find(|dialog| dialog.target.workflow == FeatureId::Train)
            .unwrap();
        write_field(
            &mut settings,
            train.source_field_id,
            SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Custom),
        );
        write_field(
            &mut settings,
            train.input_field_id,
            SettingsFieldValue::ModelArtifactInputKind(train.target.input),
        );
        write_field(
            &mut settings,
            train.stable_field_id,
            SettingsFieldValue::String("/tmp/original.pt".into()),
        );
        let selected = accepted_model_for(&model, &settings, FeatureId::Train);
        model.reduce_event(ApplicationEvent::ModelModelChanged(
            crate::generated::ModelChanged { snapshot: selected },
        ));
        assert!(model.model_selection_matches(&settings, FeatureId::Train));
        write_field(
            &mut settings,
            train.stable_field_id,
            SettingsFieldValue::String("/tmp/replaced.pt".into()),
        );
        assert!(!model.model_selection_matches(&settings, FeatureId::Train));

        let validate = crate::generated::MODEL_ARTIFACT_DIALOGS
            .iter()
            .find(|dialog| {
                dialog.target.workflow == FeatureId::Validate
                    && dialog.target.input == ModelArtifactInputKind::Onnx
            })
            .unwrap();
        write_field(
            &mut settings,
            validate.source_field_id,
            SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Custom),
        );
        write_field(
            &mut settings,
            validate.input_field_id,
            SettingsFieldValue::ModelArtifactInputKind(validate.target.input),
        );
        write_field(
            &mut settings,
            validate.stable_field_id,
            SettingsFieldValue::String("/tmp/validate.onnx".into()),
        );
        let selected = accepted_model_for(&model, &settings, FeatureId::Validate);
        model.reduce_event(ApplicationEvent::ModelModelChanged(
            crate::generated::ModelChanged { snapshot: selected },
        ));
        assert!(model.model_selection_matches(&settings, FeatureId::Validate));
        let canonical = crate::generated::MODEL_ARTIFACT_DIALOGS
            .iter()
            .find(|dialog| {
                dialog.target.workflow == FeatureId::Validate
                    && dialog.target.input == ModelArtifactInputKind::Weights
            })
            .unwrap();
        write_field(
            &mut settings,
            canonical.source_field_id,
            SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Canonical),
        );
        write_field(
            &mut settings,
            canonical.input_field_id,
            SettingsFieldValue::ModelArtifactInputKind(canonical.target.input),
        );
        assert!(
            effective_model_selection(&settings, FeatureId::Validate)
                .unwrap()
                .can_prepare()
        );
        assert!(!model.model_selection_matches(&settings, FeatureId::Validate));
    }
}
