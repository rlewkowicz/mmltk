use crate::generated::{
    GuiSettingsState, SettingsFieldValue, SettingsUiState, SettingsUpdateRequest,
    SettingsValueUpdate,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditCadence {
    Debounced,
    Immediate,
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
    queued_lanes: Option<crate::generated::TrainLaneConfiguration>,
    queued_model_count: Option<u32>,
    model_count_in_flight: bool,
    pub recipe_model: Option<u64>,
    pub execution_locked: [bool; 3],
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
            queued_lanes: None,
            queued_model_count: None,
            model_count_in_flight: false,
            recipe_model: None,
            execution_locked: [false; 3],
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
        if !self.update_in_flight && (self.queued.is_empty() && self.queued_lanes.is_none() && self.queued_model_count.is_none()) {
            self.draft = Some(snapshot.settingsstate.clone());
            self.reconcile_recipe_scope();
        }
    }

    pub fn edit(
        &mut self,
        cadence: EditCadence,
        apply: impl FnOnce(&mut GuiSettingsState) -> SettingsValueUpdate,
    ) -> Result<EditSchedule, String> {
        self.edit_group(cadence, |draft| [apply(draft)])
    }

    fn queue_capacity(&self) -> usize {
        if self.queued_lanes.is_some() || self.queued_model_count.is_some() {
            crate::generated::SETTINGS_UPDATE_CAPACITY
        } else {
            crate::generated::SETTINGS_EDIT_CAPACITY
        }
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
            if self.field_locked(&update.path) { return Err("Execution settings are locked during admitted work.".into()); }
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
        if self.queued.len() + additions > self.queue_capacity() {
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

    pub fn replace_training_lanes(
        &mut self,
        value: crate::generated::TrainLaneConfiguration,
        execution_active: bool,
    ) -> Result<EditSchedule, String> {
        if self.queued.len() > crate::generated::SETTINGS_UPDATE_CAPACITY { return Err("Settings transaction capacity exceeded.".into()); }
        if execution_active || self.execution_locked[0] { return Err("Training configuration is locked during an admitted run.".into()); }
        if self.model_count_in_flight { return Err("Model membership is updating.".into()); }
        let draft = self.draft.as_mut().ok_or("settings draft is not installed")?;
        draft.workflows.train.request.laneconfiguration = value.clone();
        self.queued_lanes = Some(value);
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        Ok(EditSchedule::FlushNow)
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
            if self.field_locked(&update.path) { return Err("Execution settings are locked during admitted work.".into()); }
            if let Some(prior) = queued.iter_mut().find(|prior| prior.path == update.path) {
                *prior = update;
            } else {
                if queued.len() >= self.queue_capacity() {
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
            && !(self.queued.is_empty() && self.queued_lanes.is_none() && self.queued_model_count.is_none())
            && !self.update_in_flight
    }

    pub fn take_request(&mut self, training_active: bool) -> Option<SettingsUpdateRequest> {
        if self.update_in_flight {
            return None;
        }
        let train_locked = training_active || self.execution_locked[0];
        let locked = [train_locked, self.execution_locked[1], self.execution_locked[2]];
        let updates: Vec<_> = self.queued.extract_if(.., |update| {
            !Self::execution_field_locked(&update.path, locked)
        }).take(crate::generated::SETTINGS_UPDATE_CAPACITY).collect();
        let laneconfiguration = if train_locked { None } else { self.queued_lanes.take() };
        let trainingmodelcount = if train_locked { None } else { self.queued_model_count.take() };
        if updates.is_empty() && laneconfiguration.is_none() && trainingmodelcount.is_none() { return None; }
        self.update_in_flight = true;
        self.model_count_in_flight = trainingmodelcount.is_some() || laneconfiguration.as_ref().is_some_and(|configuration|
            configuration.mode != crate::generated::TrainLaneMode::SharedGradients && configuration.models.is_empty());
        Some(SettingsUpdateRequest { updates, laneconfiguration, trainingmodelcount })
    }

    pub fn training_membership_pending(&self) -> bool { self.model_count_in_flight || self.queued_model_count.is_some() }

    pub fn resize_training_models(&mut self, count: u32) -> Result<EditSchedule, String> {
        if self.execution_locked[0] { return Err("Training configuration is locked during an admitted run.".into()); }
        if count == 0 || count as usize > crate::generated::TRAINING_MODEL_CAPACITY { return Err("Invalid native model count.".into()); }
        if self.queued.len() > crate::generated::SETTINGS_UPDATE_CAPACITY { return Err("settings transaction capacity exceeded".into()); }
        let draft = self.draft.as_mut().ok_or("settings draft is not installed")?;
        // Membership, seeds, IDs and recipe copies arrive only in the native reply.
        draft.workflows.train.request.lanes = count as i32;
        self.queued.retain(|update| update.path != "workflows.train.request.lanes");
        self.queued_model_count = Some(count);
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        Ok(EditSchedule::FlushNow)
    }

    fn field_locked(&self, path: &str) -> bool {
        Self::execution_field_locked(path, self.execution_locked)
    }

    fn execution_field_locked(path: &str, locked: [bool; 3]) -> bool {
        // These selectors derive the native request or select its model. Keep
        // their queued drafts with the request until admission has settled.
        (locked[0] && matches!(path, "workflows.train.compiled_dataset_dir" | "workflows.train.use_compiled_directory_defaults" | "workflows.train.model_source" | "workflows.train.model_input")) ||
        ["workflows.train.request.", "workflows.validate.request.", "workflows.predict.request."].iter().zip(locked)
            .any(|(prefix, active)| active && path.starts_with(prefix))
    }

    fn reconcile_recipe_scope(&mut self) {
        if self.recipe_model.is_some_and(|id| self.draft.as_ref().is_none_or(|draft|
            !draft.workflows.train.request.laneconfiguration.models.iter().any(|entry| entry.modelid == id))) {
            self.recipe_model = None;
        }
    }

    pub fn settle_success(&mut self, authoritative: &SettingsUiState) {
        self.update_in_flight = false;
        if self.model_count_in_flight {
            if let Some(draft) = &mut self.draft {
                draft.workflows.train.request.laneconfiguration = authoritative.settingsstate.workflows.train.request.laneconfiguration.clone();
                if self.queued_model_count.is_none() { draft.workflows.train.request.lanes = authoritative.settingsstate.workflows.train.request.lanes; }
            }
            self.model_count_in_flight = false;
        }
        if self.queued.is_empty() && self.queued_lanes.is_none() && self.queued_model_count.is_none() {
            self.draft = Some(authoritative.settingsstate.clone());
        }
        self.reconcile_recipe_scope();
    }

    pub fn settle_failure(&mut self, authoritative: Option<&SettingsUiState>) {
        self.update_in_flight = false;
        self.queued.clear();
        self.queued_lanes = None;
        self.queued_model_count = None;
        self.model_count_in_flight = false;
        self.debounce_generation = self.debounce_generation.wrapping_add(1).max(1);
        self.draft = authoritative.map(|snapshot| snapshot.settingsstate.clone());
        self.reconcile_recipe_scope();
    }

    pub fn has_local_edits(&self) -> bool {
        self.update_in_flight || !(self.queued.is_empty() && self.queued_lanes.is_none() && self.queued_model_count.is_none())
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
        let request = model.take_request(false).unwrap();
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
        let request = model.take_request(false).unwrap();
        assert_eq!(request.updates.len(), 1);
        model
            .edit(EditCadence::Immediate, |draft| {
                crate::generated::edit_uishowworkspaceperformance(draft, true)
            })
            .unwrap();
        assert!(model.take_request(false).is_none());
        model.settle_success(&snapshot);
        assert_eq!(model.queued_len(), 1);
        assert!(model.draft.as_ref().unwrap().ui.showworkspaceperformance);
        assert!(model.take_request(false).is_some());
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
        let request = model.take_request(false).unwrap();
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
        assert!(model.take_request(false).is_some());
        model.settle_failure(Some(&authoritative));
        assert_eq!(model.draft, Some(authoritative.settingsstate.clone()));

        authoritative.revision += 1;
        authoritative.settingsstate.ui.darkmode = true;
        model.install(&authoritative);
        assert_eq!(model.draft, Some(authoritative.settingsstate));
    }
    #[test]
    fn recipe_value_selection_and_reset_are_scoped_and_keep_overrides() {
        use crate::generated::*;
        let mut global = settings_snapshot().settingsstate.workflows.train.request.recipe;
        edit_trainrecipesettings_lr(&mut global, 0.004);
        let mut model = global.clone();
        select_trainrecipesettings(&mut model, TrainOptimizerKind::SGD);
        assert_eq!(model.lr, 0.004);
        assert_eq!(model.lrencoder, 0.001);
        assert_eq!(model.lrscheduler, TrainLrSchedulerKind::UltralyticsLinear);
        reset_trainrecipesettings_lr(&mut model);
        assert_eq!(model.lr, 0.01);
        assert!(!model.overrides.overridden(TrainRecipeCatalogRelationField::Lr));
        edit_trainrecipesettings_warmupbiaslr(&mut model, 0.25);
        reset_trainrecipesettings(&mut model);
        assert_eq!(model.optimizer, TrainOptimizerKind::SGD);
        assert_eq!(model.warmupbiaslr, 0.1);
        assert_eq!(global.lr, 0.004);
        assert_eq!(global.optimizer, TrainOptimizerKind::AdamW);
        let mut settings = settings_snapshot().settingsstate;
        settings.workflows.train.request.recipe = global;
        let resets = reset_relation_workflowstrainrequestrecipe(&mut settings);
        assert_eq!(resets.len(), TRAIN_RECIPE_CATALOG_RELATION.len());
        assert_eq!(settings.workflows.train.request.recipe.lr, 0.0001);
        assert_eq!(model.lr, 0.01);
    }

    #[test]
    fn detached_recipe_defaults_and_effective_values_match_every_catalog_field() {
        use crate::application_codec::{FromApplicationValue, IntoApplicationValue, Value};
        use crate::generated::*;
        let pristine = settings_snapshot().settingsstate.workflows.train.request.recipe;
        for row in TRAIN_RECIPE_CATALOG {
            let mut recipe = pristine.clone();
            // Selection must replace stale unoverridden stored values using the catalog.
            recipe.lr = 7.0;
            recipe.warmupbiaslr = 8.0;
            select_trainrecipesettings(&mut recipe, row.optimizer);
            let Value::Object(mut expected) = row.clone().into_application_value() else {
                panic!("recipe catalog must be an object");
            };
            expected.push(("overrides".into(), pristine.overrides.clone().into_application_value()));
            let expected = TrainRecipeSettings::from_application_value(Value::Object(expected)).unwrap();
            assert_eq!(recipe, expected);
            assert_eq!(effective_trainrecipesettings_lr(&recipe), row.lr);
            assert_eq!(effective_trainrecipesettings_lrscheduler(&recipe), row.lrscheduler);
            assert_eq!(effective_trainrecipesettings_nesterov(&recipe), row.nesterov);
            let mut settings = settings_snapshot().settingsstate;
            settings.workflows.train.request.recipe = recipe.clone();
            let updates = reset_relation_workflowstrainrequestrecipe(&mut settings);
            assert_eq!(settings.workflows.train.request.recipe, recipe);
            assert_eq!(updates.len(), TRAIN_RECIPE_CATALOG_RELATION.len());
            for (update, fact) in updates.iter().zip(TRAIN_RECIPE_CATALOG_RELATION) {
                assert_eq!(update.path, fact.destination_path);
                assert_eq!(update.value, Value::Null);
            }
            let encoded = recipe.clone().into_application_value();
            assert_eq!(TrainRecipeSettings::from_application_value(encoded).unwrap(), recipe);
        }
    }

    #[test]
    fn recipe_field_operations_pin_effective_values_and_reset_only_their_field() {
        use crate::generated::*;
        type NumericOperations = (
            TrainRecipeCatalogRelationField,
            fn(&mut TrainRecipeSettings, f64),
            fn(&TrainRecipeSettings) -> f64,
            fn(&mut TrainRecipeSettings),
        );
        let operations: &[NumericOperations] = &[
            (TrainRecipeCatalogRelationField::Lr, edit_trainrecipesettings_lr, effective_trainrecipesettings_lr, reset_trainrecipesettings_lr),
            (TrainRecipeCatalogRelationField::LrEncoder, edit_trainrecipesettings_lrencoder, effective_trainrecipesettings_lrencoder, reset_trainrecipesettings_lrencoder),
            (TrainRecipeCatalogRelationField::LrComponentDecay, edit_trainrecipesettings_lrcomponentdecay, effective_trainrecipesettings_lrcomponentdecay, reset_trainrecipesettings_lrcomponentdecay),
            (TrainRecipeCatalogRelationField::EncoderLayerDecay, edit_trainrecipesettings_encoderlayerdecay, effective_trainrecipesettings_encoderlayerdecay, reset_trainrecipesettings_encoderlayerdecay),
            (TrainRecipeCatalogRelationField::Momentum, edit_trainrecipesettings_momentum, effective_trainrecipesettings_momentum, reset_trainrecipesettings_momentum),
            (TrainRecipeCatalogRelationField::WeightDecay, edit_trainrecipesettings_weightdecay, effective_trainrecipesettings_weightdecay, reset_trainrecipesettings_weightdecay),
            (TrainRecipeCatalogRelationField::WarmupEpochs, edit_trainrecipesettings_warmupepochs, effective_trainrecipesettings_warmupepochs, reset_trainrecipesettings_warmupepochs),
            (TrainRecipeCatalogRelationField::WarmupMomentum, edit_trainrecipesettings_warmupmomentum, effective_trainrecipesettings_warmupmomentum, reset_trainrecipesettings_warmupmomentum),
            (TrainRecipeCatalogRelationField::LrMinFactor, edit_trainrecipesettings_lrminfactor, effective_trainrecipesettings_lrminfactor, reset_trainrecipesettings_lrminfactor),
            (TrainRecipeCatalogRelationField::WarmupBiasLr, edit_trainrecipesettings_warmupbiaslr, effective_trainrecipesettings_warmupbiaslr, reset_trainrecipesettings_warmupbiaslr),
        ];
        let pristine = settings_snapshot().settingsstate.workflows.train.request.recipe;
        for row in TRAIN_RECIPE_CATALOG {
            let mut defaults = pristine.clone();
            select_trainrecipesettings(&mut defaults, row.optimizer);
            for &(field, edit, effective, reset) in operations {
                let mut recipe = defaults.clone();
                edit(&mut recipe, 0.375);
                assert!(recipe.overrides.overridden(field));
                assert_eq!(effective(&recipe), 0.375);
                reset(&mut recipe);
                assert!(!recipe.overrides.overridden(field));
                assert_eq!(recipe, defaults);
            }
            let mut recipe = defaults.clone();
            edit_trainrecipesettings_lrdrop(&mut recipe, 9);
            edit_trainrecipesettings_lrscheduler(&mut recipe, TrainLrSchedulerKind::Cosine);
            edit_trainrecipesettings_nesterov(&mut recipe, true);
            assert_eq!(effective_trainrecipesettings_lrdrop(&recipe), 9);
            assert_eq!(effective_trainrecipesettings_lrscheduler(&recipe), TrainLrSchedulerKind::Cosine);
            assert!(effective_trainrecipesettings_nesterov(&recipe));
            reset_trainrecipesettings_lrdrop(&mut recipe);
            assert!(recipe.overrides.overridden(TrainRecipeCatalogRelationField::LrScheduler));
            reset_trainrecipesettings_lrscheduler(&mut recipe);
            assert!(recipe.overrides.overridden(TrainRecipeCatalogRelationField::Nesterov));
            reset_trainrecipesettings_nesterov(&mut recipe);
            assert_eq!(recipe, defaults);
            edit_trainrecipesettings_lr(&mut recipe, 0.4);
            edit_trainrecipesettings_lrdrop(&mut recipe, 9);
            reset_trainrecipesettings(&mut recipe);
            assert_eq!(recipe, defaults);
        }
    }

    #[test]
    fn recipe_projection_rejects_invalid_constraints_without_changing_the_draft() {
        use crate::application_codec::{FromApplicationValue, IntoApplicationValue, Value};
        use crate::generated::*;
        let recipe = settings_snapshot().settingsstate.workflows.train.request.recipe;
        let original = recipe.clone().into_application_value();
        for fact in TRAIN_RECIPE_CATALOG_RELATION {
            let leaf = SETTINGS_LEAVES.iter().find(|leaf| leaf.path == fact.destination_path).unwrap();
            let Value::Object(fields) = &original else { panic!("recipe must be an object") };
            let (_, current) = fields.iter().find(|(name, _)| name == fact.source_path).unwrap();
            let mut invalid_values = Vec::new();
            match current {
                Value::Float(_) => {
                    invalid_values.push(Value::Float(f64::INFINITY));
                    if let Some(minimum) = leaf.minimum {
                        invalid_values.push(Value::Float(minimum - 1.0));
                    }
                    if let Some(maximum) = leaf.maximum {
                        invalid_values.push(Value::Float(maximum + 1.0));
                    }
                }
                Value::Signed(_) => invalid_values.push(Value::Signed(-1)),
                Value::Text(_) => invalid_values.push(Value::Text("unknown scheduler".into())),
                Value::Bool(_) => invalid_values.push(Value::Unsigned(2)),
                _ => panic!("unexpected recipe field shape"),
            }
            for invalid in invalid_values {
                let mut fields = fields.clone();
                fields.iter_mut().find(|(name, _)| name == fact.source_path).unwrap().1 = invalid;
                assert!(TrainRecipeSettings::from_application_value(Value::Object(fields)).is_err());
            }
        }
        assert_eq!(recipe.into_application_value(), original);
    }

    #[test]
    fn typed_lane_replacement_coalesces_with_scalar_transaction_and_preserves_reply_order() {
        use crate::generated::*;
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default(); model.install(&snapshot);
        let mut lanes = snapshot.settingsstate.workflows.train.request.laneconfiguration.clone();
        lanes.mode = TrainLaneMode::Independent;
        lanes.models.push(TrainModelSettings {
            modelid: 1, seed: 42, coefficient: 1.0,
            recipe: snapshot.settingsstate.workflows.train.request.recipe.clone(),
        });
        lanes.nextmodelid = 2;
        assert!(model.replace_training_lanes(lanes.clone(), true).is_err());
        assert!(!model.has_local_edits());
        model.replace_training_lanes(lanes.clone(), false).unwrap();
        model.edit(EditCadence::Immediate, |draft| edit_workflowstrainrequestlanes(draft, 1)).unwrap();
        lanes.mergerounds = 3;
        model.replace_training_lanes(lanes.clone(), false).unwrap();
        assert!(model.take_request(true).is_none());
        let first = model.take_request(false).unwrap();
        assert_eq!(first.laneconfiguration, Some(lanes.clone()));
        assert_eq!(first.updates.len(), 1);
        lanes.mergerounds = 4;
        model.replace_training_lanes(lanes.clone(), false).unwrap();
        assert!(model.take_request(false).is_none());
        model.settle_success(&snapshot);
        assert_eq!(model.draft.as_ref().unwrap().workflows.train.request.laneconfiguration, lanes);
        assert_eq!(model.take_request(false).unwrap().laneconfiguration, Some(lanes));
        model.settle_failure(Some(&snapshot));
        assert!(!model.has_local_edits());
        assert_eq!(model.draft, Some(snapshot.settingsstate));
    }

    #[test]
    fn locked_lane_transaction_allows_ordered_independent_edits_during_pending_and_active_training() {
        use crate::generated::*;
        let mut snapshot = settings_snapshot();
        let mut model = SettingsModel::default();
        model.install(&snapshot);
        let lanes = snapshot.settingsstate.workflows.train.request.laneconfiguration.clone();
        model.replace_training_lanes(lanes.clone(), false).unwrap();
        model.edit(EditCadence::Immediate, |draft| edit_workflowstrainrequestlanes(draft, 2)).unwrap();
        model.edit(EditCadence::Immediate, |draft| edit_workflowstraincompileddatasetdir(draft, "/next/dataset".into())).unwrap();
        model.edit(EditCadence::Immediate, |draft| edit_uidarkmode(draft, true)).unwrap();
        model.edit(EditCadence::Immediate, |draft| edit_uishowworkspaceperformance(draft, true)).unwrap();
        // Pending training and active training both lock admission at the caller.
        let pending = model.take_request(true).unwrap();
        assert!(pending.laneconfiguration.is_none());
        assert_eq!(pending.updates, vec![update_uidarkmode(true), update_uishowworkspaceperformance(true)]);
        assert_eq!(model.queued_len(), 2);
        model.edit(EditCadence::Immediate, |draft| edit_uidarkmode(draft, false)).unwrap();
        assert!(model.take_request(true).is_none());
        snapshot.revision += 1;
        snapshot.settingsstate.ui.darkmode = true;
        snapshot.settingsstate.ui.showworkspaceperformance = true;
        model.settle_success(&snapshot);
        assert!(!model.draft.as_ref().unwrap().ui.darkmode);
        assert_eq!(model.draft.as_ref().unwrap().workflows.train.request.lanes, 2);
        assert_eq!(model.draft.as_ref().unwrap().workflows.train.compileddatasetdir, "/next/dataset");
        let active = model.take_request(true).unwrap();
        assert!(active.laneconfiguration.is_none());
        assert_eq!(active.updates, vec![update_uidarkmode(false)]);
        snapshot.revision += 1;
        snapshot.settingsstate.ui.darkmode = false;
        model.settle_success(&snapshot);
        assert!(model.take_request(true).is_none());
        assert!(!model.update_in_flight());
        let settled = model.take_request(false).unwrap();
        assert_eq!(settled.laneconfiguration, Some(lanes));
        assert_eq!(settled.updates, vec![update_workflowstrainrequestlanes(2), update_workflowstraincompileddatasetdir("/next/dataset".into())]);
        model.settle_failure(Some(&snapshot));
        assert!(!model.has_local_edits());
        assert_eq!(model.draft, Some(snapshot.settingsstate));
    }

    #[test]
    fn lane_transaction_capacity_rejects_partial_scalar_admission() {
        let snapshot = settings_snapshot();
        let mut model = SettingsModel::default(); model.install(&snapshot);
        let lanes = snapshot.settingsstate.workflows.train.request.laneconfiguration.clone();
        model.replace_training_lanes(lanes, false).unwrap();
        model.queued = (0..crate::generated::SETTINGS_UPDATE_CAPACITY).map(|i| SettingsValueUpdate {
            path: format!("occupied.{i}"), value: crate::application_codec::Value::Null,
        }).collect();
        let prior = model.draft.clone();
        assert!(model.edit(EditCadence::Immediate, |draft| { let value = !draft.ui.darkmode; crate::generated::edit_uidarkmode(draft, value) }).is_err());
        assert_eq!(model.draft, prior);
        let request = model.take_request(false).unwrap();
        assert!(request.laneconfiguration.is_some());
        assert_eq!(request.updates.len(), crate::generated::SETTINGS_UPDATE_CAPACITY);
        assert_eq!(model.queued_len(), 0);
    }

    #[test]
    fn native_count_queue_keeps_recipe_edits_atomic_and_never_synthesizes_membership() {
        use crate::generated::*;
        let mut snapshot = settings_snapshot();
        let mut model = SettingsModel::default(); model.install(&snapshot);
        model.edit(EditCadence::Immediate, |draft| edit_workflowstrainrequestrecipeoptimizer(draft, TrainOptimizerKind::SGD)).unwrap();
        model.resize_training_models(2).unwrap();
        assert!(model.draft.as_ref().unwrap().workflows.train.request.laneconfiguration.models.is_empty());
        model.edit(EditCadence::Immediate, |draft| edit_uidarkmode(draft, true)).unwrap();
        model.edit(EditCadence::Immediate, |draft| edit_workflowstrainoutputautomatic(draft, false)).unwrap();
        let independent = model.take_request(true).unwrap();
        assert_eq!(independent.updates, vec![update_uidarkmode(true), update_workflowstrainoutputautomatic(false)]);
        assert!(independent.trainingmodelcount.is_none());
        snapshot.settingsstate.ui.darkmode = true;
        snapshot.settingsstate.workflows.train.output.automatic = false;
        model.settle_success(&snapshot);
        let request = model.take_request(false).unwrap();
        assert_eq!(request.trainingmodelcount, Some(2));
        assert_eq!(request.updates, vec![update_workflowstrainrequestrecipeoptimizer(TrainOptimizerKind::SGD)]);
        assert!(request.laneconfiguration.is_none());
        assert!(model.training_membership_pending());
        let recipe = snapshot.settingsstate.workflows.train.request.recipe.clone();
        snapshot.settingsstate.workflows.train.request.lanes = 2;
        snapshot.settingsstate.workflows.train.request.laneconfiguration.models = vec![TrainModelSettings { modelid: 41, seed: 77, recipe, coefficient: 1.0 }];
        snapshot.settingsstate.workflows.train.request.laneconfiguration.nextmodelid = 42;
        model.settle_success(&snapshot);
        assert_eq!(model.draft.as_ref().unwrap().workflows.train.request.laneconfiguration, snapshot.settingsstate.workflows.train.request.laneconfiguration);
        assert!(!model.training_membership_pending());
        assert!(model.resize_training_models(0).is_err());
        assert!(model.resize_training_models(TRAINING_MODEL_CAPACITY as u32 + 1).is_err());
        model.execution_locked = [true, true, true];
        assert!(model.resize_training_models(1).is_err());
        assert!(model.edit(EditCadence::Immediate, |draft| edit_workflowstrainrequestbatchsize(draft, 4)).is_err());
        assert!(model.edit(EditCadence::Immediate, |draft| edit_workflowstraincompileddatasetdir(draft, "/next/dataset".into())).is_err());
        model.edit(EditCadence::Immediate, |draft| edit_uidarkmode(draft, false)).unwrap();
        assert_eq!(model.take_request(true).unwrap().updates, vec![update_uidarkmode(false)]);
    }

}
