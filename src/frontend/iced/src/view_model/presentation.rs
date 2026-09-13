use super::*;

#[derive(Debug, Clone, Default)]
pub(super) struct PresentationModel {
    foreground: Option<PresentationSourceKind>,
    sent: Option<PresentationSourceIdentity>,
}

impl PresentationModel {
    pub(super) fn foreground(&self) -> Option<PresentationSourceKind> {
        self.foreground
    }

    pub(super) fn clear_sent(&mut self) {
        self.sent = None;
    }

    fn select(&mut self, foreground: Option<PresentationSourceKind>) {
        if self.foreground != foreground {
            self.foreground = foreground;
            self.clear_sent();
        }
    }

    fn refresh(
        &mut self,
        frame: Option<VisualFrame>,
    ) -> Option<VisualFrame> {
        let Some(frame) = frame else {
            self.clear_sent();
            return None;
        };
        if self.sent.as_ref().is_some_and(|sent| sent != &frame.source) {
            self.clear_sent();
        }
        (self.sent.as_ref() != Some(&frame.source))
            .then_some(frame)
    }

    fn sent(&mut self, frame: VisualFrame) {
        self.sent = Some(frame.source);
    }


}

impl ApplicationModel {
    pub(crate) fn foreground_visual(&self) -> Option<PresentationSourceKind> {
        self.presentation_model.foreground()
    }

    pub(crate) fn selected_detail_source(&self) -> Option<crate::generated::AnnotationOpen> {
        let snapshot = self.explore.snapshot.as_ref()?;
        (snapshot.mode == crate::generated::ExploreMode::Detail
            && snapshot.selectedimage.is_some()
            && self
                .explore
                .requested_selection
                .is_none_or(|image| snapshot.selectedimage == Some(image))
            && Self::valid_visual_source(&snapshot.frame).is_some())
        .then(|| crate::generated::AnnotationOpen {
            source: snapshot.frame.clone(),
            originalcontent: snapshot.detail.showoriginaldimensions,
        })
    }

    pub fn request_upscale(&mut self, request: crate::generated::UpscaleRequest) {
        self.explore.requested_upscale = Some(request);
        if self.current_upscale().is_some() {
            self.set_foreground_visual(Some(PresentationSourceKind::Upscale));
        } else {
            self.set_foreground_visual(Some(PresentationSourceKind::Explore));
        }
    }

    pub fn displayed_upscale_kernel(&self) -> Option<crate::generated::UpscaleKernel> {
        let explore = self.explore.snapshot.as_ref()?;
        let upscale = self.upscale_snapshot.as_ref()?;
        let (surface, _) = crate::presentation_surface::drawn_detail()?;
        let drawn = surface.frame?;
        upscale
            .methods
            .iter()
            .zip(crate::generated::UPSCALE_KERNEL_VALUES.iter().copied())
            .find_map(|(method, kernel)| {
                (method.available
                    && method.completed.as_ref().is_some_and(|request| {
                        request.source == explore.frame
                            && request.document == explore.document
                            && request.kernel == kernel
                    })
                    && drawn.matches_content(&method.frame))
                .then_some(kernel)
            })
    }

    pub fn current_upscale(&self) -> Option<&crate::generated::UpscaleSnapshot> {
        let explore = self.explore.snapshot.as_ref()?;
        let request = self.explore.requested_upscale.as_ref()?;
        let upscale = self.upscale_snapshot.as_ref()?;
        (explore.mode == crate::generated::ExploreMode::Detail
            && explore.selectedimage.is_some()
            && self
                .explore
                .requested_selection
                .is_none_or(|image| explore.selectedimage == Some(image))
            && upscale.ready
            && upscale.kernel == request.kernel
            && upscale.input == request.source
            && explore.frame == request.source
            && explore.document == request.document
            && upscale.methods.iter().any(|method| {
                method.available
                    && method.completed.as_ref() == Some(request)
                    && method.frame == upscale.frame
            }))
        .then_some(upscale)
    }

    pub fn viewed_explore_frame(&self) -> Option<VisualFrame> {
        let explore = self.explore.snapshot.as_ref()?;
        if explore.mode != crate::generated::ExploreMode::Detail
            || explore.selectedimage.is_none()
            || self
                .explore
                .requested_selection
                .is_some_and(|image| explore.selectedimage != Some(image))
        {
            return None;
        }
        let expected = match self.presentation_model.foreground()? {
            PresentationSourceKind::Explore => &explore.frame,
            PresentationSourceKind::Upscale => &self.current_upscale()?.frame,
            _ => return None,
        };
        Self::valid_visual_source(expected).map(|_| expected.clone())
    }
    pub fn valid_visual_source(frame: &VisualFrame) -> Option<PresentationSourceIdentity> {
        (frame.source.kind != PresentationSourceKind::None
            && frame.source.instance != 0
            && frame.extent.width != 0
            && frame.extent.height != 0
            && frame.revision != 0)
            .then(|| frame.source.clone())
    }

