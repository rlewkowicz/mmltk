#[derive(Debug, Clone, Default)]
pub struct ExploreModel {
    pub snapshot: Option<crate::generated::ExploreSnapshot>,
    pub desired_overlay: Option<crate::generated::ExploreOverlay>,
    pub desired_filter: Option<crate::generated::ExploreFilterUpdate>,
    pub desired_augmentation: Option<crate::generated::ExploreAugmentationUpdate>,
    pub desired_detail: Option<crate::generated::ExploreDetailUpdate>,
    pub desired_selection: Option<u32>,
    pub requested_selection: Option<u32>,
    pub navigation_offset: i64,
    pub desired_open: bool,
    pub desired_close: bool,
    pub desired_reroll: bool,
    pub desired_augmentation_reroll: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExplorePresentationState {
    Loading,
    Empty,
    Error,
    Populated,
}

impl ExploreModel {
    pub fn gallery_progress(&self) -> Option<(usize, usize)> {
        let snapshot = self.snapshot.as_ref()?;
        if !snapshot.ready
            || snapshot.mode != crate::generated::ExploreMode::Gallery
            || snapshot.gallery.generation == 0
            || snapshot.gallery.slots.len() != snapshot.order.visibleindices.len()
        {
            return None;
        }
        Some((
            snapshot
                .gallery
                .slots
                .iter()
                .filter(|ready| **ready)
                .count(),
            snapshot.order.visibleindices.len(),
        ))
    }

    pub fn presentation_state(&self) -> ExplorePresentationState {
        match self.snapshot.as_ref() {
            Some(snapshot) if snapshot.busy && snapshot.failure.is_empty() => {
                ExplorePresentationState::Loading
            }
            Some(snapshot) if snapshot.ready && snapshot.order.matchingcount != 0 => {
                ExplorePresentationState::Populated
            }
            Some(snapshot) if !snapshot.failure.is_empty() => ExplorePresentationState::Error,
            Some(_) | None => ExplorePresentationState::Empty,
        }
    }

    pub fn presentation_title(&self) -> &'static str {
        match self.presentation_state() {
            ExplorePresentationState::Loading => "Opening compiled dataset",
            ExplorePresentationState::Empty
                if self
                    .snapshot
                    .as_ref()
                    .is_some_and(|snapshot| snapshot.ready) =>
            {
                "No samples match the filters"
            }
            ExplorePresentationState::Empty => "Waiting for a dataset",
            ExplorePresentationState::Error => {
                match self.snapshot.as_ref().map(|value| value.failurekind) {
                    Some(crate::generated::ExploreFailureKind::SelectedTransportUnavailable) => {
                        "Selected GDR transport is unavailable; select H2D loading in Settings"
                    }
                    Some(crate::generated::ExploreFailureKind::RuntimeInitialization) => {
                        "Explore GPU runtime could not initialize"
                    }
                    _ => "Explore operation failed",
                }
            }
            ExplorePresentationState::Populated
                if self
                    .snapshot
                    .as_ref()
                    .is_some_and(|snapshot| !snapshot.failure.is_empty()) =>
            {
                "Explore operation failed; showing the last completed product"
            }
            ExplorePresentationState::Populated
                if self
                    .gallery_progress()
                    .is_some_and(|(ready, total)| ready < total) =>
            {
                "Preparing visible tiles"
            }
            ExplorePresentationState::Populated => "Dataset ready",
        }
    }

    pub fn gallery_title(
        &self,
        drawable: bool,
        presentation: super::GalleryPresentation,
    ) -> &'static str {
        if self
            .snapshot
            .as_ref()
            .is_some_and(|snapshot| !snapshot.failure.is_empty())
        {
            return self.presentation_title();
        }
        if !drawable
            && self
                .snapshot
                .as_ref()
                .is_some_and(|snapshot| snapshot.ready && snapshot.order.matchingcount != 0)
        {
            if matches!(presentation, super::GalleryPresentation::Unavailable) {
                return "Gallery unavailable";
            }
            if self.snapshot.as_ref().is_some_and(|snapshot| snapshot.busy)
                || self
                    .gallery_progress()
                    .is_some_and(|(ready, total)| ready < total)
            {
                return "Preparing visible tiles";
            }
            return "Restoring gallery";
        }
        self.presentation_title()
    }

    pub fn reset_transport(&mut self) {
        *self = Self::default();
    }
}

