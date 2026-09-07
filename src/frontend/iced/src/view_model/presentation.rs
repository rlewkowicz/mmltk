use super::*;

impl ApplicationModel {
    pub(crate) fn completed_presentation_is_obsolete(&self) -> Result<bool, UiError> {
        let Some(presentation) = self.presentation.as_ref() else {
            return Ok(false);
        };
        let completed = &presentation.completed;
        let Some(kind) = self.foreground_visual else {
            return Ok(true);
        };
        if kind == PresentationSourceKind::Explore
            && completed.source.kind == PresentationSourceKind::Upscale
        {
            // The native mailbox may beat the matching Upscale snapshot. Keep
            // that publication while its explicit viewer demand still exists.
            return Ok(self.explore.requested_upscale.is_none()
                || self.upscale_snapshot.as_ref().is_some_and(|snapshot| {
                    snapshot.frame.source == completed.source
                        && snapshot.frame.revision >= completed.revision
                        && self.current_upscale().is_none()
                }));
        }
        if completed.source.kind != kind {
            return Ok(true);
        }
        let Some((current, current_observation)) = self.frame_observation_for(kind) else {
            return Ok(false);
        };
        if completed == current {
            return Ok((kind == PresentationSourceKind::Upscale && self.current_upscale().is_none())
                || (kind == PresentationSourceKind::Explore
                    && self.explore.requested_selection.is_some_and(|image| {
                        self.explore
                            .snapshot
                            .as_ref()
                            .is_none_or(|snapshot| snapshot.selectedimage != Some(image))
                    })));
        }
        if completed.source != current.source {
            return Ok(true);
        }
        match current_observation.cmp(&presentation.completedsourcerevision) {
            std::cmp::Ordering::Less => Ok(false),
            std::cmp::Ordering::Greater => Ok(true),
            std::cmp::Ordering::Equal => Err(UiError::protocol(
                "one source observation described different completed frames",
            )),
        }
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

    pub(super) fn same_clean_source(left: &VisualFrame, right: &VisualFrame) -> bool {
        let clean = |frame: &VisualFrame| {
            if frame.cleanrevision == 0 {
                frame.revision
            } else {
                frame.cleanrevision
            }
        };
        left.source == right.source
            && left.extent == right.extent
            && left.content == right.content
            && clean(left) == clean(right)
    }

    pub fn displayed_upscale_kernel(&self) -> Option<crate::generated::UpscaleKernel> {
        let upscale = self.current_upscale()?;
        let shown = self.viewed_explore_frame()?;
        let (drawn, _) = crate::presentation_surface::drawn_detail()?;
        (shown == upscale.frame
            && drawn.presentation_revision == self.presentation.as_ref()?.presentationrevision
            && drawn.content_sequence == shown.revision
            && drawn.content_width == shown.extent.width
            && drawn.content_height == shown.extent.height)
            .then_some(upscale.kernel)
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
            && !upscale.busy
            && upscale.kernel == request.kernel
            && upscale.input == request.source
            && Self::same_clean_source(&explore.frame, &request.source))
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
        let expected = match self.foreground_visual? {
            PresentationSourceKind::Explore => &explore.frame,
            PresentationSourceKind::Upscale => &self.current_upscale()?.frame,
            _ => return None,
        };
        let completed = &self.presentation.as_ref()?.completed;
        (completed == expected && Self::valid_visual_source(completed).is_some())
            .then(|| completed.clone())
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
        match kind {
            PresentationSourceKind::Explore => self
                .explore
                .snapshot
                .as_ref()
                .map(|snapshot| &snapshot.frame),
            PresentationSourceKind::Annotation => self
                .annotation
                .snapshot
                .as_ref()
                .map(|snapshot| &snapshot.frame),
            PresentationSourceKind::Live => {
                self.live_snapshot.as_ref().map(|snapshot| &snapshot.frame)
            }
            PresentationSourceKind::Upscale => {
                self.current_upscale().map(|snapshot| &snapshot.frame)
            }
            PresentationSourceKind::Predict => self
                .predict_snapshot
                .as_ref()
                .filter(|snapshot| {
                    !snapshot.operation.active
                        && snapshot.operation.terminal.outcome == ComputeOperationOutcome::Succeeded
                })
                .map(|snapshot| &snapshot.frame),
            PresentationSourceKind::None => None,
        }
        .filter(|frame| Self::valid_visual_source(frame).is_some())
    }

    fn frame_observation_for(
        &self,
        kind: PresentationSourceKind,
    ) -> Option<(&VisualFrame, u64)> {
        match kind {
            PresentationSourceKind::Explore => self
                .explore
                .snapshot
                .as_ref()
                .map(|snapshot| (&snapshot.frame, snapshot.revision)),
            PresentationSourceKind::Annotation => self
                .annotation
                .snapshot
                .as_ref()
                .map(|snapshot| (&snapshot.frame, snapshot.revision)),
            PresentationSourceKind::Live => self
                .live_snapshot
                .as_ref()
                .map(|snapshot| (&snapshot.frame, snapshot.revision)),
            PresentationSourceKind::Upscale => self
                .upscale_snapshot
                .as_ref()
                .map(|snapshot| (&snapshot.frame, snapshot.revision)),
            PresentationSourceKind::Predict => self
                .predict_snapshot
                .as_ref()
                .map(|snapshot| (&snapshot.frame, snapshot.revision)),
            PresentationSourceKind::None => None,
        }
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
        if self.foreground_visual != foreground {
            self.foreground_visual = foreground;
            self.sent_presentation_frame = None;
        }
    }

