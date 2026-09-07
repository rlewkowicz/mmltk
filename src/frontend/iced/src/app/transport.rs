use super::*;

impl App {
    pub(super) fn retire_peer(&mut self, error: UiError) {
        if self.connection.is_none()
            && self.model.connection == crate::view_model::ConnectionState::Reconnecting
        {
            return;
        }
        self.connection = None;
        self.model.peer_disconnected(error);
        self.settings.reset_transport();
        self.workspace.reset_transport(&self.model);
    }

    pub(super) fn retire_if_closed(
        &mut self,
        result: Result<
            crate::transport_connection::SendDisposition,
            crate::transport_connection::OutboundSendError,
        >,
    ) -> bool {
        match result {
            Ok(_) | Err(crate::transport_connection::OutboundSendError::Capacity) => false,
            Err(crate::transport_connection::OutboundSendError::Closed) => {
                self.retire_peer(UiError::transport(
                    crate::transport_connection::OutboundSendError::Closed.to_string(),
                ));
                true
            }
        }
    }

    pub(super) fn on_transport(&mut self, event: TransportEvent) -> Task<Message> {
        if self.connection.is_none()
            && self.model.connection == crate::view_model::ConnectionState::Reconnecting
            && matches!(
                &event,
                TransportEvent::Bootstrap(_)
                    | TransportEvent::IntentReply(_)
                    | TransportEvent::SystemEvent(_)
            )
        {
            return Task::none();
        }
        match event {
            TransportEvent::Connected(mut connection) => {
                self.peer_generation = self.peer_generation.wrapping_add(1).max(1);
                self.model.peer_connected();
                self.settings.reset_transport();
                self.workspace.reset_transport(&self.model);
                self.presentation.reset_failure();
                if matches!(
                    connection.send_renderer_observation(RendererObservation::Ready),
                    Err(crate::transport_connection::OutboundSendError::Closed)
                ) {
                    self.retire_peer(UiError::transport(
                        crate::transport_connection::OutboundSendError::Closed.to_string(),
                    ));
                    return Task::none();
                }
                self.connection = Some(connection);
                self.send_surface_observation();
            }
            TransportEvent::Bootstrap(bootstrap) => self.install_bootstrap(bootstrap),
            TransportEvent::IntentReply(reply) => self.reduce_reply(reply),
            TransportEvent::SystemEvent(event) => self.reduce_event(event),
            TransportEvent::Disconnected(reason) => {
                self.retire_peer(UiError::transport(reason));
            }
            TransportEvent::ProtocolError(error) => {
                self.retire_peer(UiError::protocol(error));
            }
        }
        self.dispatch_explore_viewport()
    }

    pub(super) fn install_bootstrap(&mut self, bootstrap: Bootstrap) {
        self.presentation.reset_failure();
        if let Err(error) = self
            .model
            .install_bootstrap(bootstrap.schema_fingerprint, bootstrap.snapshots)
        {
            self.retire_peer(error);
            return;
        }
        if let Some(authoritative) = self.model.settings_snapshot.as_ref() {
            self.settings.install(authoritative);
            self.workspace
                .rebase(authoritative.settingsstate.currentview, &self.model);
            self.model
                .set_foreground_feature(authoritative.settingsstate.currentview);
        }
        self.workspace.bootstrap_components(&self.model);
        self.reconcile_explore_viewport();
        self.reconcile_presentation(None, true);
    }

    pub(super) fn reduce_reply(&mut self, reply: IntentReply) {
        let Some(endpoint_id) = self.model.pending_endpoint(reply.correlation) else {
            self.model.error = Some(UiError::protocol("unknown or duplicate IntentReply"));
            return;
        };
        let context = self.model.pending_intent(reply.correlation);
        let settings_revision_before = self
            .model
            .settings_snapshot
            .as_ref()
            .map(|snapshot| snapshot.revision);
        let decoded = reply.result.and_then(|value| {
            crate::generated::decode_application_reply(endpoint_id, value).map_err(|detail| {
                crate::protocol::ApplicationError {
                    category: crate::generated::ApplicationErrorCategory::Failed,
                    detail: format!("Invalid IntentReply: {detail}"),
                }
            })
        });
        let settings_mutation_succeeded = context.is_some_and(|endpoint| {
            matches!(
                endpoint,
                ApplicationIntentEndpoint::SettingsUpdate
                    | ApplicationIntentEndpoint::SettingsReset
            )
        }) && decoded.is_ok();
        let (filter_admission_revision, filter_failed) =
            Self::classify_explore_reply(context, &decoded);
        let mut refresh = self.model.reduce_reply(reply.correlation, decoded);
        let installed_settings = self
            .model
            .settings_snapshot
            .as_ref()
            .is_some_and(|snapshot| Some(snapshot.revision) > settings_revision_before);
        self.settle_explore_reply(filter_admission_revision, filter_failed);
        if context == Some(ApplicationIntentEndpoint::ExploreUpdateFilter) {
            self.integration.observe_explore_filter_settlement(
                "intent-reply",
                self.model.explore.snapshot.as_ref(),
                self.model.explore_mutation_available(),
            );
        }
        if context.is_some_and(|endpoint| {
            matches!(
                crate::generated::application_intent_system(endpoint),
                crate::generated::ApplicationSystem::Explore
                    | crate::generated::ApplicationSystem::Annotation
            )
        }) {
            self.workspace.install_authoritative_components(&self.model);
        }
        self.settle_settings_reply(
            context,
            settings_mutation_succeeded,
            installed_settings,
            &mut refresh,
        );
        self.reconcile_presentation(refresh, false);
    }

