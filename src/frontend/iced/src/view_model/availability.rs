use super::model_selection::effective_model_selection;
use super::*;

impl ApplicationModel {
    pub fn native_settings_unsettled(&self) -> bool {
        self.has_pending(ApplicationIntentEndpoint::ExploreUpdateFilter)
            || self.has_pending(ApplicationIntentEndpoint::SettingsUpdate)
            || self.has_pending(ApplicationIntentEndpoint::SettingsReset)
            || self.dialog_context.is_some()
    }

    pub fn settings_edit_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.settings_snapshot.is_some()
            && self.dialog_context.is_none()
            && !self.has_pending(ApplicationIntentEndpoint::SettingsReset)
    }

    pub fn settings_reset_available(&self) -> bool {
        self.settings_edit_available() && !self.native_settings_unsettled()
    }

    pub fn file_dialog_open_available(&self, fact: &FileDialogFact, feature: FeatureId) -> bool {
        self.connection == ConnectionState::Connected
            && fact.stable_field_id != 0
            && crate::generated::FILE_DIALOGS
                .iter()
                .any(|candidate| std::ptr::eq(candidate, fact))
            && fact.workflows.contains(&feature)
            && self.dialog_context.is_none()
            && !self.native_settings_unsettled()
            && self
                .file_dialog
                .as_ref()
                .is_some_and(|snapshot| !snapshot.active)
            && !self.has_system_pending(crate::generated::ApplicationSystem::FileDialog)
    }

    pub(super) fn training_family_pending(&self) -> bool {
        self.has_system_pending(crate::generated::ApplicationSystem::Training)
    }

    pub fn model_selection_available(&self, settings: &GuiSettingsState, page: FeatureId) -> bool {
        if self.connection != ConnectionState::Connected {
            return false;
        }
        if self.settings_snapshot.is_none()
            || !self
                .model_snapshot
                .as_ref()
                .is_some_and(|state| !state.active)
            || self.native_settings_unsettled()
            || self.model_request_pending()
        {
            return false;
        }
        effective_model_selection(settings, page).is_some_and(|effective| effective.can_prepare())
    }

    pub fn model_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.model_snapshot.as_ref().is_some_and(|state| {
                state.active
                    && state.terminal.outcome != ModelSelectionOutcome::CancellationRequested
            })
            && !self.model_request_pending()
    }

    pub fn dataset_compile_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.settings_snapshot.is_some()
            && self
                .workflow
                .dataset
                .as_ref()
                .is_some_and(|state| !state.active)
            && !self.native_settings_unsettled()
            && !self.has_pending(ApplicationIntentEndpoint::DatasetCompile)
            && !self.has_pending(ApplicationIntentEndpoint::DatasetStop)
    }

    pub fn dataset_unsettled(&self) -> bool {
        self.has_pending(ApplicationIntentEndpoint::DatasetCompile)
            || self
                .workflow
                .dataset
                .as_ref()
                .is_some_and(|snapshot| snapshot.active)
    }

    pub fn dataset_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.workflow.dataset.as_ref().is_some_and(|snapshot| {
                snapshot.active
                    && snapshot.terminal.outcome
                        != crate::generated::ArtifactTerminalOutcome::CancellationRequested
            })
            && !self.has_pending(ApplicationIntentEndpoint::DatasetCompile)
            && !self.has_pending(ApplicationIntentEndpoint::DatasetStop)
    }

    pub fn compute_start_available(&self, settings: &GuiSettingsState, page: FeatureId) -> bool {
        if self.connection != ConnectionState::Connected
            || self.settings_snapshot.is_none()
            || self.native_settings_unsettled()
            || self.model_request_pending()
            || !self.model_selection_matches(settings, page)
        {
            return false;
        }
        if matches!(
            page,
            FeatureId::Train | FeatureId::Validate | FeatureId::Predict
        ) && (self.workflow.dataset.is_none() || self.dataset_unsettled())
        {
            return false;
        }
        match page {
            FeatureId::Train => {
                self.workflow.training.as_ref().is_some_and(|snapshot| {
                    snapshot.activity == crate::generated::TrainingActivity::Idle
                }) && !self.training_family_pending()
            }
            FeatureId::Validate => self.compute_start_available_for(
                self.workflow.validation.as_ref(),
                ApplicationIntentEndpoint::ValidationStart,
                ApplicationIntentEndpoint::ValidationStop,
            ),
            FeatureId::Predict => self.compute_start_available_for(
                self.predict_snapshot
                    .as_ref()
                    .map(|snapshot| &snapshot.operation),
                ApplicationIntentEndpoint::PredictStart,
                ApplicationIntentEndpoint::PredictStop,
            ),
            FeatureId::Export => self.compute_start_available_for(
                self.workflow.export.as_ref(),
                ApplicationIntentEndpoint::ExportSystemStart,
                ApplicationIntentEndpoint::ExportSystemStop,
            ),
            FeatureId::Live | FeatureId::Annotate | FeatureId::Explore => false,
        }
    }

    pub(super) fn compute_start_available_for(
        &self,
        snapshot: Option<&ComputeUiState>,
        start: ApplicationIntentEndpoint,
        stop: ApplicationIntentEndpoint,
    ) -> bool {
        snapshot.is_some_and(|value| !value.active)
            && !self.has_pending(start)
            && !self.has_pending(stop)
    }

    pub fn compute_stop_available(&self, page: FeatureId) -> bool {
        let (snapshot, start, stop) = match page {
            FeatureId::Validate => (
                self.workflow.validation.as_ref(),
                ApplicationIntentEndpoint::ValidationStart,
                ApplicationIntentEndpoint::ValidationStop,
            ),
            FeatureId::Predict => (
                self.predict_snapshot
                    .as_ref()
                    .map(|snapshot| &snapshot.operation),
                ApplicationIntentEndpoint::PredictStart,
                ApplicationIntentEndpoint::PredictStop,
            ),
            FeatureId::Export => (
                self.workflow.export.as_ref(),
                ApplicationIntentEndpoint::ExportSystemStart,
                ApplicationIntentEndpoint::ExportSystemStop,
            ),
            _ => return false,
        };
        self.connection == ConnectionState::Connected
            && snapshot.is_some_and(|value| {
                value.active
                    && value.terminal.outcome != ComputeOperationOutcome::CancellationRequested
            })
            && !self.has_pending(start)
            && !self.has_pending(stop)
    }

    pub fn training_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                snapshot.activity == crate::generated::TrainingActivity::Local
                    && snapshot.local.terminal.outcome
                        != ComputeOperationOutcome::CancellationRequested
            })
            && !self.has_pending(ApplicationIntentEndpoint::TrainingStart)
            && !self.has_pending(ApplicationIntentEndpoint::TrainingStop)
    }

    pub fn live_start_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self.window_width != 0
            && self.window_height != 0
            && self
                .live_snapshot
                .as_ref()
                .is_some_and(|snapshot| !snapshot.running && !snapshot.cancellationrequested)
            && !self.has_pending(ApplicationIntentEndpoint::LiveStart)
            && !self.has_pending(ApplicationIntentEndpoint::LiveStop)
    }

    pub fn live_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self
                .live_snapshot
                .as_ref()
                .is_some_and(|snapshot| snapshot.running && !snapshot.cancellationrequested)
            && !self.has_pending(ApplicationIntentEndpoint::LiveStart)
            && !self.has_pending(ApplicationIntentEndpoint::LiveStop)
    }

    pub fn provider_query_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                snapshot.activity == crate::generated::TrainingActivity::Idle
                    && !snapshot.offers.cancellationrequested
            })
            && !self.training_family_pending()
    }

    pub fn provider_select_available(
        &self,
        identity: &crate::generated::ProviderOfferIdentity,
    ) -> bool {
        self.provider_query_available()
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                snapshot
                    .offers
                    .offers
                    .iter()
                    .any(|offer| offer.offerid == identity.offerid)
            })
    }

    pub fn provider_clear_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                (snapshot.activity == crate::generated::TrainingActivity::Idle
                    || snapshot.activity == crate::generated::TrainingActivity::ProviderQuery)
                    && !snapshot.offers.cancellationrequested
            })
            && !self.training_family_pending()
    }

    pub fn remote_start_available(&self) -> bool {
        !self.native_settings_unsettled()
            && self.provider_query_available()
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                snapshot.offers.selected.is_some()
                    && snapshot.remote.phase != crate::generated::RemoteSessionPhase::Running
                    && !snapshot.remote.reconciliationpending
            })
    }

    pub fn remote_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                snapshot.activity == crate::generated::TrainingActivity::Idle
                    && snapshot.remote.phase == crate::generated::RemoteSessionPhase::Running
                    && !snapshot.remote.reconciliationpending
            })
            && !self.training_family_pending()
    }

    pub fn remote_retry_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.workflow.training.as_ref().is_some_and(|snapshot| {
                snapshot.activity == crate::generated::TrainingActivity::Idle
                    && snapshot.remote.reconciliationpending
            })
            && !self.training_family_pending()
    }

    pub fn dialog_stop_available(&self) -> bool {
        let Some(context) = self.dialog_context.as_ref() else {
            return false;
        };
        self.file_dialog.as_ref().is_some_and(|snapshot| {
            snapshot.active && !snapshot.cancellationrequested && snapshot.target == context.target
        }) && !self.has_system_pending(crate::generated::ApplicationSystem::FileDialog)
    }

    pub fn explore_open_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self.window_width != 0
            && self.window_height != 0
            && self
                .settings_snapshot
                .as_ref()
                .is_some_and(|settings| settings.exploresource.available)
            && self
                .explore
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| !snapshot.busy && !snapshot.cancellationrequested)
            && !self.has_explore_pending()
    }

    pub fn explore_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self
                .explore
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| snapshot.busy && !snapshot.cancellationrequested)
            && !self.has_explore_pending()
    }

    pub fn explore_mutation_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self
                .explore
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| snapshot.ready && !snapshot.cancellationrequested)
    }

    pub fn explore_augmentation_update_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self
                .explore
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| !snapshot.cancellationrequested)
    }

    pub fn explore_viewport_available(&self) -> bool {
        self.explore_mutation_available()
    }

    pub fn annotation_open_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self
                .annotation
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| !snapshot.busy && !snapshot.cancellationrequested)
            && !self.has_annotation_pending()
            && (self.explore.snapshot.as_ref().is_some_and(|snapshot| {
                snapshot.mode == crate::generated::ExploreMode::Detail
                    && snapshot.selectedimage.is_some()
                    && Self::valid_visual_source(&snapshot.frame).is_some()
            }))
    }

    pub fn upscale_start_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.explore.snapshot.as_ref().is_some_and(|snapshot| {
                snapshot.mode == crate::generated::ExploreMode::Detail
                    && snapshot.selectedimage.is_some()
                    && Self::valid_visual_source(&snapshot.frame).is_some()
            })
    }

    pub fn annotation_save_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && !self.native_settings_unsettled()
            && self.settings_snapshot.as_ref().is_some_and(|settings| {
                !settings
                    .settingsstate
                    .workflows
                    .annotate
                    .outputdir
                    .is_empty()
            })
            && self.annotation.snapshot.as_ref().is_some_and(|snapshot| {
                snapshot.ready
                    && !snapshot.busy
                    && !snapshot.cancellationrequested
                    && snapshot.ui.documentrevision != 0
            })
            && !self.has_annotation_pending()
    }

    pub fn annotation_edit_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self.annotation.snapshot.as_ref().is_some_and(|snapshot| {
                snapshot.ready
                    && !snapshot.busy
                    && !snapshot.cancellationrequested
                    && snapshot.ui.documentrevision != 0
            })
            && !self.has_annotation_pending()
    }

    pub fn annotation_stop_available(&self) -> bool {
        self.connection == ConnectionState::Connected
            && self
                .annotation
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| snapshot.busy && !snapshot.cancellationrequested)
            && !self.has_annotation_pending()
    }

    pub fn model_request_pending(&self) -> bool {
        self.has_system_pending(crate::generated::ApplicationSystem::Model)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::*;
    #[test]
    fn installed_cancellation_facts_close_stop_admission() {
        let mut model = bootstrapped();
        let start = model
            .begin_intent(ApplicationIntentEndpoint::ValidationStart)
            .unwrap();
        let mut validation = model.workflow.validation.clone().unwrap();
        validation.generationfrontier += 1;
        validation.active = true;
        validation.terminal.generation = validation.generationfrontier;
        validation.terminal.outcome = ComputeOperationOutcome::Running;
        model.reduce_reply(
            start,
            Ok(ApplicationReply::ValidationStart(validation.clone())),
        );
        assert!(model.compute_stop_available(FeatureId::Validate));
        let stop = model
            .begin_intent(ApplicationIntentEndpoint::ValidationStop)
            .unwrap();
        validation.terminal.outcome = ComputeOperationOutcome::CancellationRequested;
        model.reduce_reply(stop, Ok(ApplicationReply::ValidationStop(validation)));
        assert!(!model.compute_stop_available(FeatureId::Validate));
    }

    #[test]
    fn settings_settlement_gates_provider_and_remote_start_but_not_terminal_controls() {
        let mut model = bootstrapped();
        let training = model.workflow.training.as_mut().unwrap();
        training.activity = crate::generated::TrainingActivity::Idle;
        training.offers.selected = Some(crate::generated::ProviderOfferIdentity { offerid: 17 });
        training.remote.phase = crate::generated::RemoteSessionPhase::Stopped;
        training.remote.reconciliationpending = false;
        assert!(model.provider_query_available());
        assert!(model.remote_start_available());
        let settings = model
            .begin_intent(ApplicationIntentEndpoint::SettingsUpdate)
            .unwrap();
        assert!(!model.provider_query_available());
        assert!(!model.remote_start_available());
        model.workflow.training.as_mut().unwrap().remote.phase =
            crate::generated::RemoteSessionPhase::Running;
        assert!(model.remote_stop_available());
        model.workflow.training.as_mut().unwrap().remote.phase =
            crate::generated::RemoteSessionPhase::Stopped;
        model
            .workflow
            .training
            .as_mut()
            .unwrap()
            .remote
            .reconciliationpending = true;
        assert!(model.remote_retry_available());
        model.abandon_intent(settings);
    }

    #[test]
    fn augmentation_toggle_is_admitted_without_a_dataset_but_other_mutations_are_not() {
        let mut model = bootstrapped();
        let snapshot = model.explore.snapshot.as_mut().unwrap();
        snapshot.ready = false;
        snapshot.busy = false;
        snapshot.cancellationrequested = false;
        assert!(model.explore_augmentation_update_available());
        assert!(!model.explore_mutation_available());

        let pending = model
            .begin_intent(ApplicationIntentEndpoint::ExploreUpdateAugmentation)
            .unwrap();
        assert!(model.explore_augmentation_update_available());
        assert!(!model.explore_mutation_available());
        model.abandon_intent(pending);

        model.explore.snapshot.as_mut().unwrap().busy = true;
        assert!(model.explore_augmentation_update_available());
    }
}