use super::{
    ApplicationEvent, ApplicationIntentEndpoint, ApplicationModel, ApplicationReply, Observation,
    PresentationSourceKind, UiError, UiErrorKind, merge_observation,
};

impl crate::generated::ExploreApplicationProjection<UiError> for ApplicationModel {
    fn project_explore_snapshot(
        &mut self,
        value: crate::generated::ExploreSnapshot,
    ) -> Result<(), UiError> {
        self.install_explore_snapshot(value, true).map(|_| ())
    }

    fn project_explore_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::ExploreExploreChanged(value) => {
                match self.install_explore_snapshot(value.snapshot, false) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if self.presentation_model.foreground()
                            == Some(PresentationSourceKind::Explore)
                        {
                            self.set_foreground_visual(Some(PresentationSourceKind::Explore));
                        }
                    }
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            ApplicationEvent::ExploreExploreFailed(value) => {
                let retained_product = value.snapshot.ready;
                match self.install_explore_snapshot(value.snapshot, false) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Stale) => {}
                    Ok(Observation::Installed | Observation::Current) => {
                        if !retained_product {
                            self.failed(value.detail);
                        }
                    }
                }
            }
            _ => unreachable!("generated Explore dispatch supplied another system event"),
        }
    }

    fn project_explore_reply(&mut self, correlation: u64, reply: ApplicationReply) {
        let endpoint = self.pending_intent(correlation);
        let (snapshot, bootstrap) = match reply {
            ApplicationReply::ExploreOpen(snapshot) => (snapshot, true),
            ApplicationReply::ExploreUpdateFilter(snapshot) => {
                if endpoint != Some(ApplicationIntentEndpoint::ExploreUpdateFilter) {
                    self.error = Some(UiError::protocol(
                        "Explore filter reply did not match its pending endpoint",
                    ));
                    return;
                }
                (snapshot, false)
            }
            ApplicationReply::ExploreReroll(snapshot)
            | ApplicationReply::ExploreUpdateOverlay(snapshot)
            | ApplicationReply::ExploreUpdateAugmentation(snapshot)
            | ApplicationReply::ExploreRerollAugmentation(snapshot)
            | ApplicationReply::ExploreUpdateDetail(snapshot)
            | ApplicationReply::ExploreSelect(snapshot)
            | ApplicationReply::ExploreNavigate(snapshot)
            | ApplicationReply::ExploreCloseDetail(snapshot)
            | ApplicationReply::ExploreStop(snapshot) => (snapshot, false),
            _ => unreachable!("generated Explore dispatch supplied another system reply"),
        };
        if let Err(error) = self.install_explore_snapshot(snapshot, bootstrap) {
            self.error = Some(error);
        }
    }
}

impl crate::generated::UpscaleApplicationProjection<UiError> for ApplicationModel {
    fn project_upscale_snapshot(
        &mut self,
        value: crate::generated::UpscaleSnapshot,
    ) -> Result<(), UiError> {
        merge_observation(
            &mut self.upscale_snapshot,
            value,
            |snapshot| snapshot.revision,
            "Upscale",
        )
        .map(|_| ())
    }

