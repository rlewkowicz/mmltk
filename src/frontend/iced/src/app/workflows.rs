use super::*;
use crate::view_model::StartPreparation;

impl App {
    pub(super) fn on_workspace(&mut self, message: crate::view::router::Message) -> Task<Message> {
        let outcome = match self
            .workspace
            .update(&mut self.model, &mut self.settings, message)
        {
            Ok(Some(outcome)) => outcome,
            Ok(None) => return Task::none(),
            Err(detail) => {
                self.model.error = Some(UiError::invalid(detail));
                return Task::none();
            }
        };
        match outcome {
            crate::view::router::Outcome::FeatureSelected(feature) => {
                let task = self.transition_page(feature);
                if let Some(integration) = self.integration.as_mut() {
                    integration.observe_reporting(|reporting| {
                        reporting.observe_navigation_outcome(feature, self.workspace.active());
                    });
                }
                task
            }
            crate::view::router::Outcome::SettingsRequested => {
                self.settings.open();
                Task::none()
            }
            crate::view::router::Outcome::Train(outcome) => self.on_train(outcome),
            crate::view::router::Outcome::Validate(outcome) => self.on_validate(outcome),
            crate::view::router::Outcome::Predict(outcome) => self.on_predict(outcome),
            crate::view::router::Outcome::Live(outcome) => self.on_live(outcome),
            crate::view::router::Outcome::Export(outcome) => self.on_export(outcome),
            crate::view::router::Outcome::Explore(outcome) => self.on_explore(outcome),
            crate::view::router::Outcome::Annotation(outcome) => self.on_annotation(outcome),
        }
    }

    fn show_live_training(&mut self) {
        if let Some(train) = self.settings.draft().map(|draft| &draft.workflows.train) {
            self.model.workflow.output.start(train, self.model.file_dialog.as_ref().map(|dialog| dialog.generation));
        } else { self.model.workflow.output.live(); }
        self.workspace.sync_workflows(&self.model);
    }

    fn request_start(&mut self, feature: FeatureId) {
        if !self
            .settings
            .draft()
            .is_some_and(|draft| self.model.compute_start_available(draft, feature))
        {
            return;
        }
        let Some(inputs) = self
            .settings
            .draft()
            .and_then(|draft| crate::view_model::StartInputs::capture(draft, feature))
        else {
            return;
        };
        let resume_checkpoint = self.settings.draft().and_then(|draft| {
            let train = &draft.workflows.train;
            let continuation = &self.model.workflow.train_continuation;
            (feature == FeatureId::Train && continuation.matches(train)
                && train.modelsource == crate::generated::ModelSelectionSource::Custom
                && continuation.mode == crate::view_model::ContinuationMode::Resume)
                .then(|| continuation.checkpoint().filter(|checkpoint| checkpoint.resumable).map(|checkpoint| checkpoint.path.clone())).flatten()
        });
        if feature == FeatureId::Train {
            self.show_live_training();
        }
        self.model.workflow.pending_start = Some(crate::view_model::PendingStart {
            feature,
            inputs,
            preparation: if resume_checkpoint.is_some() { StartPreparation::ResumeQueued } else { StartPreparation::Waiting },
            resume_checkpoint,
        });
        self.flush_settings_edits();
        self.advance_start();
    }

    fn advance_training_selection(&mut self) {
        if self.settings_unsettled() { return; }
        let Some(train) = self.settings.draft().map(|draft| &draft.workflows.train) else { return; };
        let browse = self.model.file_dialog.as_ref().and_then(|dialog| {
            if !dialog.active
                && let Some(selection) = &dialog.selection
                && let crate::generated::FileDialogTarget::SettingsFieldTarget(target) = &selection.target
                && target.stableid == crate::generated::constraint_workflowstrainrequestoutputdir().stable_field_id
                && let crate::generated::FileDialogCancelledOrFileDialogSelectedVariant::FileDialogSelected(selected) = &selection.result
                && !train.autooutput && selected.path == train.request.outputdir {
                Some(dialog.generation)
            } else { None }
        });
        if self.model.workflow.output.synchronize(train, self.model.workflow.training.as_ref(), browse) {
            self.workspace.sync_workflows(&self.model);
        }
        // A deliberate confirmation waits for admitted restoration/model replies
        // to settle before replacing the continuation selection.
        if self.model.workflow.train_continuation.refresh_requested
            && (self.model.workflow.pending_start.is_some() || self.model.training_family_pending()) { return; }
        let restoring = self.model.workflow.pending_start.as_ref().is_some_and(|pending|
            matches!(pending.preparation, StartPreparation::Restoring { .. } | StartPreparation::Restored));
        if !restoring && !self.model.workflow.train_continuation.matches(train) {
            let bootstrap = self.model.workflow.train_continuation.selection.is_none()
                && !self.model.workflow.train_continuation.refresh_requested;
            self.model.workflow.train_continuation.select(train);
            if bootstrap && self.model.workflow.training.as_ref().is_some_and(|snapshot|
                snapshot.inspection.status == crate::generated::TrainingInspectionStatus::Running) {
                self.model.workflow.train_continuation.cancel_needed = true;
            }
            // A reconnect may recover an inspection that already settled natively.
            if bootstrap && train.modelsource == crate::generated::ModelSelectionSource::Custom
                && let Some(snapshot) = &self.model.workflow.training
                && snapshot.inspection.path == train.request.weightspath
                && snapshot.inspection.status != crate::generated::TrainingInspectionStatus::Idle {
                self.model.workflow.train_continuation.cancel_needed = false;
                self.model.workflow.train_continuation.capability = crate::view_model::CheckpointCapability::Pending {
                    request: None, generation: Some(snapshot.inspection.generation),
                };
                self.model.workflow.train_continuation.observe(snapshot.inspection.clone());
            }
        }
        if self.model.training_family_pending() { return; }
        if !restoring && train.modelsource == crate::generated::ModelSelectionSource::Custom
            && !train.request.weightspath.is_empty()
            && matches!(self.model.workflow.train_continuation.capability, crate::view_model::CheckpointCapability::Idle) {
            let path = train.request.weightspath.clone();
            let registered = self.model.register_intent(ApplicationIntentEndpoint::TrainingInspectCheckpoint, |correlation|
                crate::generated::encode_training_InspectCheckpoint(correlation, crate::generated::TrainingCheckpointQuery { path }));
            if let Ok(intent) = &registered { self.model.workflow.train_continuation.begin(intent.correlation); }
            self.submit_registered_intent(ApplicationIntentEndpoint::TrainingInspectCheckpoint, registered);
            return;
        }
        if !restoring && self.model.workflow.train_continuation.cancel_needed {
            let registered = self.model.register_intent(ApplicationIntentEndpoint::TrainingCancelCheckpointInspection,
                crate::generated::encode_training_CancelCheckpointInspection);
            if registered.is_ok() { self.model.workflow.train_continuation.cancel_needed = false; }
            self.submit_registered_intent(ApplicationIntentEndpoint::TrainingCancelCheckpointInspection, registered);
            return;
        }
        if self.model.workflow.pending_start.is_some() { return; }
        let Some(saved) = self.model.workflow.output.saved() else { return; };
        if !matches!(saved.load, crate::view_model::HistoryLoad::Idle) { return; }
        if saved.run.is_none() {
            let directory = saved.directory.clone();
            let registered = self.model.register_intent(ApplicationIntentEndpoint::TrainingOpenRun, |correlation|
                crate::generated::encode_training_OpenRun(correlation, crate::generated::TrainingDirectoryQuery { directory }));
            if let Ok(intent) = &registered { self.model.workflow.output.saved_mut().unwrap().load = crate::view_model::HistoryLoad::Opening(intent.correlation); }
            self.submit_registered_intent(ApplicationIntentEndpoint::TrainingOpenRun, registered);
            return;
        }
        if let Some(run) = &saved.run
            && run.run.is_some() && saved.page.as_ref().is_none_or(|page| page.more) {
            let query = crate::generated::TrainingHistoryQuery {
                generation: run.generation,
                cursor: saved.page.as_ref().map_or(0, |page| page.nextcursor),
                count: 32,
            };
            let registered = self.model.register_intent(ApplicationIntentEndpoint::TrainingHistory, |correlation|
                crate::generated::encode_training_History(correlation, query));
            if let Ok(intent) = &registered { self.model.workflow.output.saved_mut().unwrap().load = crate::view_model::HistoryLoad::Paging(intent.correlation); }
            self.submit_registered_intent(ApplicationIntentEndpoint::TrainingHistory, registered);
        }
    }