    pub(super) fn frame_for(&self, kind: PresentationSourceKind) -> Option<&VisualFrame> {
        if kind == PresentationSourceKind::Upscale && self.current_upscale().is_none() {
            return None;
        }
        if kind == PresentationSourceKind::Predict
            && self.predict_snapshot.as_ref().is_none_or(|snapshot| {
                snapshot.operation.active
                    || snapshot.operation.terminal.outcome != ComputeOperationOutcome::Succeeded
            })
        {
            return None;
        }
        self.frame_observation_for(kind)
            .map(|observation| observation.frame)
            .filter(|frame| Self::valid_visual_source(frame).is_some())
    }

    pub(crate) fn frame_observation_for(
        &self,
        kind: PresentationSourceKind,
    ) -> Option<crate::generated::ApplicationVisualObservation<'_>> {
        crate::generated::ApplicationVisualSnapshots {
            explore: self.explore.snapshot.as_ref(),
            annotation: self.annotation.snapshot.as_ref(),
            live: self.live_snapshot.as_ref(),
            upscale: self.upscale_snapshot.as_ref(),
            predict: self.predict_snapshot.as_ref(),
        }
        .observe(kind)
    }

    pub fn source_for(&self, kind: PresentationSourceKind) -> Option<PresentationSourceIdentity> {
        self.frame_for(kind).map(|frame| frame.source.clone())
    }

    pub(super) fn page_visual_source(page: FeatureId) -> Option<PresentationSourceKind> {
        match page {
            FeatureId::Predict => Some(PresentationSourceKind::Predict),
            FeatureId::Live => Some(PresentationSourceKind::Live),
            FeatureId::Explore => Some(PresentationSourceKind::Explore),
            FeatureId::Annotate => Some(PresentationSourceKind::Annotation),
            FeatureId::Train | FeatureId::Validate | FeatureId::Export => None,
        }
    }

    pub(crate) fn set_foreground_visual(&mut self, foreground: Option<PresentationSourceKind>) {
        self.presentation_model.select(foreground);
    }

    pub fn set_foreground_feature(&mut self, feature: FeatureId) {
        if feature == FeatureId::Explore
            && matches!(
                self.presentation_model.foreground(),
                Some(PresentationSourceKind::Explore | PresentationSourceKind::Upscale)
            )
        {
            return;
        }
        self.abandon_viewer();
        self.set_foreground_visual(Self::page_visual_source(feature));
    }

    pub(crate) fn abandon_viewer(&mut self) {
        self.explore.requested_upscale = None;
        self.explore.sent_upscale = None;
        self.explore.requested_selection = None;
        self.explore.desired_selection = None;
    }

    pub fn presentation_refresh(&mut self) -> Option<VisualFrame> {
        if self.connection != ConnectionState::Connected
            || self.has_pending(ApplicationIntentEndpoint::PresentationSelect)
        {
            return None;
        }
        let foreground = self.presentation_model.foreground();
        let frame = foreground.and_then(|kind| self.frame_for(kind)).cloned();
        self.presentation_model.refresh(frame)
    }

    pub fn presentation_recovery_refresh(&self) -> Option<VisualFrame> {
        let kind = self.presentation_model.foreground()?;
        self.frame_for(kind).cloned()
    }

    pub fn record_presentation_sent(&mut self, frame: VisualFrame) {
        if self
            .presentation_model
            .foreground()
            .and_then(|kind| self.frame_for(kind))
            == Some(&frame)
        {
            self.presentation_model.sent(frame);
        }
    }
}

impl crate::generated::PresentationApplicationProjection<UiError> for ApplicationModel {
    fn project_presentation_snapshot(
        &mut self,
        value: PresentationState,
    ) -> Result<(), UiError> {
        merge_presentation_snapshot(&mut self.presentation, value).map(|_| ())
    }