    fn project_upscale_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::UpscaleUpscaleChanged(value) => {
                match merge_observation(
                    &mut self.upscale_snapshot,
                    value.snapshot,
                    |snapshot| snapshot.revision,
                    "Upscale",
                ) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if self.current_upscale().is_some()
                            && matches!(
                                self.presentation_model.foreground(),
                                Some(
                                    PresentationSourceKind::Explore
                                        | PresentationSourceKind::Validation
                                        | PresentationSourceKind::Upscale
                                )
                            )
                        {
                            self.set_foreground_visual(Some(PresentationSourceKind::Upscale));
                        }
                    }
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            ApplicationEvent::UpscaleUpscaleFailed(value) => {
                let current_failure = value.kind == crate::generated::UpscaleFailureKind::Physical
                    || value.request.is_none()
                    || value.request.as_ref() == self.requested_upscale.as_ref();
                match merge_observation(
                    &mut self.upscale_snapshot,
                    value.snapshot,
                    |snapshot| snapshot.revision,
                    "Upscale",
                ) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Installed) => {
                        if self
                            .requested_upscale
                            .as_ref()
                            .is_some_and(|request| value.request.as_ref() == Some(request))
                        {
                            let source_kind = self.viewer_native_kind();
                            self.requested_upscale = None;
                            if self.presentation_model.foreground()
                                == Some(PresentationSourceKind::Upscale)
                            {
                                self.set_foreground_visual(Some(source_kind));
                            }
                        }
                        if value.request.as_ref() == self.sent_upscale.as_ref() {
                            self.sent_upscale = None;
                        }
                        if current_failure {
                            let (kind, title) = match value.kind {
                                crate::generated::UpscaleFailureKind::Unavailable => {
                                    (UiErrorKind::Unavailable, "Service unavailable")
                                }
                                crate::generated::UpscaleFailureKind::Failed
                                | crate::generated::UpscaleFailureKind::Physical => {
                                    (UiErrorKind::Failed, "Operation failed")
                                }
                            };
                            self.error = Some(UiError {
                                kind,
                                title,
                                detail: value.detail,
                            });
                        }
                    }
                    Ok(Observation::Current | Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated Upscale dispatch supplied another system event"),
        }
    }

    fn project_upscale_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        match reply {
            ApplicationReply::UpscaleStart(snapshot) => {
                if let Err(error) = merge_observation(
                    &mut self.upscale_snapshot,
                    snapshot,
                    |value| value.revision,
                    "Upscale",
                ) {
                    self.error = Some(error);
                }
                if self.current_upscale().is_some() {
                    self.set_foreground_visual(Some(PresentationSourceKind::Upscale));
                }
            }
            ApplicationReply::UpscaleStop(()) => {}
            _ => unreachable!("generated Upscale dispatch supplied another system reply"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view_model::test_support::{bootstrapped, explore_snapshot, visual_frame};

    #[test]
    fn gallery_progress_counts_only_the_current_exact_visible_readiness() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.order.visibleindices = vec![7, 2, 9];
        snapshot.gallery.generation = 8;
        snapshot.gallery.slots = vec![true, false, true];
        let mut model = ExploreModel {
            snapshot: Some(snapshot),
            ..ExploreModel::default()
        };
        assert_eq!(model.gallery_progress(), Some((2, 3)));
        model.snapshot.as_mut().unwrap().gallery.slots = vec![false; 3];
        assert_eq!(model.gallery_progress(), Some((0, 3)));
        model.snapshot.as_mut().unwrap().gallery.slots.clear();
        assert_eq!(model.gallery_progress(), None);
        model
            .snapshot
            .as_mut()
            .unwrap()
            .order
            .visibleindices
            .clear();
        assert_eq!(model.gallery_progress(), Some((0, 0)));
        model.snapshot.as_mut().unwrap().mode = crate::generated::ExploreMode::Detail;
        assert_eq!(model.gallery_progress(), None);
    }

    #[test]
    fn typed_snapshot_exhaustively_drives_explore_presentation_state() {
        let mut model = ExploreModel::default();
        assert_eq!(model.presentation_state(), ExplorePresentationState::Empty);

        let mut value = explore_snapshot();
        value.busy = true;
        model.snapshot = Some(value.clone());
        assert_eq!(
            model.presentation_state(),
            ExplorePresentationState::Loading
        );

        value.busy = false;
        value.failure = "open failed".into();
        model.snapshot = Some(value.clone());
        assert_eq!(model.presentation_state(), ExplorePresentationState::Error);

        value.failure.clear();
        value.ready = true;
        model.snapshot = Some(value.clone());
        assert_eq!(model.presentation_state(), ExplorePresentationState::Empty);

        value.order.matchingcount = 1;
        model.snapshot = Some(value.clone());
        assert_eq!(
            model.presentation_state(),
            ExplorePresentationState::Populated
        );
        value.failure = "replacement failed".into();
        model.snapshot = Some(value);
        assert_eq!(
            model.presentation_state(),
            ExplorePresentationState::Populated
        );
        assert!(
            model
                .presentation_title()
                .contains("last completed product")
        );
    }

    #[test]
    fn dataset_readiness_never_claims_an_absent_gallery_is_drawable() {
        let mut snapshot = explore_snapshot();
        snapshot.ready = true;
        snapshot.order.matchingcount = 1;
        let mut model = ExploreModel {
            snapshot: Some(snapshot),
            ..Default::default()
        };
        for state in [
            super::super::GalleryPresentation::Inactive,
            super::super::GalleryPresentation::Restoring,
        ] {
            assert_eq!(model.gallery_title(false, state), "Restoring gallery");
            assert_eq!(model.gallery_title(true, state), "Dataset ready");
        }
        assert_eq!(
            model.gallery_title(false, super::super::GalleryPresentation::Unavailable),
            "Gallery unavailable"
        );
        model.snapshot.as_mut().unwrap().busy = true;
        assert_eq!(
            model.gallery_title(false, super::super::GalleryPresentation::Inactive),
            "Preparing visible tiles"
        );
        model.snapshot.as_mut().unwrap().failure = "failed".into();
        for drawable in [false, true] {
            for busy in [false, true] {
                model.snapshot.as_mut().unwrap().busy = busy;
                for state in [
                    super::super::GalleryPresentation::Inactive,
                    super::super::GalleryPresentation::Unavailable,
                ] {
                    assert_eq!(
                        model.gallery_title(drawable, state),
                        model.presentation_title()
                    );
                    assert!(
                        model
                            .gallery_title(drawable, state)
                            .contains("Explore operation failed")
                    );
                }
            }
        }
    }

    #[test]
    fn failed_runtime_and_transport_are_visible_even_if_a_stale_busy_flag_remains() {
        let mut snapshot = explore_snapshot();
        snapshot.failure = "Unavailable".into();
        snapshot.busy = true;
        snapshot.failurekind = crate::generated::ExploreFailureKind::RuntimeInitialization;
        let mut model = ExploreModel {
            snapshot: Some(snapshot),
            ..Default::default()
        };
        assert_eq!(
            model.presentation_title(),
            "Explore GPU runtime could not initialize"
        );
        model.snapshot.as_mut().unwrap().failurekind =
            crate::generated::ExploreFailureKind::SelectedTransportUnavailable;
        assert!(model.presentation_title().contains("select H2D"));
        assert_eq!(
            model.gallery_title(false, super::super::GalleryPresentation::Unavailable),
            model.presentation_title(),
        );
        assert_eq!(model.presentation_state(), ExplorePresentationState::Error);
    }

    #[test]
    fn recoverable_failure_keeps_the_retained_gallery_visible() {
        let mut model = bootstrapped();
        let mut snapshot = model.explore.snapshot.clone().unwrap();
        snapshot.revision += 1;
        snapshot.ready = true;
        snapshot.order.matchingcount = 1;
        snapshot.failure = "replacement failed".into();
        snapshot.frame = visual_frame(PresentationSourceKind::Explore, 1);
        model.reduce_event(ApplicationEvent::ExploreExploreFailed(
            crate::generated::ExploreFailed {
                snapshot,
                detail: "replacement failed".into(),
            },
        ));

        assert!(model.error.is_none());
        assert_eq!(
            model.explore.presentation_state(),
            ExplorePresentationState::Populated
        );
    }

    #[test]
    fn sent_method_failure_cannot_surface_over_a_newer_desired_method() {
        let mut model = bootstrapped();
        let mut explore = explore_snapshot();
        explore.mode = crate::generated::ExploreMode::Detail;
        explore.selectedimage = Some(0);
        explore.ready = true;
        explore.frame =
            super::super::test_support::visual_frame(PresentationSourceKind::Explore, 1);
        let first = crate::generated::UpscaleRequest {
            source: explore.frame.clone(),
            document: explore.document.clone(),
            kernel: crate::generated::UpscaleKernel::Default,
        };
        let second = crate::generated::UpscaleRequest {
            kernel: crate::generated::UpscaleKernel::ShiftLut,
            ..first.clone()
        };
        model.explore.snapshot = Some(explore);
        let first_correlation = model
            .begin_intent(ApplicationIntentEndpoint::UpscaleStart)
            .unwrap();
        model.sent_upscale = Some(first.clone());
        model.request_upscale(second.clone());
        let mut snapshot = model.upscale_snapshot.clone().unwrap();
        snapshot.revision += 1;
        model.reduce_event(ApplicationEvent::UpscaleUpscaleFailed(
            crate::generated::UpscaleFailed {
                snapshot: snapshot.clone(),
                detail: "superseded Basic failure".into(),
                request: Some(first),
                kind: crate::generated::UpscaleFailureKind::Failed,
            },
        ));
        assert!(model.error.is_none());
        assert!(model.sent_upscale.is_none());
        assert_eq!(model.requested_upscale.as_ref(), Some(&second));
        model.reduce_reply(
            first_correlation,
            Err(crate::protocol::ApplicationError {
                category: crate::generated::ApplicationErrorCategory::Failed,
                detail: "superseded Basic admission failure".into(),
            }),
        );
        assert!(model.error.is_none());
        assert!(!model.has_pending(ApplicationIntentEndpoint::UpscaleStart));
        model.sent_upscale = Some(second.clone());
        snapshot.revision += 1;
        snapshot.ready = true;
        snapshot.kernel = second.kernel;
        snapshot.input = second.source.clone();
        snapshot.frame =
            super::super::test_support::visual_frame(PresentationSourceKind::Upscale, 2);
        snapshot.methods[1].available = true;
        snapshot.methods[1].completed = Some(second.clone());
        snapshot.methods[1].frame = snapshot.frame.clone();
        model.reduce_event(ApplicationEvent::UpscaleUpscaleChanged(
            crate::generated::UpscaleChanged { snapshot },
        ));
        assert!(model.error.is_none());
        assert_eq!(model.requested_upscale.as_ref(), Some(&second));
    }

    #[test]
    fn duplicate_upscale_failure_preserves_a_later_error() {
        let mut model = bootstrapped();
        let mut snapshot = model.upscale_snapshot.clone().unwrap();
        snapshot.revision += 1;
        let event = ApplicationEvent::UpscaleUpscaleFailed(crate::generated::UpscaleFailed {
            snapshot,
            detail: "upscale failed".into(),
            request: None,
            kind: crate::generated::UpscaleFailureKind::Failed,
        });

        model.reduce_event(event.clone());
        assert_eq!(model.error.as_ref().unwrap().detail, "upscale failed");

        let later = UiError::transport("later transport failure");
        model.error = Some(later.clone());
        model.reduce_event(event);
        assert_eq!(model.error, Some(later));
    }

    #[test]
    fn preview_and_detail_replies_install_authoritative_state_and_stale_events_do_not_rewind_it() {
        let mut model = bootstrapped();
        let mut preview = explore_snapshot();
        preview.revision += 10;
        preview.ready = true;
        preview.augmentation.enabled = true;
        preview.augmentation.seed = 7;
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::ExploreRerollAugmentation)
            .unwrap();
        model.reduce_reply(
            correlation,
            Ok(ApplicationReply::ExploreRerollAugmentation(preview.clone())),
        );
        assert_eq!(
            model.explore.snapshot.as_ref().unwrap().augmentation.seed,
            7
        );

        let mut detail = preview.clone();
        detail.revision += 1;
        detail.detail.showoriginaldimensions = true;
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::ExploreUpdateDetail)
            .unwrap();
        model.reduce_reply(
            correlation,
            Ok(ApplicationReply::ExploreUpdateDetail(detail.clone())),
        );
        let mut stale = preview;
        stale.augmentation.seed = 2;
        stale.detail.showoriginaldimensions = false;
        model.reduce_event(ApplicationEvent::ExploreExploreChanged(
            crate::generated::ExploreChanged { snapshot: stale },
        ));
        let installed = model.explore.snapshot.as_ref().unwrap();
        assert_eq!(installed.revision, detail.revision);
        assert_eq!(installed.augmentation.seed, 7);
        assert!(installed.detail.showoriginaldimensions);
    }

    #[test]
    fn no_dataset_toggle_changes_only_from_the_authoritative_reply() {
        let mut model = bootstrapped();
        let before = model.explore.snapshot.clone().unwrap();
        assert!(!before.ready);
        let correlation = model
            .begin_intent(ApplicationIntentEndpoint::ExploreUpdateAugmentation)
            .unwrap();
        assert_eq!(model.explore.snapshot.as_ref().unwrap(), &before);

        let mut returned = before.clone();
        returned.revision += 1;
        returned.augmentation.enabled = !before.augmentation.enabled;
        model.reduce_reply(
            correlation,
            Ok(ApplicationReply::ExploreUpdateAugmentation(
                returned.clone(),
            )),
        );
        let installed = model.explore.snapshot.as_ref().unwrap();
        assert_eq!(installed, &returned);
        assert_eq!(installed.frame, before.frame);
        assert_eq!(installed.augmentation.seed, before.augmentation.seed);
        assert!(!installed.ready);
    }
}
