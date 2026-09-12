use super::*;

impl App {
    fn handle_explore_send_error(
        &mut self,
        error: crate::transport_connection::OutboundSendError,
    ) -> Task<Message> {
        match error {
            crate::transport_connection::OutboundSendError::Closed => {
                self.retire_peer(UiError::transport(error.to_string()));
            }
            crate::transport_connection::OutboundSendError::Capacity => {}
            crate::transport_connection::OutboundSendError::Allocation => {
                self.retire_peer(UiError::transport("retained outbound allocation failed"));
            }
        }
        Task::none()
    }

    pub(super) fn classify_explore_reply(
        endpoint: Option<ApplicationIntentEndpoint>,
        decoded: &Result<crate::generated::ApplicationReply, crate::protocol::ApplicationError>,
    ) -> (Option<u64>, bool) {
        match (endpoint, decoded) {
            (
                Some(ApplicationIntentEndpoint::ExploreUpdateFilter),
                Ok(crate::generated::ApplicationReply::ExploreUpdateFilter(snapshot)),
            )
            | (
                Some(ApplicationIntentEndpoint::ExploreUpdateOverlay),
                Ok(crate::generated::ApplicationReply::ExploreUpdateOverlay(snapshot)),
            ) => (Some(snapshot.revision), false),
            (Some(ApplicationIntentEndpoint::ExploreUpdateFilter), Err(_)) => (None, true),
            (Some(ApplicationIntentEndpoint::ExploreUpdateOverlay), Err(_)) => (None, true),
            _ => (None, false),
        }
    }

    pub(super) fn explore_event_failed(event: &crate::generated::ApplicationEvent) -> bool {
        matches!(
            event,
            crate::generated::ApplicationEvent::ExploreExploreFailed(_)
        )
    }

    pub(super) fn abandon_explore_edit(&mut self, context: ApplicationIntentEndpoint) {
        if matches!(
            context,
            ApplicationIntentEndpoint::ExploreUpdateFilter
                | ApplicationIntentEndpoint::ExploreUpdateOverlay
        ) {
            self.workspace.explore_abandon_submission();
            self.model.explore.desired_filter = None;
            self.model.explore.desired_overlay = None;
        }
    }

    pub(super) fn settle_explore_reply(&mut self, admission_revision: Option<u64>, failed: bool) {
        if let Some(revision) = admission_revision {
            self.workspace.explore_record_admission(revision);
        } else if failed {
            self.workspace.explore_abandon_submission();
        }
    }

    pub(super) fn settle_explore_event(&mut self, failed: bool) {
        if failed {
            self.workspace.explore_abandon_submission();
            self.workspace.explore_clear_viewport_admission();
        }
        self.reconcile_explore_viewport();
    }

