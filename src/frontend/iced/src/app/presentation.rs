use super::*;

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
    retained: Option<Surface>,
    failed: bool,
}

struct Update {
    native: Option<(FrameReady, Option<Surface>)>,
    observation: Option<RendererObservation>,
    redraw: bool,
}

impl Controller {
    pub(super) fn surface(&self) -> Option<Surface> {
        if let Some(pending) = self.pending {
            return Some(pending);
        }
        self.surface.map(|mut surface| {
            surface.frame = self.retained
                    .filter(|retained| crate::presentation_surface::same_allocation(*retained, surface))
                    .and_then(|retained| retained.frame);
            surface
        })
    }

    #[cfg(test)]
    pub(super) fn set_test_surface(&mut self, surface: Surface) {
        self.pending = surface.frame.map(|_| surface);
        self.surface = Some(Surface { frame: None, ..surface });
        self.incumbent = self.surface;
    }

    pub(super) fn reset_failure(&mut self) {
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
                update.redraw = true;
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
            _ if self.surface() != previous
                || self.surface().and_then(|surface| surface.frame)
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
    pub(super) fn on_presentation(&mut self, message: Message) -> Task<crate::message::Message> {
        let update = self.presentation.update(message, &self.model);
        if let Some((frame, surface)) = update.native {
            self.integration.observe_native_frame(frame, surface);
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

    pub(super) fn on_window(&mut self, event: iced::window::Event) -> Task<crate::message::Message> {
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
        self.presentation.retire_frame();
        self.workspace.select(feature);
        self.model.set_foreground_feature(feature);
        if let Some(frame) = self.model.presentation_refresh() {
            self.select_presentation(frame);
        }
        if self.settings.draft().is_none() || !self.model.settings_edit_available() {
            return Task::none();
        }
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
        if let Err(error) = self.presentation.reconcile(&self.model, self.workspace.active(), false) {
            self.retire_peer(error);
        }
    }

    pub(super) fn sync_surface(&mut self) {
        if let Err(error) = self.presentation.sync(&self.model, self.config.integration) {
            self.model.error = Some(UiError::presentation(error));
        }
        self.reconcile_surface_frame();
    }

    pub(super) fn reconcile_presentation(&mut self, refresh: Option<VisualFrame>, recovery: bool) {
        if let Err(error) = self.model.completed_presentation_is_obsolete() {
            self.retire_peer(error);
            return;
        }
        self.sync_surface();
        if recovery
            && let Err(error) = self.presentation.reconcile(&self.model, self.workspace.active(), true)
        {
            self.retire_peer(error);
            return;
        }
        let refresh = if recovery {
            self.model.presentation_recovery_refresh().filter(|frame| {
                self.model.presentation.as_ref().is_none_or(|snapshot| {
                    !self.presentation.pending.and_then(|surface| surface.frame)
                        .is_some_and(|pending| pending.matches_content(frame) && pending.matches_completed(snapshot))
                        && crate::presentation_surface::completed_content(frame, snapshot).is_none()
                })
            })
        } else {
            refresh
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
        let completed = model.presentation.as_ref()
            .is_some_and(|snapshot| frame.matches_completed(snapshot));
        let Some(surface) = self.surface.filter(|surface| frame.belongs_to(*surface))
            .or_else(|| self.incumbent.filter(|surface| completed && frame.belongs_to(*surface)))
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
        self.pending = Some(Surface { frame: Some(frame), ..surface });
        if completed {
            self.incumbent = Some(surface);
        }
        true
    }

    pub(super) fn retire_frame(&mut self) {
        self.retire_pending();
        self.retained = None;
        crate::presentation_surface::clear_drawn_detail();
    }

    fn retire_pending(&mut self) {
        if let Some(frame) = self
            .pending
            .take()
            .and_then(|surface| surface.frame)
        {
            crate::presentation_surface::retire_publication(frame);
            crate::presentation_surface::discard_capture(frame);
        }
    }

    pub(super) fn discard(&mut self) {
        self.retire_frame();
        self.surface = None;
        self.incumbent = None;
    }

    fn reconcile(&mut self, model: &ApplicationModel, feature: FeatureId, recovery: bool) -> Result<(), UiError> {
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
            && let Some(retained) = crate::presentation_surface::completed_content(&snapshot.completed, snapshot)
        {
            crate::presentation_surface::reconcile_completed(
                retained.frame.expect("completed retained surface"), model,
            );
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
        if frame.presentation_revision > snapshot.presentationrevision {
            return Ok(());
        }
        if decision == crate::view_model::PresentationReconciliation::Superseded || !completed {
            self.retire_pending();
        } else if decision == crate::view_model::PresentationReconciliation::Matching {
            crate::presentation_surface::gallery::confirm(
                frame, &snapshot.completed, model.explore.snapshot.as_ref(),
            );
            crate::presentation_surface::reconcile_completed(frame, model);
            if let Some(retained) = crate::presentation_surface::retained_surface()
                .filter(|surface| surface.frame == Some(frame))
            {
                self.retained = Some(retained);
                self.pending = None;
            }
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

    fn record_draw(app: &App, frame: FrameReady, crop: [u32; 4]) {
        let surface = app.presentation.surface.unwrap();
        assert!(frame.belongs_to(surface));
        record_drawn_detail(Surface { frame: Some(frame), ..surface }, crop);
    }

    fn viewer_app() -> (App, FrameReady) {
        reset_test_releases();
        let (mut app, task) = crate::app::boot();
        drop(task);
        app.model = crate::view_model::test_support::bootstrapped();
        app.workspace.select(FeatureId::Explore);
        app.model.set_foreground_feature(FeatureId::Explore);
        let source =
            crate::view_model::test_support::visual_frame(PresentationSourceKind::Explore, 1);
        let explore = app.model.explore.snapshot.as_mut().unwrap();
        explore.ready = true;
        explore.revision = 10;
        explore.mode = ExploreMode::Detail;
        explore.selectedimage = Some(0);
        explore.frame = source.clone();
        let snapshot = app.model.presentation.as_mut().unwrap();
        snapshot.selected = source.source.clone();
        snapshot.completed = source;
        snapshot.presentationrevision = 5;
        snapshot.completedsourcerevision = 10;
        snapshot.capability = crate::generated::PresentationCapability {
            surfacehigh: 1,
            surfacelow: 2,
            generation: 1,
            extent: VisualExtent {
                width: 640,
                height: 480,
            },
            condition: PresentationCapabilityCondition::Ready,
        };
        app.sync_surface();
        let frame = FrameReady {
            high: 1,
            low: 2,
            layer: 0,
            slot: 0,
            content_session: 1,
            content_sequence: 1,
            presentation_revision: 5,
            content_width: 640,
            content_height: 480,
        };
        (app, frame)
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
        for receipt_first in [false, true] {
            let (mut app, frame) = viewer_app();
            let incumbent = app.presentation.surface().unwrap();
            if receipt_first {
                app.present_native_frame(frame);
            }
            let snapshot = app.model.presentation.as_mut().unwrap();
            snapshot.capability.surfacelow += 1;
            snapshot.capability.generation += 1;
            snapshot.capability.extent.width *= 2;
            snapshot.capability.condition = PresentationCapabilityCondition::Admitted;
            app.sync_surface();
            if !receipt_first {
                app.present_native_frame(frame);
            }
            let capture_surface = app.presentation.surface().unwrap();
            assert_eq!(capture_surface, Surface { frame: Some(frame), ..incumbent });
            assert_ne!(capture_surface.low, app.presentation.surface.unwrap().low);
            assert!(test_releases().is_empty());
            let capture = crate::presentation_surface::test_capture_borrow(frame);
            app.sync_surface();
            assert_eq!(app.presentation.surface(), Some(capture_surface));
            assert!(test_releases().is_empty());
            drop(capture);
            drop(app.on_presentation(Message::Surface(crate::presentation_surface::Notification::Completed(frame))));
            assert_eq!(test_releases(), vec![frame]);
            app.presentation.discard();
            assert_eq!(test_releases(), vec![frame]);
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
                    drop(app.on_presentation(Message::Surface(crate::presentation_surface::Notification::Completed(frame))));
                }
                app.retire_peer(UiError::transport("capture continuity"));
                assert_eq!(app.presentation.surface().unwrap().frame, Some(frame));
                let (sender, _receiver) = futures_channel::mpsc::channel(4);
                drop(app.on_transport(TransportEvent::Connected(Connection::new(sender))));
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
                app.reconcile_presentation(None, true);
                assert_eq!(app.presentation.pending.is_some(), matching);
                assert_eq!(test_releases(), if complete_before_disconnect { vec![frame] } else { vec![] });
                // Matching receiver custody suppresses another Select even
                // while the callback is outstanding; nonmatching recovery asks
                // for the new foreground product.
                assert_eq!(app.model.has_pending(ApplicationIntentEndpoint::PresentationSelect), !matching);
                drop(capture);
                drop(app.on_presentation(Message::Surface(crate::presentation_surface::Notification::Completed(frame))));
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
        let completed = Surface { frame: Some(frame), ..app.presentation.surface.unwrap() };
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
        assert_eq!(crate::presentation_surface::drawn_detail(), Some((completed, completed.content_region())));
        let (sender, _receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
        assert!(app.copy_viewer_to_annotation());
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn domain_control_publication_and_capture_reconcile_in_every_arrival_order() {
        for domain_position in 0..4 {
            for control_position in (0..4).filter(|position| *position != domain_position) {
                let remaining: Vec<_> = (0..4)
                    .filter(|position| *position != domain_position && *position != control_position)
                    .collect();
                for physical_position in remaining {
                    let (mut app, frame) = viewer_app();
                    let next = FrameReady { content_sequence: 2, presentation_revision: 6, ..frame };
                    for position in 0..4 {
                        if position == domain_position {
                            let explore = app.model.explore.snapshot.as_mut().unwrap();
                            explore.frame.revision = 2;
                            explore.revision = 20;
                            app.reconcile_presentation(None, false);
                        } else if position == control_position {
                            let snapshot = app.model.presentation.as_mut().unwrap();
                            snapshot.completed.revision = 2;
                            snapshot.completedsourcerevision = 20;
                            snapshot.presentationrevision = 6;
                            app.reconcile_presentation(None, false);
                        } else if position == physical_position {
                            app.present_native_frame(next);
                        } else {
                            drop(app.on_presentation(Message::Surface(
                                crate::presentation_surface::Notification::Completed(next),
                            )));
                        }
                    }
                    assert_eq!(app.presentation.surface().unwrap().frame, Some(next));
                    assert!(test_releases().is_empty());
                    app.presentation.discard();
                    assert_eq!(test_releases(), vec![next]);
                }
            }
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
            Some((Surface { frame: Some(frame), ..app.presentation.surface.unwrap() }, [0, 0, 640, 480]))
        );
        assert_eq!(test_releases(), vec![frame, pending]);
        app.present_native_frame(frame);
        assert!(app.presentation.surface().unwrap().frame.is_none());
        assert_eq!(test_releases(), vec![frame, pending]);
        let (sender, _receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
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
                    drop(app.transition_page(FeatureId::Train));
                }
                1 => {
                    drop(app.on_explore(crate::view::explore::Outcome::CloseDetailRequested));
                }
                2 => app.retire_peer(UiError::transport("peer closed")),
                _ => {
                    let (sender, _receiver) = futures_channel::mpsc::channel(4);
                    drop(app.on_transport(TransportEvent::Connected(Connection::new(sender))));
                }
            }
            if operation < 2 {
                assert_eq!(test_releases(), vec![pending]);
                assert!(app.presentation.surface().is_none_or(|surface| surface.frame.is_none()));
            } else {
                assert!(test_releases().is_empty());
                assert_eq!(app.presentation.surface().unwrap().frame, Some(pending));
            }
        }
    }

    #[test]
    fn annotation_page_open_and_viewer_copy_have_distinct_source_contracts() {
        let (mut app, frame) = viewer_app();
        let before = app.model.annotation.snapshot.clone();
        let (sender, _receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
        assert!(!app.copy_viewer_to_annotation());
        assert_eq!(app.model.annotation.snapshot, before);
        app.workspace.select(FeatureId::Annotate);
        app.model.set_foreground_feature(FeatureId::Annotate);
        assert!(app.open_annotation());
        assert_eq!(app.model.annotation.snapshot, before);
        drop(app);

        let (mut app, _) = viewer_app();
        let (sender, _receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
        app.presentation.pending = Some(Surface { frame: Some(frame), ..app.presentation.surface.unwrap() });
        record_draw(&app, frame, [0, 0, 640, 480]);
        assert!(app.copy_viewer_to_annotation());
    }

    #[test]
    fn unavailable_and_stale_annotation_sources_leave_the_document_unchanged() {
        for condition in 0..5 {
            let (mut app, frame) = viewer_app();
            let (sender, _receiver) = futures_channel::mpsc::channel(4);
            app.connection = Some(Connection::new(sender));
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
        });
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
        app.presentation.pending = Some(Surface { frame: Some(frame), ..app.presentation.surface.unwrap() });
        let (sender, mut receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
        record_draw(&app, frame, [0, 0, 2560, 1920]);
        assert!(!app.copy_viewer_to_annotation());
        record_draw(&app, frame, [40, 80, 2000, 1200]);
        assert!(app.copy_viewer_to_annotation());
        let crate::transport_connection::OutboundRecord::Intent(intent) =
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
