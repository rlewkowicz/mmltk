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
        self.model.workflow.training_run = None;
        self.model.workflow.training_history = None;
        self.workspace.sync_workflows(&self.model);
    }

    fn request_start(&mut self, feature: FeatureId) {
        if feature == FeatureId::Train {
            self.model.workflow.resume_ready = None;
        }
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
        if feature == FeatureId::Train {
            self.show_live_training();
        }
        self.model.workflow.pending_start = Some(crate::view_model::PendingStart {
            feature,
            inputs,
            preparation: StartPreparation::Waiting,
            resume_checkpoint: None,
        });
        self.model.workflow.start_status = Some((feature, "Saving current settings…".to_owned()));
        self.flush_settings_edits();
        self.advance_start();
    }

    pub(super) fn advance_start(&mut self) {
        if self.model.workflow.pending_start.is_none()
            && self.workspace.active() == FeatureId::Train
        {
            if let Some(checkpoint) = self.model.workflow.resume_ready.as_ref() {
                if self.settings.draft().is_some_and(|draft| {
                    draft.workflows.train.request.resumepath == checkpoint.path
                }) && !self.settings_unsettled()
                {
                    let checkpoint = self.model.workflow.resume_ready.take().unwrap();
                    self.show_live_training();
                    let inputs = crate::view_model::StartInputs::capture(
                        self.settings.draft().unwrap(),
                        FeatureId::Train,
                    )
                    .unwrap();
                    self.model.workflow.pending_start = Some(crate::view_model::PendingStart {
                        feature: FeatureId::Train,
                        inputs,
                        preparation: StartPreparation::Waiting,
                        resume_checkpoint: Some(checkpoint.path),
                    });
                }
            }
        }
        let Some(pending) = self.model.workflow.pending_start.as_ref() else {
            return;
        };
        let feature = pending.feature;
        if pending.preparation.cancelled() {
            self.advance_start_cancellation();
            return;
        }
        if self.workspace.active() != feature {
            self.model
                .workflow
                .cancel_start("Start cancelled after leaving the workflow.");
            self.advance_start_cancellation();
            return;
        }
        let Some(draft) = self.settings.draft() else {
            return;
        };
        if !pending.inputs.matches(draft) {
            self.model
                .workflow
                .cancel_start("Start cancelled because its inputs changed.");
            self.advance_start_cancellation();
            return;
        }
        if self.settings_unsettled() {
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
                self.model.workflow.start_status =
                    Some((feature, "Waiting for model preparation…".to_owned()));
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
                let detail = if snapshot.terminal.detail.is_empty() {
                    "Model preparation did not produce the selected model.".to_owned()
                } else {
                    snapshot.terminal.detail.clone()
                };
                self.model.workflow.start_status = Some((feature, detail));
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
            self.model.workflow.start_status =
                Some((feature, "Preparing the selected model…".to_owned()));
            if !self.submit_registered_intent(ApplicationIntentEndpoint::ModelSelect, registered) {
                self.model.workflow.start_status = Some((
                    feature,
                    "Model preparation could not be submitted.".to_owned(),
                ));
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
        self.model.workflow.start_status = Some((
            feature,
            "Inspecting selected inputs and starting…".to_owned(),
        ));
        let submitted = match feature {
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
        if !submitted {
            self.model.workflow.start_status =
                Some((feature, "Start could not be submitted.".to_owned()));
        }
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
            self.model.workflow.start_status = Some((
                feature,
                "Model preparation cancellation could not be submitted.".to_owned(),
            ));
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
            self.model.workflow.cancel_start("Start cancelled.");
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
            self.model.workflow.cancel_start("Start cancelled.");
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
                    Output::Browse(id) => self.open_dialog(id),
                    Output::Live => self.show_live_training(),
                    Output::Open(directory) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::TrainingOpenRun,
                            |correlation| {
                                crate::generated::encode_training_OpenRun(
                                    correlation,
                                    crate::generated::TrainingDirectoryQuery { directory },
                                )
                            },
                        );
                    }
                    Output::History(query) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::TrainingHistory,
                            |correlation| {
                                crate::generated::encode_training_History(correlation, query)
                            },
                        );
                    }
                    Output::Inspect(path) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::TrainingInspectCheckpoint,
                            |correlation| {
                                crate::generated::encode_training_InspectCheckpoint(
                                    correlation,
                                    crate::generated::TrainingCheckpointQuery { path },
                                )
                            },
                        );
                    }
                    Output::Resume(path) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::TrainingPrepareResume,
                            |correlation| {
                                crate::generated::encode_training_PrepareResume(
                                    correlation,
                                    crate::generated::TrainingCheckpointQuery { path },
                                )
                            },
                        );
                    }
                }
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
            crate::view::validate::Outcome::Details(query) => {
                self.submit_intent(
                    ApplicationIntentEndpoint::ValidationDetails,
                    |correlation| crate::generated::encode_validation_Details(correlation, query),
                );
            }
            crate::view::validate::Outcome::Sample(message) => {
                use crate::view::validate::samples::Message as Sample;
                match message {
                    Sample::Select(identity) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::ValidationSelectSample,
                            |correlation| {
                                crate::generated::encode_validation_SelectSample(
                                    correlation,
                                    identity,
                                )
                            },
                        );
                    }
                    Sample::Close => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::ValidationCloseDetail,
                            crate::generated::encode_validation_CloseDetail,
                        );
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
                    Sample::Fit | Sample::Labels(..) => {}
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
                self.submit_intent(ApplicationIntentEndpoint::PredictPause, |correlation| {
                    crate::generated::encode_predict_Pause(
                        correlation,
                        crate::generated::PredictPauseIntent { paused },
                    )
                });
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

    #[test]
    fn output_inspection_and_history_use_direct_operations_without_starting() {
        use crate::view::train::{Outcome, output::Message as Output};
        for (message, endpoint) in [
            (
                Output::Open("/saved/run".into()),
                ApplicationIntentEndpoint::TrainingOpenRun,
            ),
            (
                Output::Inspect("/copied/full.pt".into()),
                ApplicationIntentEndpoint::TrainingInspectCheckpoint,
            ),
            (
                Output::History(crate::generated::TrainingHistoryQuery {
                    generation: 7,
                    cursor: 128,
                    count: 32,
                }),
                ApplicationIntentEndpoint::TrainingHistory,
            ),
        ] {
            let (mut app, mut capture) = start_app();
            drop(app.on_train(Outcome::Output(message)));
            next_intent(&mut capture, endpoint);
            assert!(app.model.workflow.pending_start.is_none());
            assert!(app.model.workflow.resume_ready.is_none());
            assert!(capture.try_recv().is_err());
        }
    }

    #[test]
    fn start_resume_and_live_selection_invalidate_selected_history_immediately() {
        for action in 0..3 {
            let resume = action == 1;
            let (mut app, _capture) = start_app();
            app.model.workflow.training_history = Some(crate::generated::TrainingHistoryPage {
                generation: 7,
                nextcursor: 32,
                more: true,
                records: Vec::new(),
            });
            if resume {
                let snapshot = app.model.settings_snapshot.as_mut().unwrap();
                snapshot.settingsstate.workflows.train.request.resumepath = "/saved/full.pt".into();
                app.settings.install(snapshot);
                app.model.workflow.resume_ready = Some(crate::generated::TrainingCheckpoint {
                    path: "/saved/full.pt".into(),
                    attemptid: "old".into(),
                    originalweights: String::new(),
                    originalclassdescriptor: String::new(),
                    resumable: true,
                    epoch: 1,
                    configuration: None,
                    classlayout: None,
                    evaluatedweights: crate::generated::EvaluatedWeights::Ordinary,
                });
                app.advance_start();
            } else if action == 0 {
                app.request_start(FeatureId::Train);
            } else {
                drop(app.on_train(crate::view::train::Outcome::Output(
                    crate::view::train::output::Message::Live,
                )));
            }
            assert!(app.model.workflow.training_run.is_none());
            assert!(app.model.workflow.training_history.is_none());
            if action == 2 {
                assert!(app.model.workflow.pending_start.is_none());
            } else {
                let pending = app.model.workflow.pending_start.as_ref().unwrap();
                assert_eq!(pending.resume_checkpoint.is_some(), resume);
            }
        }
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
        assert!(capture.try_recv().is_err());
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
