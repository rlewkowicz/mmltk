use super::*;
use crate::generated::PresentationSourceKind;

#[derive(Debug, Clone)]
pub enum Message {
    Surface(crate::presentation_surface::Notification),
    Redraw(Surface),
}

#[derive(Default)]
pub(super) struct Controller {
    surface: Option<Surface>,
    // Last completion-associated allocation. An advertised growth allocation
    // is not evidence that the incumbent's delayed receipt is obsolete.
    incumbent: Option<Surface>,
    pending: Option<Surface>,
    pending_rejected: bool,
    retained: Option<Surface>,
    failed: bool,
    viewer: Option<(u64, u32)>,
    suspended: Option<SuspendedViewer>,
    pub(super) stop_requested: bool,
}

struct SuspendedViewer {
    route: FeatureId,
    request: Option<crate::generated::UpscaleRequest>,
}

enum ViewerOutcome {
    Opened(crate::generated::UpscaleRequest),
    Replaced(crate::generated::UpscaleRequest),
    Abandoned,
}

struct Update {
    native: Option<(FrameReady, Option<Surface>)>,
    observation: Option<RendererObservation>,
    redraw: bool,
}

fn queued_redraw(surface: Surface) -> Task<Message> {
    crate::presentation_surface::trace_surface("redraw_queued", surface);
    let mut first_poll = true;
    Task::perform(
        std::future::poll_fn(move |context| {
            if std::mem::take(&mut first_poll) {
                context.waker().wake_by_ref();
                std::task::Poll::Pending
            } else {
                std::task::Poll::Ready(())
            }
        }),
        move |()| Message::Redraw(surface),
    )
}

impl Controller {
    pub(super) fn suspend_viewer(&mut self, model: &ApplicationModel, route: FeatureId) {
        crate::presentation_surface::authorize_draw(None);
        self.stop_requested |= model.has_pending(ApplicationIntentEndpoint::UpscaleStop);
        if self.suspended.is_none() {
            self.suspended = Some(SuspendedViewer {
                route,
                request: model.explore.requested_upscale.clone(),
            });
        }
    }

    fn abandon_viewer(&mut self) -> Option<ViewerOutcome> {
        self.suspended = None;
        self.viewer.take().map(|_| ViewerOutcome::Abandoned)
    }

    fn reconcile_viewer(
        &mut self,
        model: &ApplicationModel,
        route: FeatureId,
    ) -> Option<ViewerOutcome> {
        if model.connection != crate::view_model::ConnectionState::Connected {
            return None;
        }
        let source = model.explore.snapshot.as_ref().filter(|snapshot| {
            route == FeatureId::Explore
                && snapshot.mode == crate::generated::ExploreMode::Detail
                && !model.explore.desired_close
                && model.explore.desired_navigation.is_none()
                && !model.has_pending(ApplicationIntentEndpoint::ExploreCloseDetail)
                && !model.has_pending(ApplicationIntentEndpoint::ExploreNavigate)
                && snapshot.selectedimage.is_some()
                && model
                    .explore
                    .requested_selection
                    .is_none_or(|image| snapshot.selectedimage == Some(image))
        });
        let identity = source.and_then(|snapshot| {
            snapshot
                .selectedimage
                .map(|image| (snapshot.dataset.identity, image))
        });
        if let Some(suspended) = self.suspended.take() {
            let matching = suspended.route == route
                && identity == self.viewer
                && suspended.request.as_ref().is_some_and(|request| {
                    source.is_some_and(|snapshot| {
                        snapshot.frame == request.source && snapshot.document == request.document
                    })
                });
            if matching {
                return suspended.request.map(ViewerOutcome::Opened);
            }
            if self.viewer.is_some() {
                let abandoned = self.abandon_viewer();
                if let Some(snapshot) = source.filter(|snapshot| snapshot.ready && !snapshot.busy) {
                    self.viewer = identity;
                    return Some(ViewerOutcome::Replaced(crate::generated::UpscaleRequest {
                        source: snapshot.frame.clone(),
                        document: snapshot.document.clone(),
                        kernel: crate::generated::UpscaleKernel::Default,
                    }));
                }
                return abandoned;
            }
        }
        if identity == self.viewer {
            return None;
        }
        if let Some(snapshot) = source.filter(|snapshot| snapshot.ready && !snapshot.busy) {
            let replacing = self.viewer.is_some();
            self.viewer = identity;
            let request = crate::generated::UpscaleRequest {
                source: snapshot.frame.clone(),
                kernel: crate::generated::UpscaleKernel::Default,
                document: snapshot.document.clone(),
            };
            return Some(if replacing {
                ViewerOutcome::Replaced(request)
            } else {
                ViewerOutcome::Opened(request)
            });
        }
        if identity != self.viewer {
            return self.abandon_viewer();
        }
        None
    }

    pub(super) fn surface(&self) -> Option<Surface> {
        if let Some(pending) = self.pending {
            return Some(pending);
        }
        self.surface.map(|mut surface| {
            surface.frame = self
                .retained
                .filter(|retained| crate::presentation_surface::same_allocation(*retained, surface))
                .and_then(|retained| retained.frame);
            surface
        })
    }

    #[cfg(test)]
    pub(super) fn set_test_surface(&mut self, surface: Surface) {
        self.pending = surface.frame.map(|_| surface);
        self.pending_rejected = false;
        self.surface = Some(Surface {
            frame: None,
            ..surface
        });
        self.incumbent = self.surface;
    }

    pub(super) fn reset_failure(&mut self) {
        crate::presentation_surface::authorize_draw(None);
        self.failed = false;
    }

    pub(super) fn failed(&mut self) {
        self.failed = true;
    }

    fn update(&mut self, message: Message, model: &ApplicationModel) -> Update {
        let mut update = Update {
            native: None,
            observation: None,
            redraw: false,
        };
        match message {
            Message::Surface(crate::presentation_surface::Notification::Native(frame)) => {
                update.native = Some((frame, self.surface()));
                if self.present(frame, model) {
                    update.observation = Some(RendererObservation::Presented {
                        sample_revision: frame.presentation_revision,
                    });
                }
            }
            Message::Surface(crate::presentation_surface::Notification::Completed(frame)) => {
                crate::presentation_surface::complete_capture(frame);
                if self.pending.and_then(|surface| surface.frame) == Some(frame) {
                    self.pending_rejected = false;
                }
                update.redraw = true;
            }
            Message::Surface(crate::presentation_surface::Notification::CaptureRejected(frame)) => {
                if self.pending.and_then(|surface| surface.frame) == Some(frame) {
                    crate::presentation_surface::trace_surface(
                        "capture_rejection_received",
                        self.pending.expect("matching rejected publication"),
                    );
                    self.pending_rejected = true;
                }
            }
            Message::Surface(crate::presentation_surface::Notification::Drawn) => {
                update.redraw = true;
            }
            Message::Redraw(surface) => {
                if self.surface.is_some_and(|current| {
                    crate::presentation_surface::same_allocation(current, surface)
                }) {
                    crate::presentation_surface::trace_surface("redraw_requested", surface);
                    update.redraw = true;
                }
            }
        }
        update
    }

    pub(super) fn redraw(&self, previous: Option<Surface>) -> Task<Message> {
        match self.surface() {
            Some(surface) if self.surface() != previous && surface.frame.is_none() => {
                queued_redraw(surface)
            }
            _ if self.surface() != previous
                || self
                    .surface()
                    .and_then(|surface| surface.frame)
                    .is_some_and(crate::presentation_surface::gallery::awaiting_display) =>
            {
                iced::window::request_redraw()
            }
            _ => Task::none(),
        }
    }
}

impl Drop for Controller {
    fn drop(&mut self) {
        self.retire_frame();
    }
}

impl App {
    pub(super) fn abandon_viewer(&mut self) {
        let outcome = self.presentation.abandon_viewer().or_else(|| {
            self.model
                .explore
                .requested_upscale
                .is_some()
                .then_some(ViewerOutcome::Abandoned)
        });
        self.finish_viewer_departure(outcome.is_some());
    }

    fn finish_viewer_departure(&mut self, stop: bool) {
        self.model.abandon_viewer();
        self.model
            .set_foreground_visual(Some(PresentationSourceKind::Explore));
        self.presentation.retire_frame();
        self.presentation.stop_requested |= stop;
        self.dispatch_viewer_desired();
    }

    pub(super) fn rebase_page(&mut self, feature: FeatureId) {
        let prior = self
            .presentation
            .suspended
            .as_ref()
            .map_or(self.workspace.active(), |suspended| suspended.route);
        if prior != feature {
            self.abandon_viewer();
        }
        self.model.set_foreground_feature(feature);
        self.workspace.rebase(feature, &self.model);
    }

