use super::*;

impl App {
    pub(super) fn on_window(&mut self, event: iced::window::Event) -> Task<Message> {
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

    pub(super) fn transition_page(&mut self, feature: FeatureId) -> Task<Message> {
        self.retire_surface_frame();
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

    pub(super) fn present_native_frame(&mut self, frame: FrameReady) {
        self.integration.observe_native_frame(frame, self.surface);
        crate::presentation_surface::invalidate_drawn_slot(frame);
        if self.presentation_failed
            && self
                .model
                .presentation
                .as_ref()
                .is_none_or(|snapshot| !matches_completed(frame, snapshot))
        {
            if self
                .surface
                .and_then(|surface| surface.frame)
                .is_some_and(|current| {
                    crate::presentation_surface::same_mailbox_slot(current, frame)
                        && current != frame
                })
            {
                self.retire_surface_frame();
            }
            crate::presentation_surface::release(frame);
            return;
        }
        let Some(surface) = self.surface.as_mut() else {
            crate::presentation_surface::release(frame);
            return;
        };
        if surface.high != frame.high
            || surface.low != frame.low
            || frame.content_width > surface.width
            || frame.content_height > surface.height
        {
            crate::presentation_surface::release(frame);
            return;
        }
        if let Some(previous) = surface.frame
            && previous.presentation_revision >= frame.presentation_revision
        {
            if previous != frame {
                crate::presentation_surface::release(frame);
            }
            return;
        }
        if let Some(previous) = surface.frame
            && previous != frame
        {
            // Batched UI updates may replace a frame before its first draw.
            // Capture owns an in-flight borrow until GPU completion; otherwise
            // this returns an unused sample or deduplicates its settled release.
            crate::presentation_surface::retire_publication(previous);
        }
        if !crate::presentation_surface::accept_publication(frame) {
            return;
        }
        surface.frame = Some(frame);
        if let Some(connection) = self.connection.as_mut() {
            let result = connection.send_renderer_observation(RendererObservation::Presented {
                sample_revision: frame.presentation_revision,
            });
            self.retire_if_closed(result);
        }
        self.reconcile_surface_frame();
    }

    pub(super) fn retire_surface_frame(&mut self) {
        if let Some(frame) = self
            .surface
            .as_mut()
            .and_then(|surface| surface.frame.take())
        {
            crate::presentation_surface::retire_publication(frame);
        }
        crate::presentation_surface::clear_drawn_detail();
    }

    pub(super) fn discard_surface(&mut self) {
        self.retire_surface_frame();
        self.surface = None;
    }

    pub(super) fn reconcile_surface_frame(&mut self) {
        if crate::presentation_surface::drawn_detail().is_some_and(|(frame, _)| {
            self.model
                .viewed_explore_frame()
                .is_none_or(|source| frame.content_sequence != source.revision)
                || self.model.presentation.as_ref().is_none_or(|snapshot| {
                    snapshot.presentationrevision != frame.presentation_revision
                })
        }) {
            crate::presentation_surface::clear_drawn_detail();
        }
        let Some(frame) = self.surface.and_then(|surface| surface.frame) else {
            return;
        };
        if matches!(
            self.workspace.active(),
            FeatureId::Train | FeatureId::Validate | FeatureId::Export
        ) {
            self.retire_surface_frame();
            return;
        }
        let Some(snapshot) = self.model.presentation.as_ref() else {
            return;
        };
        let completed = matches_completed(frame, snapshot);
        if completed {
            crate::presentation_surface::gallery::confirm(
                frame,
                &snapshot.completed,
                self.model.explore.snapshot.as_ref(),
            );
        }
        if self.presentation_failed && !completed {
            let previous = crate::presentation_surface::drawn_detail().filter(|(drawn, _)| {
                matches_completed(*drawn, snapshot)
                    && drawn.high == frame.high
                    && drawn.low == frame.low
            });
            self.retire_surface_frame();
            if let Some((drawn, crop)) = previous {
                // This receipt authorizes Copy, never another external GPU read.
                crate::presentation_surface::record_drawn_detail(drawn, crop);
            }
            return;
        }
        // A future physical publication may be awaiting its control snapshot.
        // Neither that ordering nor a later domain snapshot alone abandons it.
        if frame.presentation_revision > snapshot.presentationrevision {
            return;
        }
        let obsolete = match self.model.completed_presentation_is_obsolete() {
            Ok(obsolete) => obsolete,
            Err(error) => {
                self.retire_peer(error);
                return;
            }
        };
        if obsolete
            || (self.workspace.active() == FeatureId::Explore && !completed)
        {
            self.retire_surface_frame();
        }
    }

    pub(super) fn sync_surface(&mut self) {
        let Some(snapshot) = self.model.presentation.as_ref() else {
            self.discard_surface();
            return;
        };
        match surface_from_snapshot(snapshot) {
            Ok(None) => {
                if self.config.integration {
                    crate::integration_control::report_surface_sync(self.surface, None);
                }
                self.discard_surface();
                crate::presentation_surface::retire_imports();
            }
            Ok(mut surface) => {
                if let Some(surface) = surface.as_mut() {
                    surface.integration = self.config.integration;
                }
                if self.config.integration {
                    crate::integration_control::report_surface_sync(self.surface, surface);
                }
                if self.surface.map(|surface| surface.generation)
                    != surface.map(|surface| surface.generation)
                {
                    if let Some(previous) = self.surface {
                        crate::presentation_surface::trace_surface("capability_replaced", previous);
                    }
                    if let Some(updated) = surface {
                        crate::presentation_surface::trace_surface("capability_selected", updated);
                    }
                    self.retire_surface_frame();
                    self.surface = surface;
                } else if let (Some(current), Some(updated)) = (&mut self.surface, surface) {
                    current.timeline_ready = updated.timeline_ready;
                }
            }
            Err(error) => self.model.error = Some(UiError::presentation(error)),
        }
        self.reconcile_surface_frame();
    }
}

fn matches_completed(frame: FrameReady, snapshot: &PresentationSnapshot) -> bool {
    frame.presentation_revision == snapshot.presentationrevision
        && frame.content_sequence == snapshot.completed.revision
        && frame.content_width == snapshot.completed.extent.width
        && frame.content_height == snapshot.completed.extent.height
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
        explore.mode = ExploreMode::Detail;
        explore.selectedimage = Some(0);
        explore.frame = source.clone();
        let snapshot = app.model.presentation.as_mut().unwrap();
        snapshot.selected = source.source.clone();
        snapshot.completed = source;
        snapshot.presentationrevision = 5;
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
        app.reconcile_surface_frame();
        assert!(test_releases().is_empty());
        app.model.explore.snapshot.as_mut().unwrap().frame =
            app.model.presentation.as_ref().unwrap().completed.clone();
        app.reconcile_surface_frame();
        assert_eq!(app.surface.unwrap().frame, Some(pending));
        record_drawn_detail(pending, [0, 0, 640, 480]);
        crate::presentation_surface::release(pending);
        app.discard_surface();
        assert_eq!(test_releases(), vec![pending]);
        assert!(crate::presentation_surface::drawn_detail().is_none());
    }

    #[test]
    fn invalidated_late_frame_and_surface_replacement_release_exactly_once() {
        let (mut app, frame) = viewer_app();
        app.model.explore.requested_selection = Some(9);
        app.present_native_frame(frame);
        assert!(app.surface.unwrap().frame.is_none());
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
        assert_eq!(test_releases(), vec![frame, pending]);
        app.discard_surface();
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
        assert!(app.surface.is_none());
        assert_eq!(test_releases(), vec![frame]);
        app.sync_surface();
        crate::presentation_surface::retire_imports();
        assert_eq!(test_releases(), vec![frame]);
    }

    #[test]
    fn admitted_replacements_reject_late_frames_with_the_old_physical_identity() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        let mut latest = frame;
        for generation in 2..5 {
            let snapshot = app.model.presentation.as_mut().unwrap();
            snapshot.capability.generation = generation;
            snapshot.capability.surfacelow = generation + 10;
            snapshot.capability.condition = PresentationCapabilityCondition::Admitted;
            app.sync_surface();
            assert!(app.surface.unwrap().frame.is_none());
            app.present_native_frame(latest);
            assert!(app.surface.unwrap().frame.is_none());
            latest.low = generation + 10;
            latest.presentation_revision += 1;
        }
        // Only the current identity can acquire a physical publication, even
        // when earlier candidates share the same geometry and logical source.
        app.present_native_frame(latest);
        assert_eq!(app.surface.unwrap().frame, Some(latest));
        app.discard_surface();
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
        app.presentation_failed = true;
        app.reconcile_surface_frame();
        assert_eq!(app.surface.unwrap().frame, Some(frame));
        assert!(test_releases().is_empty());
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            slot: 1,
            ..frame
        };
        app.present_native_frame(pending);
        assert_eq!(app.surface.unwrap().frame, Some(frame));
        assert_eq!(test_releases(), vec![pending]);
    }

    #[test]
    fn failure_after_pending_publication_keeps_only_the_completed_copy_receipt() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        record_drawn_detail(frame, [0, 0, 640, 480]);
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
        assert!(app.surface.unwrap().frame.is_none());
        assert_eq!(
            crate::presentation_surface::drawn_detail(),
            Some((frame, [0, 0, 640, 480]))
        );
        assert_eq!(test_releases(), vec![frame, pending]);
        app.present_native_frame(frame);
        assert!(app.surface.unwrap().frame.is_none());
        assert_eq!(test_releases(), vec![frame, pending]);
        let (sender, _receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
        assert!(app.copy_viewer_to_annotation());
    }

    #[test]
    fn overwritten_mailbox_slot_cannot_restore_an_old_draw_receipt() {
        let (mut app, frame) = viewer_app();
        app.present_native_frame(frame);
        record_drawn_detail(frame, [0, 0, 640, 480]);
        crate::presentation_surface::release(frame);
        let pending = FrameReady {
            presentation_revision: 6,
            content_sequence: 2,
            ..frame
        };
        app.present_native_frame(pending);
        assert!(crate::presentation_surface::drawn_detail().is_none());
        app.presentation_failed = true;
        app.reconcile_surface_frame();
        assert!(app.surface.unwrap().frame.is_none());
        assert_eq!(test_releases(), vec![frame, pending]);
    }

    #[test]
    fn navigation_close_and_peer_retirement_return_withheld_frames() {
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
            assert_eq!(test_releases(), vec![pending]);
            assert!(app.surface.is_none_or(|surface| surface.frame.is_none()));
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
        app.surface.as_mut().unwrap().frame = Some(frame);
        record_drawn_detail(frame, [0, 0, 640, 480]);
        assert!(app.copy_viewer_to_annotation());
    }

    #[test]
    fn unavailable_and_stale_annotation_sources_leave_the_document_unchanged() {
        for condition in 0..5 {
            let (mut app, frame) = viewer_app();
            let (sender, _receiver) = futures_channel::mpsc::channel(4);
            app.connection = Some(Connection::new(sender));
            let before = app.model.annotation.snapshot.clone();
            record_drawn_detail(frame, [0, 0, 640, 480]);
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
        app.surface.as_mut().unwrap().width = frame.content_width;
        app.surface.as_mut().unwrap().height = frame.content_height;
        app.surface.as_mut().unwrap().frame = Some(frame);
        let (sender, mut receiver) = futures_channel::mpsc::channel(4);
        app.connection = Some(Connection::new(sender));
        record_drawn_detail(frame, [0, 0, 2560, 1920]);
        assert!(!app.copy_viewer_to_annotation());
        record_drawn_detail(frame, [40, 80, 2000, 1200]);
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