    pub(super) fn advance_start(&mut self) {
        self.advance_training_selection();
        let Some(pending) = self.model.workflow.pending_start.as_ref() else {
            return;
        };
        let feature = pending.feature;
        if pending.preparation.cancelled() {
            self.advance_start_cancellation();
            if self.model.workflow.pending_start.is_none() && self.model.workflow.train_continuation.refresh_requested {
                self.advance_training_selection();
            }
            return;
        }
        if self.workspace.active() != feature {
            self.model.workflow.cancel_start();
            self.advance_start_cancellation();
            return;
        }
        if matches!(pending.preparation, StartPreparation::Restoring { .. }) {
            return;
        }
        if pending.preparation == StartPreparation::Restored {
            let Some(path) = pending.resume_checkpoint.as_ref() else { return; };
            if self.settings_unsettled() || self.settings.draft().is_none_or(|draft|
                draft.workflows.train.request.resumepath != *path) { return; }
            let inputs = crate::view_model::StartInputs::capture(self.settings.draft().unwrap(), feature).unwrap();
            let pending = self.model.workflow.pending_start.as_mut().unwrap();
            pending.inputs = inputs;
            pending.preparation = StartPreparation::Waiting;
            // Restored canonical input identity is deliberate; ordinary edits still cancel.
            let train = &self.settings.draft().unwrap().workflows.train;
            self.model.workflow.train_continuation.selection = Some((train.modelsource, train.request.weightspath.clone(), train.request.presetname.clone()));
        }
        let pending = self.model.workflow.pending_start.as_ref().unwrap();
        let Some(draft) = self.settings.draft() else {
            return;
        };
        if !pending.inputs.matches(draft) {
            self.model.workflow.cancel_start();
            self.advance_start_cancellation();
            return;
        }
        if self.settings_unsettled() {
            return;
        }
        if feature == FeatureId::Train && self.model.training_family_pending() { return; }
        if let Some(path) = pending.resume_checkpoint.clone()
            && pending.preparation == StartPreparation::ResumeQueued {
            let registered = self.model.register_intent(ApplicationIntentEndpoint::TrainingPrepareResume, |correlation|
                crate::generated::encode_training_PrepareResume(correlation, crate::generated::TrainingCheckpointQuery { path }));
            if let Ok(intent) = &registered {
                self.model.workflow.pending_start.as_mut().unwrap().preparation = StartPreparation::Restoring { correlation: intent.correlation, cancelled: false };
            }
            if !self.submit_registered_intent(ApplicationIntentEndpoint::TrainingPrepareResume, registered) {
                self.model.workflow.pending_start = None;
            }
            return;
        }
        if self.model.model_request_pending() {
            return;
        }
        if !self
            .model
            .model_selection_matches(self.settings.draft().unwrap(), feature)
        {
            let Some(snapshot) = self.model.model_snapshot.as_ref() else {
                return;
            };
            if snapshot.active {
                return;
            }
            if matches!(
                self.model
                    .workflow
                    .pending_start
                    .as_ref()
                    .unwrap()
                    .preparation,
                StartPreparation::Active { .. }
            ) {
                self.model.workflow.pending_start = None;
                return;
            }
            let Some(receipt) = self.model.workflow.model_selection_receipt(feature) else {
                return;
            };
            let registered = self
                .model
                .register_model_select_intent(receipt, move |correlation| {
                    crate::generated::encode_model_Select(
                        correlation,
                        crate::generated::ModelSelectionRequest { workflow: feature },
                    )
                });
            if let Ok(intent) = &registered {
                self.model
                    .workflow
                    .pending_start
                    .as_mut()
                    .unwrap()
                    .preparation = StartPreparation::Selecting {
                    correlation: intent.correlation,
                    cancelled: false,
                };
            }
            if !self.submit_registered_intent(ApplicationIntentEndpoint::ModelSelect, registered) {
                self.model.workflow.pending_start = None;
            }
            return;
        }
        let resume_checkpoint = self
            .model
            .workflow
            .pending_start
            .take()
            .and_then(|pending| pending.resume_checkpoint);
        match feature {
            FeatureId::Train => {
                if let Some(path) = resume_checkpoint {
                    self.submit_intent(ApplicationIntentEndpoint::TrainingResume, |correlation| {
                        crate::generated::encode_training_Resume(
                            correlation,
                            crate::generated::TrainingCheckpointQuery { path },
                        )
                    })
                } else {
                    self.submit_intent(ApplicationIntentEndpoint::TrainingStart, |correlation| {
                        crate::generated::encode_training_Start(correlation, Train {})
                    })
                }
            }
            FeatureId::Validate => {
                self.submit_intent(ApplicationIntentEndpoint::ValidationStart, |correlation| {
                    crate::generated::encode_validation_Start(
                        correlation,
                        crate::generated::ValidateWorkflowIntent {},
                    )
                })
            }
            FeatureId::Predict => {
                self.submit_intent(ApplicationIntentEndpoint::PredictStart, |correlation| {
                    crate::generated::encode_predict_Start(
                        correlation,
                        crate::generated::PredictWorkflowIntent {},
                    )
                })
            }
            _ => false,
        };
    }

    fn advance_start_cancellation(&mut self) {
        let Some(pending) = self.model.workflow.pending_start.as_ref() else {
            return;
        };
        let feature = pending.feature;
        let (generation, stop_submitted) = match pending.preparation {
            StartPreparation::Active {
                generation,
                cancelled: true,
            } => (generation, false),
            StartPreparation::Stopping { generation, .. } => (generation, true),
            _ => return,
        };
        // Select and Stop share one native-system admission slot. A terminal
        // event may arrive first, but its registered reply must still settle.
        if self.model.model_request_pending() {
            return;
        }
        let Some(snapshot) = self.model.model_snapshot.as_ref() else {
            return;
        };
        if snapshot.generation != generation || !snapshot.active {
            self.model.workflow.pending_start = None;
            return;
        }
        if stop_submitted {
            return;
        }
        if snapshot.terminal.outcome
            == crate::generated::ModelSelectionOutcome::CancellationRequested
        {
            return;
        }
        let registered = self
            .model
            .register_model_stop_intent(feature, crate::generated::encode_model_Stop);
        if let Ok(intent) = &registered {
            self.model
                .workflow
                .pending_start
                .as_mut()
                .unwrap()
                .preparation = StartPreparation::Stopping {
                generation,
                correlation: intent.correlation,
            };
        }
        if !self.submit_registered_intent(ApplicationIntentEndpoint::ModelStop, registered) {
            self.model.workflow.pending_start = None;
        }
    }

    pub(super) fn guard_compute_start(&mut self, page: FeatureId) -> bool {
        let available = !self.settings.has_local_edits()
            && self
                .settings
                .draft()
                .is_some_and(|draft| self.model.compute_start_available(draft, page));
        if !available {
            self.model.error = Some(
                if self.settings.has_local_edits()
                    || self.model.native_settings_unsettled()
                    || self.model.model_request_pending()
                    || self.model.compute_stop_available(page)
                    || self
                        .settings
                        .draft()
                        .is_some_and(|draft| self.model.model_selection_matches(draft, page))
                {
                    UiError::busy(
                        "Wait for typed inputs, model selection, and active operations to settle.",
                    )
                } else {
                    UiError::invalid("Prepare the current workflow model before starting.")
                },
            );
        }
        available
    }

    pub(super) fn guard_compute_stop(&mut self, page: FeatureId) -> bool {
        if self
            .model
            .workflow
            .pending_start
            .as_ref()
            .is_some_and(|pending| pending.feature == page)
        {
            self.model.workflow.cancel_start();
            self.advance_start_cancellation();
            return false;
        }
        let available = match page {
            FeatureId::Train => self.model.training_stop_available(),
            FeatureId::Validate | FeatureId::Predict | FeatureId::Export => {
                self.model.compute_stop_available(page)
            }
            FeatureId::Live | FeatureId::Annotate | FeatureId::Explore => false,
        };
        if !available {
            self.model.error = Some(UiError::busy(
                "The operation is inactive or already changing state.",
            ));
            return false;
        }
        true
    }