    pub(super) fn reconcile_viewer(&mut self) {
        let outcome = self
            .presentation
            .reconcile_viewer(&self.model, self.workspace.active());
        self.on_viewer(outcome);
    }

    fn on_viewer(&mut self, outcome: Option<ViewerOutcome>) {
        match outcome {
            Some(ViewerOutcome::Opened(request)) => self.model.request_upscale(request),
            Some(ViewerOutcome::Replaced(request)) => {
                self.finish_viewer_departure(true);
                self.model.request_upscale(request);
            }
            Some(ViewerOutcome::Abandoned) => {
                self.finish_viewer_departure(true);
            }
            None => {}
        }
    }
    pub(super) fn on_presentation(&mut self, message: Message) -> Task<crate::message::Message> {
        let update = self.presentation.update(message, &self.model);
        if let Some((frame, surface)) = update.native {
            if let Some(integration) = self.integration.as_mut() {
                integration.observe_reporting(|reporting| {
                    reporting.observe_native_frame(frame, surface);
                });
            }
        }
        if let Some(observation) = update.observation
            && let Some(connection) = self.connection.as_mut()
        {
            let result = connection.send_renderer_observation(observation);
            self.retire_if_closed(result);
        }
        self.reconcile_surface_frame();
        if let Some(frame) = self.model.presentation_refresh() {
            self.select_presentation(frame);
        }
        if update.redraw {
            iced::window::request_redraw()
        } else {
            Task::none()
        }
    }

    pub(super) fn on_window(
        &mut self,
        event: iced::window::Event,
    ) -> Task<crate::message::Message> {
        match event {
            iced::window::Event::Opened {
                size, scale_factor, ..
            } => {
                self.model.window_width = size.width as u32;
                self.model.window_height = size.height as u32;
                self.model.scale_factor = f64::from(scale_factor);
            }
            iced::window::Event::Resized(size) => {
                self.model.window_width = size.width as u32;
                self.model.window_height = size.height as u32;
            }
            iced::window::Event::Rescaled(scale_factor) => {
                self.model.scale_factor = f64::from(scale_factor);
            }
            _ => return Task::none(),
        }
        self.send_surface_observation();
        self.dispatch_explore_viewport()
    }

    pub(super) fn transition_page(&mut self, feature: FeatureId) -> Task<crate::message::Message> {
        if self.workspace.active() == feature {
            return Task::none();
        }
        self.abandon_viewer();
        self.workspace.select(feature);
        self.model.set_foreground_feature(feature);
        if let Some(frame) = self.model.presentation_refresh() {
            self.select_presentation(frame);
        }
        let task = if self.settings.draft().is_some() && self.model.settings_edit_available() {
            match self
                .settings
                .state_mut()
                .edit(EditCadence::Immediate, |draft| {
                    crate::generated::edit_currentview(draft, feature)
                }) {
                Ok(schedule) => self.handle_settings_schedule(schedule),
                Err(detail) => {
                    self.model.error = Some(UiError::invalid(detail));
                    Task::none()
                }
            }
        } else {
            Task::none()
        };
        self.reconcile_viewer();
        self.dispatch_viewer_desired();
        task
    }

    pub(super) fn select_presentation(&mut self, frame: VisualFrame) {
        let source = frame.source.clone();
        if self.submit_intent(
            ApplicationIntentEndpoint::PresentationSelect,
            move |correlation| crate::generated::encode_presentation_Select(correlation, source),
        ) {
            crate::presentation_surface::gallery::select(
                self.model.explore.snapshot.as_ref(),
                &frame,
            );
            self.model.record_presentation_sent(frame);
        }
    }

    pub(super) fn send_surface_observation(&mut self) {
        if self.model.window_width == 0
            || self.model.window_height == 0
            || !self.model.scale_factor.is_finite()
            || self.model.scale_factor <= 0.0
        {
            return;
        }
        if let Some(connection) = self.connection.as_mut() {
            let result = connection.send_renderer_observation(RendererObservation::Surface {
                width: self.model.window_width,
                height: self.model.window_height,
                scale: self.model.scale_factor,
            });
            self.retire_if_closed(result);
        }
    }

    pub(super) fn reconcile_surface_frame(&mut self) {
        if let Err(error) = self
            .presentation
            .reconcile(&self.model, self.workspace.active(), false)
        {
            self.retire_peer(error);
        }
    }

    pub(super) fn sync_surface(&mut self) {
        let collect_integration =
            self.config.integration && crate::integration_control::reporting_enabled();
        if let Err(error) = self.presentation.sync(&self.model, collect_integration) {
            self.model.error = Some(UiError::presentation(error));
        }
        self.reconcile_surface_frame();
    }

    pub(super) fn reconcile_presentation(&mut self, recovery: bool) {
        self.reconcile_viewer();
        self.dispatch_viewer_desired();
        if let Some(snapshot) = self.model.explore.snapshot.as_ref() {
            crate::presentation_surface::gallery::select(
                (self.model.foreground_visual() == Some(PresentationSourceKind::Explore))
                    .then_some(snapshot),
                &snapshot.frame,
            );
        }
        if let Err(error) = self.model.completed_presentation_is_obsolete() {
            self.retire_peer(error);
            return;
        }
        self.sync_surface();
        if recovery
            && let Err(error) =
                self.presentation
                    .reconcile(&self.model, self.workspace.active(), true)
        {
            self.retire_peer(error);
            return;
        }
        let refresh = if recovery {
            self.model.presentation_recovery_refresh().filter(|frame| {
                self.model.presentation.as_ref().is_none_or(|snapshot| {
                    !self
                        .presentation
                        .pending
                        .and_then(|surface| surface.frame)
                        .is_some_and(|pending| {
                            pending.matches_content(frame) && pending.matches_completed(snapshot)
                        })
                        && crate::presentation_surface::completed_content(frame, snapshot).is_none()
                })
            })
        } else {
            self.model.presentation_refresh()
        };
        if let Some(frame) = refresh {
            self.select_presentation(frame);
        }
    }

    #[cfg(test)]
    pub(super) fn present_native_frame(&mut self, frame: FrameReady) {
        drop(self.on_presentation(Message::Surface(
            crate::presentation_surface::Notification::Native(frame),
        )));
    }
}

impl Controller {
    fn present(&mut self, frame: FrameReady, model: &ApplicationModel) -> bool {
        crate::presentation_surface::invalidate_drawn_slot(frame);
        if self.pending.and_then(|pending| pending.frame) == Some(frame) {
            return false;
        }
        if self.failed
            && model
                .presentation
                .as_ref()
                .is_none_or(|snapshot| !frame.matches_completed(snapshot))
        {
            if self
                .pending
                .and_then(|surface| surface.frame)
                .is_some_and(|current| {
                    crate::presentation_surface::same_mailbox_slot(current, frame)
                        && current != frame
                })
            {
                self.retire_pending();
            }
            crate::presentation_surface::release(frame);
            return false;
        }
        let completed = model
            .presentation
            .as_ref()
            .is_some_and(|snapshot| frame.matches_completed(snapshot));
        let Some(surface) = self
            .surface
            .filter(|surface| frame.belongs_to(*surface))
            .or_else(|| {
                self.incumbent
                    .filter(|surface| completed && frame.belongs_to(*surface))
            })
        else {
            crate::presentation_surface::release(frame);
            return false;
        };
        if let Some(previous) = self.pending.and_then(|pending| pending.frame)
            && previous.presentation_revision >= frame.presentation_revision
        {
            if previous != frame {
                crate::presentation_surface::release(frame);
            }
            return false;
        }
        if let Some(previous) = self.pending.and_then(|pending| pending.frame)
            && previous != frame
        {
            // Batched UI updates may replace a frame before its first draw.
            // Capture owns an in-flight borrow until GPU completion; otherwise
            // this returns an unused sample or deduplicates its settled release.
            crate::presentation_surface::retire_publication(previous);
            crate::presentation_surface::discard_capture(previous);
        }
        if !crate::presentation_surface::accept_publication(frame) {
            return false;
        }
        self.pending = Some(Surface {
            frame: Some(frame),
            ..surface
        });
        self.pending_rejected = false;
        if completed {
            self.incumbent = Some(surface);
        }
        true
    }

    pub(super) fn retire_frame(&mut self) {
        crate::presentation_surface::authorize_draw(None);
        self.retire_pending();
        self.retained = None;
        crate::presentation_surface::clear_drawn_detail();
    }

