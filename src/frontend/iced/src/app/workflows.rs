use super::*;

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
                if self.guard_compute_start(FeatureId::Train) {
                    self.submit_intent(ApplicationIntentEndpoint::TrainingStart, |correlation| {
                        crate::generated::encode_training_Start(correlation, Train {})
                    });
                }
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
            crate::view::validate::Outcome::StartRequested => {
                if self.guard_compute_start(FeatureId::Validate) {
                    self.submit_intent(ApplicationIntentEndpoint::ValidationStart, |correlation| {
                        crate::generated::encode_validation_Start(
                            correlation,
                            crate::generated::ValidateWorkflowIntent {},
                        )
                    });
                }
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
            crate::view::predict::Outcome::StartRequested => {
                if self.guard_compute_start(FeatureId::Predict) {
                    self.submit_intent(ApplicationIntentEndpoint::PredictStart, |correlation| {
                        crate::generated::encode_predict_Start(
                            correlation,
                            crate::generated::PredictWorkflowIntent {},
                        )
                    });
                }
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