    pub(super) fn request_model(&mut self, page: FeatureId) {
        if self.settings.has_local_edits()
            || !self
                .settings
                .draft()
                .is_some_and(|draft| self.model.model_selection_available(draft, page))
        {
            self.model.error = Some(UiError::busy(
                "Wait for settings and model activity to finish.",
            ));
            return;
        }
        let workflow = page;
        let Some(receipt) = self.model.workflow.model_selection_receipt(workflow) else {
            self.model.error = Some(UiError::invalid("Settings are not installed yet."));
            return;
        };
        self.submit_model_select_intent(receipt, move |correlation| {
            crate::generated::encode_model_Select(
                correlation,
                crate::generated::ModelSelectionRequest { workflow },
            )
        });
    }

    pub(super) fn stop_model(&mut self, page: FeatureId) {
        if self
            .model
            .workflow
            .pending_start
            .as_ref()
            .is_some_and(|pending| {
                pending.feature == page && pending.preparation != StartPreparation::Waiting
            })
        {
            self.model.workflow.cancel_start();
            self.advance_start_cancellation();
            return;
        }
        if self.model.model_stop_available() {
            self.submit_model_stop_intent(page, crate::generated::encode_model_Stop);
        }
    }

    fn on_model(
        &mut self,
        workflow: FeatureId,
        outcome: crate::view::workflow::model_card::Outcome,
    ) -> Task<Message> {
        match outcome {
            crate::view::workflow::model_card::Outcome::ArtifactConfirmed(schedule) => {
                if workflow == FeatureId::Train {
                    if self.model.workflow.pending_start.as_ref().is_some_and(|pending| pending.feature == FeatureId::Train) {
                        self.model.workflow.cancel_start();
                    }
                    self.model.workflow.train_continuation.refresh_requested = true;
                }
                let task = self.handle_settings_schedule(schedule);
                self.advance_start();
                task
            }
            crate::view::workflow::model_card::Outcome::SettingsEdited(schedule) => {
                self.handle_settings_schedule(schedule)
            }
            crate::view::workflow::model_card::Outcome::BrowseRequested(target) => {
                self.open_model_dialog(target);
                Task::none()
            }
            crate::view::workflow::model_card::Outcome::PrepareRequested => {
                self.request_model(workflow);
                Task::none()
            }
            crate::view::workflow::model_card::Outcome::StopRequested => {
                self.stop_model(workflow);
                Task::none()
            }
        }
    }