    fn retire_pending(&mut self) {
        self.pending_rejected = false;
        if let Some(frame) = self.pending.take().and_then(|surface| surface.frame) {
            crate::presentation_surface::retire_publication(frame);
            crate::presentation_surface::discard_capture(frame);
        }
    }

    pub(super) fn discard(&mut self) {
        self.retire_frame();
        self.surface = None;
        self.incumbent = None;
    }

    fn reconcile(
        &mut self,
        model: &ApplicationModel,
        feature: FeatureId,
        recovery: bool,
    ) -> Result<(), UiError> {
        crate::presentation_surface::authorize_draw(None);
        // Transport loss clears domain facts, not receiver-owned image custody.
        let Some(snapshot) = model.presentation.as_ref() else {
            return Ok(());
        };
        if matches!(
            feature,
            FeatureId::Train | FeatureId::Validate | FeatureId::Export
        ) {
            self.retire_frame();
            return Ok(());
        }
        let decision = model.completed_presentation_reconciliation()?;
        if decision == crate::view_model::PresentationReconciliation::Matching
            && let Some(snapshot) = model.presentation.as_ref()
            && let Some(retained) =
                crate::presentation_surface::completed_content(&snapshot.completed, snapshot)
        {
            crate::presentation_surface::reconcile_completed(retained, model);
            self.retained = Some(retained);
            self.incumbent = Some(retained);
        }
        let Some(frame) = self.pending.and_then(|surface| surface.frame) else {
            return Ok(());
        };
        let completed = frame.matches_completed(snapshot);
        if recovery && !completed {
            self.retire_pending();
            return Ok(());
        }
        if completed {
            self.incumbent = self.pending;
        }
        if self.failed && !completed {
            self.retire_pending();
            return Ok(());
        }
        // A future physical publication may be awaiting its control snapshot.
        // Neither that ordering nor a later domain snapshot alone abandons it.
        if frame.presentation_revision <= snapshot.presentationrevision {
            if decision == crate::view_model::PresentationReconciliation::Superseded || !completed {
                self.retire_pending();
            } else if decision == crate::view_model::PresentationReconciliation::Matching {
                crate::presentation_surface::gallery::confirm(
                    frame,
                    &snapshot.completed,
                    model.explore.snapshot.as_ref(),
                );
                crate::presentation_surface::reconcile_completed(
                    self.pending.expect("matching pending surface"),
                    model,
                );
                if let Some(retained) = crate::presentation_surface::retained_surface()
                    .filter(|surface| surface.frame == Some(frame))
                {
                    self.retained = Some(retained);
                    self.pending = None;
                    self.pending_rejected = false;
                }
            }
        }
        if self.pending_rejected && crate::presentation_surface::gallery::awaiting_display(frame) {
            // Matching metadata may have arrived after the rejected draw.
            self.pending_rejected = false;
        }
        let selection_superseded = !completed
            && snapshot.selected.instance != 0
            && snapshot.selected.kind != PresentationSourceKind::None
            && frame.content_session
                != crate::generated::presentation_source_session(snapshot.selected.kind);
        if (self.pending_rejected || selection_superseded)
            && self
                .pending
                .zip(self.surface)
                .is_some_and(|(pending, surface)| {
                    surface.generation > pending.generation
                        && (surface.high != pending.high || surface.low != pending.low)
                })
        {
            // An uncapturable incumbent must not hide an admitted replacement.
            // A publication from an abandoned source selection will never receive
            // matching completion metadata. Active copies keep CaptureBorrow.
            if let Some(pending) = self.pending {
                crate::presentation_surface::trace_surface(
                    if selection_superseded {
                        "selection_superseded_publication_retired"
                    } else {
                        "rejected_publication_retired"
                    },
                    pending,
                );
            }
            self.retire_pending();
        }
        Ok(())
    }

    fn sync(&mut self, model: &ApplicationModel, integration: bool) -> Result<(), &'static str> {
        let Some(snapshot) = model.presentation.as_ref() else {
            return Ok(());
        };
        match surface_from_snapshot(snapshot) {
            Ok(None) => {
                if integration {
                    crate::integration_control::report_surface_sync(self.surface, None);
                }
                self.discard();
                crate::presentation_surface::retire_imports();
            }
            Ok(mut surface) => {
                if let Some(surface) = surface.as_mut() {
                    surface.integration = integration;
                }
                if integration {
                    crate::integration_control::report_surface_sync(self.surface, surface);
                }
                if !self.surface.zip(surface).is_some_and(|(current, updated)| {
                    crate::presentation_surface::same_allocation(current, updated)
                }) {
                    if let Some(previous) = self.surface {
                        crate::presentation_surface::trace_surface("capability_replaced", previous);
                    }
                    if let Some(updated) = surface {
                        crate::presentation_surface::trace_surface("capability_selected", updated);
                    }
                    self.surface = surface;
                    if self.incumbent.is_none() {
                        self.incumbent = surface;
                    }
                } else if let (Some(current), Some(updated)) = (&mut self.surface, surface) {
                    current.timeline_ready = updated.timeline_ready;
                }
            }
            Err(error) => return Err(error),
        }
        Ok(())
    }
}

