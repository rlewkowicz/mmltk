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
                if let Some(snapshot) = source
                    .filter(|snapshot| snapshot.ready && !snapshot.busy && !snapshot.renderpending)
                {
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
        if let Some(snapshot) =
            source.filter(|snapshot| snapshot.ready && !snapshot.busy && !snapshot.renderpending)
        {
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

    fn update(&mut self, message: Message, model: &ApplicationModel) -> Update {
        let mut update = Update {
            native: None,
            redraw: false,
        };
        match message {
            Message::Surface(crate::presentation_surface::Notification::Native(frame)) => {
                update.native = Some((frame, self.surface()));
                update.redraw = self.present(frame, model);
            }
            Message::Surface(crate::presentation_surface::Notification::Copied(frame)) => {
                crate::presentation_surface::complete_sample(frame);
                if self.pending.and_then(|surface| surface.frame) == Some(frame) {
                    self.pending_rejected = false;
                }
                update.redraw = true;
            }
            Message::Surface(crate::presentation_surface::Notification::SampleRejected(frame)) => {
                if self.pending.and_then(|surface| surface.frame) == Some(frame) {
                    crate::presentation_surface::trace_surface(
                        "sample_rejection_received",
                        self.pending.expect("matching rejected publication"),
                    );
                    self.pending_rejected = true;
                    update.redraw = true;
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

    fn trace_redraw_decision(&self, previous: Option<Surface>, outcome: &str) {
        #[cfg(target_arch = "wasm32")]
        if crate::presentation_surface::surface_trace_enabled() {
            // Surface contains only numeric identities, geometry and flags.
            // Project those fields only when the existing surface trace is enabled.
            let identity = |surface: Option<Surface>| {
                surface.map_or_else(
                    || "null".to_owned(),
                    |surface| {
                        let crop = surface.crop.map_or_else(
                            || "null".to_owned(),
                            |crop| format!("{crop:?}"),
                        );
                        let viewer = surface.viewer_identity.map_or_else(
                            || "null".to_owned(),
                            |(dataset, image)| format!("[{dataset},{image}]"),
                        );
                        format!(
                            "{{{},\"crop\":{crop},\"viewer_identity\":{viewer},\"fit_revision\":{}}}",
                            crate::presentation_surface::surface_trace_fields(surface, surface),
                            surface.fit_revision,
                        )
                    },
                )
            };
            let current = self.surface();
            let gallery_awaiting_display = current
                .and_then(|surface| surface.frame)
                .is_some_and(crate::presentation_surface::gallery::awaiting_display);
            crate::presentation_surface::emit_surface_trace(&format!(
                "{{\"event\":\"iced.presentation.redraw_decision\",\"outcome\":\"{outcome}\",\"changed\":{},\"gallery_awaiting_display\":{gallery_awaiting_display},\"pending_rejected\":{},\"previous\":{},\"current\":{},\"pending\":{},\"retained\":{},\"renderer_retained\":{}}}",
                current != previous,
                self.pending_rejected,
                identity(previous),
                identity(current),
                identity(self.pending),
                identity(self.retained),
                identity(crate::presentation_surface::retained_surface()),
            ));
        }
        #[cfg(not(target_arch = "wasm32"))]
        let _ = (previous, outcome);
    }

    pub(super) fn redraw(&self, previous: Option<Surface>) -> Task<Message> {
        match self.surface() {
            Some(surface) if self.surface() != previous && surface.frame.is_none() => {
                self.trace_redraw_decision(previous, "queued");
                queued_redraw(surface)
            }
            _ if self.surface() != previous
                || self
                    .surface()
                    .and_then(|surface| surface.frame)
                    .is_some_and(crate::presentation_surface::gallery::awaiting_display) =>
            {
                self.trace_redraw_decision(previous, "requested");
                iced::window::request_redraw()
            }
            _ => {
                self.trace_redraw_decision(previous, "none");
                Task::none()
            }
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
            self.presentation.retire_frame();
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

    fn on_viewer(&mut self, mut outcome: Option<ViewerOutcome>) {
        // A method can be selected while this viewer's initial render is
        // pending. Keep that exact request when applying the automatic open.
        if let Some(ViewerOutcome::Opened(request) | ViewerOutcome::Replaced(request)) =
            &mut outcome
            && let Some(selected) = self.model.explore.requested_upscale.as_ref()
            && selected.source == request.source
            && selected.document == request.document
        {
            request.kernel = selected.kernel;
        }
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
        self.dispatch_explore_viewport()
    }

    pub(super) fn transition_page(&mut self, feature: FeatureId) -> Task<crate::message::Message> {
        if self.workspace.active() == feature {
            return Task::none();
        }
        self.abandon_viewer();
        self.presentation.retire_frame();
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
            self.model.record_presentation_sent(frame);
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

    pub(super) fn reconcile_presentation(&mut self, recovery: bool) {
        self.reconcile_viewer();
        self.dispatch_viewer_desired();
        self.reconcile_surface_frame();
        if recovery
            && let Err(error) =
                self.presentation
                    .reconcile(&self.model, self.workspace.active(), true)
        {
            self.retire_peer(error);
            return;
        }
        let refresh = if recovery {
            self.model.presentation_recovery_refresh()
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
    fn present(&mut self, frame: FrameReady, _model: &ApplicationModel) -> bool {
        if self.pending.and_then(|pending| pending.frame) == Some(frame) {
            return false;
        }
        let Some(surface) = crate::presentation_surface::metadata::surface(frame) else {
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
        if !crate::presentation_surface::accept_publication(frame) {
            crate::presentation_surface::release(frame);
            return false;
        }
        if let Some(previous) = self.pending.and_then(|pending| pending.frame)
            && previous != frame
        {
            // Batched UI updates may replace a frame before its first draw.
            // Actual encoded readers own independent sample holds; otherwise
            // this returns an unused sample or deduplicates its settled release.
            crate::presentation_surface::retire_publication(previous);
            crate::presentation_surface::discard_sample(previous);
        }
        crate::presentation_surface::invalidate_drawn_slot(frame);
        self.surface = Some(Surface {
            frame: None,
            ..surface
        });
        self.pending = Some(Surface {
            frame: Some(frame),
            ..surface
        });
        self.pending_rejected = false;
        self.incumbent = Some(surface);
        true
    }

    pub(super) fn retire_frame(&mut self) {
        crate::presentation_surface::retire_samples();
        self.retire_pending();
        self.retained = None;
        crate::presentation_surface::clear_drawn_detail();
    }

    fn retire_pending(&mut self) {
        self.pending_rejected = false;
        if let Some(frame) = self.pending.take().and_then(|surface| surface.frame) {
            crate::presentation_surface::retire_publication(frame);
            crate::presentation_surface::discard_sample(frame);
        }
    }

    #[cfg(test)]
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
        // Transport reset temporarily installs the router's default page.
        // The suspended owner retains the actual route until reentry or departure.
        let feature = self
            .suspended
            .as_ref()
            .map_or(feature, |suspended| suspended.route);
        if matches!(
            feature,
            FeatureId::Train | FeatureId::Validate | FeatureId::Export
        ) {
            self.retire_frame();
            return Ok(());
        }
        let _ = recovery;
        if self.pending_rejected {
            self.retire_pending();
        }
        if let Some(surface) = self.pending {
            crate::presentation_surface::reconcile_completed(surface, model);
            if let Some(retained) = crate::presentation_surface::retained_surface()
                .filter(|retained| retained.frame == surface.frame)
            {
                self.retained = Some(retained);
                self.incumbent = Some(retained);
                self.pending = None;
                self.pending_rejected = false;
            }
        } else if let Some(retained) = self.retained {
            crate::presentation_surface::reconcile_completed(retained, model);
        }

        Ok(())
    }
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

        let mut busy = accept_upscale_start(&mut app, start_correlation, basic.clone());
        assert_presentation_selection(&mut receiver, basic.source.source.clone());
        assert!(receiver.try_recv().is_err());

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
                crate::application_codec::IntoApplicationValue::into_application_transport_value(
                    busy,
                ),
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

        accept_upscale_start(&mut app, start_correlation, basic.clone());
        let crate::transport_connection::CapturedRecord::Intent(stop) =
            receiver.try_recv().expect("expected deferred Stop")
        else {
            panic!("expected Stop intent");
        };
        assert_eq!(
            stop,
            crate::generated::encode_upscale_Stop(stop.correlation).record
        );
        assert_presentation_selection(&mut receiver, basic.source.source.clone());
        assert!(!app.presentation.stop_requested);
        assert!(app.model.error.is_none());

        app.dispatch_viewer_desired();
        assert!(app.model.error.is_none());
        assert!(receiver.try_recv().is_err());
        settle_upscale_stop(&mut app, stop.correlation);
        assert!(app.model.error.is_none());
        let latest = app.model.explore.requested_upscale.clone().unwrap();
        let crate::transport_connection::CapturedRecord::Intent(start) = receiver
            .try_recv()
            .expect("expected restart after Stop settled")
        else {
            panic!("expected Start intent");
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
                viewer_identity: app.model.explore.snapshot.as_ref().and_then(|snapshot| {
                    snapshot
                        .selectedimage
                        .map(|image| (snapshot.dataset.identity, image))
                }),
                crop: (crop != [0, 0, frame.content_width, frame.content_height]).then_some(crop),
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
        app.presentation.surface =
            crate::presentation_surface::metadata::surface(frame).map(|surface| Surface {
                frame: None,
                ..surface
            });
        app.reconcile_surface_frame();
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

    fn accept_upscale_start(
        app: &mut App,
        correlation: u64,
        request: crate::generated::UpscaleRequest,
    ) -> crate::generated::UpscaleSnapshot {
        let mut busy = app.model.upscale_snapshot.clone().unwrap();
        busy.revision += 1;
        busy.busy = true;
        busy.pending = Some(request);
        app.reduce_reply(IntentReply {
            correlation,
            result: Ok(
                crate::application_codec::IntoApplicationValue::into_application_transport_value(
                    busy.clone(),
                ),
            ),
        });
        busy
    }

    fn assert_presentation_selection(
        receiver: &mut crate::transport_connection::Capture,
        source: crate::generated::PresentationSourceIdentity,
    ) {
        let crate::transport_connection::CapturedRecord::Intent(selection) = receiver
            .try_recv()
            .expect("expected presentation selection")
        else {
            panic!("expected presentation intent");
        };
        assert_eq!(
            selection,
            crate::generated::encode_presentation_Select(selection.correlation, source).record
        );
    }

    #[test]
    fn logical_empty_and_missing_snapshots_cannot_retire_physical_samples() {
        for direct_sampling in [false, true] {
            for condition in 0..4 {
                let (mut app, mut frame) = viewer_app();
                frame.direct_sampling = direct_sampling;
                let (sender, _receiver) = Connection::test_channel();
                app.connection = Some(sender);
                app.reconcile_viewer();
                install_next(&app, frame);
                app.present_native_frame(frame);
                let retained = app.presentation.surface().unwrap();
                app.presentation.retained = Some(retained);
                let read = crate::presentation_surface::test_sample_read(frame);
                let explore = app.model.explore.snapshot.as_mut().unwrap();
                explore.ready = true;
                explore.mode = ExploreMode::Gallery;
                explore.order.matchingcount = 0;
                explore.frame.revision += 1;
                match condition {
                    0 => explore.busy = true,
                    1 => explore.ready = false,
                    2 => app.model.explore.snapshot = None,
                    _ => app.model.presentation = None,
                }
                app.reconcile_viewer();
                app.presentation
                    .reconcile(&app.model, FeatureId::Explore, false)
                    .unwrap();
                assert_eq!(app.presentation.retained, Some(retained));
                assert_eq!(app.presentation.pending, Some(retained));
                assert!(test_releases().is_empty());
                app.presentation.discard();
                assert!(test_releases().is_empty());
                drop(read);
                assert_eq!(test_releases(), vec![frame]);
            }
        }
    }

    #[test]
    fn graphics_notifications_schedule_detail_and_empty_composition_without_application_events() {
        for empty in [false, true] {
            let (mut app, frame) = viewer_app();
            if empty {
                let mut metadata = app.model.explore.snapshot.clone().unwrap();
                metadata.mode = ExploreMode::Gallery;
                metadata.order.matchingcount = 0;
                metadata.order.visibleindices.clear();
                metadata.gallery.slots.clear();
                metadata.viewport.columns = 4;
                metadata.viewport.rowcount = 3;
                metadata.viewport.firstrow = 0;
                metadata.viewport.extent = metadata.frame.extent.clone();
                crate::view_model::test_support::gallery_layout(&mut metadata);
                crate::presentation_surface::metadata::install_explore(frame, &metadata);
            }
            app.model.explore.snapshot = None;
            app.model.presentation = None;
            let update = app.presentation.update(
                Message::Surface(crate::presentation_surface::Notification::Native(frame)),
                &app.model,
            );
            assert!(update.redraw);
            app.reconcile_surface_frame();
            let surface = app.presentation.surface();
            match crate::presentation_surface::explore_display(surface).unwrap() {
                crate::presentation_surface::ExploreDisplay::Gallery(shown, metadata) => {
                    assert!(empty);
                    assert_eq!(shown.frame, Some(frame));
                    assert_eq!(metadata.order.matchingcount, 0);
                }
                crate::presentation_surface::ExploreDisplay::Detail(shown, _) => {
                    assert!(!empty);
                    assert_eq!(shown.frame, Some(frame));
                }
            }
            let state = crate::view::explore::state::State::default();
            drop(crate::view::explore::view(
                &state,
                &app.model,
                app.settings.state(),
                surface,
                1200.0,
                crate::workspace_input::Binding::default(),
            ));
            assert!(test_releases().is_empty());
            // Import readiness and copy completion use the same independent wake path.
            assert!(
                app.presentation
                    .update(
                        Message::Surface(crate::presentation_surface::Notification::Drawn),
                        &app.model
                    )
                    .redraw
            );
            assert!(
                app.presentation
                    .update(
                        Message::Surface(crate::presentation_surface::Notification::Copied(frame)),
                        &app.model
                    )
                    .redraw
            );
            app.presentation.discard();
            assert_eq!(test_releases(), vec![frame]);
        }
    }

    #[test]
    fn quiet_integration_keeps_real_presentation_without_surface_instrumentation() {
        let (mut app, frame) = viewer_app();
        app.config.integration = true;
        app.reconcile_surface_frame();
        assert!(!crate::integration_control::reporting_enabled());
        app.present_native_frame(frame);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
        assert!(test_releases().is_empty());
        app.presentation.discard();
        assert_eq!(test_releases(), vec![frame]);
    }

    fn install_next(app: &App, frame: FrameReady) {
        let mut product = app.model.explore.snapshot.clone().unwrap();
        product.frame.revision = frame.content_sequence;
        product.frame.extent.width = frame.content_width;
        product.frame.extent.height = frame.content_height;
        crate::presentation_surface::metadata::install_explore(frame, &product);
    }

    #[test]
    fn graphics_receipt_is_usable_without_control_or_domain_observations() {
        let (mut app, frame) = viewer_app();
        app.model.presentation = None;
        app.model.explore.snapshot = None;
        app.present_native_frame(frame);
        app.reconcile_surface_frame();
        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
        assert!(test_releases().is_empty());
        app.presentation.discard();
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn replacement_receipts_preserve_encoder_custody_and_reject_stale_publications() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        let capture = crate::presentation_surface::test_sample_read(frame);
        let next = FrameReady {
            low: frame.low + 1,
            presentation_revision: frame.presentation_revision + 1,
            ..frame
        };
        install_next(&app, next);
        app.present_native_frame(next);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(next));
        assert!(test_releases().is_empty());
        app.present_native_frame(frame);
        assert_eq!(app.presentation.surface().unwrap().frame, Some(next));
        drop(capture);
        assert_eq!(test_releases(), vec![frame]);
        app.presentation.discard();
        assert_eq!(test_releases(), vec![frame, next]);
    }

    #[test]
    fn reconnect_preserves_capture_with_newer_or_missing_application_metadata() {
        for changed in [false, true] {
            let (mut app, frame) = viewer_app();
            app.present_native_frame(frame);
            let capture = crate::presentation_surface::test_sample_read(frame);
            let mut restored = app.model.explore.snapshot.clone();
            app.retire_peer(UiError::transport("capture continuity"));
            assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
            if changed {
                restored.as_mut().unwrap().frame.revision += 10;
            }
            app.model.explore.snapshot = restored;
            app.model.presentation = None;
            app.model.set_foreground_feature(FeatureId::Explore);
            app.reconcile_surface_frame();
            assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
            assert!(test_releases().is_empty());
            app.presentation.discard();
            assert!(test_releases().is_empty());
            drop(capture);
            assert_eq!(test_releases(), vec![frame]);
        }
    }

    #[test]
    fn domain_control_publication_and_copy_reconcile_in_every_notification_order() {
        for [
            domain_position,
            control_position,
            physical_position,
            copy_position,
        ] in crate::view_model::test_support::presentation_arrival_orders()
        {
            let (mut app, frame) = viewer_app();
            let next = FrameReady {
                content_sequence: 2,
                presentation_revision: 6,
                ..frame
            };
            install_next(&app, next);
            for position in 0..4 {
                if position == domain_position {
                    let explore = app.model.explore.snapshot.as_mut().unwrap();
                    explore.frame.revision = 99;
                    explore.revision += 20;
                    app.reconcile_surface_frame();
                } else if position == control_position {
                    app.model.presentation = None;
                    app.reconcile_surface_frame();
                } else if position == physical_position {
                    app.present_native_frame(next);
                } else {
                    assert_eq!(position, copy_position);
                    drop(app.on_presentation(Message::Surface(
                        crate::presentation_surface::Notification::Copied(next),
                    )));
                }
                assert_eq!(
                    app.presentation.surface().unwrap().frame,
                    (position >= physical_position).then_some(next)
                );
                assert!(test_releases().is_empty());
            }
            app.presentation.discard();
            assert_eq!(test_releases(), vec![next]);
        }
    }

    #[test]
    fn rejected_graphics_receipt_preserves_retained_fallback_and_actual_readers() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        let retained = app.presentation.surface().unwrap();
        app.presentation.retained = Some(retained);
        let retained_read = crate::presentation_surface::test_sample_read(frame);
        let next = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            slot: 1,
            ..frame
        };
        install_next(&app, next);
        app.present_native_frame(next);
        let read = crate::presentation_surface::test_sample_read(next);
        drop(app.on_presentation(Message::Surface(
            crate::presentation_surface::Notification::SampleRejected(next),
        )));
        assert_eq!(app.presentation.retained, Some(retained));
        assert!(app.presentation.pending.is_none());
        assert!(test_releases().is_empty());
        drop(read);
        assert_eq!(test_releases(), vec![next]);
        app.presentation.discard();
        drop(retained_read);
        assert_eq!(test_releases(), vec![next, frame]);
    }

    #[test]
    fn application_writer_failure_does_not_revoke_a_completed_graphics_receipt() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
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
        assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
        assert!(app.model.error.is_some());
        assert!(test_releases().is_empty());
        app.presentation.discard();
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn page_departure_retires_while_image_navigation_and_transport_preserve_frames() {
        for operation in 0..7 {
            let (mut app, frame) = viewer_app();
            let pending = FrameReady {
                presentation_revision: 6,
                ..frame
            };
            install_next(&app, pending);
            app.present_native_frame(pending);
            match operation {
                0 => {
                    navigate(&mut app, FeatureId::Train);
                }
                1 => {
                    drop(app.on_explore(crate::view::explore::Outcome::CloseDetailRequested));
                }
                2 => app.retire_peer(UiError::transport("peer closed")),
                3 => {
                    let (sender, _receiver) = Connection::test_channel();
                    drop(app.on_transport(TransportEvent::Connected(sender)));
                }
                4 => {
                    drop(app.on_explore(crate::view::explore::Outcome::NextRequested));
                }
                5 => {
                    drop(app.on_explore(crate::view::explore::Outcome::PreviousRequested));
                }
                _ => {
                    drop(app.on_explore(crate::view::explore::Outcome::ImageSelected(1)));
                }
            }
            if operation == 0 {
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
            app.presentation.discard();
            assert_eq!(test_releases(), vec![pending]);
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
            result: Ok(crate::application_codec::IntoApplicationValue::into_application_transport_value(())),
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
            install_next(&app, pending);
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
            let crate::transport_connection::CapturedRecord::Intent(stop) = receiver
                .try_recv()
                .expect("expected mapped navigation Stop")
            else {
                panic!("expected Stop intent");
            };
            assert_eq!(
                stop,
                crate::generated::encode_upscale_Stop(stop.correlation).record
            );
            if persist {
                let crate::transport_connection::CapturedRecord::Intent(settings) = receiver
                    .try_recv()
                    .expect("expected persisted page selection")
                else {
                    panic!("expected Settings intent");
                };
                assert_eq!(
                    settings.endpoint_id,
                    crate::generated::application_intent_endpoint_stable_id(
                        ApplicationIntentEndpoint::SettingsUpdate,
                    )
                );
                assert_eq!(app.settings.draft().unwrap().currentview, FeatureId::Train);
            }
            assert!(receiver.try_recv().is_err());

            settle_upscale_stop(&mut app, stop.correlation);
            assert!(receiver.try_recv().is_err());
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
            assert_presentation_selection(&mut receiver, basic.source.source.clone());
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
    fn annotation_copy_admits_busy_work_and_rejects_unavailable_or_stale_sources() {
        for condition in 0..5 {
            let (mut app, frame) = viewer_app();
            app.model
                .annotation
                .snapshot
                .as_mut()
                .unwrap()
                .inputdocumentepoch = 31;
            app.integration = Some(crate::integration_control::Controller::new(
                true,
                false,
                String::new(),
                String::new(),
                String::new(),
                "copy".into(),
            ));
            let (sender, mut receiver) = Connection::test_channel();
            app.connection = Some(sender);
            let before = app.model.annotation.snapshot.clone();
            record_draw(&app, frame, [0, 0, 640, 480]);
            let drawn_source = crate::presentation_surface::viewer_annotation_request().unwrap();
            match condition {
                0 => app.model.annotation.snapshot = None,
                1 => app.model.annotation.snapshot.as_mut().unwrap().busy = true,
                2 => crate::presentation_surface::metadata::retire(frame),
                3 => record_draw(&app, frame, [1, 0, 639, 480]),
                _ => crate::presentation_surface::clear_drawn_detail(),
            }
            let at_request = app.model.annotation.snapshot.clone();
            if condition == 1 {
                assert!(app.copy_viewer_to_annotation());
                assert_eq!(
                    app.integration.as_ref().unwrap().annotation_open_for_test(),
                    Some((
                        &drawn_source,
                        at_request.as_ref().unwrap().inputdocumentepoch
                    ))
                );
                let crate::transport_connection::CapturedRecord::Intent(open) = receiver
                    .try_recv()
                    .expect("expected queued Annotation Open")
                else {
                    panic!("expected Annotation intent");
                };
                assert_eq!(
                    open,
                    crate::generated::encode_annotation_Open(open.correlation, drawn_source).record
                );
                assert_eq!(app.model.pending_count(), 1);
                assert!(
                    app.model
                        .has_pending(ApplicationIntentEndpoint::AnnotationOpen)
                );
            } else {
                assert!(!app.copy_viewer_to_annotation());
                assert_eq!(
                    app.integration.as_ref().unwrap().annotation_open_for_test(),
                    None
                );
                assert_eq!(app.model.pending_count(), 0);
            }
            assert!(receiver.try_recv().is_err());
            assert_eq!(app.model.annotation.snapshot, at_request);
            if condition >= 2 {
                assert_eq!(app.model.annotation.snapshot, before);
            }
        }
    }

    #[test]
    fn viewer_copy_ignores_unrelated_logical_sources_and_encodes_the_drawn_frame() {
        for condition in 0..5 {
            let (mut app, frame) = viewer_app();
            let expected = app.model.explore.snapshot.as_ref().unwrap().frame.clone();
            record_draw(&app, frame, [0, 0, 640, 480]);
            let (sender, mut receiver) = Connection::test_channel();
            app.connection = Some(sender);
            match condition {
                0 => app.model.explore.snapshot = None,
                1 => app.model.explore.snapshot.as_mut().unwrap().mode = ExploreMode::Gallery,
                2 => app.model.explore.snapshot.as_mut().unwrap().frame.revision = 0,
                3 => {
                    app.model
                        .explore
                        .snapshot
                        .as_mut()
                        .unwrap()
                        .detail
                        .showoriginaldimensions = true
                }
                _ => app.model.presentation = None,
            }
            if condition <= 2 {
                assert!(!app.open_annotation());
            }
            assert!(app.copy_viewer_to_annotation());
            let crate::transport_connection::CapturedRecord::Intent(intent) =
                receiver.try_recv().unwrap()
            else {
                panic!("expected exact Annotation Open");
            };
            assert_eq!(
                intent,
                crate::generated::encode_annotation_Open(
                    intent.correlation,
                    AnnotationOpen {
                        source: expected,
                        originalcontent: false
                    }
                )
                .record
            );
        }
    }

    #[test]
    fn viewer_copy_classifies_the_drawn_crop_and_exact_retained_upscale() {
        let (mut app, mut frame) = viewer_app();
        let explore = app.model.explore.snapshot.as_mut().unwrap();
        explore.detail.showoriginaldimensions = false;
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
        frame.content_session = 5;
        frame.content_sequence = expected.revision;
        frame.content_width = expected.extent.width;
        frame.content_height = expected.extent.height;
        use crate::presentation_surface::metadata;
        let bytes = metadata::encode(
            expected.clone(),
            metadata::encode_product(
                crate::generated::ApplicationSystem::Upscale,
                crate::generated::UpscaleImageMetadata::from(
                    app.model.upscale_snapshot.as_ref().unwrap(),
                ),
            ),
            Some(metadata::encode_product(
                crate::generated::ApplicationSystem::Explore,
                crate::generated::ExploreImageMetadata::from(
                    app.model.explore.snapshot.as_ref().unwrap(),
                ),
            )),
        );
        metadata::install(
            frame,
            frame.content_width,
            frame.content_height,
            frame.presentation_revision,
            &bytes,
        )
        .unwrap();
        app.presentation.surface.as_mut().unwrap().width = frame.content_width;
        app.presentation.surface.as_mut().unwrap().height = frame.content_height;
        app.presentation.pending = Some(Surface {
            frame: Some(frame),
            ..app.presentation.surface.unwrap()
        });
        let (sender, mut receiver) = Connection::test_channel();
        app.connection = Some(sender);
        // The captured Explore preference is deliberately the opposite of this draw.
        app.model
            .explore
            .snapshot
            .as_mut()
            .unwrap()
            .detail
            .showoriginaldimensions = true;
        app.model.explore.snapshot.as_mut().unwrap().frame.revision += 100;
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