    fn project_presentation_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::PresentationPresentationFailed(value) => {
                match merge_presentation_snapshot(&mut self.presentation, value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Stale) => {}
                    Ok(Observation::Installed | Observation::Current) => {
                        self.error = Some(UiError::presentation(value.detail));
                    }
                }
            }
            _ => unreachable!("generated Presentation dispatch supplied another system event"),
        }
    }

    fn project_presentation_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let ApplicationReply::PresentationSelect(snapshot) = reply else {
            unreachable!("generated Presentation dispatch supplied another system reply");
        };
        if let Err(error) = merge_presentation_snapshot(&mut self.presentation, snapshot) {
            self.error = Some(error);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::*;

    fn select_upscale_source(
        model: &mut ApplicationModel,
        kernel: crate::generated::UpscaleKernel,
    ) -> VisualFrame {
        model.set_foreground_feature(FeatureId::Explore);
        let source = visual_frame(PresentationSourceKind::Explore, 7);
        let explore = model.explore.snapshot.as_mut().unwrap();
        explore.ready = true;
        explore.mode = crate::generated::ExploreMode::Detail;
        explore.selectedimage = Some(3);
        explore.frame = source.clone();
        model.explore.requested_upscale = Some(crate::generated::UpscaleRequest {
            source: source.clone(),
            kernel,
            document: explore.document.clone(),
        });
        source
    }

    #[test]
    fn completed_method_requires_matching_draw_and_same_method_click_reselects_it() {
        let mut model = bootstrapped();
        let kernel = crate::generated::UpscaleKernel::ShiftLut;
        let source = select_upscale_source(&mut model, kernel);
        let request = model.explore.requested_upscale.clone().unwrap();
        let upscale = model.upscale_snapshot.as_mut().unwrap();
        upscale.ready = true;
        upscale.kernel = kernel;
        upscale.input = source;
        upscale.frame = visual_frame(PresentationSourceKind::Upscale, 13);
        upscale.methods[1].available = true;
        upscale.methods[1].completed = Some(request.clone());
        upscale.methods[1].frame = upscale.frame.clone();
        let frame = upscale.frame.clone();
        crate::presentation_surface::clear_drawn_detail();
        model.explore.sent_upscale = Some(request.clone());
        model.request_upscale(request.clone());
        assert_eq!(
            model.presentation_model.foreground(),
            Some(PresentationSourceKind::Upscale)
        );
        assert_eq!(model.explore.sent_upscale, Some(request.clone()));
        assert_eq!(model.displayed_upscale_kernel(), None);
        let drawn = crate::presentation_surface::FrameReady {
            source_high: 0,
            source_low: 0,
            direct_sampling: false,
            high: 1,
            low: 2,
            layer: 0,
            slot: 0,
            content_session: crate::generated::presentation_source_session(frame.source.kind),
            content_sequence: frame.revision,
            presentation_revision: 31,
            content_width: frame.extent.width,
            content_height: frame.extent.height,
        };
        crate::presentation_surface::record_drawn_detail(
            physical_surface(drawn),
            [0, 0, frame.extent.width, frame.extent.height],
        );
        assert_eq!(model.displayed_upscale_kernel(), Some(kernel));
        let mut other = request.clone();
        other.kernel = crate::generated::UpscaleKernel::Default;
        model.request_upscale(other);
        assert_eq!(model.displayed_upscale_kernel(), Some(kernel));
        model.request_upscale(request);
        assert_eq!(model.displayed_upscale_kernel(), Some(kernel));
        model.upscale_snapshot.as_mut().unwrap().busy = true;
        assert_eq!(model.displayed_upscale_kernel(), Some(kernel));
        crate::presentation_surface::clear_drawn_detail();
    }

    #[test]
    fn upscale_source_matching_includes_crop_and_legacy_clean_revision() {
        let mut model = bootstrapped();
        let source = select_upscale_source(&mut model, crate::generated::UpscaleKernel::Default);
        let mut other = source.clone();
        other.content.x += 1;
        assert_ne!(
            crate::generated::visual_clean_content_identity(&source),
            crate::generated::visual_clean_content_identity(&other)
        );
        let mut legacy = source.clone();
        legacy.cleanrevision = 0;
        other = legacy.clone();
        other.revision += 1;
        assert_ne!(
            crate::generated::visual_clean_content_identity(&legacy),
            crate::generated::visual_clean_content_identity(&other)
        );
        other = source.clone();
        other.cleanrevision = 42;
        let mut semantic = other.clone();
        semantic.revision += 1;
        assert_eq!(
            crate::generated::visual_clean_content_identity(&other),
            crate::generated::visual_clean_content_identity(&semantic)
        );
    }

    #[test]
    fn dataset_replacement_retires_upscale_even_when_frame_numbers_repeat() {
        let mut model = bootstrapped();
        select_upscale_source(&mut model, crate::generated::UpscaleKernel::Default);
        let mut replacement = model.explore.snapshot.clone().unwrap();
        replacement.revision += 1;
        replacement.dataset.identity += 1;
        model.install_explore_snapshot(replacement, true).unwrap();
        assert!(model.explore.requested_upscale.is_none());
        assert!(model.current_upscale().is_none());
    }

    #[test]
    fn upscale_failed_admission_can_be_explicitly_retried_without_retrying_automatically() {
        let mut model = bootstrapped();
        select_upscale_source(&mut model, crate::generated::UpscaleKernel::Default);
        let request = model.explore.requested_upscale.clone().unwrap();
        model.explore.sent_upscale = Some(request.clone());
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::UpscaleStart)
            .unwrap();
        let mut failed = model.upscale_snapshot.clone().unwrap();
        failed.revision += 1;
        failed.ready = false;
        failed.busy = false;
        failed.input = request.source.clone();
        model.reduce_event(ApplicationEvent::UpscaleUpscaleFailed(
            crate::generated::UpscaleFailed {
                request: Some(request.clone()),
                snapshot: failed,
                detail: "model unavailable".into(),
                kind: crate::generated::UpscaleFailureKind::Unavailable,
            },
        ));
        assert!(model.explore.requested_upscale.is_none());
        assert!(model.explore.sent_upscale.is_none());
        // The admitted reply still settles its correlation; retry is a later user intent.
        let reply = model.upscale_snapshot.clone().unwrap();
        model.reduce_reply(correlation, Ok(ApplicationReply::UpscaleStart(reply)));
        model.request_upscale(request.clone());
        assert_eq!(model.explore.requested_upscale, Some(request));
        assert!(model.explore.sent_upscale.is_none());
    }

    #[test]
    fn presentation_failure_suppresses_same_frame_and_releases_newer_frame() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Live);
        let mut live = model.live_snapshot.clone().unwrap();
        live.revision += 1;
        live.running = true;
        live.completedframes += 1;
        live.frame = visual_frame(PresentationSourceKind::Live, 1);
        let first = model
            .reduce_event(ApplicationEvent::LiveLiveFrameCompleted(
                crate::generated::LiveFrameCompleted {
                    snapshot: live.clone(),
                },
            ))
            .unwrap();
        let selection = model
            .begin_intent(ApplicationIntentEndpoint::PresentationSelect)
            .unwrap();
        model.record_presentation_sent(first);
        assert!(
            model
                .reduce_reply(
                    selection,
                    Err(ApplicationError {
                        category: ApplicationErrorCategory::Failed,
                        detail: "copy failed".into(),
                    }),
                )
                .is_none()
        );
        live.revision += 1;
        live.completedframes += 1;
        live.frame = visual_frame(PresentationSourceKind::Live, 2);
        assert_eq!(
            model.reduce_event(ApplicationEvent::LiveLiveFrameCompleted(
                crate::generated::LiveFrameCompleted { snapshot: live },
            )),
            Some(visual_frame(PresentationSourceKind::Live, 2))
        );
    }

    #[test]
    fn reconnect_bootstrap_rederives_foreground_presentation() {
        let snapshots = || {
            crate::generated::application_snapshot_defaults()
                .unwrap()
                .into_iter()
                .map(|fact| match fact.value {
                    ApplicationSnapshot::Settings(mut snapshot) => {
                        snapshot.settingsstate.currentview = FeatureId::Explore;
                        ApplicationSnapshot::Settings(snapshot)
                    }
                    ApplicationSnapshot::Explore(mut snapshot) => {
                        snapshot.revision = 7;
                        snapshot.ready = true;
                        snapshot.frame = visual_frame(PresentationSourceKind::Explore, 4);
                        ApplicationSnapshot::Explore(snapshot)
                    }
                    snapshot => snapshot,
                })
                .collect()
        };
        let mut model = ApplicationModel::default();
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots())
            .unwrap();
        model.set_foreground_feature(FeatureId::Explore);
        let frame = model.presentation_refresh().expect("Explore foreground");
        model.record_presentation_sent(frame);
        assert!(model.presentation_refresh().is_none());
        model.peer_disconnected(UiError::transport("reconnect"));
        model
            .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots())
            .unwrap();
        model.set_foreground_feature(FeatureId::Explore);
        assert_eq!(
            model.presentation_refresh(),
            Some(visual_frame(PresentationSourceKind::Explore, 4))
        );
    }

    #[test]
    fn logical_observations_never_authorize_or_replace_graphics_metadata() {
        use crate::presentation_surface::metadata;
        for cached_revision in [1, 3, 19] {
            crate::presentation_surface::reset_test_releases();
            let (mut model, frame) = explore_presentation();
            let immutable = metadata::product(frame).unwrap();
            let current = model.explore.snapshot.as_mut().unwrap();
            current.revision += 10;
            current.frame.revision = cached_revision;
            current.dataset.identity += 1;
            assert_eq!(metadata::product(frame), Some(immutable.clone()));
            model.presentation = None;
            model.peer_disconnected(UiError::transport("graphics remains bound"));
            assert_eq!(metadata::product(frame), Some(immutable));
        }
    }

    #[test]
    fn predict_observation_retains_native_metadata_revision() {
        let mut model = bootstrapped();
        let mut snapshot = model.predict_snapshot.clone().unwrap();
        snapshot.revision = 10;
        snapshot.operation.generationfrontier = 1;
        snapshot.operation.terminal.generation = 1;
        snapshot.operation.terminal.outcome = ComputeOperationOutcome::Succeeded;
        snapshot.frame = visual_frame(PresentationSourceKind::Predict, 4);
        model.install_predict_snapshot(snapshot.clone()).unwrap();
        snapshot.revision = 12;
        model.install_predict_snapshot(snapshot.clone()).unwrap();
        let observation = model
            .frame_observation_for(PresentationSourceKind::Predict)
            .unwrap();
        assert_eq!(observation.frame, &snapshot.frame);
        assert_eq!(observation.snapshotrevision, 12);
        assert_eq!(
            model.install_predict_snapshot(snapshot.clone()).unwrap(),
            super::super::reduction::Observation::Current
        );
        let mut conflict = snapshot.clone();
        conflict.operation.progress.completed += 1;
        assert!(model.install_predict_snapshot(conflict).is_err());
        let mut conflict = snapshot.clone();
        conflict.frame.revision += 1;
        assert!(model.install_predict_snapshot(conflict).is_err());
        assert_eq!(model.predict_snapshot.as_ref(), Some(&snapshot));
        snapshot.revision = 11;
        snapshot.frame.revision = 5;
        assert_eq!(
            model.install_predict_snapshot(snapshot).unwrap(),
            super::super::reduction::Observation::Stale
        );
        assert_eq!(
            model
                .frame_observation_for(PresentationSourceKind::Predict)
                .unwrap()
                .snapshotrevision,
            12
        );
    }

    #[test]
    fn reconnect_requests_the_foreground_product_even_when_native_completion_matches() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Explore);
        let frame = visual_frame(PresentationSourceKind::Explore, 4);
        {
            let explore = model.explore.snapshot.as_mut().unwrap();
            explore.ready = true;
            explore.revision = 7;
            explore.frame = frame.clone();
        }
        model.connection = ConnectionState::Connected;
        model.record_presentation_sent(frame.clone());
        assert!(model.presentation_refresh().is_none());
        assert_eq!(model.presentation_recovery_refresh(), Some(frame));
    }

    #[test]
    fn selected_product_refresh_coalesces_pending_frames() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Explore);
        let mut explore = model.explore.snapshot.clone().unwrap();
        explore.revision += 1;
        explore.ready = true;
        explore.frame = visual_frame(PresentationSourceKind::Explore, 1);
        let first = model
            .reduce_event(ApplicationEvent::ExploreExploreChanged(
                crate::generated::ExploreChanged {
                    snapshot: explore.clone(),
                },
            ))
            .unwrap();
        let selection = model
            .begin_intent(ApplicationIntentEndpoint::PresentationSelect)
            .unwrap();
        model.record_presentation_sent(first.clone());
        explore.revision += 1;
        explore.frame.revision = 2;
        assert!(
            model
                .reduce_event(ApplicationEvent::ExploreExploreChanged(
                    crate::generated::ExploreChanged {
                        snapshot: explore.clone(),
                    },
                ))
                .is_none()
        );
        let mut admitted = model.presentation.clone().unwrap();
        admitted.revision += 1;
        admitted.selected = first.source;
        assert_eq!(
            model.reduce_reply(
                selection,
                Ok(ApplicationReply::PresentationSelect(admitted)),
            ),
            None
        );
        assert_eq!(
            model.explore.snapshot.as_ref().unwrap().frame,
            explore.frame
        );
    }

    #[test]
    fn explore_filter_detail_selection_navigation_and_viewport_refresh_foreground() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Explore);
        let mut explore = model.explore.snapshot.clone().unwrap();
        explore.ready = true;
        explore.frame = visual_frame(PresentationSourceKind::Explore, 0);
        for revision in 1..=5 {
            explore.revision += 1;
            explore.frame.revision = revision;
            match revision {
                1 => explore.filter.minimuminstances = 2,
                2 => {
                    explore.mode = crate::generated::ExploreMode::Detail;
                    explore.selectedimage = Some(1);
                }
                3 => explore.selectedimage = Some(2),
                4 => explore.selectedimage = Some(1),
                5 => explore.viewport.firstrow = 3,
                _ => unreachable!(),
            }
            let refresh = model.reduce_event(ApplicationEvent::ExploreExploreChanged(
                crate::generated::ExploreChanged {
                    snapshot: explore.clone(),
                },
            ));
            assert_eq!(refresh.is_some(), revision == 1);
            assert_eq!(
                model.explore.snapshot.as_ref().unwrap().frame.revision,
                revision
            );
            if let Some(frame) = refresh {
                model.record_presentation_sent(frame);
            }
        }
    }

    #[test]
    fn annotation_edits_and_live_frames_refresh_selected_sources_without_duplicates() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Annotate);
        let mut annotation = model.annotation.snapshot.clone().unwrap();
        annotation.ready = true;
        annotation.frame = visual_frame(PresentationSourceKind::Annotation, 0);
        for revision in 1..=3 {
            annotation.revision += 1;
            annotation.ui.documentrevision += 1;
            annotation.frame.revision = revision;
            let refresh = model.reduce_event(ApplicationEvent::AnnotationAnnotationChanged(
                crate::generated::AnnotationChanged {
                    snapshot: annotation.clone(),
                },
            ));
            assert_eq!(refresh.is_some(), revision == 1);
            assert_eq!(
                model.annotation.snapshot.as_ref().unwrap().frame.revision,
                revision
            );
            if let Some(frame) = refresh {
                model.record_presentation_sent(frame);
            }
        }
        assert!(
            model
                .reduce_event(ApplicationEvent::AnnotationAnnotationChanged(
                    crate::generated::AnnotationChanged {
                        snapshot: annotation,
                    },
                ))
                .is_none()
        );

        for frame_first in [true, false] {
            let mut full = model.annotation.snapshot.clone().unwrap();
            full.revision += 1;
            full.uirevision = full.revision;
            full.ui.documentrevision += 1;
            let mut pixels = full.frame.clone();
            pixels.revision += 1;
            let small = crate::generated::AnnotationFrameChanged {
                snapshot: crate::generated::AnnotationFrameState {
                    rendered: full.rendered.clone(),
                    revision: full.revision + 1,
                    uirevision: full.uirevision,
                    frame: pixels.clone(),
                },
            };
            if frame_first {
                assert!(
                    model
                        .reduce_event(ApplicationEvent::AnnotationAnnotationFrameChanged(
                            small.clone()
                        ))
                        .is_none()
                );
            }
            let observed = model.reduce_event(ApplicationEvent::AnnotationAnnotationChanged(
                crate::generated::AnnotationChanged {
                    snapshot: full.clone(),
                },
            ));
            assert!(observed.is_none());
            if !frame_first {
                assert!(
                    model
                        .reduce_event(ApplicationEvent::AnnotationAnnotationFrameChanged(
                            small.clone()
                        ))
                        .is_none()
                );
            }
            model.record_presentation_sent(pixels.clone());
            assert_eq!(model.annotation.snapshot.as_ref().unwrap().frame, pixels);
            assert!(
                model
                    .reduce_event(ApplicationEvent::AnnotationAnnotationFrameChanged(small))
                    .is_none()
            );
            assert!(
                model
                    .reduce_event(ApplicationEvent::AnnotationAnnotationChanged(
                        crate::generated::AnnotationChanged { snapshot: full },
                    ))
                    .is_none()
            );
        }

        let installed = model.annotation.snapshot.clone().unwrap();
        let mut malformed = installed.clone();
        malformed.uirevision = malformed.revision + 1;
        assert!(model.annotation.install_snapshot(malformed).is_err());
        assert_eq!(model.annotation.snapshot.as_ref(), Some(&installed));
        for ui_revision in [installed.uirevision - 1, installed.uirevision + 1] {
            model.error = None;
            model.reduce_event(ApplicationEvent::AnnotationAnnotationFrameChanged(
                crate::generated::AnnotationFrameChanged {
                    snapshot: crate::generated::AnnotationFrameState {
                        rendered: installed.rendered.clone(),
                        revision: installed.revision,
                        uirevision: ui_revision,
                        frame: installed.frame.clone(),
                    },
                },
            ));
            assert!(model.error.is_some());
            assert_eq!(model.annotation.snapshot.as_ref(), Some(&installed));
        }
        model.error = None;
        let future = crate::generated::AnnotationFrameState {
            rendered: installed.rendered.clone(),
            revision: installed.revision + 3,
            uirevision: installed.revision + 2,
            frame: visual_frame(
                PresentationSourceKind::Annotation,
                installed.frame.revision + 1,
            ),
        };
        model.reduce_event(ApplicationEvent::AnnotationAnnotationFrameChanged(
            crate::generated::AnnotationFrameChanged {
                snapshot: future.clone(),
            },
        ));
        let mut intermediate = installed.clone();
        intermediate.revision += 1;
        intermediate.uirevision = intermediate.revision;
        assert!(
            model
                .annotation
                .install_snapshot(intermediate.clone())
                .is_ok()
        );
        let mut malformed = intermediate.clone();
        malformed.uirevision = future.uirevision;
        assert!(model.annotation.install_snapshot(malformed).is_err());
        let mut matching = intermediate.clone();
        matching.revision = future.uirevision;
        matching.uirevision = future.uirevision;
        assert!(model.annotation.install_snapshot(matching).is_ok());
        assert_eq!(
            model.annotation.snapshot.as_ref().unwrap().frame,
            future.frame
        );
        assert_eq!(
            model.annotation.snapshot.as_ref().unwrap().revision,
            future.revision
        );
        assert!(model.annotation.install_snapshot(intermediate).is_ok());
        assert_eq!(
            model.annotation.snapshot.as_ref().unwrap().revision,
            future.revision
        );
        assert!(model.error.is_none());

        model.set_foreground_feature(FeatureId::Live);
        let mut live = model.live_snapshot.clone().unwrap();
        live.running = true;
        live.frame = visual_frame(PresentationSourceKind::Live, 0);
        for revision in 1..=2 {
            live.revision += 1;
            live.completedframes += 1;
            live.frame.revision = revision;
            let refresh = model.reduce_event(ApplicationEvent::LiveLiveFrameCompleted(
                crate::generated::LiveFrameCompleted {
                    snapshot: live.clone(),
                },
            ));
            assert_eq!(refresh.is_some(), revision == 1);
            assert_eq!(
                model.live_snapshot.as_ref().unwrap().frame.revision,
                revision
            );
            if let Some(frame) = refresh {
                model.record_presentation_sent(frame);
            }
        }
    }

    #[test]
    fn predict_and_upscale_keep_typed_private_source_continuity() {
        let mut model = bootstrapped();
        let mut predict = model.predict_snapshot.clone().unwrap();
        predict.revision += 1;
        predict.operation.generationfrontier = 1;
        predict.operation.terminal.generation = 1;
        predict.operation.terminal.outcome = ComputeOperationOutcome::Succeeded;
        predict.frame = visual_frame(PresentationSourceKind::Predict, 1);
        assert_eq!(
            model.reduce_event(ApplicationEvent::PredictPredictChanged(
                crate::generated::PredictChanged {
                    snapshot: predict.clone(),
                },
            )),
            Some(predict.frame.clone())
        );
        assert!(model.source_for(PresentationSourceKind::Predict).is_some());

        let mut upscale = model.upscale_snapshot.clone().unwrap();
        let source = select_upscale_source(&mut model, upscale.kernel);
        upscale.input = source;
        upscale.revision += 1;
        upscale.ready = true;
        upscale.frame = visual_frame(PresentationSourceKind::Upscale, 1);
        upscale.methods[0].available = true;
        upscale.methods[0].completed = model.explore.requested_upscale.clone();
        upscale.methods[0].frame = upscale.frame.clone();
        assert_eq!(
            model.reduce_event(ApplicationEvent::UpscaleUpscaleChanged(
                crate::generated::UpscaleChanged {
                    snapshot: upscale.clone(),
                },
            )),
            Some(upscale.frame.clone())
        );
        assert!(model.source_for(PresentationSourceKind::Upscale).is_some());
    }

    #[test]
    fn delayed_upscale_cannot_reselect_another_image_kernel_or_page() {
        let mut model = bootstrapped();
        let source = select_upscale_source(&mut model, crate::generated::UpscaleKernel::RealPlksr);
        let mut upscale = model.upscale_snapshot.clone().unwrap();
        upscale.ready = true;
        upscale.input = source.clone();
        upscale.frame = visual_frame(PresentationSourceKind::Upscale, 1);
        upscale.kernel = crate::generated::UpscaleKernel::Default;
        model.upscale_snapshot = Some(upscale.clone());
        assert!(model.current_upscale().is_none());
        upscale.kernel = crate::generated::UpscaleKernel::RealPlksr;
        upscale.methods[2].available = true;
        upscale.methods[2].completed = model.explore.requested_upscale.clone();
        upscale.methods[2].frame = upscale.frame.clone();
        model.upscale_snapshot = Some(upscale.clone());
        assert!(model.current_upscale().is_some());
        model.explore.requested_selection = Some(4);
        assert!(model.current_upscale().is_none());
        model.explore.requested_selection = None;
        model.explore.snapshot.as_mut().unwrap().frame.cleanrevision += 1;
        assert!(model.current_upscale().is_none());
        model.explore.snapshot.as_mut().unwrap().frame = source;
        model.set_foreground_feature(FeatureId::Annotate);
        upscale.revision += 1;
        model.reduce_event(ApplicationEvent::UpscaleUpscaleChanged(
            crate::generated::UpscaleChanged { snapshot: upscale },
        ));
        assert_eq!(
            model.presentation_model.foreground(),
            Some(PresentationSourceKind::Annotation)
        );
        assert!(model.current_upscale().is_none());
    }

    #[test]
    fn every_upscale_mode_encodes_the_complete_selected_frame() {
        use crate::application_codec::IntoApplicationValue;

        let source = visual_frame(PresentationSourceKind::Explore, 19);
        let document = bootstrapped().explore.snapshot.unwrap().document;
        for kernel in crate::generated::UPSCALE_KERNEL_VALUES.iter().copied() {
            let encoded = crate::generated::encode_upscale_Start(
                41,
                crate::generated::UpscaleRequest {
                    source: source.clone(),
                    kernel,
                    document: document.clone(),
                },
            );
            assert_eq!(encoded.endpoint, ApplicationIntentEndpoint::UpscaleStart);
            assert_eq!(encoded.record.correlation, 41);
            assert_eq!(encoded.record.fields.len(), 3);
            assert_eq!(
                encoded.record.fields[2].value,
                document.clone().into_application_value()
            );
            assert_eq!(
                encoded.record.fields[0].value,
                source.clone().into_application_value()
            );
            assert_eq!(
                encoded.record.fields[1].value,
                kernel.into_application_value()
            );
        }
    }

    #[test]
    fn predict_generation_clears_old_frame_and_ignores_stale_failure() {
        let mut model = bootstrapped();
        let mut succeeded = model.predict_snapshot.clone().unwrap();
        succeeded.revision += 1;
        succeeded.operation.generationfrontier = 1;
        succeeded.operation.terminal.generation = 1;
        succeeded.operation.terminal.outcome = ComputeOperationOutcome::Succeeded;
        succeeded.frame = visual_frame(PresentationSourceKind::Predict, 1);
        model.reduce_event(ApplicationEvent::PredictPredictChanged(
            crate::generated::PredictChanged {
                snapshot: succeeded,
            },
        ));
        let mut running = model.predict_snapshot.clone().unwrap();
        running.revision += 1;
        running.operation.generationfrontier = 2;
        running.operation.active = true;
        running.operation.terminal.generation = 2;
        running.operation.terminal.outcome = ComputeOperationOutcome::Running;
        model.reduce_event(ApplicationEvent::PredictPredictChanged(
            crate::generated::PredictChanged {
                snapshot: running.clone(),
            },
        ));
        assert!(model.source_for(PresentationSourceKind::Predict).is_none());
        let mut newer = running.clone();
        newer.revision += 2;
        newer.operation.generationfrontier = 3;
        newer.operation.active = false;
        newer.operation.terminal.generation = 3;
        newer.operation.terminal.outcome = ComputeOperationOutcome::Succeeded;
        newer.frame = visual_frame(PresentationSourceKind::Predict, 2);
        model.reduce_event(ApplicationEvent::PredictPredictChanged(
            crate::generated::PredictChanged { snapshot: newer },
        ));
        running.operation.active = false;
        running.revision += 1;
        running.operation.terminal.outcome = ComputeOperationOutcome::Failed;
        model.error = None;
        model.reduce_event(ApplicationEvent::PredictPredictFailed(
            crate::generated::PredictFailed {
                snapshot: running,
                detail: "stale failure".into(),
            },
        ));
        assert_eq!(
            model
                .predict_snapshot
                .as_ref()
                .unwrap()
                .operation
                .generationfrontier,
            3
        );
        assert!(model.error.is_none());
    }
}