    pub fn set_foreground_feature(&mut self, feature: FeatureId) {
        self.explore.requested_upscale = None;
        self.explore.sent_upscale = None;
        self.explore.requested_selection = None;
        self.explore.desired_selection = None;
        self.set_foreground_visual(Self::page_visual_source(feature));
    }

    pub fn presentation_refresh(&mut self) -> Option<VisualFrame> {
        if self.connection != ConnectionState::Connected
            || self.has_pending(ApplicationIntentEndpoint::PresentationSelect)
        {
            return None;
        }
        let Some(foreground) = self.foreground_visual else {
            self.sent_presentation_frame = None;
            return None;
        };
        let Some(frame) = self.frame_for(foreground).cloned() else {
            self.sent_presentation_frame = None;
            return None;
        };
        if self
            .sent_presentation_frame
            .as_ref()
            .is_some_and(|sent| sent.source != frame.source)
        {
            self.sent_presentation_frame = None;
        }
        let completed = self
            .presentation
            .as_ref()
            .map(|snapshot| &snapshot.completed);
        if let (Some(presentation), Some((_, observation))) = (
            self.presentation.as_ref(),
            self.frame_observation_for(foreground),
        ) && presentation.completed.source == frame.source
            && presentation.completed != frame
            && presentation.completedsourcerevision >= observation
        {
            return None;
        }
        (completed != Some(&frame) && self.sent_presentation_frame.as_ref() != Some(&frame))
            .then_some(frame)
    }

    pub fn presentation_recovery_refresh(&self) -> Option<VisualFrame> {
        self.foreground_visual
            .and_then(|kind| self.frame_for(kind))
            .cloned()
    }

    pub fn record_presentation_sent(&mut self, frame: VisualFrame) {
        if self.foreground_visual.and_then(|kind| self.frame_for(kind)) == Some(&frame) {
            self.sent_presentation_frame = Some(frame);
        }
    }
}

impl crate::generated::PresentationApplicationProjection<UiError> for ApplicationModel {
    fn project_presentation_snapshot(
        &mut self,
        value: PresentationSnapshot,
    ) -> Result<(), UiError> {
        merge_presentation_snapshot(&mut self.presentation, value).map(|_| ())
    }