    pub(super) fn on_train(&mut self, outcome: crate::view::train::Outcome) -> Task<Message> {
        match outcome {
            crate::view::train::Outcome::Output(message) => {
                use crate::view::train::output::Message as Output;
                match message {
                    Output::Auto(value) => {
                        let schedule = self.settings.state_mut().edit(
                            crate::view::settings::EditCadence::Immediate,
                            |draft| crate::generated::edit_workflowstrainautooutput(draft, value));
                        return match schedule {
                            Ok(schedule) => self.handle_settings_schedule(schedule),
                            Err(error) => { self.model.error = Some(UiError::invalid(error)); Task::none() }
                        };
                    }
                    Output::Browse(id) => self.open_dialog(id),
                }
            }
            crate::view::train::Outcome::Continuation(mode) => {
                if !self.model.settings_edit_available() || (mode == crate::view_model::ContinuationMode::Resume
                    && (!self.settings.draft().is_some_and(|draft| self.model.workflow.train_continuation.matches(&draft.workflows.train))
                        || !self.model.workflow.train_continuation.checkpoint().is_some_and(|checkpoint| checkpoint.resumable))) { return Task::none(); }
                if self.model.workflow.train_continuation.mode == mode {
                    self.model.workflow.train_continuation.mode_chosen = true;
                    return Task::none();
                }
                self.model.workflow.cancel_start();
                self.model.workflow.train_continuation.mode = mode;
                self.model.workflow.train_continuation.mode_chosen = true;
            }

            crate::view::train::Outcome::CompileRequested => {
                if self.settings.has_local_edits() || !self.model.dataset_compile_available() {
                    self.model.error = Some(UiError::busy(
                        "Dataset compilation is unavailable or already active.",
                    ));
                } else {
                    self.submit_intent(ApplicationIntentEndpoint::DatasetCompile, |correlation| {
                        crate::generated::encode_dataset_Compile(correlation, Train {})
                    });
                }
            }
            crate::view::train::Outcome::Model(outcome) => {
                return self.on_model(FeatureId::Train, outcome);
            }
            crate::view::train::Outcome::StartRequested => {
                self.request_start(FeatureId::Train);
            }
            crate::view::train::Outcome::DatasetStopRequested => {
                if self.model.dataset_stop_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::DatasetStop,
                        crate::generated::encode_dataset_Stop,
                    );
                } else {
                    self.model.error = Some(UiError::busy(
                        "Dataset compilation is inactive or already changing state.",
                    ));
                }
            }
            crate::view::train::Outcome::TrainingStopRequested => {
                if self.guard_compute_stop(FeatureId::Train) {
                    self.submit_intent(ApplicationIntentEndpoint::TrainingStop, |correlation| {
                        crate::generated::encode_training_Stop(correlation, Train {})
                    });
                }
            }
            crate::view::train::Outcome::DialogRequested(id) => self.open_dialog(id),
            crate::view::train::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
            crate::view::train::Outcome::QueryOffersRequested => {
                if !self.settings_unsettled() && self.model.provider_query_available() {
                    self.submit_intent(ApplicationIntentEndpoint::TrainingQuery, |correlation| {
                        crate::generated::encode_training_Query(
                            correlation,
                            crate::generated::ProviderQueryIntent {},
                        )
                    });
                } else {
                    self.model.error = Some(UiError::busy("Provider query is unavailable."));
                }
            }
            crate::view::train::Outcome::ClearOffersRequested => {
                if self.model.provider_clear_available() {
                    self.submit_intent(ApplicationIntentEndpoint::TrainingClear, |correlation| {
                        crate::generated::encode_training_Clear(
                            correlation,
                            crate::generated::ProviderClearIntent {},
                        )
                    });
                } else {
                    self.model.error = Some(UiError::busy(
                        "Provider offers are unavailable or cancellation is already requested.",
                    ));
                }
            }
            crate::view::train::Outcome::OfferSelected(offer) => {
                self.select_provider_offer(offer);
            }
            crate::view::train::Outcome::StartRemoteRequested => {
                if !self.settings.has_local_edits() && self.model.remote_start_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::TrainingStartRemote,
                        |correlation| {
                            crate::generated::encode_training_StartRemote(
                                correlation,
                                crate::generated::ProviderStartIntent {},
                            )
                        },
                    );
                } else {
                    self.model.error = Some(UiError::busy("Remote start is unavailable."));
                }
            }
            crate::view::train::Outcome::StopRemoteRequested => {
                if self.model.remote_stop_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::TrainingStopRemote,
                        |correlation| {
                            crate::generated::encode_training_StopRemote(
                                correlation,
                                crate::generated::ProviderStopIntent {},
                            )
                        },
                    );
                } else {
                    self.model.error = Some(UiError::busy("Remote stop is unavailable."));
                }
            }
            crate::view::train::Outcome::RetryReconciliationRequested => {
                if self.model.remote_retry_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::TrainingRetryReconciliation,
                        crate::generated::encode_training_RetryReconciliation,
                    );
                } else {
                    self.model.error = Some(UiError::busy("Remote reconciliation is unavailable."));
                }
            }
        }
        Task::none()
    }

    pub(super) fn select_provider_offer(
        &mut self,
        identity: crate::generated::ProviderOfferIdentity,
    ) {
        if self.settings.has_local_edits() || !self.model.provider_select_available(&identity) {
            self.model.error = Some(UiError::invalid(
                "The selected provider offer is no longer available.",
            ));
            return;
        }
        self.submit_intent(
            ApplicationIntentEndpoint::TrainingSelect,
            move |correlation| crate::generated::encode_training_Select(correlation, identity),
        );
    }

    pub(super) fn on_validate(&mut self, outcome: crate::view::validate::Outcome) -> Task<Message> {
        match outcome {
            crate::view::validate::Outcome::Sample(message) => {
                use crate::view::validate::samples::Message as Sample;
                match message {
                    Sample::Select(identity) => {
                        if self.model.validation_navigation_available() && self.submit_intent(
                            ApplicationIntentEndpoint::ValidationSelectSample,
                            |correlation| {
                                crate::generated::encode_validation_SelectSample(
                                    correlation,
                                    identity,
                                )
                            },
                        ) {
                            self.abandon_viewer();
                            self.model.set_foreground_visual(Some(crate::generated::PresentationSourceKind::Validation));
                        }
                    }
                    Sample::Close => {
                        if self.model.validation_navigation_available() && self.submit_intent(
                            ApplicationIntentEndpoint::ValidationCloseDetail,
                            crate::generated::encode_validation_CloseDetail,
                        ) {
                            self.abandon_viewer();
                            self.model.set_foreground_visual(Some(crate::generated::PresentationSourceKind::Validation));
                        }
                    }
                    Sample::Overlays(overlays) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::ValidationSetOverlays,
                            |correlation| {
                                crate::generated::encode_validation_SetOverlays(
                                    correlation,
                                    overlays,
                                )
                            },
                        );
                    }
                    Sample::Upscale(kernel) => {
                        if self.model.upscale_start_available() && let Some(request) = crate::presentation_surface::viewer_upscale_request(kernel) {
                            self.model.request_upscale(request);
                            self.dispatch_viewer_desired();
                        }
                    }
                    Sample::OpenAnnotation => {
                        if self.copy_viewer_to_annotation() { return self.transition_page(FeatureId::Annotate); }
                    }
                    Sample::Atlas(_) | Sample::Fit | Sample::Labels(..) => {}
                }
            }
            crate::view::validate::Outcome::StartRequested => {
                self.request_start(FeatureId::Validate);
            }
            crate::view::validate::Outcome::StopRequested => {
                if self.guard_compute_stop(FeatureId::Validate) {
                    self.submit_intent(
                        ApplicationIntentEndpoint::ValidationStop,
                        crate::generated::encode_validation_Stop,
                    );
                }
            }
            crate::view::validate::Outcome::Model(outcome) => {
                return self.on_model(FeatureId::Validate, outcome);
            }
            crate::view::validate::Outcome::DialogRequested(id) => self.open_dialog(id),
            crate::view::validate::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
        }
        Task::none()
    }

    pub(super) fn on_predict(&mut self, outcome: crate::view::predict::Outcome) -> Task<Message> {
        match outcome {
            crate::view::predict::Outcome::DialogRequested(id) => self.open_dialog(id),
            crate::view::predict::Outcome::StartRequested => {
                self.request_start(FeatureId::Predict);
            }
            crate::view::predict::Outcome::StopRequested => {
                if self.guard_compute_stop(FeatureId::Predict) {
                    self.submit_intent(ApplicationIntentEndpoint::PredictStop, |correlation| {
                        crate::generated::encode_predict_Stop(
                            correlation,
                            crate::generated::PredictWorkflowIntent {},
                        )
                    });
                }
            }
            crate::view::predict::Outcome::PauseRequested(paused) => {
                if self.model.predict_pause_available() {
                    self.submit_intent(ApplicationIntentEndpoint::PredictPause, |correlation| {
                        crate::generated::encode_predict_Pause(
                            correlation,
                            crate::generated::PredictPauseIntent { paused },
                        )
                    });
                }
            }
            crate::view::predict::Outcome::Model(outcome) => {
                return self.on_model(FeatureId::Predict, outcome);
            }
            crate::view::predict::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
        }
        Task::none()
    }

    pub(super) fn on_live(&mut self, outcome: crate::view::live::Outcome) -> Task<Message> {
        match outcome {
            crate::view::live::Outcome::StartRequested => {
                if self.settings.has_local_edits() || !self.model.live_start_available() {
                    self.model.error = Some(UiError::busy(
                        "Live is unavailable or already changing state.",
                    ));
                    return Task::none();
                }
                let extent = VisualExtent {
                    width: self.model.window_width,
                    height: self.model.window_height,
                };
                let Ok(frames_per_second) =
                    crate::generated::default_request_liveStartframespersecond()
                else {
                    self.model.error = Some(UiError::protocol(
                        "The generated Live frame-rate default is unavailable.",
                    ));
                    return Task::none();
                };
                self.submit_intent(ApplicationIntentEndpoint::LiveStart, move |correlation| {
                    crate::generated::encode_live_Start(
                        correlation,
                        LiveStart {
                            extent,
                            framespersecond: frames_per_second,
                        },
                    )
                });
            }
            crate::view::live::Outcome::StopRequested => {
                if self.model.live_stop_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::LiveStop,
                        crate::generated::encode_live_Stop,
                    );
                } else {
                    self.model.error =
                        Some(UiError::busy("Live is inactive or already changing state."));
                }
            }
            crate::view::live::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
        }
        Task::none()
    }

    pub(super) fn on_export(&mut self, outcome: crate::view::export::Outcome) -> Task<Message> {
        match outcome {
            crate::view::export::Outcome::StartRequested => {
                if self.guard_compute_start(FeatureId::Export) {
                    self.submit_intent(
                        ApplicationIntentEndpoint::ExportSystemStart,
                        |correlation| {
                            crate::generated::encode_exportsystem_Start(
                                correlation,
                                crate::generated::ExportWorkflowIntent {},
                            )
                        },
                    );
                }
            }
            crate::view::export::Outcome::StopRequested => {
                if self.guard_compute_stop(FeatureId::Export) {
                    self.submit_intent(
                        ApplicationIntentEndpoint::ExportSystemStop,
                        crate::generated::encode_exportsystem_Stop,
                    );
                }
            }
            crate::view::export::Outcome::Model(outcome) => {
                return self.on_model(FeatureId::Export, outcome);
            }
            crate::view::export::Outcome::DialogRequested(id) => self.open_dialog(id),
            crate::view::export::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
        }
        Task::none()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transport_connection::{Capture, CapturedRecord};
    use crate::view_model::test_support::{accepted_model_for, bootstrapped};
    use crate::generated::SettingsApplicationProjection;

    fn start_app() -> (App, Capture) {
        let (mut app, task) = crate::app::boot();
        drop(task);
        app.model = bootstrapped();
        app.workspace.select(FeatureId::Train);
        app.settings
            .install(app.model.settings_snapshot.as_ref().unwrap());
        let (connection, capture) = Connection::test_channel();
        app.connection = Some(connection);
        (app, capture)
    }

    fn next_intent(capture: &mut Capture, expected: ApplicationIntentEndpoint) -> Intent {
        let CapturedRecord::Intent(intent) = capture.try_recv().expect("submitted intent") else {
            panic!("expected intent");
        };
        assert_eq!(
            crate::generated::decode_application_intent_endpoint(intent.endpoint_id),
            Some(expected)
        );
        intent
    }

    fn validation_viewer() -> (App, Capture, crate::generated::UpscaleRequest) {
        use crate::generated::{PresentationSourceKind, UpscaleKernel};
        let (mut app, capture) = start_app();
        app.workspace.select(FeatureId::Validate);
        let metadata = crate::view_model::test_support::validation_image_metadata();
        let request = crate::generated::UpscaleRequest {
            source: metadata.frame, document: metadata.document, kernel: UpscaleKernel::Default,
        };
        app.model.requested_upscale = Some(request.clone());
        app.model.sent_upscale = Some(request.clone());
        app.model.set_foreground_visual(Some(PresentationSourceKind::Upscale));
        (app, capture, request)
    }

    fn validation_navigation() -> [crate::view::validate::samples::Message; 2] {
        use crate::view::validate::samples::Message as Sample;
        [Sample::Select(crate::generated::ValidationSampleIdentity { generation: 7, datasetindex: 42 }), Sample::Close]
    }

    #[test]
    fn compute_primary_stop_routes_to_each_native_owner_once() {
        for (page, endpoint) in [
            (FeatureId::Train, ApplicationIntentEndpoint::TrainingStop),
            (FeatureId::Validate, ApplicationIntentEndpoint::ValidationStop),
            (FeatureId::Predict, ApplicationIntentEndpoint::PredictStop),
            (FeatureId::Export, ApplicationIntentEndpoint::ExportSystemStop),
        ] {
            let (mut app, mut capture) = start_app();
            let training = app.model.workflow.training.as_mut().unwrap();
            training.activity = crate::generated::TrainingActivity::Local;
            training.local.active = true;
            training.local.terminal.outcome = crate::generated::ComputeOperationOutcome::Running;
            for operation in [
                &mut app.model.workflow.validation.as_mut().unwrap().operation,
                &mut app.model.predict_snapshot.as_mut().unwrap().operation,
                app.model.workflow.export.as_mut().unwrap(),
            ] {
                operation.active = true;
                operation.terminal.outcome = crate::generated::ComputeOperationOutcome::Running;
            }
            let stop = |app: &mut App| match page {
                FeatureId::Train => app.on_train(crate::view::train::Outcome::TrainingStopRequested),
                FeatureId::Validate => app.on_validate(crate::view::validate::Outcome::StopRequested),
                FeatureId::Predict => app.on_predict(crate::view::predict::Outcome::StopRequested),
                FeatureId::Export => app.on_export(crate::view::export::Outcome::StopRequested),
                _ => unreachable!(),
            };
            drop(stop(&mut app));
            next_intent(&mut capture, endpoint);
            assert!(app.model.primary_action_active(page));
            drop(stop(&mut app));
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn live_primary_routes_typed_start_and_stop_with_pending_reply_protection() {
        let (mut app, mut capture) = start_app();
        app.model.window_width = 64;
        app.model.window_height = 64;
        drop(app.on_live(crate::view::live::Outcome::StartRequested));
        let start = next_intent(&mut capture, ApplicationIntentEndpoint::LiveStart);
        assert!(app.model.primary_action_active(FeatureId::Live));
        assert!(!app.model.live_stop_available());
        let mut running = app.model.live_snapshot.clone().unwrap();
        running.revision += 1;
        running.running = true;
        app.model.reduce_reply(start.correlation, Ok(crate::generated::ApplicationReply::LiveStart(running.clone())));
        assert!(app.model.live_stop_available());
        drop(app.on_live(crate::view::live::Outcome::StopRequested));
        let stop = next_intent(&mut capture, ApplicationIntentEndpoint::LiveStop);
        assert!(app.model.primary_action_active(FeatureId::Live));
        assert!(!app.model.live_stop_available());
        drop(app.on_live(crate::view::live::Outcome::StopRequested));
        assert!(capture.try_recv().is_err());
        running.revision += 1;
        running.cancellationrequested = true;
        app.model.reduce_reply(stop.correlation, Ok(crate::generated::ApplicationReply::LiveStop(running.clone())));
        assert!(app.model.primary_action_active(FeatureId::Live));
        assert!(!app.model.live_stop_available());
        running.revision += 1;
        running.running = false;
        running.cancellationrequested = false;
        app.model.reduce_event(crate::generated::ApplicationEvent::LiveLiveChanged(crate::generated::LiveChanged { snapshot: running }));
        assert!(!app.model.primary_action_active(FeatureId::Live));
    }

    #[test]
    fn validation_navigation_refusal_preserves_the_viewer_without_busy_or_replay() {
        for message in validation_navigation() {
            for disconnected in [false, true] {
                let (mut app, mut capture, request) = validation_viewer();
                let pending = (!disconnected).then(|| app.model.begin_intent(ApplicationIntentEndpoint::ValidationSetOverlays).unwrap());
                if disconnected { app.model.connection = crate::view_model::ConnectionState::Reconnecting; }
                for _ in 0..4 {
                    drop(app.on_validate(crate::view::validate::Outcome::Sample(message.clone())));
                }
                assert_eq!(app.model.requested_upscale.as_ref(), Some(&request));
                assert_eq!(app.model.sent_upscale.as_ref(), Some(&request));
                assert_eq!(app.model.foreground_visual(), Some(crate::generated::PresentationSourceKind::Upscale));
                assert!(!app.presentation.stop_requested);
                assert!(app.model.error.is_none());
                assert!(capture.try_recv().is_err());
                if let Some(pending) = pending { app.model.abandon_intent(pending); }
                app.model.connection = crate::view_model::ConnectionState::Connected;
                app.dispatch_viewer_desired();
                assert!(capture.try_recv().is_err());
            }
        }
    }

    #[test]
    fn validation_navigation_failed_submission_does_not_depart() {
        for message in validation_navigation() {
            let (mut app, mut capture, request) = validation_viewer();
            app.connection = None;
            drop(app.on_validate(crate::view::validate::Outcome::Sample(message)));
            assert_eq!(app.model.requested_upscale.as_ref(), Some(&request));
            assert_eq!(app.model.sent_upscale.as_ref(), Some(&request));
            assert_eq!(app.model.foreground_visual(), Some(crate::generated::PresentationSourceKind::Upscale));
            assert!(!app.presentation.stop_requested);
            assert!(app.model.error.is_some());
            assert!(app.model.validation_navigation_available());
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn accepted_validation_navigation_submits_before_one_viewer_stop() {
        use crate::view::validate::samples::Message as Sample;
        for message in validation_navigation() {
            let (mut app, mut capture, _) = validation_viewer();
            drop(app.on_validate(crate::view::validate::Outcome::Sample(message.clone())));
            let endpoint = match &message {
                Sample::Select(_) => ApplicationIntentEndpoint::ValidationSelectSample,
                _ => ApplicationIntentEndpoint::ValidationCloseDetail,
            };
            let navigation = next_intent(&mut capture, endpoint);
            let expected = match &message {
                Sample::Select(identity) => crate::generated::encode_validation_SelectSample(navigation.correlation, identity.clone()),
                _ => crate::generated::encode_validation_CloseDetail(navigation.correlation),
            };
            assert_eq!(navigation, expected.record);
            next_intent(&mut capture, ApplicationIntentEndpoint::UpscaleStop);
            assert!(app.model.requested_upscale.is_none());
            assert!(app.model.sent_upscale.is_none());
            assert_eq!(app.model.foreground_visual(), Some(crate::generated::PresentationSourceKind::Validation));
            assert!(!app.presentation.stop_requested);
            drop(app.on_validate(crate::view::validate::Outcome::Sample(message)));
            assert!(app.model.error.is_none());
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn output_edits_only_schedule_settings_and_browse_only_opens_the_dialog() {
        use crate::view::train::{Outcome, output::Message as Output};
        let (mut app, mut capture) = start_app();
        drop(app.on_train(Outcome::Output(Output::Auto(false))));
        assert!(!app.settings.draft().unwrap().workflows.train.autooutput);
        next_intent(&mut capture, ApplicationIntentEndpoint::SettingsUpdate);
        assert!(app.model.workflow.pending_start.is_none());

        let (mut app, mut capture) = start_app();
        let before = app
            .settings
            .draft()
            .unwrap()
            .workflows
            .train
            .request
            .outputdir
            .clone();
        let id = crate::generated::constraint_workflowstrainrequestoutputdir().stable_field_id;
        drop(app.on_train(Outcome::Output(Output::Browse(id))));
        let intent = next_intent(&mut capture, ApplicationIntentEndpoint::FileDialogOpen);
        let target = crate::generated::FileDialogTarget::SettingsFieldTarget(
            crate::generated::SettingsFieldTarget { stableid: id },
        );
        app.model.reduce_reply(intent.correlation, Ok(crate::generated::ApplicationReply::FileDialogOpen(
            crate::generated::FileDialogSnapshot {
                generation: 1, active: false, cancellationrequested: false, target: target.clone(),
                selection: Some(crate::generated::FileDialogSelection {
                    target,
                    result: crate::generated::FileDialogCancelledOrFileDialogSelectedVariant::FileDialogCancelled(
                        crate::generated::FileDialogCancelled {}),
                }),
            })));
        assert_eq!(
            app.settings
                .draft()
                .unwrap()
                .workflows
                .train
                .request
                .outputdir,
            before
        );
        assert!(app.model.error.is_none());
        assert!(
            app.model
                .file_dialog
                .as_ref()
                .is_some_and(|dialog| !dialog.active)
        );
        assert!(app.model.workflow.pending_start.is_none());
        assert!(app.model.workflow.output.run().is_none());
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn folder_selection_loads_history_without_starting_and_ignores_replaced_reply() {
        let (mut app, mut capture) = start_app();
        let mut settings = app.model.settings_snapshot.clone().unwrap();
        settings.revision += 1;
        settings.settingsstate.workflows.train.autooutput = false;
        settings.settingsstate.workflows.train.request.outputdir = "/saved/one".into();
        app.model.project_settings_snapshot(settings.clone()).unwrap();
        app.settings.install(&settings);
        app.advance_start();
        let opened = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingOpenRun);
        settings.revision += 1;
        settings.settingsstate.workflows.train.request.outputdir = "/saved/two".into();
        app.model.project_settings_snapshot(settings.clone()).unwrap();
        app.settings.install(&settings);
        app.advance_start();
        app.model.reduce_reply(opened.correlation, Ok(crate::generated::ApplicationReply::TrainingOpenRun(crate::generated::TrainingOpenedRun {
            generation: 1, directory: "/saved/one".into(), run: None,
        })));
        assert!(app.model.workflow.output.run().is_none());
        app.advance_start();
        next_intent(&mut capture, ApplicationIntentEndpoint::TrainingOpenRun);
        assert!(app.model.workflow.pending_start.is_none());
    }

    #[test]
    fn explicit_start_supersedes_open_and_intermediate_history_without_another_page() {
        for history in [false, true] {
            for resume in [false, true] {
                let (mut app, mut capture) = start_app();
                if resume { select_resumable(&mut app); }
                let mut settings = app.model.settings_snapshot.clone().unwrap();
                settings.revision += 1;
                settings.settingsstate.workflows.train.autooutput = false;
                settings.settingsstate.workflows.train.request.outputdir = "saved-output".into();
                app.model.project_settings_snapshot(settings.clone()).unwrap();
                app.settings.install(&settings);
                app.advance_start();
                let opened = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingOpenRun);
                let pending = if history {
                    app.model.reduce_reply(opened.correlation, Ok(crate::generated::ApplicationReply::TrainingOpenRun(
                        crate::view_model::test_support::saved_training_run(settings.settingsstate.workflows.train.request.clone()))));
                    app.advance_start();
                    let first = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingHistory);
                    app.model.reduce_reply(first.correlation, Ok(crate::generated::ApplicationReply::TrainingHistory(
                        crate::generated::TrainingHistoryPage { generation: 3, nextcursor: 100, more: true, records: vec![] })));
                    app.advance_start();
                    next_intent(&mut capture, ApplicationIntentEndpoint::TrainingHistory)
                } else { opened };
                assert!(app.model.compute_start_available(app.settings.draft().unwrap(), FeatureId::Train));
                app.request_start(FeatureId::Train);
                assert!(app.model.workflow.output.saved().is_none());
                assert!(app.model.workflow.pending_start.is_some());
                assert!(capture.try_recv().is_err());
                let reply = if history {
                    crate::generated::ApplicationReply::TrainingHistory(crate::generated::TrainingHistoryPage {
                        generation: 3, nextcursor: 200, more: true, records: vec![],
                    })
                } else {
                    crate::generated::ApplicationReply::TrainingOpenRun(
                        crate::view_model::test_support::saved_training_run(settings.settingsstate.workflows.train.request.clone()))
                };
                app.model.reduce_reply(pending.correlation, Ok(reply));
                app.advance_start();
                next_intent(&mut capture, if resume { ApplicationIntentEndpoint::TrainingPrepareResume } else { ApplicationIntentEndpoint::ModelSelect });
                assert!(app.model.workflow.output.saved().is_none());
                assert!(capture.try_recv().is_err());
            }
        }
    }

    #[test]
    fn custom_inspection_failures_block_start_without_automatic_retry() {
        for native_error in [false, true] {
            let (mut app, mut capture) = start_app();
            let mut snapshot = app.model.settings_snapshot.clone().unwrap();
            snapshot.revision += 1;
            snapshot.settingsstate.workflows.train.modelsource = crate::generated::ModelSelectionSource::Custom;
            snapshot.settingsstate.workflows.train.request.weightspath = "/invalid.pt".into();
            app.model.project_settings_snapshot(snapshot.clone()).unwrap();
            app.settings.install(&snapshot);
            app.advance_start();
            let inspection = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingInspectCheckpoint);
            assert!(!app.model.compute_start_available(app.settings.draft().unwrap(), FeatureId::Train));
            if native_error {
                let mut fact = app.model.workflow.training.as_ref().unwrap().inspection.clone();
                fact.path = "/invalid.pt".into();
                fact.generation = 1;
                fact.status = crate::generated::TrainingInspectionStatus::Failed;
                fact.error = "corrupt checkpoint".into();
                app.model.reduce_reply(inspection.correlation, Ok(crate::generated::ApplicationReply::TrainingInspectCheckpoint(fact)));
            } else {
                app.model.reduce_reply(inspection.correlation, Err(crate::protocol::ApplicationError {
                    category: crate::generated::ApplicationErrorCategory::Failed, detail: "unavailable checkpoint".into(),
                }));
            }
            app.advance_start();
            assert!(matches!(app.model.workflow.train_continuation.capability, crate::view_model::CheckpointCapability::Failed(_)));
            assert!(!app.model.compute_start_available(app.settings.draft().unwrap(), FeatureId::Train));
            app.request_start(FeatureId::Train);
            assert!(app.model.workflow.pending_start.is_none());
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn deliberate_same_path_confirmation_refreshes_once_without_starting() {
        for failed in [false, true] {
            let (mut app, mut capture) = start_app();
            let checkpoint = select_resumable(&mut app);
            app.model.workflow.train_continuation.mode = crate::view_model::ContinuationMode::Transfer;
            app.model.workflow.train_continuation.mode_chosen = true;
            if failed {
                app.model.workflow.train_continuation.capability = crate::view_model::CheckpointCapability::Failed("old failure".into());
            }
            // Ordinary updates preserve the chosen mode and do no archive work.
            let mut unrelated = app.model.settings_snapshot.clone().unwrap();
            unrelated.revision += 1;
            unrelated.settingsstate.ui.darkmode = !unrelated.settingsstate.ui.darkmode;
            app.model.project_settings_snapshot(unrelated.clone()).unwrap();
            app.settings.install(&unrelated);
            app.advance_start();
            assert_eq!(app.model.workflow.train_continuation.mode, crate::view_model::ContinuationMode::Transfer);
            assert!(capture.try_recv().is_err());
            drop(app.on_model(FeatureId::Train, crate::view::workflow::model_card::Outcome::ArtifactConfirmed(EditSchedule::Debounce(1))));
            let refresh = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingInspectCheckpoint);
            assert!(app.model.workflow.pending_start.is_none());
            app.advance_start();
            assert!(capture.try_recv().is_err());
            let mut ready = app.model.workflow.training.as_ref().unwrap().inspection.clone();
            ready.path = checkpoint.path.clone();
            ready.generation += 1;
            ready.status = crate::generated::TrainingInspectionStatus::Ready;
            ready.checkpoint = Some(checkpoint);
            app.model.reduce_reply(refresh.correlation, Ok(crate::generated::ApplicationReply::TrainingInspectCheckpoint(ready)));
            app.advance_start();
            assert_eq!(app.model.workflow.train_continuation.mode, crate::view_model::ContinuationMode::Resume);
            assert!(app.model.workflow.pending_start.is_none());
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn reconnect_recovers_compact_capability_without_inspecting_again() {
        for status in [crate::generated::TrainingInspectionStatus::Ready, crate::generated::TrainingInspectionStatus::Failed] {
            let (mut app, mut capture) = start_app();
            let checkpoint = select_resumable(&mut app);
            app.model.workflow.train_continuation = Default::default();
            let inspection = &mut app.model.workflow.training.as_mut().unwrap().inspection;
            inspection.generation = 7;
            inspection.path = checkpoint.path.clone();
            inspection.status = status;
            inspection.checkpoint = (status == crate::generated::TrainingInspectionStatus::Ready).then_some(checkpoint.clone());
            inspection.error = "saved failure".into();
            app.advance_start();
            assert!(capture.try_recv().is_err());
            assert!(app.model.workflow.pending_start.is_none());
            if status == crate::generated::TrainingInspectionStatus::Ready {
                assert_eq!(app.model.workflow.train_continuation.checkpoint(), Some(&checkpoint));
            } else {
                assert!(matches!(app.model.workflow.train_continuation.capability, crate::view_model::CheckpointCapability::Failed(_)));
            }
            app.model.workflow.train_continuation = Default::default();
            drop(app.on_model(FeatureId::Train, crate::view::workflow::model_card::Outcome::ArtifactConfirmed(EditSchedule::Debounce(1))));
            next_intent(&mut capture, ApplicationIntentEndpoint::TrainingInspectCheckpoint);
            assert!(app.model.workflow.pending_start.is_none());
        }
    }

    #[test]
    fn confirmation_waits_for_cancelled_restore_and_validate_has_no_continuation_effect() {
        let (mut app, mut capture) = start_app();
        let checkpoint = select_resumable(&mut app);
        drop(app.on_model(FeatureId::Validate, crate::view::workflow::model_card::Outcome::ArtifactConfirmed(EditSchedule::Debounce(1))));
        assert!(!app.model.workflow.train_continuation.refresh_requested);
        assert_eq!(app.model.workflow.train_continuation.checkpoint(), Some(&checkpoint));
        assert!(capture.try_recv().is_err());
        app.request_start(FeatureId::Train);
        let restore = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingPrepareResume);
        drop(app.on_model(FeatureId::Train, crate::view::workflow::model_card::Outcome::ArtifactConfirmed(EditSchedule::Debounce(1))));
        assert!(app.model.workflow.pending_start.as_ref().unwrap().preparation.cancelled());
        assert!(capture.try_recv().is_err());
        app.model.reduce_reply(restore.correlation, Ok(crate::generated::ApplicationReply::TrainingPrepareResume(checkpoint)));
        app.advance_start();
        next_intent(&mut capture, ApplicationIntentEndpoint::TrainingInspectCheckpoint);
        assert!(app.model.workflow.pending_start.is_none());
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn unrequested_resume_reply_never_creates_start() {
        let (mut app, mut capture) = start_app();
        use crate::generated::TrainingApplicationProjection;
        app.model.project_training_reply(71, crate::generated::ApplicationReply::TrainingPrepareResume(crate::generated::TrainingCheckpointCapability {
            path: "/saved/full.pt".into(), resumable: true,
        }));
        app.advance_start();
        assert!(app.model.workflow.pending_start.is_none());
        assert!(capture.try_recv().is_err());
    }

    fn select_resumable(app: &mut App) -> crate::generated::TrainingCheckpointCapability {
        let mut snapshot = app.model.settings_snapshot.clone().unwrap();
        snapshot.revision += 1;
        let train = &mut snapshot.settingsstate.workflows.train;
        train.modelsource = crate::generated::ModelSelectionSource::Custom;
        train.request.weightspath = "/saved/full.pt".into();
        let checkpoint = crate::generated::TrainingCheckpointCapability {
            path: train.request.weightspath.clone(), resumable: true,
        };
        app.model.workflow.train_continuation.selection = Some((train.modelsource, train.request.weightspath.clone(), train.request.presetname.clone()));
        app.model.workflow.train_continuation.capability = crate::view_model::CheckpointCapability::Ready(checkpoint.clone());
        app.model.workflow.train_continuation.mode = crate::view_model::ContinuationMode::Resume;
        app.model.project_settings_snapshot(snapshot.clone()).unwrap();
        app.settings.install(&snapshot);
        checkpoint
    }

    #[test]
    fn external_weights_settle_to_transfer_before_explicit_model_preparation() {
        let (mut app, mut capture) = start_app();
        let mut checkpoint = select_resumable(&mut app);
        checkpoint.resumable = false;
        app.model.workflow.train_continuation.capability = crate::view_model::CheckpointCapability::Idle;
        app.advance_start();
        let inspection = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingInspectCheckpoint);
        let mut fact = app.model.workflow.training.as_ref().unwrap().inspection.clone();
        fact.path = checkpoint.path.clone();
        fact.generation = 1;
        fact.status = crate::generated::TrainingInspectionStatus::Running;
        app.model.reduce_reply(inspection.correlation, Ok(crate::generated::ApplicationReply::TrainingInspectCheckpoint(fact.clone())));
        app.advance_start();
        assert!(!app.model.compute_start_available(app.settings.draft().unwrap(), FeatureId::Train));
        assert!(capture.try_recv().is_err());
        fact.status = crate::generated::TrainingInspectionStatus::Ready;
        fact.checkpoint = Some(checkpoint);
        app.model.reduce_event(crate::generated::ApplicationEvent::TrainingTrainingInspectionChanged(
            crate::generated::TrainingInspectionChanged { inspection: fact }));
        app.advance_start();
        assert!(app.model.compute_start_available(app.settings.draft().unwrap(), FeatureId::Train));
        assert_eq!(app.model.workflow.train_continuation.mode, crate::view_model::ContinuationMode::Transfer);
        assert!(capture.try_recv().is_err());
        app.request_start(FeatureId::Train);
        next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        assert!(app.model.workflow.pending_start.as_ref().unwrap().resume_checkpoint.is_none());
    }

    #[test]
    fn explicit_resume_waits_for_restore_reply_and_settings_before_model_preparation() {
        for event_first in [false, true] {
            let (mut app, mut capture) = start_app();
            let checkpoint = select_resumable(&mut app);
            app.request_start(FeatureId::Train);
            let restore = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingPrepareResume);
            let mut snapshot = app.model.settings_snapshot.clone().unwrap();
            snapshot.revision += 1;
            let train = &mut snapshot.settingsstate.workflows.train;
            train.request.resumepath = checkpoint.path.clone();
            train.usecompileddirectorydefaults = false;
            train.request.traincompiledpath = "/restored/train.bin".into();
            train.request.valcompiledpath = "/restored/val.bin".into();
            if event_first {
                app.model.project_settings_snapshot(snapshot.clone()).unwrap();
                app.settings.install(&snapshot);
                app.advance_start();
                assert!(capture.try_recv().is_err());
            }
            app.model.reduce_reply(restore.correlation, Ok(crate::generated::ApplicationReply::TrainingPrepareResume(checkpoint)));
            if !event_first {
                app.advance_start();
                assert!(capture.try_recv().is_err());
                app.model.project_settings_snapshot(snapshot.clone()).unwrap();
                app.settings.install(&snapshot);
            }
            app.advance_start();
            next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
            let pending = app.model.workflow.pending_start.as_ref().unwrap();
            assert!(pending.inputs.matches(app.settings.draft().unwrap()));
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn mode_change_cancels_unstarted_resume_even_when_restore_reply_arrives_late() {
        let (mut app, mut capture) = start_app();
        let checkpoint = select_resumable(&mut app);
        app.request_start(FeatureId::Train);
        let restore = next_intent(&mut capture, ApplicationIntentEndpoint::TrainingPrepareResume);
        drop(app.on_train(crate::view::train::Outcome::Continuation(crate::view_model::ContinuationMode::Transfer)));
        app.model.reduce_reply(restore.correlation, Ok(crate::generated::ApplicationReply::TrainingPrepareResume(checkpoint)));
        app.advance_start();
        assert!(app.model.workflow.pending_start.is_none());
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn start_prepares_once_and_waits_for_event_before_reply_to_settle() {
        let (mut app, mut capture) = start_app();
        app.request_start(FeatureId::Train);
        let prepare = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        app.request_start(FeatureId::Train);
        assert!(capture.try_recv().is_err());
        let accepted =
            accepted_model_for(&app.model, app.settings.draft().unwrap(), FeatureId::Train);
        app.model
            .reduce_event(crate::generated::ApplicationEvent::ModelModelChanged(
                crate::generated::ModelChanged {
                    snapshot: accepted.clone(),
                },
            ));
        app.advance_start();
        assert!(capture.try_recv().is_err());
        app.model.reduce_reply(
            prepare.correlation,
            Ok(crate::generated::ApplicationReply::ModelSelect(accepted)),
        );
        app.advance_start();
        next_intent(&mut capture, ApplicationIntentEndpoint::TrainingStart);
        app.advance_start();
        app.request_start(FeatureId::Train);
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn start_flushes_drafts_and_waits_for_settings_reply_after_its_event() {
        let (mut app, mut capture) = start_app();
        app.settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_workflowstrainrequesttraincompiledpath(
                    draft,
                    "/selected/train.bin".into(),
                )
            })
            .unwrap();
        let mut saved = app.model.settings_snapshot.clone().unwrap();
        saved.revision += 1;
        saved.settingsstate = app.settings.draft().unwrap().clone();
        app.request_start(FeatureId::Train);
        let settings = next_intent(&mut capture, ApplicationIntentEndpoint::SettingsUpdate);
        app.model.settings_snapshot = Some(saved.clone());
        app.model.workflow.install_settings(&saved);
        app.advance_start();
        assert!(capture.try_recv().is_err());
        app.model.reduce_reply(
            settings.correlation,
            Ok(crate::generated::ApplicationReply::SettingsUpdate(saved)),
        );
        app.settle_settings_reply(Some(ApplicationIntentEndpoint::SettingsUpdate), true, false);
        app.advance_start();
        next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        assert!(capture.try_recv().is_err());
    }

    fn active_preparation(app: &App, feature: FeatureId) -> crate::generated::ModelUiState {
        let mut active = accepted_model_for(&app.model, app.settings.draft().unwrap(), feature);
        active.active = true;
        active.terminal.outcome = crate::generated::ModelSelectionOutcome::Idle;
        active
    }

    fn model_event(app: &mut App, snapshot: crate::generated::ModelUiState) {
        app.model
            .reduce_event(crate::generated::ApplicationEvent::ModelModelChanged(
                crate::generated::ModelChanged { snapshot },
            ));
        app.advance_start();
    }

    #[test]
    fn owned_preparation_receives_one_stop_before_or_after_select_admission() {
        for feature in [FeatureId::Train, FeatureId::Validate, FeatureId::Predict] {
            for stop_before_reply in [true, false] {
                let (mut app, mut capture) = start_app();
                app.workspace.select(feature);
                app.request_start(feature);
                let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
                let active = active_preparation(&app, feature);
                if stop_before_reply {
                    assert!(!app.guard_compute_stop(feature));
                    assert!(!app.guard_compute_stop(feature));
                    app.advance_start();
                    assert!(capture.try_recv().is_err());
                }
                app.model.reduce_reply(
                    select.correlation,
                    Ok(crate::generated::ApplicationReply::ModelSelect(
                        active.clone(),
                    )),
                );
                app.advance_start();
                if !stop_before_reply {
                    assert!(capture.try_recv().is_err());
                    assert!(!app.guard_compute_stop(feature));
                }
                let stop = next_intent(&mut capture, ApplicationIntentEndpoint::ModelStop);
                assert!(!app.guard_compute_stop(feature));
                app.advance_start();
                app.request_start(feature);
                assert!(capture.try_recv().is_err());
                let mut stopping = active.clone();
                stopping.terminal.outcome =
                    crate::generated::ModelSelectionOutcome::CancellationRequested;
                let mut terminal = active;
                terminal.active = false;
                terminal.terminal.outcome = crate::generated::ModelSelectionOutcome::Cancelled;
                if stop_before_reply {
                    model_event(&mut app, terminal.clone());
                    assert!(!app.guard_compute_stop(feature));
                    assert!(app.model.workflow.pending_start.is_some());
                    assert!(capture.try_recv().is_err());
                }
                app.model.reduce_reply(
                    stop.correlation,
                    Ok(crate::generated::ApplicationReply::ModelStop(stopping)),
                );
                app.advance_start();
                assert!(!app.guard_compute_stop(feature));
                assert!(capture.try_recv().is_err());
                if !stop_before_reply {
                    model_event(&mut app, terminal);
                }
                assert!(app.model.workflow.pending_start.is_none());
                assert!(capture.try_recv().is_err());
            }
        }
    }

    #[test]
    fn terminal_event_before_select_reply_retires_cancellation_without_stop_or_launch() {
        for outcome in [
            crate::generated::ModelSelectionOutcome::Accepted,
            crate::generated::ModelSelectionOutcome::Rejected,
            crate::generated::ModelSelectionOutcome::Cancelled,
        ] {
            let (mut app, mut capture) = start_app();
            app.request_start(FeatureId::Train);
            let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
            let active = active_preparation(&app, FeatureId::Train);
            assert!(!app.guard_compute_stop(FeatureId::Train));
            let mut terminal = active.clone();
            terminal.active = false;
            terminal.terminal.outcome = outcome;
            model_event(&mut app, terminal);
            assert!(!app.guard_compute_stop(FeatureId::Train));
            assert!(capture.try_recv().is_err());
            app.model.reduce_reply(
                select.correlation,
                Ok(crate::generated::ApplicationReply::ModelSelect(active)),
            );
            app.advance_start();
            assert!(app.model.workflow.pending_start.is_none());
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn stop_does_not_cancel_a_preexisting_independent_model_select() {
        let (mut app, mut capture) = start_app();
        app.request_model(FeatureId::Train);
        let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        app.request_start(FeatureId::Train);
        assert_eq!(
            app.model
                .workflow
                .pending_start
                .as_ref()
                .unwrap()
                .preparation,
            StartPreparation::Waiting
        );
        assert!(!app.guard_compute_stop(FeatureId::Train));
        assert!(app.model.workflow.pending_start.is_none());
        assert!(capture.try_recv().is_err());
        let accepted =
            accepted_model_for(&app.model, app.settings.draft().unwrap(), FeatureId::Train);
        app.model.reduce_reply(
            select.correlation,
            Ok(crate::generated::ApplicationReply::ModelSelect(accepted)),
        );
        app.advance_start();
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn cancellation_never_stops_a_newer_model_generation() {
        let (mut app, mut capture) = start_app();
        app.request_start(FeatureId::Train);
        let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        let owned = active_preparation(&app, FeatureId::Train);
        assert!(!app.guard_compute_stop(FeatureId::Train));
        let mut unrelated = owned.clone();
        unrelated.generation += 1;
        model_event(&mut app, unrelated);
        app.model.reduce_reply(
            select.correlation,
            Ok(crate::generated::ApplicationReply::ModelSelect(owned)),
        );
        app.advance_start();
        assert!(app.model.workflow.pending_start.is_none());
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn rejected_owned_select_retires_cancellation_without_stopping_other_work() {
        let (mut app, mut capture) = start_app();
        app.request_start(FeatureId::Train);
        let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        assert!(!app.guard_compute_stop(FeatureId::Train));
        app.model.reduce_reply(
            select.correlation,
            Err(crate::protocol::ApplicationError {
                category: crate::generated::ApplicationErrorCategory::Busy,
                detail: "A separate model operation was admitted first.".to_owned(),
            }),
        );
        app.advance_start();
        assert!(app.model.workflow.pending_start.is_none());
        assert_eq!(
            app.model.error.as_ref().unwrap().detail,
            "A separate model operation was admitted first."
        );
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn failed_start_submissions_retire_pending_work_and_keep_transport_error() {
        for endpoint in [
            ApplicationIntentEndpoint::TrainingPrepareResume,
            ApplicationIntentEndpoint::ModelSelect,
            ApplicationIntentEndpoint::TrainingStart,
            ApplicationIntentEndpoint::ModelStop,
        ] {
            let (mut app, mut capture) = start_app();
            match endpoint {
                ApplicationIntentEndpoint::TrainingPrepareResume => {
                    select_resumable(&mut app);
                }
                ApplicationIntentEndpoint::TrainingStart => {
                    app.model.model_snapshot = Some(accepted_model_for(
                        &app.model,
                        app.settings.draft().unwrap(),
                        FeatureId::Train,
                    ));
                }
                ApplicationIntentEndpoint::ModelStop => {
                    app.request_start(FeatureId::Train);
                    let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
                    let active = active_preparation(&app, FeatureId::Train);
                    app.model.reduce_reply(
                        select.correlation,
                        Ok(crate::generated::ApplicationReply::ModelSelect(active)),
                    );
                }
                _ => {}
            }
            app.connection = None;
            if endpoint == ApplicationIntentEndpoint::ModelStop {
                assert!(!app.guard_compute_stop(FeatureId::Train));
            } else {
                app.request_start(FeatureId::Train);
            }
            assert!(app.model.workflow.pending_start.is_none());
            assert!(!app.model.has_pending(endpoint));
            assert_eq!(
                app.model.error.as_ref().unwrap().detail,
                "browser connection is not ready"
            );
            app.advance_start();
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn input_edits_keep_owned_cancellation_until_select_settles() {
        let (mut app, mut capture) = start_app();
        app.request_start(FeatureId::Train);
        let select = next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        let active = active_preparation(&app, FeatureId::Train);
        let schedule = app
            .settings
            .state_mut()
            .edit(EditCadence::Debounced, |draft| {
                crate::generated::edit_workflowstrainrequesttraincompiledpath(
                    draft,
                    "/other/train.bin".into(),
                )
            })
            .unwrap();
        drop(app.handle_settings_schedule(schedule));
        assert!(
            app.model
                .workflow
                .pending_start
                .as_ref()
                .unwrap()
                .preparation
                .cancelled()
        );
        assert!(capture.try_recv().is_err());
        app.model.reduce_reply(
            select.correlation,
            Ok(crate::generated::ApplicationReply::ModelSelect(active)),
        );
        app.advance_start();
        next_intent(&mut capture, ApplicationIntentEndpoint::ModelStop);
        assert!(capture.try_recv().is_err());
    }

    #[test]
    fn route_and_connection_replacement_retire_unlaunched_work() {
        let (mut app, mut capture) = start_app();
        app.request_start(FeatureId::Train);
        next_intent(&mut capture, ApplicationIntentEndpoint::ModelSelect);
        app.workspace.select(FeatureId::Predict);
        app.advance_start();
        // CLEANUP-IGNORE: Route changes independently assert cancellation, as do input edits.
        assert!(
            app.model
                .workflow
                .pending_start
                .as_ref()
                .unwrap()
                .preparation
                .cancelled()
        );
        assert!(capture.try_recv().is_err());
        app.model.peer_connected();
        assert!(app.model.workflow.pending_start.is_none());
    }
}
