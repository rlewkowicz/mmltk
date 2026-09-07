use crate::generated::{
    GuiSettingsState, SettingsFieldValue, SettingsUiState, SettingsUpdateRequest,
    SettingsValueUpdate,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditCadence {
    Debounced,
    Immediate,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SettingsEdit {
    pub update: SettingsValueUpdate,
    pub cadence: EditCadence,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditSchedule {
    Debounce(u64),
    FlushNow,
}

#[derive(Debug, Clone)]
pub struct SettingsModel {
    pub open: bool,
    pub reset_confirmation: bool,
    pub draft: Option<GuiSettingsState>,
    queued: Vec<SettingsValueUpdate>,
    debounce_generation: u64,
    update_in_flight: bool,
}

impl Default for SettingsModel {
    fn default() -> Self {
        Self {
            open: false,
            reset_confirmation: false,
            draft: None,
            queued: Vec::with_capacity(crate::generated::SETTINGS_EDIT_CAPACITY),
            debounce_generation: 0,
            update_in_flight: false,
        }
    }
}

impl SettingsModel {
    pub fn typography(
        &self,
        authoritative: crate::view_model::Typography,
    ) -> crate::view_model::Typography {
        self.draft
            .as_ref()
            .map_or(authoritative, |draft| crate::view_model::Typography {
                primary: draft.ui.fontsize,
                secondary: draft.ui.secondaryfontsize,
                monospace: draft.ui.monofontsize,
                text_input: draft.ui.textinputfontsize,
            })
    }

    pub fn install(&mut self, snapshot: &SettingsUiState) {
        if !self.update_in_flight && self.queued.is_empty() {
            self.draft = Some(snapshot.settingsstate.clone());
        }
    }

    pub fn edit(
        &mut self,
        cadence: EditCadence,
        apply: impl FnOnce(&mut GuiSettingsState) -> SettingsValueUpdate,
    ) -> Result<EditSchedule, String> {
        let draft = self
            .draft
            .as_mut()
            .ok_or_else(|| "settings draft is not installed".to_owned())?;
        let edit = SettingsEdit {
            update: apply(draft),
            cadence,
        };
        if let Some(queued) = self
            .queued
            .iter_mut()
            .find(|queued| queued.path == edit.update.path)
        {
            *queued = edit.update;
        } else {
            if self.queued.len() >= crate::generated::SETTINGS_EDIT_CAPACITY {
                return Err("settings edit queue capacity exceeded".to_owned());
            }
            self.queued.push(edit.update);
        }
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        Ok(match edit.cadence {
            EditCadence::Debounced => EditSchedule::Debounce(self.debounce_generation),
            EditCadence::Immediate => EditSchedule::FlushNow,
        })
    }

    pub fn edit_group<const COUNT: usize>(
        &mut self,
        cadence: EditCadence,
        apply: impl FnOnce(&mut GuiSettingsState) -> [SettingsValueUpdate; COUNT],
    ) -> Result<EditSchedule, String> {
        let mut candidate = self
            .draft
            .clone()
            .ok_or_else(|| "settings draft is not installed".to_owned())?;
        let updates = apply(&mut candidate);
        for (index, update) in updates.iter().enumerate() {
            if update.path.is_empty()
                || updates[..index]
                    .iter()
                    .any(|prior| prior.path == update.path)
            {
                return Err("settings edit group contains an invalid or duplicate field".to_owned());
            }
        }
        let additions = updates
            .iter()
            .filter(|update| !self.queued.iter().any(|queued| queued.path == update.path))
            .count();
        if self.queued.len() + additions > crate::generated::SETTINGS_EDIT_CAPACITY {
            return Err("settings edit queue capacity exceeded".to_owned());
        }
        self.draft = Some(candidate);
        for update in updates {
            if let Some(queued) = self
                .queued
                .iter_mut()
                .find(|queued| queued.path == update.path)
            {
                *queued = update;
            } else {
                self.queued.push(update);
            }
        }
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        Ok(match cadence {
            EditCadence::Debounced => EditSchedule::Debounce(self.debounce_generation),
            EditCadence::Immediate => EditSchedule::FlushNow,
        })
    }

    pub fn edit_fields<const COUNT: usize>(
        &mut self,
        cadence: EditCadence,
        edits: [(u64, SettingsFieldValue); COUNT],
    ) -> Result<EditSchedule, String> {
        let mut candidate = self
            .draft
            .clone()
            .ok_or_else(|| "settings draft is not installed".to_owned())?;
        let mut queued = self.queued.clone();
        for (index, (stable_id, _)) in edits.iter().enumerate() {
            if *stable_id == 0 || edits[..index].iter().any(|(prior, _)| prior == stable_id) {
                return Err(
                    "settings identity group contains an invalid or duplicate field".to_owned(),
                );
            }
        }
        for (stable_id, value) in edits {
            let update = crate::generated::apply_settings_field(&mut candidate, stable_id, value)
                .map_err(|error| format!("settings identity edit failed: {error:?}"))?;
            if let Some(prior) = queued.iter_mut().find(|prior| prior.path == update.path) {
                *prior = update;
            } else {
                if queued.len() >= crate::generated::SETTINGS_EDIT_CAPACITY {
                    return Err("settings edit queue capacity exceeded".to_owned());
                }
                queued.push(update);
            }
        }
        self.draft = Some(candidate);
        self.queued = queued;
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        Ok(match cadence {
            EditCadence::Debounced => EditSchedule::Debounce(self.debounce_generation),
            EditCadence::Immediate => EditSchedule::FlushNow,
        })
    }

    pub fn debounce_elapsed(&self, generation: u64) -> bool {
        generation != 0
            && generation == self.debounce_generation
            && !self.queued.is_empty()
            && !self.update_in_flight
    }

    pub fn take_request(&mut self) -> Option<SettingsUpdateRequest> {
        if self.update_in_flight || self.queued.is_empty() {
            return None;
        }
        self.update_in_flight = true;
        let request_count = self
            .queued
            .len()
            .min(crate::generated::SETTINGS_UPDATE_CAPACITY);
        Some(SettingsUpdateRequest {
            updates: self.queued.drain(..request_count).collect(),
        })
    }

    pub fn settle_success(&mut self, authoritative: &SettingsUiState) {
        self.update_in_flight = false;
        if self.queued.is_empty() {
            self.draft = Some(authoritative.settingsstate.clone());
        }
    }

    pub fn settle_failure(&mut self, authoritative: Option<&SettingsUiState>) {
        self.update_in_flight = false;
        self.queued.clear();
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        self.draft = authoritative.map(|snapshot| snapshot.settingsstate.clone());
    }

    pub fn has_local_edits(&self) -> bool {
        self.update_in_flight || !self.queued.is_empty()
    }

    #[cfg(test)]
    pub fn update_in_flight(&self) -> bool {
        self.update_in_flight
    }

    #[cfg(test)]
    pub fn queued_len(&self) -> usize {
        self.queued.len()
    }

    pub fn close(&mut self) {
        self.open = false;
        self.reset_confirmation = false;
    }

    pub fn diagnostics_visible(&self) -> bool {
        self.draft
            .as_ref()
            .is_some_and(|draft| draft.ui.showworkspaceperformance)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings_snapshot() -> SettingsUiState {
        crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find_map(|fact| match fact.value {
                crate::generated::ApplicationSnapshot::Settings(value) => Some(value),
                _ => None,
            })
            .unwrap()
    }

    #[test]
    fn generated_helpers_mutate_the_single_typed_draft_and_coalesce_by_field() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        assert_eq!(
            model.queued.capacity(),
            crate::generated::SETTINGS_EDIT_CAPACITY
        );
        model.install(&snapshot);
        let first = model
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_uidarkmode(draft, true)
            })
            .unwrap();
        let second = model
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_uidarkmode(draft, false)
            })
            .unwrap();
        assert!(matches!(first, EditSchedule::Debounce(_)));
        assert!(matches!(second, EditSchedule::Debounce(_)));
        assert_eq!(model.queued_len(), 1);
        assert!(!model.draft.as_ref().unwrap().ui.darkmode);
        let request = model.take_request().unwrap();
        assert_eq!(request.updates.len(), 1);
        assert_eq!(
            request.updates[0].path,
            crate::generated::update_uidarkmode(false).path
        );
    }

    #[test]
    fn grouped_edits_commit_the_typed_draft_and_queue_atomically() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        let original = model.draft.clone().unwrap();
        assert!(
            model
                .edit_group(EditCadence::Debounced, |draft| {
                    let first = crate::generated::edit_uidarkmode(draft, true);
                    let duplicate = crate::generated::edit_uidarkmode(draft, false);
                    [first, duplicate]
                })
                .is_err()
        );
        assert_eq!(model.draft, Some(original));
        assert_eq!(model.queued_len(), 0);

        model
            .edit_group(EditCadence::Immediate, |draft| {
                [
                    crate::generated::edit_uidarkmode(draft, true),
                    crate::generated::edit_uishowworkspaceperformance(draft, true),
                ]
            })
            .unwrap();
        assert!(model.draft.as_ref().unwrap().ui.darkmode);
        assert!(model.draft.as_ref().unwrap().ui.showworkspaceperformance);
        assert_eq!(model.queued_len(), 2);
    }

    #[test]
    fn identity_edits_are_typed_atomic_mutability_aware_and_canonical() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        let leaf = |path: &str| {
            crate::generated::SETTINGS_LEAVES
                .iter()
                .find(|leaf| leaf.path == path)
                .unwrap()
        };
        let artifact = crate::generated::MODEL_ARTIFACT_DIALOGS.first().unwrap();
        model
            .edit_fields(
                EditCadence::Debounced,
                [(
                    artifact.stable_field_id,
                    SettingsFieldValue::String("/tmp/model.pth".into()),
                )],
            )
            .unwrap();
        assert_eq!(
            crate::generated::read_settings_field(
                model.draft.as_ref().unwrap(),
                artifact.stable_field_id,
            ),
            Ok(SettingsFieldValue::String("/tmp/model.pth".into()))
        );
        assert_eq!(model.queued[0].path, artifact.field_path);

        let dark = leaf("ui.dark_mode");
        model
            .edit_fields(
                EditCadence::Immediate,
                [(dark.stable_field_id, SettingsFieldValue::Bool(true))],
            )
            .unwrap();
        assert!(model.draft.as_ref().unwrap().ui.darkmode);

        let immutable = leaf("workflows.explore.class_catalog_identity");
        assert!(
            crate::generated::read_settings_field(
                model.draft.as_ref().unwrap(),
                immutable.stable_field_id
            )
            .is_ok()
        );
        let before = model.draft.clone();
        let queued = model.queued.clone();
        for edits in [
            [(0, SettingsFieldValue::Bool(true))],
            [(immutable.stable_field_id, SettingsFieldValue::U64(7))],
            [(
                dark.stable_field_id,
                SettingsFieldValue::String("wrong".into()),
            )],
            [(u64::MAX, SettingsFieldValue::Bool(true))],
        ] {
            assert!(model.edit_fields(EditCadence::Debounced, edits).is_err());
            assert_eq!(model.draft, before);
            assert_eq!(model.queued, queued);
        }
        assert!(
            model
                .edit_fields(
                    EditCadence::Debounced,
                    [
                        (dark.stable_field_id, SettingsFieldValue::Bool(false)),
                        (dark.stable_field_id, SettingsFieldValue::Bool(true)),
                    ],
                )
                .is_err()
        );
        assert_eq!(model.draft, before);
        assert_eq!(model.queued, queued);
    }

    #[test]
    fn identity_group_capacity_is_checked_after_coalescing() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        let dark = crate::generated::SETTINGS_LEAVES
            .iter()
            .find(|leaf| leaf.path == "ui.dark_mode")
            .unwrap();
        model.queued = (0..crate::generated::SETTINGS_EDIT_CAPACITY)
            .map(|index| SettingsValueUpdate {
                path: if index == 0 {
                    dark.path.into()
                } else {
                    format!("occupied.{index}")
                },
                value: crate::application_codec::Value::Null,
            })
            .collect();
        assert!(
            model
                .edit_fields(
                    EditCadence::Immediate,
                    [(dark.stable_field_id, SettingsFieldValue::Bool(true))],
                )
                .is_ok()
        );
        let other = crate::generated::SETTINGS_LEAVES
            .iter()
            .find(|leaf| leaf.path == "ui.show_workspace_performance")
            .unwrap();
        let before = model.draft.clone();
        assert!(
            model
                .edit_fields(
                    EditCadence::Immediate,
                    [(other.stable_field_id, SettingsFieldValue::Bool(true))],
                )
                .is_err()
        );
        assert_eq!(model.draft, before);
    }

    #[test]
    fn stale_debounce_generations_do_not_flush_and_immediate_edits_do() {
        assert_eq!(crate::app::SETTINGS_PERSIST_DEBOUNCE_MS, 450);
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        let EditSchedule::Debounce(stale) = model
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_uiuiscale(draft, 1.1)
            })
            .unwrap()
        else {
            panic!("debounced edit")
        };
        let EditSchedule::Debounce(current) = model
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_uifontsize(draft, 16.0)
            })
            .unwrap()
        else {
            panic!("debounced edit")
        };
        assert!(!model.debounce_elapsed(stale));
        assert!(model.debounce_elapsed(current));
        assert_eq!(
            model
                .edit(EditCadence::Immediate, |draft| {
                    crate::generated::edit_currentview(draft, crate::generated::FeatureId::Explore)
                })
                .unwrap(),
            EditSchedule::FlushNow
        );
    }

    #[test]
    fn later_edits_survive_one_in_flight_request_and_failure_rolls_back() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        model
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_uidarkmode(draft, true)
            })
            .unwrap();
        let request = model.take_request().unwrap();
        assert_eq!(request.updates.len(), 1);
        model
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_uishowworkspaceperformance(draft, true)
            })
            .unwrap();
        assert!(model.take_request().is_none());
        model.settle_success(&snapshot);
        assert_eq!(model.queued_len(), 1);
        assert!(model.draft.as_ref().unwrap().ui.showworkspaceperformance);
        assert!(model.take_request().is_some());
        model.settle_failure(Some(&snapshot));
        assert!(!model.has_local_edits());
        assert_eq!(model.draft, Some(snapshot.settingsstate));
    }

    #[test]
    fn diagnostics_visibility_tracks_the_current_typed_draft() {
        let mut snapshot = settings_snapshot();
        snapshot.settingsstate.ui.showworkspaceperformance = true;
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        assert!(model.diagnostics_visible());
        model
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_uishowworkspaceperformance(draft, false)
            })
            .unwrap();
        assert!(!model.diagnostics_visible());
    }

    #[test]
    fn workspace_aspect_uses_the_reflected_enum_and_generated_draft_helper() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        model
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_uiworkspaceaspectratio(
                    draft,
                    crate::generated::WorkspaceAspectRatio::Square,
                )
            })
            .unwrap();
        assert_eq!(
            model.draft.as_ref().unwrap().ui.workspaceaspectratio,
            crate::generated::WorkspaceAspectRatio::Square
        );
        let request = model.take_request().unwrap();
        assert_eq!(
            request.updates[0].path,
            crate::generated::update_uiworkspaceaspectratio(
                crate::generated::WorkspaceAspectRatio::Square
            )
            .path
        );
    }

    #[test]
    fn reconnect_installs_authoritative_typed_state_after_local_failure() {
        let mut authoritative = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&authoritative);
        model
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_uidarkmode(draft, true)
            })
            .unwrap();
        assert!(model.take_request().is_some());
        model.settle_failure(Some(&authoritative));
        assert_eq!(model.draft, Some(authoritative.settingsstate.clone()));

        authoritative.revision += 1;
        authoritative.settingsstate.ui.darkmode = true;
        model.install(&authoritative);
        assert_eq!(model.draft, Some(authoritative.settingsstate));
    }
}