    fn project_presentation_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::PresentationPresentationCompleted(
                crate::generated::PresentationCompleted { snapshot },
            )
            | ApplicationEvent::PresentationPresentationCapabilityChanged(
                crate::generated::PresentationCapabilityChanged { snapshot },
            ) => {
                if let Err(error) = merge_presentation_snapshot(&mut self.presentation, snapshot) {
                    self.error = Some(error);
                }
            }
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
        let frame = upscale.frame.clone();
        let presentation = model.presentation.as_mut().unwrap();
        presentation.completed = frame.clone();
        presentation.presentationrevision = 31;
        crate::presentation_surface::clear_drawn_detail();
        model.explore.sent_upscale = Some(request.clone());
        model.request_upscale(request.clone());
        assert_eq!(
            model.foreground_visual,
            Some(PresentationSourceKind::Upscale)
        );
        assert_eq!(model.explore.sent_upscale, Some(request.clone()));
        assert_eq!(model.displayed_upscale_kernel(), None);
        let drawn = crate::presentation_surface::FrameReady {
            high: 1,
            low: 2,
            layer: 0,
            slot: 0,
            content_session: 1,
            content_sequence: frame.revision,
            presentation_revision: 31,
            content_width: frame.extent.width,
            content_height: frame.extent.height,
        };
        crate::presentation_surface::record_drawn_detail(
            drawn,
            [0, 0, frame.extent.width, frame.extent.height],
        );
        assert_eq!(model.displayed_upscale_kernel(), Some(kernel));
        let mut other = request.clone();
        other.kernel = crate::generated::UpscaleKernel::Default;
        model.request_upscale(other);
        assert_eq!(model.displayed_upscale_kernel(), None);
        model.request_upscale(request);
        assert_eq!(model.displayed_upscale_kernel(), Some(kernel));
        model.upscale_snapshot.as_mut().unwrap().busy = true;
        assert_eq!(model.displayed_upscale_kernel(), None);
        crate::presentation_surface::clear_drawn_detail();
    }

    #[test]
    fn upscale_source_matching_includes_crop_and_legacy_clean_revision() {
        let mut model = bootstrapped();
        let source = select_upscale_source(&mut model, crate::generated::UpscaleKernel::Default);
        let mut other = source.clone();
        other.content.x += 1;
        assert!(!ApplicationModel::same_clean_source(&source, &other));
        let mut legacy = source.clone();
        legacy.cleanrevision = 0;
        other = legacy.clone();
        other.revision += 1;
        assert!(!ApplicationModel::same_clean_source(&legacy, &other));
        other = source.clone();
        other.cleanrevision = 42;
        let mut semantic = other.clone();
        semantic.revision += 1;
        assert!(ApplicationModel::same_clean_source(&other, &semantic));
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
                snapshot: failed,
                detail: "model unavailable".into(),
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
    fn source_observation_orders_metadata_and_cached_product_arrivals() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Explore);
        let first = visual_frame(PresentationSourceKind::Explore, 1);
        let second = visual_frame(PresentationSourceKind::Explore, 2);
        {
            let explore = model.explore.snapshot.as_mut().unwrap();
            explore.ready = true;
            explore.revision = 10;
            explore.frame = first.clone();
        }
        {
            let presentation = model.presentation.as_mut().unwrap();
            presentation.completed = second.clone();
            presentation.completedsourcerevision = 20;
        }
        assert!(!model.completed_presentation_is_obsolete().unwrap());

        {
            let explore = model.explore.snapshot.as_mut().unwrap();
            explore.revision = 30;
            explore.frame = first.clone();
        }
        assert!(model.completed_presentation_is_obsolete().unwrap());

        {
            let explore = model.explore.snapshot.as_mut().unwrap();
            explore.revision = 20;
            explore.frame = first;
        }
        assert!(model.completed_presentation_is_obsolete().is_err());

        model.explore.snapshot.as_mut().unwrap().frame = second.clone();
        assert!(!model.completed_presentation_is_obsolete().unwrap());
        model.explore.snapshot.as_mut().unwrap().revision += 1;
        assert!(!model.completed_presentation_is_obsolete().unwrap());
    }

    #[test]
    fn reconnect requests_the_foreground_product_even_when_native_completion_matches() {
        let mut model = bootstrapped();
        model.set_foreground_feature(FeatureId::Explore);
        let frame = visual_frame(PresentationSourceKind::Explore, 4);
        {
            let explore = model.explore.snapshot.as_mut().unwrap();
            explore.ready = true;
            explore.revision = 7;
            explore.frame = frame.clone();
        }
        model.presentation.as_mut().unwrap().completed = frame.clone();

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
            Some(explore.frame)
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
            let frame = model
                .reduce_event(ApplicationEvent::ExploreExploreChanged(
                    crate::generated::ExploreChanged {
                        snapshot: explore.clone(),
                    },
                ))
                .expect("changed Explore foreground");
            assert_eq!(frame.revision, revision);
            model.record_presentation_sent(frame);
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
            let frame = model
                .reduce_event(ApplicationEvent::AnnotationAnnotationChanged(
                    crate::generated::AnnotationChanged {
                        snapshot: annotation.clone(),
                    },
                ))
                .expect("changed Annotation foreground");
            assert_eq!(frame.revision, revision);
            model.record_presentation_sent(frame);
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

        model.set_foreground_feature(FeatureId::Live);
        let mut live = model.live_snapshot.clone().unwrap();
        live.running = true;
        live.frame = visual_frame(PresentationSourceKind::Live, 0);
        for revision in 1..=2 {
            live.revision += 1;
            live.completedframes += 1;
            live.frame.revision = revision;
            let frame = model
                .reduce_event(ApplicationEvent::LiveLiveFrameCompleted(
                    crate::generated::LiveFrameCompleted {
                        snapshot: live.clone(),
                    },
                ))
                .expect("changed Live foreground");
            assert_eq!(frame.revision, revision);
            model.record_presentation_sent(frame);
        }
    }

    #[test]
    fn predict_and_upscale_keep_typed_private_source_continuity() {
        let mut model = bootstrapped();
        let mut predict = model.predict_snapshot.clone().unwrap();
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
            model.foreground_visual,
            Some(PresentationSourceKind::Annotation)
        );
        assert!(model.current_upscale().is_none());
    }

    #[test]
    fn every_upscale_mode_encodes_the_complete_selected_frame() {
        use crate::application_codec::IntoApplicationValue;

        let source = visual_frame(PresentationSourceKind::Explore, 19);
        for kernel in crate::generated::UPSCALE_KERNEL_VALUES.iter().copied() {
            let encoded = crate::generated::encode_upscale_Start(
                41,
                crate::generated::UpscaleRequest {
                    source: source.clone(),
                    kernel,
                },
            );
            assert_eq!(encoded.endpoint, ApplicationIntentEndpoint::UpscaleStart);
            assert_eq!(encoded.record.correlation, 41);
            assert_eq!(encoded.record.fields.len(), 2);
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
        newer.operation.generationfrontier = 3;
        newer.operation.active = false;
        newer.operation.terminal.generation = 3;
        newer.operation.terminal.outcome = ComputeOperationOutcome::Succeeded;
        newer.frame = visual_frame(PresentationSourceKind::Predict, 2);
        model.reduce_event(ApplicationEvent::PredictPredictChanged(
            crate::generated::PredictChanged { snapshot: newer },
        ));
        running.operation.active = false;
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