    pub(super) fn on_explore(&mut self, outcome: crate::view::explore::Outcome) -> Task<Message> {
        match outcome {
            crate::view::explore::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
            crate::view::explore::Outcome::OpenRequested => {
                let local_edits = self.settings.has_local_edits();
                let open_available = self.model.explore_open_available();
                if let Some(integration) = self.integration.as_mut() {
                    integration.observe_reporting(|reporting| {
                        reporting.observe_explore_open_request(local_edits, open_available);
                    });
                }
                if local_edits || !open_available {
                    self.model.error = Some(UiError::busy(
                        "Explore is unavailable or already changing state.",
                    ));
                    return Task::none();
                }
                if self.settings_unsettled() {
                    self.model.error =
                        Some(UiError::busy("Wait for Explore settings to finish saving."));
                    return Task::none();
                }
                let Some(settings) = self.model.settings_snapshot.as_ref() else {
                    self.model.error =
                        Some(UiError::invalid("Explore settings are not installed yet."));
                    return Task::none();
                };
                if !settings.exploresource.available {
                    self.model.error = Some(UiError::invalid(
                        "The selected Explore dataset has no compiled artifact path.",
                    ));
                    return Task::none();
                }
                if let Some(integration) = self.integration.as_mut() {
                    integration.observe_reporting(|reporting| {
                        let columns = settings
                            .settingsstate
                            .workflows
                            .explore
                            .gridwidth
                            .clamp(1, 99) as u32;
                        reporting.observe_explore_open_layout(
                            self.workspace
                                .explore_measured_viewport(columns, 0, 0)
                                .is_some(),
                            self.model.explore.snapshot.as_ref(),
                            columns,
                        );
                    });
                }
                self.model.explore.desired_open = true;
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::StopRequested => {
                if self.model.explore_stop_available() {
                    self.workspace.explore_clear_viewport_admission();
                    self.submit_intent(
                        ApplicationIntentEndpoint::ExploreStop,
                        crate::generated::encode_explore_Stop,
                    );
                } else {
                    self.model.error = Some(UiError::busy(
                        "Explore is not running or is already stopping.",
                    ));
                }
            }
            crate::view::explore::Outcome::DialogRequested(field_id) => {
                self.open_dialog(field_id);
            }
            crate::view::explore::Outcome::OpenAnnotationRequested => {
                if self.copy_viewer_to_annotation() {
                    return self.transition_page(FeatureId::Annotate);
                }
            }
            crate::view::explore::Outcome::UpscaleRequested(kernel) => {
                if !self.model.upscale_start_available() {
                    self.model.error = Some(UiError::busy(
                        "The selected Explore detail is unavailable for upscaling.",
                    ));
                    return Task::none();
                }
                let source = self
                    .model
                    .explore
                    .snapshot
                    .as_ref()
                    .map(|snapshot| snapshot.frame.clone());
                let Some(source) =
                    source.filter(|frame| ApplicationModel::valid_visual_source(frame).is_some())
                else {
                    self.model.error = Some(UiError::presentation(
                        "Explore detail frame is unavailable.",
                    ));
                    return Task::none();
                };
                if let Some(integration) = self.integration.as_mut() {
                    integration.observe_upscale_request(kernel);
                }
                self.model
                    .request_upscale(crate::generated::UpscaleRequest {
                        source,
                        kernel,
                        document: self
                            .model
                            .explore
                            .snapshot
                            .as_ref()
                            .expect("selected source")
                            .document
                            .clone(),
                    });
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::OverlayUpdated(request) => {
                self.model.explore.desired_overlay = Some(request);
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::FilterEdited(request) => {
                let local_edits = self.settings.has_local_edits();
                let mutation_available = self.model.explore_mutation_available();
                if let Some(integration) = self.integration.as_mut() {
                    integration.observe_reporting(|reporting| {
                        reporting.observe_explore_filter_request(
                            &request,
                            self.model.explore.snapshot.as_ref(),
                            local_edits,
                            mutation_available,
                        );
                    });
                }
                if local_edits || !mutation_available {
                    self.model.error = Some(UiError::busy(
                        "Explore filters are unavailable or already changing.",
                    ));
                    return Task::none();
                }
                self.workspace.explore_record_submission(request.clone());
                self.model.explore.desired_filter = Some(request);
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::RerollRequested => {
                self.model.explore.desired_reroll = true;
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::AugmentationUpdated(request) => {
                if self.settings.has_local_edits()
                    || !self.model.explore_augmentation_update_available()
                {
                    self.model.error = Some(UiError::busy(
                        "Explore augmentation preview is already changing.",
                    ));
                    return Task::none();
                }
                self.model.set_foreground_feature(FeatureId::Explore);
                self.model.explore.desired_augmentation = Some(request);
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::AugmentationRerollRequested => {
                self.model.set_foreground_feature(FeatureId::Explore);
                self.model.explore.desired_augmentation_reroll = true;
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::DetailUpdated(request) => {
                self.model.explore.desired_detail = Some(request);
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::ImageSelected(compiledindex) => {
                if self
                    .model
                    .explore
                    .snapshot
                    .as_ref()
                    .is_none_or(|snapshot| snapshot.selectedimage != Some(compiledindex))
                {
                    self.abandon_viewer();
                }
                self.model.set_foreground_feature(FeatureId::Explore);
                self.model.explore.requested_selection = Some(compiledindex);
                self.model.explore.desired_selection = Some(compiledindex);
                self.model.explore.desired_navigation = None;
                self.model.explore.desired_close = false;
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::PreviousRequested => {
                self.request_explore_navigation(ExploreNavigation::Previous);
            }
            crate::view::explore::Outcome::NextRequested => {
                self.request_explore_navigation(ExploreNavigation::Next);
            }
            crate::view::explore::Outcome::CloseDetailRequested => {
                self.abandon_viewer();
                self.model.set_foreground_feature(FeatureId::Explore);
                self.model.explore.desired_navigation = None;
                self.model.explore.desired_close = true;
                self.dispatch_explore_desired();
            }
            crate::view::explore::Outcome::ScrollRequested(offset) => {
                return iced::widget::operation::scroll_to(
                    crate::view::explore::GALLERY_WORKSPACE_ID,
                    iced::widget::operation::AbsoluteOffset { x: 0.0, y: offset },
                );
            }
            crate::view::explore::Outcome::ViewportChanged(request) => {
                return self.request_explore_viewport(request);
            }
        }
        Task::none()
    }

    pub(super) fn request_explore_viewport(
        &mut self,
        request: ExploreViewportUpdate,
    ) -> Task<Message> {
        self.workspace
            .explore_request_viewport(self.model.explore.snapshot.as_ref(), request);
        self.dispatch_explore_viewport()
    }

    pub(super) fn reconcile_explore_viewport(&mut self) {
        if self.settings.has_local_edits() {
            return;
        }
        let Some(columns) = self.model.settings_snapshot.as_ref().map(|settings| {
            settings
                .settingsstate
                .workflows
                .explore
                .gridwidth
                .clamp(1, 99) as u32
        }) else {
            return;
        };
        let Some(snapshot) = self
            .model
            .explore
            .snapshot
            .as_ref()
            .filter(|snapshot| snapshot.ready)
        else {
            return;
        };
        if let Some(request) = self.workspace.explore_measured_layout_request(
            Some(snapshot),
            columns,
            snapshot.order.matchingcount,
        ) {
            self.workspace
                .explore_request_viewport(self.model.explore.snapshot.as_ref(), request);
        }
    }

    pub(super) fn dispatch_explore_viewport(&mut self) -> Task<Message> {
        self.dispatch_explore_desired();
        if self.settings.has_local_edits()
            || !self.model.explore_viewport_available()
            || self.model.has_explore_pending()
            || self
                .model
                .explore
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| snapshot.busy)
        {
            return Task::none();
        }
        let Some(request) = self.workspace.explore_dispatchable_viewport() else {
            return Task::none();
        };
        let Some(connection) = self.connection.as_mut() else {
            self.workspace.explore_clear_viewport_admission();
            return Task::none();
        };
        let interaction = match crate::generated::encode_explore_UpdateViewport(request.clone()) {
            Ok(interaction) => interaction,
            Err(error) => {
                self.retire_peer(UiError::transport(error.to_string()));
                return Task::none();
            }
        };
        match connection.send_interaction(interaction) {
            Ok(crate::transport_connection::SendDisposition::Queued) => {
                self.workspace.explore_viewport_queued(request);
                Task::none()
            }
            Ok(crate::transport_connection::SendDisposition::Dropped) => {
                if self.workspace.explore_arm_writable_wait() {
                    let peer_generation = self.peer_generation;
                    Task::perform(connection.clone().writable(), move |result| {
                        Message::ExploreWritable {
                            peer_generation,
                            result,
                        }
                    })
                } else {
                    Task::none()
                }
            }
            Err(error) => self.handle_explore_send_error(error),
        }
    }

    fn dispatch_explore_desired(&mut self) {
        self.reconcile_viewer();
        if self.connection.is_none() {
            self.abandon_explore_edit(ApplicationIntentEndpoint::ExploreUpdateOverlay);
            return;
        }
        self.dispatch_explore_open();
        if !self.model.has_explore_pending()
            && self.model.explore.desired_close
            && self.submit_intent(
                ApplicationIntentEndpoint::ExploreCloseDetail,
                crate::generated::encode_explore_CloseDetail,
            )
        {
            self.model.explore.desired_close = false;
        }
        if !self.model.has_explore_pending()
            && let Some(direction) = self.model.explore.desired_navigation
            && self.submit_intent(ApplicationIntentEndpoint::ExploreNavigate, |correlation| {
                crate::generated::encode_explore_Navigate(
                    correlation,
                    ExploreNavigate { direction },
                )
            })
        {
            self.model.explore.desired_navigation = None;
        }
        if !self.model.has_explore_pending()
            && let Some(mut request) = self.model.explore.desired_filter.clone()
        {
            if let Some(overlay) = self.model.explore.desired_overlay.as_ref() {
                request.overlay = overlay.clone();
            }
            let submitted = self.submit_intent(
                ApplicationIntentEndpoint::ExploreUpdateFilter,
                |correlation| crate::generated::encode_explore_UpdateFilter(correlation, request),
            );
            if let Some(integration) = self.integration.as_mut() {
                integration.observe_reporting(|reporting| {
                    reporting.observe_explore_filter_submission(submitted);
                });
            }
            if submitted {
                self.model.explore.desired_filter = None;
                self.model.explore.desired_overlay = None;
            }
        }
        if !self.model.has_explore_pending()
            && let Some(request) = self.model.explore.desired_overlay.clone()
            && self.submit_intent(
                ApplicationIntentEndpoint::ExploreUpdateOverlay,
                |correlation| crate::generated::encode_explore_UpdateOverlay(correlation, request),
            )
        {
            self.model.explore.desired_overlay = None;
        }
        if !self.model.has_explore_pending()
            && let Some(request) = self.model.explore.desired_augmentation.clone()
            && self.submit_intent(
                ApplicationIntentEndpoint::ExploreUpdateAugmentation,
                |correlation| {
                    crate::generated::encode_explore_UpdateAugmentation(correlation, request)
                },
            )
        {
            self.model.explore.desired_augmentation = None;
        }
        if !self.model.has_explore_pending()
            && let Some(request) = self.model.explore.desired_detail.clone()
            && self.submit_intent(
                ApplicationIntentEndpoint::ExploreUpdateDetail,
                |correlation| crate::generated::encode_explore_UpdateDetail(correlation, request),
            )
        {
            self.model.explore.desired_detail = None;
        }
        if !self.model.has_explore_pending()
            && self.model.explore.desired_reroll
            && self.submit_intent(
                ApplicationIntentEndpoint::ExploreReroll,
                crate::generated::encode_explore_Reroll,
            )
        {
            self.model.explore.desired_reroll = false;
        }
        if !self.model.has_explore_pending()
            && self.model.explore.desired_augmentation_reroll
            && self.submit_intent(
                ApplicationIntentEndpoint::ExploreRerollAugmentation,
                crate::generated::encode_explore_RerollAugmentation,
            )
        {
            self.model.explore.desired_augmentation_reroll = false;
        }
        if !self.model.has_explore_pending()
            && let Some(compiledindex) = self.model.explore.desired_selection
            && self.submit_intent(ApplicationIntentEndpoint::ExploreSelect, |correlation| {
                crate::generated::encode_explore_Select(
                    correlation,
                    ExploreSelect { compiledindex },
                )
            })
        {
            self.model.explore.desired_selection = None;
        }
        self.dispatch_viewer_desired();
    }

    fn dispatch_explore_open(&mut self) {
        if !self.model.explore.desired_open
            || self.settings.has_local_edits()
            || self.settings_unsettled()
            || !self.model.explore_open_available()
        {
            return;
        }
        let Some(settings) = self.model.settings_snapshot.as_ref() else {
            return;
        };
        let columns = settings
            .settingsstate
            .workflows
            .explore
            .gridwidth
            .clamp(1, 99) as u32;
        let Some(viewport) = self.workspace.explore_measured_viewport(columns, 0, 0) else {
            if let Some(integration) = self.integration.as_mut() {
                integration.observe_reporting(|reporting| {
                    reporting.observe_explore_open_layout(
                        false,
                        self.model.explore.snapshot.as_ref(),
                        columns,
                    );
                });
            }
            return;
        };
        let compiled_source = settings.exploresource.compiledsource.clone();
        let submitted =
            self.submit_intent(ApplicationIntentEndpoint::ExploreOpen, move |correlation| {
                crate::generated::encode_explore_Open(
                    correlation,
                    ExploreOpen {
                        viewport,
                        compiledsource: compiled_source,
                    },
                )
            });
        if let Some(integration) = self.integration.as_mut() {
            integration.observe_reporting(|reporting| {
                reporting.observe_explore_open_submission(submitted);
            });
        }
        if submitted {
            self.model.explore.desired_open = false;
        }
    }

    pub(super) fn dispatch_viewer_desired(&mut self) {
        if self.presentation.stop_requested {
            if self.model.has_upscale_pending() {
                return;
            }
            if !self.submit_intent(
                ApplicationIntentEndpoint::UpscaleStop,
                crate::generated::encode_upscale_Stop,
            ) {
                return;
            }
            self.presentation.stop_requested = false;
            return;
        }
        if !self.model.has_upscale_pending()
            && self
                .model
                .explore
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| !snapshot.renderpending)
            && let Some(request) = self.model.explore.requested_upscale.clone()
            && self.model.explore.sent_upscale.as_ref() != Some(&request)
        {
            let sent = request.clone();
            if self.submit_intent(ApplicationIntentEndpoint::UpscaleStart, |correlation| {
                crate::generated::encode_upscale_Start(correlation, request)
            }) {
                self.model.explore.sent_upscale = Some(sent);
            }
        }
    }

    fn request_explore_navigation(&mut self, direction: ExploreNavigation) {
        self.abandon_viewer();
        self.model.set_foreground_feature(FeatureId::Explore);
        self.model.explore.desired_close = false;
        self.model.explore.desired_navigation = Some(direction);
        self.dispatch_explore_desired();
    }

    pub(super) fn on_explore_writable(
        &mut self,
        peer_generation: u64,
        result: Result<(), crate::transport_connection::OutboundSendError>,
    ) -> Task<Message> {
        if peer_generation != self.peer_generation {
            return Task::none();
        }
        self.workspace.explore_viewport_writable();
        match result {
            Ok(()) => self.dispatch_explore_viewport(),
            Err(error) => self.handle_explore_send_error(error),
        }
    }
}