    pub(super) fn reduce_event(&mut self, event: SystemEvent) {
        let failure_snapshot = match &event.event {
            crate::generated::ApplicationEvent::PresentationPresentationFailed(failure) => {
                Some(failure.snapshot.clone())
            }
            _ => None,
        };
        let system = crate::generated::application_event_system(&event.event);
        let settings_revision_before = self
            .model
            .settings_snapshot
            .as_ref()
            .map(|snapshot| snapshot.revision);
        let install_component_snapshots = matches!(
            system,
            crate::generated::ApplicationSystem::Explore
                | crate::generated::ApplicationSystem::Annotation
        );
        let reconcile_explore = system == crate::generated::ApplicationSystem::Explore;
        let explore_failed = Self::explore_event_failed(&event.event);
        let refresh = self.model.reduce_event(event.event);
        if failure_snapshot
            .as_ref()
            .is_some_and(|snapshot| self.model.presentation.as_ref() == Some(snapshot))
        {
            self.presentation.failed();
        }
        if install_component_snapshots {
            self.workspace.install_authoritative_components(&self.model);
        }
        let settings_installed = system == crate::generated::ApplicationSystem::Settings
            && self
                .model
                .settings_snapshot
                .as_ref()
                .is_some_and(|snapshot| Some(snapshot.revision) > settings_revision_before);
        if settings_installed && let Some(authoritative) = self.model.settings_snapshot.as_ref() {
            let authoritative_route = authoritative.settingsstate.currentview;
            self.settings.install(authoritative);
            if self.workspace.active() != authoritative_route {
                self.presentation.retire_frame();
            }
            self.workspace.rebase(authoritative_route, &self.model);
            self.model.set_foreground_feature(authoritative_route);
            self.integration.observe_authoritative_route(
                "settings.event",
                authoritative_route,
                self.workspace.active(),
            );
        }
        if reconcile_explore {
            self.settle_explore_event(explore_failed);
            self.integration.observe_explore_filter_settlement(
                "system-event",
                self.model.explore.snapshot.as_ref(),
                self.model.explore_mutation_available(),
            );
        }
        self.reconcile_presentation(refresh, false);
    }

    pub(super) fn submit_intent(
        &mut self,
        context: ApplicationIntentEndpoint,
        encode: impl FnOnce(u64) -> crate::generated::EncodedApplicationIntent,
    ) -> bool {
        let registered = self.model.register_intent(context, encode);
        self.submit_registered_intent(context, registered)
    }

    pub(super) fn submit_model_select_intent(
        &mut self,
        receipt: crate::view_model::ModelSelectionReceipt,
        encode: impl FnOnce(u64) -> crate::generated::EncodedApplicationIntent,
    ) -> bool {
        let registered = self.model.register_model_select_intent(receipt, encode);
        self.submit_registered_intent(ApplicationIntentEndpoint::ModelSelect, registered)
    }

    pub(super) fn submit_model_stop_intent(
        &mut self,
        feature: crate::generated::FeatureId,
        encode: impl FnOnce(u64) -> crate::generated::EncodedApplicationIntent,
    ) -> bool {
        let registered = self.model.register_model_stop_intent(feature, encode);
        self.submit_registered_intent(ApplicationIntentEndpoint::ModelStop, registered)
    }

    fn submit_registered_intent(
        &mut self,
        context: ApplicationIntentEndpoint,
        registered: Result<Intent, UiError>,
    ) -> bool {
        let intent = match registered {
            Ok(intent) => intent,
            Err(error) => {
                self.abandon_explore_edit(context);
                if matches!(
                    context,
                    ApplicationIntentEndpoint::FileDialogOpen
                        | ApplicationIntentEndpoint::FileDialogStop
                ) {
                    self.model.clear_dialog_context();
                }
                self.model.error = Some(error);
                return false;
            }
        };
        let correlation = intent.correlation;
        let Some(connection) = self.connection.as_mut() else {
            self.model.abandon_intent(correlation);
            self.abandon_explore_edit(context);
            self.model.error = Some(UiError::transport("browser connection is not ready"));
            return false;
        };
        if let Err(error) = connection.send_intent(intent) {
            self.model.abandon_intent(correlation);
            self.abandon_explore_edit(context);
            if matches!(
                context,
                ApplicationIntentEndpoint::FileDialogOpen
                    | ApplicationIntentEndpoint::FileDialogStop
            ) {
                self.model.clear_dialog_context();
            }
            match error {
                crate::transport_connection::OutboundSendError::Closed => {
                    self.retire_peer(UiError::transport(error.to_string()));
                }
                crate::transport_connection::OutboundSendError::Capacity => {
                    self.model.error = Some(UiError::busy(error.to_string()));
                }
            }
            return false;
        }
        true
    }
}
