use super::notices::{Origin, warning};
use super::*;

impl crate::generated::SettingsApplicationProjection<UiError> for ApplicationModel {
    fn project_settings_snapshot(&mut self, value: SettingsUiState) -> Result<(), UiError> {
        self.install_settings_snapshot(value).map(|_| ())
    }

    fn project_settings_event(&mut self, event: ApplicationEvent) {
        let ApplicationEvent::SettingsSettingsChanged(value) = event else {
            unreachable!("generated Settings dispatch supplied another system event");
        };
        if let Err(error) = self.install_settings_snapshot(value.snapshot) {
            self.report_error(crate::view_model::notices::Origin::Protocol, error);
        }
    }

    fn project_settings_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::SettingsUpdate(snapshot)
            | ApplicationReply::SettingsReset(snapshot) => snapshot,
            _ => unreachable!("generated Settings dispatch supplied another system reply"),
        };
        if let Err(error) = self.install_settings_snapshot(snapshot) {
            self.report_error(crate::view_model::notices::Origin::Protocol, error);
        }
    }
}

impl crate::generated::FileDialogApplicationProjection<UiError> for ApplicationModel {
    fn project_filedialog_snapshot(&mut self, value: FileDialogSnapshot) -> Result<(), UiError> {
        self.install_dialog_snapshot(value).map(|_| ())
    }

    fn project_filedialog_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::FileDialogFileDialogCompleted(value) => {
                let target = value.snapshot.target.clone();
                if let Err(error) = self.install_dialog_terminal(value.snapshot) {
                    self.clear_dialog_target_if_matches(&target);
                    self.report_error(crate::view_model::notices::Origin::Protocol, error);
                }
            }
            ApplicationEvent::FileDialogFileDialogFailed(value) => {
                let target = value.snapshot.target.clone();
                let generation = value.snapshot.generation;
                match self.install_dialog_terminal(value.snapshot) {
                    Err(error) => {
                        self.clear_dialog_target_if_matches(&target);
                        self.report_error(crate::view_model::notices::Origin::Protocol, error);
                    }
                    Ok(Observation::Installed | Observation::Current) => {
                        self.notices.terminal(super::notices::Origin::Dialog, 0, generation, || Some(super::notices::failure(value.detail)));
                    }
                    Ok(Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated FileDialog dispatch supplied another system event"),
        }
    }

    fn project_filedialog_reply(&mut self, correlation: u64, reply: ApplicationReply) {
        match reply {
            ApplicationReply::FileDialogOpen(snapshot) => self.install_dialog_reply(
                correlation,
                ApplicationIntentEndpoint::FileDialogOpen,
                snapshot,
            ),
            ApplicationReply::FileDialogStop(snapshot) => self.install_dialog_reply(
                correlation,
                ApplicationIntentEndpoint::FileDialogStop,
                snapshot,
            ),
            _ => unreachable!("generated FileDialog dispatch supplied another system reply"),
        }
    }
}

pub(crate) fn selected_gpu_ordinals(feature: FeatureId, state: &crate::generated::GuiSettingsState) -> &[i32] {
    match feature {
        FeatureId::Train => &state.workflows.train.request.deviceids,
        FeatureId::Validate => std::slice::from_ref(&state.workflows.validate.request.deviceid),
        FeatureId::Predict => std::slice::from_ref(&state.workflows.predict.request.deviceid),
        FeatureId::Export => std::slice::from_ref(&state.workflows.exportstate.deviceid),
        _ => unreachable!(),
    }
}

pub(crate) fn settings_constraint_bounds(constraint: crate::generated::SettingsLeafConstraint) -> Option<(f32, f32)> {
    let (minimum, maximum) = constraint.minimum.zip(constraint.maximum)?;
    let range = (minimum as f32, maximum as f32);
    (constraint.finite && minimum.is_finite() && maximum.is_finite() && range.0 < range.1)
        .then_some(range)
}

impl ApplicationModel {
    pub(super) fn observe_settings_notices(&mut self) {
        let Some(state) = &self.settings_snapshot else { return; };
        for feature in [FeatureId::Train, FeatureId::Validate, FeatureId::Predict, FeatureId::Export] {
            let selected = super::selected_gpu_ordinals(feature, &state.settingsstate);
            let unavailable = |ordinal: &&i32| state.cudadevices.iter().all(|device| device.ordinal != **ordinal);
            self.notices.condition(Origin::Gpu(feature), selected.iter().any(|ordinal| unavailable(&ordinal)), || {
                let missing: Vec<_> = selected.iter().filter(unavailable).collect();
                warning("Selected GPU unavailable", format!("{feature:?}: selected CUDA device ordinals {missing:?} are unavailable."))
            });
        }
        let missing = [crate::generated::constraint_uiuiscale(), crate::generated::constraint_uifontsize(), crate::generated::constraint_uisecondaryfontsize(), crate::generated::constraint_uimonofontsize(), crate::generated::constraint_uitextinputfontsize()].into_iter().any(|constraint| super::settings_constraint_bounds(constraint).is_none());
        self.notices.condition(Origin::Settings, missing, || warning("Settings constraints unavailable", "Native range constraints are missing; affected controls are unavailable."));
    }
    pub(super) fn install_dialog_snapshot(&mut self, value: FileDialogSnapshot) -> Result<Observation, UiError> {
        let observation = merge_dialog_snapshot(&mut self.file_dialog, value)?;
        if observation != Observation::Stale {
            self.notices.terminal(Origin::Dialog, 0, self.file_dialog.as_ref().expect("installed dialog snapshot").generation, || None);
        }
        Ok(observation)
    }
}