fn surface_from_snapshot(snapshot: &PresentationSnapshot) -> Result<Option<Surface>, &'static str> {
    if snapshot.capability.condition == PresentationCapabilityCondition::Unavailable {
        return Ok(None);
    }
    if !matches!(
        snapshot.capability.condition,
        PresentationCapabilityCondition::Admitted | PresentationCapabilityCondition::Ready
    ) {
        return Err("invalid Presentation capability condition");
    }
    let surface = Surface {
        high: snapshot.capability.surfacehigh,
        low: snapshot.capability.surfacelow,
        generation: snapshot.capability.generation,
        width: snapshot.capability.extent.width,
        height: snapshot.capability.extent.height,
        timeline_ready: snapshot.timelineready,
        frame: None,
        integration: false,
        crop: None,
        viewer_identity: None,
        fit_revision: 0,
    };
    surface
        .valid()
        .then_some(Some(surface))
        .ok_or("invalid Presentation capability")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::generated::{ExploreMode, PresentationSourceKind};
    use crate::presentation_surface::{record_drawn_detail, reset_test_releases, test_releases};

    #[test]
    fn full_bootstrap_restores_only_the_exact_suspended_viewer() {
        use crate::generated::{ApplicationSnapshot, UpscaleKernel};
        for changed in 0..4 {
            let (mut app, _) = viewer_app();
            app.reconcile_viewer();
            let mut request = app.model.explore.requested_upscale.clone().unwrap();
            request.kernel = UpscaleKernel::RealPlksr;
            app.model.request_upscale(request.clone());
            app.reconcile_viewer();
            let mut explore = app.model.explore.snapshot.clone().unwrap();
            let mut settings = app.model.settings_snapshot.clone().unwrap();
            let presentation = app.model.presentation.clone().unwrap();
            settings.settingsstate.currentview = if changed == 3 {
                FeatureId::Train
            } else {
                FeatureId::Explore
            };
            if changed == 1 {
                explore.frame.revision += 1;
                explore.frame.cleanrevision += 1;
                explore.revision += 1;
            } else if changed == 2 {
                explore.document.meaningidentity += 1;
                explore.revision += 1;
            }
            let snapshots = crate::generated::application_snapshot_defaults()
                .unwrap()
                .into_iter()
                .map(|fact| match fact.value {
                    ApplicationSnapshot::Explore(_) => {
                        ApplicationSnapshot::Explore(explore.clone())
                    }
                    ApplicationSnapshot::Settings(_) => {
                        ApplicationSnapshot::Settings(settings.clone())
                    }
                    ApplicationSnapshot::Presentation(_) => {
                        ApplicationSnapshot::Presentation(presentation.clone())
                    }
                    other => other,
                })
                .collect();
            app.retire_peer(UiError::transport("bootstrap identity test"));
            let (sender, mut receiver) = Connection::test_channel();
            drop(app.on_transport(TransportEvent::Connected(sender)));
            drop(app.on_transport(TransportEvent::Bootstrap(Bootstrap {
                input_epoch: 1,
                schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
                snapshots,
            })));
            let mut stops = 0;
            let mut starts = 0;
            let mut stop_correlation = None;
            while let Ok(record) = receiver.try_recv() {
                if let crate::transport_connection::CapturedRecord::Intent(intent) = record {
                    if intent.endpoint_id
                        == crate::generated::application_intent_endpoint_stable_id(
                            ApplicationIntentEndpoint::UpscaleStop,
                        )
                    {
                        stops += 1;
                        stop_correlation = Some(intent.correlation);
                    } else if intent.endpoint_id
                        == crate::generated::application_intent_endpoint_stable_id(
                            ApplicationIntentEndpoint::UpscaleStart,
                        )
                    {
                        starts += 1;
                    }
                }
            }
            if let Some(correlation) = stop_correlation {
                settle_upscale_stop(&mut app, correlation);
                while let Ok(record) = receiver.try_recv() {
                    if let crate::transport_connection::CapturedRecord::Intent(intent) = record
                        && intent.endpoint_id
                            == crate::generated::application_intent_endpoint_stable_id(
                                ApplicationIntentEndpoint::UpscaleStart,
                            )
                    {
                        starts += 1;
                    }
                }
            }
            assert_eq!(stops, usize::from(changed != 0));
            assert_eq!(starts, usize::from(changed != 3));
            if changed == 3 {
                assert!(app.presentation.viewer.is_none());
                assert!(app.model.explore.requested_upscale.is_none());
            } else {
                let restored = app.model.explore.requested_upscale.as_ref().unwrap();
                assert_eq!(
                    restored.kernel,
                    if changed == 0 {
                        UpscaleKernel::RealPlksr
                    } else {
                        UpscaleKernel::Default
                    }
                );
                assert_eq!(restored.source, explore.frame);
                assert_eq!(restored.document, explore.document);
                assert_eq!(app.model.explore.sent_upscale.as_ref(), Some(restored));
            }
        }
    }

    #[test]
    fn direct_viewer_replacement_uses_one_abandonment_before_basic_dispatch() {
        let (mut app, _) = viewer_app();
        app.reconcile_viewer();
        let mut request = app.model.explore.requested_upscale.clone().unwrap();
        request.kernel = crate::generated::UpscaleKernel::RealPlksr;
        app.model.request_upscale(request);
        app.reconcile_viewer();
        let snapshot = app.model.explore.snapshot.as_mut().unwrap();
        snapshot.selectedimage = Some(1);
        snapshot.revision += 1;
        snapshot.frame.revision += 1;
        snapshot.frame.cleanrevision += 1;
        let (sender, mut receiver) = Connection::test_channel();
        app.connection = Some(sender);
        app.reconcile_presentation(false);
        let requested = app.model.explore.requested_upscale.clone().unwrap();
        assert_eq!(requested.kernel, crate::generated::UpscaleKernel::Default);
        assert_eq!(app.presentation.viewer.unwrap().1, 1);
        assert!(app.model.explore.sent_upscale.is_none());
        let crate::transport_connection::CapturedRecord::Intent(stop) =
            receiver.try_recv().expect("expected replacement Stop")
        else {
            panic!("expected replacement Stop intent");
        };
        assert_eq!(
            stop.endpoint_id,
            crate::generated::application_intent_endpoint_stable_id(
                ApplicationIntentEndpoint::UpscaleStop
            )
        );
        settle_upscale_stop(&mut app, stop.correlation);
        assert_eq!(app.model.explore.sent_upscale.as_ref(), Some(&requested));
        let start = loop {
            let crate::transport_connection::CapturedRecord::Intent(intent) =
                receiver.try_recv().expect("expected replacement Basic")
            else {
                continue;
            };
            if intent.endpoint_id
                == crate::generated::application_intent_endpoint_stable_id(
                    ApplicationIntentEndpoint::UpscaleStart,
                )
            {
                break intent;
            }
        };
        assert_eq!(
            start,
            crate::generated::encode_upscale_Start(start.correlation, requested).record
        );
        assert!(receiver.try_recv().is_err());
    }

    #[test]
    fn viewer_owner_auto_runs_basic_once_and_abandons_only_on_departure() {
        let (mut app, _) = viewer_app();
        app.reconcile_viewer();
        let request = app.model.explore.requested_upscale.clone().unwrap();
        assert_eq!(request.kernel, crate::generated::UpscaleKernel::Default);
        app.model.request_upscale(crate::generated::UpscaleRequest {
            kernel: crate::generated::UpscaleKernel::RealPlksr,
            ..request.clone()
        });
        app.reconcile_viewer();
        assert_eq!(
            app.model.explore.requested_upscale.as_ref().unwrap().kernel,
            crate::generated::UpscaleKernel::RealPlksr
        );
        assert!(matches!(
            app.presentation
                .reconcile_viewer(&app.model, FeatureId::Train),
            Some(ViewerOutcome::Abandoned)
        ));
        assert!(
            app.presentation
                .reconcile_viewer(&app.model, FeatureId::Train)
                .is_none()
        );
        assert!(matches!(
            app.presentation
                .reconcile_viewer(&app.model, FeatureId::Explore),
            Some(ViewerOutcome::Opened(crate::generated::UpscaleRequest {
                kernel: crate::generated::UpscaleKernel::Default,
                ..
            }))
        ));
    }

    #[test]
    fn viewer_dispatch_queues_latest_while_native_upscale_is_busy() {
        let (mut app, basic, start_correlation, mut receiver) = dispatch_automatic_basic();

        let mut busy = app.model.upscale_snapshot.clone().unwrap();
        busy.revision += 1;
        busy.busy = true;
        busy.pending = Some(basic.clone());
        app.reduce_reply(IntentReply {
            correlation: start_correlation,
            result: Ok(
                crate::application_codec::IntoApplicationValue::into_application_value(
                    busy.clone(),
                ),
            ),
        });

        let shift_lut = crate::generated::UpscaleRequest {
            kernel: crate::generated::UpscaleKernel::ShiftLut,
            ..basic.clone()
        };
        app.model.request_upscale(shift_lut.clone());
        app.dispatch_viewer_desired();
        let crate::transport_connection::CapturedRecord::Intent(shift_start) = receiver
            .try_recv()
            .expect("expected ShiftLUT while native work remains busy")
        else {
            panic!("expected ShiftLUT intent");
        };
        assert_eq!(
            shift_start,
            crate::generated::encode_upscale_Start(shift_start.correlation, shift_lut.clone())
                .record
        );

        let latest = crate::generated::UpscaleRequest {
            kernel: crate::generated::UpscaleKernel::RealPlksr,
            ..basic
        };
        app.model.request_upscale(latest.clone());
        app.dispatch_viewer_desired();
        assert!(receiver.try_recv().is_err());

        busy.revision += 1;
        busy.pending = Some(shift_lut);
        app.reduce_reply(IntentReply {
            correlation: shift_start.correlation,
            result: Ok(
                crate::application_codec::IntoApplicationValue::into_application_value(busy),
            ),
        });
        let crate::transport_connection::CapturedRecord::Intent(start) =
            receiver.try_recv().expect("expected latest method")
        else {
            panic!("expected latest method intent");
        };
        assert_eq!(latest.kernel, crate::generated::UpscaleKernel::RealPlksr);
        assert_eq!(
            start,
            crate::generated::encode_upscale_Start(start.correlation, latest).record
        );
        assert!(receiver.try_recv().is_err());
    }

    #[test]
    fn viewer_stop_waits_for_the_inflight_start_without_reporting_busy() {
        let (mut app, basic, start_correlation, mut receiver) = dispatch_automatic_basic();

        app.abandon_viewer();
        assert!(app.presentation.stop_requested);
        assert!(app.model.error.is_none());
        assert!(receiver.try_recv().is_err());

        let mut busy = app.model.upscale_snapshot.clone().unwrap();
        busy.revision += 1;
        busy.busy = true;
        busy.pending = Some(basic);
        app.reduce_reply(IntentReply {
            correlation: start_correlation,
            result: Ok(
                crate::application_codec::IntoApplicationValue::into_application_value(
                    busy.clone(),
                ),
            ),
        });
        let stop = loop {
            let crate::transport_connection::CapturedRecord::Intent(intent) =
                receiver.try_recv().expect("expected deferred Stop")
            else {
                continue;
            };
            if intent.endpoint_id
                == crate::generated::application_intent_endpoint_stable_id(
                    ApplicationIntentEndpoint::UpscaleStop,
                )
            {
                break intent;
            }
        };
        assert!(!app.presentation.stop_requested);
        assert!(app.model.error.is_none());

        app.dispatch_viewer_desired();
        assert!(app.model.error.is_none());
        assert!(receiver.try_recv().is_err());
        settle_upscale_stop(&mut app, stop.correlation);
        assert!(app.model.error.is_none());
        let latest = app.model.explore.requested_upscale.clone().unwrap();
        let start = loop {
            let crate::transport_connection::CapturedRecord::Intent(intent) = receiver
                .try_recv()
                .expect("expected restart after Stop settled")
            else {
                continue;
            };
            if intent.endpoint_id
                == crate::generated::application_intent_endpoint_stable_id(
                    ApplicationIntentEndpoint::UpscaleStart,
                )
            {
                break intent;
            }
        };
        assert_eq!(
            start,
            crate::generated::encode_upscale_Start(start.correlation, latest).record
        );
        assert!(app.model.error.is_none());
        assert!(receiver.try_recv().is_err());
    }

    #[test]
    fn same_route_settings_and_same_image_updates_preserve_method_in_both_orders() {
        for settings_first in [false, true] {
            let (mut app, _) = viewer_app();
            app.reconcile_viewer();
            let source = app.model.explore.snapshot.as_ref().unwrap().frame.clone();
            app.model.request_upscale(crate::generated::UpscaleRequest {
                source,
                kernel: crate::generated::UpscaleKernel::ShiftLut,
                document: app
                    .model
                    .explore
                    .snapshot
                    .as_ref()
                    .unwrap()
                    .document
                    .clone(),
            });
            app.model.explore.sent_upscale = app.model.explore.requested_upscale.clone();
            let (sender, mut receiver) = Connection::test_channel();
            app.connection = Some(sender);
            app.model
                .settings_snapshot
                .as_mut()
                .unwrap()
                .settingsstate
                .currentview = FeatureId::Explore;
            let mut revised = app.model.explore.snapshot.clone().unwrap();
            revised.revision += 1;
            revised.frame.revision += 1;
            revised.frame.cleanrevision += 1;
            let revised_frame = revised.frame.clone();
            for settings in [settings_first, !settings_first] {
                if settings {
                    app.settle_settings_reply(None, true, true);
                    app.reconcile_presentation(false);
                } else {
                    app.reduce_event(SystemEvent {
                        state_revision: revised.revision,
                        delivery: crate::generated::EventDelivery::LatestState,
                        event: crate::generated::ApplicationEvent::ExploreExploreChanged(
                            crate::generated::ExploreChanged {
                                snapshot: revised.clone(),
                            },
                        ),
                    });
                }
            }
            let requested = app.model.explore.requested_upscale.clone().unwrap();
            assert_eq!(requested.kernel, crate::generated::UpscaleKernel::ShiftLut);
            assert_eq!(requested.source, revised_frame);
            assert_eq!(app.presentation.viewer, Some((revised.dataset.identity, 0)));
            assert_eq!(app.model.explore.sent_upscale.as_ref(), Some(&requested));
            let mut dispatched = false;
            while let Ok(crate::transport_connection::CapturedRecord::Intent(intent)) =
                receiver.try_recv()
            {
                if intent.endpoint_id
                    == crate::generated::application_intent_endpoint_stable_id(
                        ApplicationIntentEndpoint::UpscaleStart,
                    )
                {
                    assert_eq!(
                        intent,
                        crate::generated::encode_upscale_Start(
                            intent.correlation,
                            requested.clone()
                        )
                        .record
                    );
                    dispatched = true;
                } else if intent.endpoint_id
                    == crate::generated::application_intent_endpoint_stable_id(
                        ApplicationIntentEndpoint::PresentationSelect,
                    )
                {
                    let admitted = app.model.presentation.clone().unwrap();
                    app.model.reduce_reply(
                        intent.correlation,
                        Ok(crate::generated::ApplicationReply::PresentationSelect(
                            admitted,
                        )),
                    );
                }
            }
            assert!(dispatched);
            let mut completed = app.model.upscale_snapshot.clone().unwrap();
            completed.revision += 1;
            completed.ready = true;
            completed.busy = false;
            completed.pending = None;
            completed.kernel = requested.kernel;
            completed.input = requested.source.clone();
            completed.frame =
                crate::view_model::test_support::visual_frame(PresentationSourceKind::Upscale, 19);
            completed.frame.extent.width = requested.source.extent.width * 4;
            completed.frame.extent.height = requested.source.extent.height * 4;
            completed.methods[1].available = true;
            completed.methods[1].completed = Some(requested);
            completed.methods[1].frame = completed.frame.clone();
            app.reduce_event(SystemEvent {
                state_revision: completed.revision,
                delivery: crate::generated::EventDelivery::LatestState,
                event: crate::generated::ApplicationEvent::UpscaleUpscaleChanged(
                    crate::generated::UpscaleChanged {
                        snapshot: completed.clone(),
                    },
                ),
            });
            let mut presentation = app.model.presentation.clone().unwrap();
            presentation.revision += 1;
            presentation.presentationrevision += 1;
            presentation.selected = completed.frame.source.clone();
            presentation.completed = completed.frame.clone();
            presentation.completedsourcerevision = completed.revision;
            presentation.capability.generation += 1;
            presentation.capability.extent = completed.frame.extent.clone();
            app.reduce_event(SystemEvent {
                state_revision: presentation.revision,
                delivery: crate::generated::EventDelivery::LatestState,
                event: crate::generated::ApplicationEvent::PresentationPresentationCompleted(
                    crate::generated::PresentationCompleted {
                        snapshot: presentation,
                    },
                ),
            });
            assert_eq!(app.model.viewed_explore_frame(), Some(completed.frame));
        }
    }

    #[test]
    fn authoritative_reset_and_failed_route_edit_abandon_and_reenter_the_same_image() {
        for reset in [false, true] {
            let (mut app, _) = viewer_app();
            app.reconcile_viewer();
            let (sender, mut receiver) = Connection::test_channel();
            app.connection = Some(sender);
            app.model
                .settings_snapshot
                .as_mut()
                .unwrap()
                .settingsstate
                .currentview = FeatureId::Train;
            app.settle_settings_reply(
                Some(if reset {
                    ApplicationIntentEndpoint::SettingsReset
                } else {
                    ApplicationIntentEndpoint::SettingsUpdate
                }),
                reset,
                reset,
            );
            assert_viewer_departed(&app);
            let mut stop_correlation = None;
            let mut stopped = false;
            while let Ok(crate::transport_connection::CapturedRecord::Intent(intent)) =
                receiver.try_recv()
            {
                if intent.endpoint_id
                    == crate::generated::application_intent_endpoint_stable_id(
                        ApplicationIntentEndpoint::UpscaleStop,
                    )
                {
                    stopped = true;
                    stop_correlation = Some(intent.correlation);
                }
            }
            assert!(stopped);
            settle_upscale_stop(
                &mut app,
                stop_correlation.expect("expected authoritative route Stop"),
            );
            navigate(&mut app, FeatureId::Explore);
            let request = app.model.explore.requested_upscale.as_ref().unwrap();
            assert_eq!(request.kernel, crate::generated::UpscaleKernel::Default);
            assert_eq!(app.model.explore.sent_upscale.as_ref(), Some(request));
        }
    }

    fn record_draw(app: &App, frame: FrameReady, crop: [u32; 4]) {
        let surface = app.presentation.surface.unwrap();
        assert!(frame.belongs_to(surface));
        record_drawn_detail(
            Surface {
                frame: Some(frame),
                ..surface
            },
            crop,
        );
    }

    fn viewer_app() -> (App, FrameReady) {
        reset_test_releases();
        let (mut app, task) = crate::app::boot();
        drop(task);
        let (model, frame) = crate::view_model::test_support::explore_presentation();
        app.model = model;
        app.workspace.select(FeatureId::Explore);
        app.sync_surface();
        (app, frame)
    }

    fn dispatch_automatic_basic() -> (
        App,
        crate::generated::UpscaleRequest,
        u64,
        crate::transport_connection::Capture,
    ) {
        let (mut app, _) = viewer_app();
        app.reconcile_viewer();
        let basic = app.model.explore.requested_upscale.clone().unwrap();
        let (sender, mut receiver) = Connection::test_channel();
        app.connection = Some(sender);
        app.dispatch_viewer_desired();
        let crate::transport_connection::CapturedRecord::Intent(start) =
            receiver.try_recv().expect("expected automatic Basic")
        else {
            panic!("expected automatic Basic intent");
        };
        assert_eq!(
            start,
            crate::generated::encode_upscale_Start(start.correlation, basic.clone()).record
        );
        (app, basic, start.correlation, receiver)
    }

    #[test]
    fn quiet_integration_keeps_real_presentation_without_surface_instrumentation() {
        let (mut app, frame) = viewer_app();
        app.config.integration = true;
        app.sync_surface();
        assert!(!app.presentation.surface().unwrap().integration);
        app.present_native_frame(frame);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
        assert!(!app.presentation.surface().unwrap().integration);
        app.presentation.discard();
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn pending_publication_waits_for_both_control_and_domain_metadata() {
        let (mut app, frame) = viewer_app();
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            ..frame
        };
        app.present_native_frame(pending);
        app.reconcile_surface_frame();
        assert!(test_releases().is_empty());
        let snapshot = app.model.presentation.as_mut().unwrap();
        snapshot.presentationrevision = 6;
        snapshot.completed.revision = 2;
        snapshot.completedsourcerevision = 20;
        app.reconcile_surface_frame();
        assert!(test_releases().is_empty());
        app.model.explore.snapshot.as_mut().unwrap().frame =
            app.model.presentation.as_ref().unwrap().completed.clone();
        app.model.explore.snapshot.as_mut().unwrap().revision = 20;
        app.reconcile_surface_frame();
        assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
        record_draw(&app, pending, [0, 0, 640, 480]);
        crate::presentation_surface::release(pending);
        app.presentation.discard();
        assert_eq!(test_releases(), vec![pending]);
        assert!(crate::presentation_surface::drawn_detail().is_none());
    }

    #[test]
    fn unpublished_successor_cannot_hide_or_reject_the_incumbent_capture() {
        for rejected in [false, true] {
            for receipt_first in [false, true] {
                let (mut app, frame) = viewer_app();
                let incumbent = app.presentation.surface().unwrap();
                if receipt_first {
                    app.present_native_frame(frame);
                    if rejected {
                        drop(app.on_presentation(Message::Surface(
                            crate::presentation_surface::Notification::CaptureRejected(frame),
                        )));
                        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
                        assert!(test_releases().is_empty());
                    }
                }
                let snapshot = app.model.presentation.as_mut().unwrap();
                snapshot.capability.surfacelow += 1;
                snapshot.capability.generation += 1;
                snapshot.capability.extent.width *= 2;
                snapshot.capability.condition = PresentationCapabilityCondition::Admitted;
                app.sync_surface();
                if !receipt_first {
                    app.present_native_frame(frame);
                    // A stale outcome cannot retire the current receipt.
                    drop(app.on_presentation(Message::Surface(
                        crate::presentation_surface::Notification::CaptureRejected(FrameReady {
                            slot: 1,
                            ..frame
                        }),
                    )));
                    assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
                    if rejected {
                        drop(app.on_presentation(Message::Surface(
                            crate::presentation_surface::Notification::CaptureRejected(frame),
                        )));
                    }
                }
                if rejected {
                    assert_eq!(app.presentation.surface(), app.presentation.surface);
                    assert_ne!(app.presentation.surface().unwrap().low, incumbent.low);
                    assert!(app.presentation.surface().unwrap().frame.is_none());
                    assert_eq!(test_releases(), vec![frame]);
                    app.present_native_frame(frame);
                    assert!(app.presentation.surface().unwrap().frame.is_none());
                } else {
                    let capture_surface = app.presentation.surface().unwrap();
                    assert_eq!(
                        capture_surface,
                        Surface {
                            frame: Some(frame),
                            ..incumbent
                        }
                    );
                    assert_ne!(capture_surface.low, app.presentation.surface.unwrap().low);
                    assert!(test_releases().is_empty());
                    let capture = crate::presentation_surface::test_capture_borrow(frame);
                    app.sync_surface();
                    assert_eq!(app.presentation.surface(), Some(capture_surface));
                    assert!(test_releases().is_empty());
                    drop(capture);
                }
                drop(app.on_presentation(Message::Surface(
                    crate::presentation_surface::Notification::Completed(frame),
                )));
                assert_eq!(test_releases(), vec![frame]);
                app.presentation.discard();
                assert_eq!(test_releases(), vec![frame]);
            }
        }
        for completed in [false, true] {
            for selected_successor in [false, true] {
                let (mut app, frame) = viewer_app();
                let pending = FrameReady {
                    presentation_revision: frame.presentation_revision + 1,
                    content_sequence: frame.content_sequence + 1,
                    ..frame
                };
                app.present_native_frame(pending);
                if completed {
                    let snapshot = app.model.presentation.as_mut().unwrap();
                    snapshot.presentationrevision = pending.presentation_revision;
                    snapshot.completed.revision = pending.content_sequence;
                    app.model.explore.snapshot.as_mut().unwrap().frame = snapshot.completed.clone();
                }
                let snapshot = app.model.presentation.as_mut().unwrap();
                if selected_successor {
                    snapshot.selected = crate::view_model::test_support::visual_frame(
                        PresentationSourceKind::Upscale,
                        4,
                    )
                    .source;
                }
                // Selecting another source alone does not abandon physical custody.
                app.reconcile_surface_frame();
                assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
                assert!(test_releases().is_empty());
                let capture = crate::presentation_surface::test_capture_borrow(pending);
                let snapshot = app.model.presentation.as_mut().unwrap();
                snapshot.capability.surfacelow += 1;
                snapshot.capability.generation += 1;
                snapshot.capability.extent.width *= 2;
                snapshot.capability.condition = PresentationCapabilityCondition::Admitted;
                app.sync_surface();
                if !completed && selected_successor {
                    assert_eq!(app.presentation.surface(), app.presentation.surface);
                    assert!(app.presentation.surface().unwrap().frame.is_none());
                } else {
                    assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
                }
                // Retirement cannot return an active physical copy early.
                assert!(test_releases().is_empty());
                drop(capture);
                app.presentation.discard();
                assert_eq!(test_releases(), vec![pending]);
            }
        }
    }

    #[test]
    fn reconnect_preserves_capture_custody_until_bootstrap_reconciliation() {
        for matching in [false, true] {
            for complete_before_disconnect in [false, true] {
                let (mut app, frame) = viewer_app();
                app.present_native_frame(frame);
                let mut capture = Some(crate::presentation_surface::test_capture_borrow(frame));
                let control = app.model.presentation.clone();
                let explore = app.model.explore.snapshot.clone();
                if complete_before_disconnect {
                    drop(capture.take());
                    drop(app.on_presentation(Message::Surface(
                        crate::presentation_surface::Notification::Completed(frame),
                    )));
                }
                app.retire_peer(UiError::transport("capture continuity"));
                assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
                let (sender, _receiver) = Connection::test_channel();
                drop(app.on_transport(TransportEvent::Connected(sender)));
                assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
                app.model.presentation = control;
                app.model.explore.snapshot = explore;
                app.model.connection = crate::view_model::ConnectionState::Connected;
                app.model.set_foreground_feature(FeatureId::Explore);
                app.workspace.select(FeatureId::Explore);
                if !matching {
                    let snapshot = app.model.presentation.as_mut().unwrap();
                    snapshot.completed.revision += 1;
                    snapshot.presentationrevision += 1;
                    snapshot.completedsourcerevision += 1;
                    let explore = app.model.explore.snapshot.as_mut().unwrap();
                    explore.frame = snapshot.completed.clone();
                    explore.revision = snapshot.completedsourcerevision;
                }
                app.reconcile_presentation(true);
                assert_eq!(app.presentation.pending.is_some(), matching);
                assert_eq!(
                    test_releases(),
                    if complete_before_disconnect {
                        vec![frame]
                    } else {
                        vec![]
                    }
                );
                // Matching receiver custody suppresses another Select even
                // while the callback is outstanding; nonmatching recovery asks
                // for the new foreground product.
                assert_eq!(
                    app.model
                        .has_pending(ApplicationIntentEndpoint::PresentationSelect),
                    !matching
                );
                drop(capture);
                drop(app.on_presentation(Message::Surface(
                    crate::presentation_surface::Notification::Completed(frame),
                )));
                assert_eq!(test_releases(), vec![frame]);
                app.presentation.discard();
                assert_eq!(test_releases(), vec![frame]);
            }
        }
    }

    #[test]
    fn advertised_successor_preserves_completed_draw_identity_and_copy() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        let completed = Surface {
            frame: Some(frame),
            ..app.presentation.surface.unwrap()
        };
        app.presentation.pending = None;
        app.presentation.retained = Some(completed);
        record_drawn_detail(completed, completed.content_region());
        crate::presentation_surface::release(frame);
        let snapshot = app.model.presentation.as_mut().unwrap();
        snapshot.capability.surfacelow += 1;
        snapshot.capability.generation += 1;
        snapshot.capability.extent.width *= 2;
        snapshot.capability.extent.height *= 2;
        snapshot.capability.condition = PresentationCapabilityCondition::Admitted;
        app.sync_surface();
        assert_eq!(app.presentation.retained, Some(completed));
        assert_ne!(app.presentation.surface().unwrap().low, completed.low);
        assert_eq!(
            crate::presentation_surface::drawn_detail(),
            Some((completed, completed.content_region()))
        );
        let (sender, _receiver) = Connection::test_channel();
        app.connection = Some(sender);
        assert!(app.copy_viewer_to_annotation());
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn domain_control_publication_and_capture_reconcile_in_every_causal_order() {
        for [
            domain_position,
            control_position,
            physical_position,
            capture_position,
        ] in crate::view_model::test_support::presentation_arrival_orders()
        {
            let (mut app, frame) = viewer_app();
            let next = FrameReady {
                content_sequence: 2,
                presentation_revision: 6,
                ..frame
            };
            // Before-publication completion is an inert negative, never a successful schedule.
            drop(app.on_presentation(Message::Surface(
                crate::presentation_surface::Notification::Completed(next),
            )));
            assert!(app.presentation.surface().unwrap().frame.is_none());
            assert!(test_releases().is_empty());
            for position in 0..4 {
                if position == domain_position {
                    let explore = app.model.explore.snapshot.as_mut().unwrap();
                    explore.frame.revision = 2;
                    explore.revision = 20;
                    app.reconcile_presentation(false);
                } else if position == control_position {
                    let snapshot = app.model.presentation.as_mut().unwrap();
                    snapshot.completed.revision = 2;
                    snapshot.completedsourcerevision = 20;
                    snapshot.presentationrevision = 6;
                    app.reconcile_presentation(false);
                } else if position == physical_position {
                    app.present_native_frame(next);
                } else {
                    assert_eq!(position, capture_position);
                    drop(app.on_presentation(Message::Surface(
                        crate::presentation_surface::Notification::Completed(next),
                    )));
                }
                assert_eq!(
                    app.presentation.surface().unwrap().frame,
                    (position >= physical_position).then_some(next),
                );
                assert!(
                    test_releases().is_empty(),
                    "handoff retains physical custody at every intermediate step",
                );
            }
            assert_eq!(app.presentation.surface().unwrap().frame, Some(next));
            assert!(test_releases().is_empty());
            app.presentation.discard();
            assert_eq!(test_releases(), vec![next]);
        }
    }

    #[test]
    fn invalidated_late_frame_and_surface_replacement_release_exactly_once() {
        let (mut app, frame) = viewer_app();
        app.model.explore.requested_selection = Some(9);
        app.present_native_frame(frame);
        assert!(app.presentation.surface().unwrap().frame.is_none());
        assert_eq!(test_releases(), vec![frame]);
        app.model.explore.requested_selection = None;
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            slot: 1,
            ..frame
        };
        app.present_native_frame(pending);
        app.model
            .presentation
            .as_mut()
            .unwrap()
            .capability
            .generation += 1;
        app.sync_surface();
        assert_eq!(test_releases(), vec![frame]);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
        app.presentation.discard();
        assert_eq!(test_releases(), vec![frame, pending]);
    }

    #[test]
    fn unavailable_capability_returns_the_frame_and_retires_imports_idempotently() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        app.model
            .presentation
            .as_mut()
            .unwrap()
            .capability
            .condition = PresentationCapabilityCondition::Unavailable;
        app.sync_surface();
        assert!(app.presentation.surface().is_none());
        assert_eq!(test_releases(), vec![frame]);
        app.sync_surface();
        crate::presentation_surface::retire_imports();
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn admitted_replacements_preserve_incumbent_and_reject_abandoned_candidates() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        let mut latest = frame;
        for generation in 2..5 {
            let snapshot = app.model.presentation.as_mut().unwrap();
            snapshot.capability.generation = generation;
            snapshot.capability.surfacelow = generation + 10;
            snapshot.capability.condition = PresentationCapabilityCondition::Admitted;
            app.sync_surface();
            assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
            app.present_native_frame(latest);
            assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
            latest.low = generation + 10;
            latest.presentation_revision += 1;
        }
        // The newest candidate may replace the incumbent; abandoned advertised
        // candidates never acquire its completion-associated identity.
        app.present_native_frame(latest);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(latest));
        app.presentation.discard();
        let releases = test_releases();
        assert_eq!(
            releases
                .iter()
                .filter(|released| **released == frame)
                .count(),
            1
        );
        assert_eq!(
            releases
                .iter()
                .filter(|released| **released == latest)
                .count(),
            1
        );
    }

    #[test]
    fn failure_retires_unpublished_frame_but_preserves_completed_product() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        app.presentation.failed = true;
        app.reconcile_surface_frame();
        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
        assert!(test_releases().is_empty());
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            slot: 1,
            ..frame
        };
        app.present_native_frame(pending);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
        assert_eq!(test_releases(), vec![pending]);
    }

    #[test]
    fn failure_after_pending_publication_keeps_only_the_completed_copy_receipt() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        record_draw(&app, frame, [0, 0, 640, 480]);
        crate::presentation_surface::release(frame);
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            slot: 1,
            ..frame
        };
        app.present_native_frame(pending);
        let mut snapshot = app.model.presentation.clone().unwrap();
        snapshot.revision += 1;
        app.reduce_event(SystemEvent {
            state_revision: 0,
            delivery: crate::generated::EventDelivery::Critical,
            event: crate::generated::ApplicationEvent::PresentationPresentationFailed(
                crate::generated::PresentationFailed {
                    snapshot,
                    detail: "writer stopped".into(),
                },
            ),
        });
        assert!(app.presentation.surface().unwrap().frame.is_none());
        assert_eq!(
            crate::presentation_surface::drawn_detail(),
            Some((
                Surface {
                    frame: Some(frame),
                    ..app.presentation.surface.unwrap()
                },
                [0, 0, 640, 480]
            ))
        );
        assert_eq!(test_releases(), vec![frame, pending]);
        app.present_native_frame(frame);
        assert!(app.presentation.surface().unwrap().frame.is_none());
        assert_eq!(test_releases(), vec![frame, pending]);
        let (sender, _receiver) = Connection::test_channel();
        app.connection = Some(sender);
        assert!(app.copy_viewer_to_annotation());
    }

    #[test]
    fn overwritten_mailbox_slot_cannot_restore_an_old_draw_receipt() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        record_draw(&app, frame, [0, 0, 640, 480]);
        crate::presentation_surface::release(frame);
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            ..frame
        };
        app.present_native_frame(pending);
        assert!(crate::presentation_surface::drawn_detail().is_none());
        app.presentation.failed = true;
        app.reconcile_surface_frame();
        assert!(app.presentation.surface().unwrap().frame.is_none());
        assert_eq!(test_releases(), vec![frame, pending]);
    }

    #[test]
    fn navigation_retires_but_transport_preserves_withheld_frames() {
        for operation in 0..4 {
            let (mut app, frame) = viewer_app();
            let pending = FrameReady {
                presentation_revision: 6,
                ..frame
            };
            app.present_native_frame(pending);
            match operation {
                0 => {
                    navigate(&mut app, FeatureId::Train);
                }
                1 => {
                    drop(app.on_explore(crate::view::explore::Outcome::CloseDetailRequested));
                }
                2 => app.retire_peer(UiError::transport("peer closed")),
                _ => {
                    let (sender, _receiver) = Connection::test_channel();
                    drop(app.on_transport(TransportEvent::Connected(sender)));
                }
            }
            if operation < 2 {
                assert_eq!(test_releases(), vec![pending]);
                assert!(
                    app.presentation
                        .surface()
                        .is_none_or(|surface| surface.frame.is_none())
                );
            } else {
                assert!(test_releases().is_empty());
                assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
            }
        }
    }

    fn navigate(app: &mut App, feature: FeatureId) {
        drop(crate::app::update(
            app,
            crate::message::Message::Workspace(crate::view::router::Message::Navigation(
                crate::view::navigation::Message::PageSelected(feature),
            )),
        ));
    }

    fn assert_viewer_departed(app: &App) {
        assert_eq!(app.workspace.active(), FeatureId::Train);
        assert!(app.presentation.viewer.is_none());
        assert!(app.model.explore.requested_upscale.is_none());
        assert!(app.model.explore.sent_upscale.is_none());
        assert!(app.model.foreground_visual().is_none());
    }

    fn settle_upscale_stop(app: &mut App, correlation: u64) {
        app.reduce_reply(IntentReply {
            correlation,
            result: Ok(crate::application_codec::IntoApplicationValue::into_application_value(())),
        });
    }

    #[test]
    fn mapped_navigation_preserves_current_viewer_and_dispatches_departure_and_reentry() {
        for persist in [false, true] {
            let (mut app, frame) = viewer_app();
            app.model
                .settings_snapshot
                .as_mut()
                .unwrap()
                .settingsstate
                .currentview = FeatureId::Explore;
            if persist {
                app.settings
                    .install(app.model.settings_snapshot.as_ref().unwrap());
            }
            app.reconcile_viewer();
            let mut requested = app.model.explore.requested_upscale.clone().unwrap();
            requested.kernel = crate::generated::UpscaleKernel::ShiftLut;
            app.model.request_upscale(requested.clone());
            let viewer = app.presentation.viewer;
            app.present_native_frame(frame);
            record_draw(&app, frame, [0, 0, 640, 480]);
            crate::presentation_surface::release(frame);
            let retained = crate::presentation_surface::drawn_detail();
            let pending = FrameReady {
                presentation_revision: 6,
                slot: 1,
                ..frame
            };
            app.present_native_frame(pending);
            let (sender, mut receiver) = Connection::test_channel();
            app.connection = Some(sender);

            navigate(&mut app, FeatureId::Explore);
            assert_eq!(app.presentation.viewer, viewer);
            assert_eq!(
                app.model.explore.requested_upscale.as_ref(),
                Some(&requested)
            );
            assert!(app.model.explore.sent_upscale.is_none());
            assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
            assert_eq!(crate::presentation_surface::drawn_detail(), retained);
            assert!(receiver.try_recv().is_err());
            assert_eq!(test_releases(), vec![frame]);

            navigate(&mut app, FeatureId::Train);
            assert_viewer_departed(&app);
            assert_eq!(test_releases(), vec![frame, pending]);
            let mut operations = Vec::new();
            let mut stop_correlation = None;
            while let Ok(record) = receiver.try_recv() {
                if let crate::transport_connection::CapturedRecord::Intent(intent) = record {
                    operations.push(intent.endpoint_id);
                    if intent.endpoint_id
                        == crate::generated::application_intent_endpoint_stable_id(
                            ApplicationIntentEndpoint::UpscaleStop,
                        )
                    {
                        assert_eq!(
                            intent,
                            crate::generated::encode_upscale_Stop(intent.correlation).record
                        );
                        stop_correlation = Some(intent.correlation);
                    }
                }
            }
            let mut expected = vec![ApplicationIntentEndpoint::UpscaleStop];
            if persist {
                expected.push(ApplicationIntentEndpoint::SettingsUpdate);
                assert_eq!(app.settings.draft().unwrap().currentview, FeatureId::Train);
            }
            assert_eq!(
                operations,
                expected
                    .into_iter()
                    .map(crate::generated::application_intent_endpoint_stable_id)
                    .collect::<Vec<_>>()
            );

            settle_upscale_stop(
                &mut app,
                stop_correlation.expect("expected mapped navigation Stop"),
            );
            // After native Stop settles, no Settings or Presentation event is
            // needed to reenter this same Explore source.
            navigate(&mut app, FeatureId::Explore);
            let basic = app.model.explore.requested_upscale.clone().unwrap();
            assert_eq!(basic.kernel, crate::generated::UpscaleKernel::Default);
            assert_eq!(basic.source, requested.source);
            assert_eq!(basic.document, requested.document);
            assert_eq!(app.presentation.viewer, viewer);
            assert_eq!(app.model.explore.sent_upscale.as_ref(), Some(&basic));
            navigate(&mut app, FeatureId::Explore);
            let crate::transport_connection::CapturedRecord::Intent(intent) =
                receiver.try_recv().unwrap()
            else {
                panic!("expected automatic Basic");
            };
            assert_eq!(
                intent,
                crate::generated::encode_upscale_Start(intent.correlation, basic).record
            );
            assert!(receiver.try_recv().is_err());
            assert_eq!(test_releases(), vec![frame, pending]);
        }
    }

    #[test]
    fn annotation_page_open_and_viewer_copy_have_distinct_source_contracts() {
        let (mut app, frame) = viewer_app();
        let before = app.model.annotation.snapshot.clone();
        let (sender, _receiver) = Connection::test_channel();
        app.connection = Some(sender);
        assert!(!app.copy_viewer_to_annotation());
        assert_eq!(app.model.annotation.snapshot, before);
        app.workspace.select(FeatureId::Annotate);
        app.model.set_foreground_feature(FeatureId::Annotate);
        assert!(app.open_annotation());
        assert_eq!(app.model.annotation.snapshot, before);
        drop(app);

        let (mut app, _) = viewer_app();
        let (sender, _receiver) = Connection::test_channel();
        app.connection = Some(sender);
        app.presentation.pending = Some(Surface {
            frame: Some(frame),
            ..app.presentation.surface.unwrap()
        });
        record_draw(&app, frame, [0, 0, 640, 480]);
        assert!(app.copy_viewer_to_annotation());
    }

    #[test]
    fn unavailable_and_stale_annotation_sources_leave_the_document_unchanged() {
        for condition in 0..5 {
            let (mut app, frame) = viewer_app();
            let (sender, _receiver) = Connection::test_channel();
            app.connection = Some(sender);
            let before = app.model.annotation.snapshot.clone();
            record_draw(&app, frame, [0, 0, 640, 480]);
            match condition {
                0 => app.model.explore.snapshot = None,
                1 => app.model.explore.snapshot.as_mut().unwrap().mode = ExploreMode::Gallery,
                2 => app.model.explore.snapshot.as_mut().unwrap().frame.revision = 0,
                3 => app.model.explore.requested_selection = Some(9),
                _ => {
                    app.model
                        .presentation
                        .as_mut()
                        .unwrap()
                        .presentationrevision += 1
                }
            }
            assert!(!app.copy_viewer_to_annotation());
            if condition != 4 {
                assert!(!app.open_annotation());
            }
            assert_eq!(app.model.annotation.snapshot, before);
            assert_eq!(app.model.pending_count(), 0);
        }
    }

    #[test]
    fn viewer_copy_requires_the_selected_original_crop_and_exact_upscale() {
        let (mut app, mut frame) = viewer_app();
        let explore = app.model.explore.snapshot.as_mut().unwrap();
        explore.detail.showoriginaldimensions = true;
        let input = explore.frame.clone();
        let upscale = app.model.upscale_snapshot.as_mut().unwrap();
        upscale.ready = true;
        upscale.busy = false;
        upscale.input = input.clone();
        upscale.frame =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Upscale, 2);
        upscale.frame.extent = VisualExtent {
            width: 2560,
            height: 1920,
        };
        upscale.frame.content = crate::generated::VisualRegion {
            x: 40,
            y: 80,
            width: 2000,
            height: 1200,
        };
        let expected = upscale.frame.clone();
        app.model.explore.requested_upscale = Some(crate::generated::UpscaleRequest {
            source: input,
            kernel: upscale.kernel,
            document: explore.document.clone(),
        });
        upscale.methods[0].available = true;
        upscale.methods[0].completed = app.model.explore.requested_upscale.clone();
        upscale.methods[0].frame = upscale.frame.clone();
        app.model
            .set_foreground_visual(Some(PresentationSourceKind::Upscale));
        app.model.presentation.as_mut().unwrap().completed = expected.clone();
        app.model.presentation.as_mut().unwrap().capability.extent = expected.extent.clone();
        frame.content_session = 5;
        frame.content_sequence = expected.revision;
        frame.content_width = expected.extent.width;
        frame.content_height = expected.extent.height;
        app.presentation.surface.as_mut().unwrap().width = frame.content_width;
        app.presentation.surface.as_mut().unwrap().height = frame.content_height;
        app.presentation.pending = Some(Surface {
            frame: Some(frame),
            ..app.presentation.surface.unwrap()
        });
        let (sender, mut receiver) = Connection::test_channel();
        app.connection = Some(sender);
        record_draw(&app, frame, [0, 0, 2560, 1920]);
        assert!(!app.copy_viewer_to_annotation());
        record_draw(&app, frame, [40, 80, 2000, 1200]);
        assert!(app.copy_viewer_to_annotation());
        let crate::transport_connection::CapturedRecord::Intent(intent) =
            receiver.try_recv().unwrap()
        else {
            panic!("expected Annotation Open");
        };
        let encoded = crate::generated::encode_annotation_Open(
            intent.correlation,
            AnnotationOpen {
                source: expected,
                originalcontent: true,
            },
        );
        assert_eq!(intent, encoded.record);
    }
}
