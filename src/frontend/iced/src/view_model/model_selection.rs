use super::*;
use crate::generated::ModelArtifactDialogFact;
#[cfg(test)]
use crate::generated::{ModelArtifactInputKind, SettingsFieldValue};

#[derive(Clone)]
pub(crate) struct ModelSettingsProjection {
    pub(crate) fields: &'static ModelArtifactDialogFact,
    pub(crate) artifact_field: Option<&'static ModelArtifactDialogFact>,
    pub(crate) selection: crate::generated::ModelSettingsProjection,
}

#[cfg(test)]
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

pub(crate) fn model_settings_projection(
    settings: &GuiSettingsState,
    workflow: FeatureId,
) -> Option<ModelSettingsProjection> {
    let selection = crate::generated::project_model_settings(settings, workflow)?;
    let matching = || {
        crate::generated::MODEL_ARTIFACT_DIALOGS
            .iter()
            .filter(|dialog| {
                dialog.target.workflow == workflow
                    && crate::generated::model_dialog_predicate_matches(settings, dialog)
            })
    };
    let fields = matching().next()?;
    let artifact_field = matching().find(|dialog| dialog.target.input == selection.key.input);
    Some(ModelSettingsProjection {
        fields,
        artifact_field,
        selection,
    })
}

impl ModelSettingsProjection {
    pub(super) fn can_prepare(&self) -> bool {
        self.selection.compatible
            && (self.selection.key.source == ModelSelectionSource::Canonical
                || !self.selection.artifact.is_empty())
    }

    pub(super) fn matches(&self, selection: &ModelSelection) -> bool {
        selection.key == self.selection.key
            && (self.selection.key.source == ModelSelectionSource::Canonical
                || selection.artifact == self.selection.artifact)
    }
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
        model_settings_projection(settings, page)
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
        settings.workflows.train.modelinput = ModelArtifactInputKind::None;
        assert!(
            !model_settings_projection(&settings, FeatureId::Train)
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
                dialog.key_fields.input,
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
                    dialog.key_fields.source,
                    SettingsFieldValue::ModelSelectionSource(source),
                );
                assert_eq!(
                    model_settings_projection(&settings, row.workflow)
                        .expect("generated model workflow")
                        .can_prepare(),
                    allowed
                );
            }
            if let Some(predicate) = dialog.predicate_field_id {
                write_field(&mut settings, predicate, SettingsFieldValue::Bool(!build));
                assert!(
                    !model_settings_projection(&settings, FeatureId::Export)
                        .expect("Export selection")
                        .can_prepare()
                );
            }
        }
    }

    #[test]
    fn descriptors_and_partial_drafts_keep_complete_keys_on_every_workflow() {
        let mut model = bootstrapped();
        let mut settings = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .clone();
        for dialog in crate::generated::MODEL_ARTIFACT_DIALOGS {
            let row = compatibility_for_dialog(dialog).unwrap();
            if let Some(predicate) = dialog.predicate_field_id {
                write_field(
                    &mut settings,
                    predicate,
                    SettingsFieldValue::Bool(row.requiredexportbuildtensorrt.unwrap()),
                );
            }
            write_field(
                &mut settings,
                dialog.key_fields.input,
                SettingsFieldValue::ModelArtifactInputKind(row.input),
            );
            write_field(
                &mut settings,
                dialog.key_fields.source,
                SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Custom),
            );
            write_field(
                &mut settings,
                dialog.stable_field_id,
                SettingsFieldValue::String("/tmp/custom-model".into()),
            );
            write_field(
                &mut settings,
                dialog.key_fields.classlayoutpath,
                SettingsFieldValue::String("/tmp/first.classes.json".into()),
            );
            let accepted = accepted_model_for(&model, &settings, row.workflow);
            assert_eq!(
                accepted.selection.key.classlayoutpath,
                "/tmp/first.classes.json"
            );
            model.model_snapshot = Some(accepted);
            assert!(model.model_selection_matches(&settings, row.workflow));
            write_field(
                &mut settings,
                dialog.key_fields.classlayoutpath,
                SettingsFieldValue::String("/tmp/second.classes.json".into()),
            );
            assert!(!model.model_selection_matches(&settings, row.workflow));
            if row.canonicalallowed {
                write_field(
                    &mut settings,
                    dialog.key_fields.source,
                    SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Canonical),
                );
                model.model_snapshot = Some(accepted_model_for(&model, &settings, row.workflow));
                write_field(
                    &mut settings,
                    dialog.stable_field_id,
                    SettingsFieldValue::String("/tmp/unused-custom-model".into()),
                );
                assert!(model.model_selection_matches(&settings, row.workflow));
            }
            write_field(
                &mut settings,
                dialog.key_fields.input,
                SettingsFieldValue::ModelArtifactInputKind(ModelArtifactInputKind::None),
            );
            let partial = model_settings_projection(&settings, row.workflow).unwrap();
            assert_eq!(partial.selection.key.input, ModelArtifactInputKind::None);
            assert_eq!(
                partial.selection.key.classlayoutpath,
                "/tmp/second.classes.json"
            );
            assert!(partial.selection.artifact.is_empty());
            assert!(partial.artifact_field.is_none());
            assert!(!partial.can_prepare());
        }
        // Preserve a raw incomplete draft, including the existing signed cast.
        settings.workflows.train.request.resolution = -1;
        settings.workflows.train.request.presetname.clear();
        let partial = model_settings_projection(&settings, FeatureId::Train).unwrap();
        assert_eq!(partial.selection.key.resolution, u32::MAX);
        assert!(partial.selection.key.preset.is_empty());
        assert!(!partial.can_prepare());
    }

    #[test]
    fn model_event_before_reply_and_stale_reply_keep_newest_selection() {
        let mut model = bootstrapped();
        model
            .settings_snapshot
            .as_mut()
            .unwrap()
            .settingsstate
            .workflows
            .train
            .request
            .classlayoutpath = "/tmp/event.classes.json".into();
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
        let accepted = accepted_model_for(&model, &settings, FeatureId::Train);
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
            train.key_fields.source,
            SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Custom),
        );
        write_field(
            &mut settings,
            train.key_fields.input,
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
            validate.key_fields.source,
            SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Custom),
        );
        write_field(
            &mut settings,
            validate.key_fields.input,
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
            canonical.key_fields.source,
            SettingsFieldValue::ModelSelectionSource(ModelSelectionSource::Canonical),
        );
        write_field(
            &mut settings,
            canonical.key_fields.input,
            SettingsFieldValue::ModelArtifactInputKind(canonical.target.input),
        );
        assert!(
            model_settings_projection(&settings, FeatureId::Validate)
                .unwrap()
                .can_prepare()
        );
        assert!(!model.model_selection_matches(&settings, FeatureId::Validate));
    }
}
