use super::UiError;

#[derive(Debug, Clone, Default)]
pub struct AnnotationModel {
    pub snapshot: Option<crate::generated::AnnotationSnapshot>,
    pending_frame: Option<crate::generated::AnnotationFrameState>,
}

impl AnnotationModel {
    pub(super) fn install_snapshot(
        &mut self,
        mut incoming: crate::generated::AnnotationSnapshot,
    ) -> Result<Observation, UiError> {
        if incoming.uirevision > incoming.revision {
            return Err(UiError::protocol(
                "invalid Annotation full-state UI revision",
            ));
        }
        if self.snapshot.as_ref().is_some_and(|installed| {
            installed.revision == incoming.revision && *installed != incoming
        }) {
            return Err(UiError::protocol(
                "inconsistent Annotation snapshot revision",
            ));
        }
        let mut keep_pending = false;
        if let Some(frame) = self.pending_frame.as_ref() {
            if frame.revision == incoming.revision
                && (frame.uirevision != incoming.uirevision
                    || frame.frame != incoming.frame
                    || frame.rendered != incoming.rendered)
            {
                return Err(UiError::protocol(
                    "inconsistent Annotation frame and full-state revision",
                ));
            }
            if frame.revision > incoming.revision {
                if frame.uirevision == incoming.uirevision
                {
                    incoming.frame = frame.frame.clone();
                    incoming.revision = frame.revision;
                    incoming.rendered = frame.rendered.clone();
                } else {
                    keep_pending = frame.uirevision >= incoming.uirevision;
                }
            }
        }
        let observation = match self.snapshot.as_ref() {
            Some(installed) if incoming.revision < installed.revision => {
                return Ok(Observation::Stale);
            }
            Some(installed)
                if incoming.revision == installed.revision && incoming != *installed =>
            {
                return Err(UiError::protocol(
                    "inconsistent Annotation snapshot revision",
                ));
            }
            Some(installed) if incoming == *installed => Observation::Current,
            _ => {
                self.snapshot = Some(incoming);
                Observation::Installed
            }
        };
        if !keep_pending {
            self.pending_frame = None;
        }
        Ok(observation)
    }

    fn install_frame(
        &mut self,
        incoming: crate::generated::AnnotationFrameState,
    ) -> Result<Observation, UiError> {
        if incoming.uirevision > incoming.revision {
            return Err(UiError::protocol("invalid Annotation frame UI revision"));
        }
        if let Some(pending) = self.pending_frame.as_ref() {
            if pending.revision == incoming.revision && *pending != incoming {
                return Err(UiError::protocol(
                    "inconsistent pending Annotation frame revision",
                ));
            }
        }
        if let Some(installed) = self.snapshot.as_mut() {
            if incoming.revision == installed.revision {
                return if incoming.uirevision == installed.uirevision
                    && incoming.frame == installed.frame
                    && incoming.rendered == installed.rendered
                {
                    Ok(Observation::Current)
                } else {
                    Err(UiError::protocol("inconsistent Annotation frame revision"))
                };
            }
            if incoming.revision < installed.revision || incoming.uirevision < installed.uirevision
            {
                return Ok(Observation::Stale);
            }
            if incoming.uirevision == installed.uirevision
            {
                if self
                    .pending_frame
                    .as_ref()
                    .is_some_and(|pending| pending.revision <= incoming.revision)
                {
                    self.pending_frame = None;
                }
                installed.revision = incoming.revision;
                installed.frame = incoming.frame;
                installed.rendered = incoming.rendered;
                return Ok(Observation::Installed);
            }
        }
        if self
            .pending_frame
            .as_ref()
            .is_some_and(|pending| pending.revision > incoming.revision)
        {
            return Ok(Observation::Stale);
        }
        self.pending_frame = Some(incoming);
        Ok(Observation::Current)
    }
}

use super::{ApplicationEvent, ApplicationModel, ApplicationReply, Observation};

impl crate::generated::AnnotationApplicationProjection<UiError> for ApplicationModel {
    fn project_annotation_snapshot(
        &mut self,
        value: crate::generated::AnnotationSnapshot,
    ) -> Result<(), UiError> {
        self.annotation.install_snapshot(value).map(|_| ())
    }

    fn project_annotation_event(&mut self, event: ApplicationEvent) {
        let observation = match event {
            ApplicationEvent::AnnotationAnnotationChanged(value) => {
                self.annotation.install_snapshot(value.snapshot)
            }
            ApplicationEvent::AnnotationAnnotationFrameChanged(value) => {
                self.annotation.install_frame(value.snapshot)
            }
            ApplicationEvent::AnnotationAnnotationFailed(value) => {
                match self.annotation.install_snapshot(value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Stale) => {}
                    Ok(Observation::Installed | Observation::Current) => self.failed(value.detail),
                }
                return;
            }
            _ => unreachable!("generated Annotation dispatch supplied another system event"),
        };
        match observation {
            Err(error) => self.error = Some(error),
            Ok(Observation::Installed) => {
                if self.presentation_model.foreground()
                    == Some(crate::generated::PresentationSourceKind::Annotation)
                {
                    self.set_foreground_visual(Some(
                        crate::generated::PresentationSourceKind::Annotation,
                    ));
                }
            }
            Ok(Observation::Current | Observation::Stale) => {}
        }
    }

    fn project_annotation_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::AnnotationOpen(snapshot)
            | ApplicationReply::AnnotationEdit(snapshot)
            | ApplicationReply::AnnotationSave(snapshot)
            | ApplicationReply::AnnotationStop(snapshot) => snapshot,
            _ => unreachable!("generated Annotation dispatch supplied another system reply"),
        };
        if let Err(error) = self.annotation.install_snapshot(snapshot) {
            self.error = Some(error);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn full_state() -> crate::generated::AnnotationSnapshot {
        let mut full = super::super::test_support::bootstrapped().annotation.snapshot.unwrap();
        full.revision = 5;
        full.uirevision = 3;
        full.inputdocumentepoch = 2;
        full.ui.documentrevision = 9;
        full.ui.scenerevision = 10;
        full.frame = super::super::test_support::visual_frame(
            crate::generated::PresentationSourceKind::Annotation, 12,
        );
        full.rendered = crate::generated::AnnotationRenderedFacts {
            generation: 7,
            documentepoch: 1,
            scenerevision: 6,
        };
        full
    }

    fn compact(full: &crate::generated::AnnotationSnapshot) -> crate::generated::AnnotationFrameState {
        crate::generated::AnnotationFrameState {
            revision: full.revision,
            uirevision: full.uirevision,
            frame: full.frame.clone(),
            rendered: full.rendered.clone(),
        }
    }

    #[test]
    fn equal_revision_requires_every_rendered_identity_field_in_both_arrival_orders() {
        for compact_first in [false, true] {
            for field in 0..3 {
                let full = full_state();
                let mut frame = compact(&full);
                match field {
                    0 => frame.rendered.generation += 1,
                    1 => frame.rendered.documentepoch += 1,
                    _ => frame.rendered.scenerevision += 1,
                }
                let mut model = AnnotationModel::default();
                if compact_first {
                    model.install_frame(frame).unwrap();
                    assert!(model.install_snapshot(full).is_err());
                    assert!(model.snapshot.is_none());
                } else {
                    model.install_snapshot(full.clone()).unwrap();
                    assert!(model.install_frame(frame).is_err());
                    assert_eq!(model.snapshot.as_ref(), Some(&full));
                }
            }
        }
    }

    #[test]
    fn complete_render_facts_merge_in_both_orders_without_rolling_back_logical_ui() {
        for compact_first in [false, true] {
            let full = full_state();
            let mut preview = compact(&full);
            preview.revision += 2;
            preview.frame.revision += 1;
            preview.rendered.generation += 1;
            let mut model = AnnotationModel::default();
            if compact_first {
                model.install_frame(preview.clone()).unwrap();
                let mut earlier = full.clone();
                earlier.revision = 2;
                earlier.uirevision = 2;
                model.install_snapshot(earlier).unwrap();
            }
            model.install_snapshot(full.clone()).unwrap();
            if !compact_first { model.install_frame(preview.clone()).unwrap(); }
            let installed = model.snapshot.as_ref().unwrap().clone();
            assert_eq!(installed.frame, preview.frame);
            assert_eq!(installed.rendered, preview.rendered);
            assert_eq!(installed.ui, full.ui);
            assert_ne!(installed.rendered.documentepoch, installed.inputdocumentepoch);
            assert!(model.install_frame(preview.clone()).is_ok());
            assert!(model.install_snapshot(full.clone()).is_ok());
            assert_eq!(model.snapshot.as_ref(), Some(&installed));

            let mut future = compact(&installed);
            future.revision += 2;
            future.uirevision = future.revision;
            future.rendered.generation += 1;
            model.install_frame(future).unwrap();
            let mut overtaking = installed.clone();
            overtaking.revision += 3;
            overtaking.uirevision = overtaking.revision;
            overtaking.ui.documentrevision += 1;
            model.install_snapshot(overtaking.clone()).unwrap();
            assert_eq!(model.snapshot.as_ref(), Some(&overtaking));
            assert!(model.pending_frame.is_none());
        }
    }

    #[test]
    fn matching_full_and_compact_revision_is_current_in_both_arrival_orders() {
        for compact_first in [false, true] {
            let full = full_state();
            let frame = compact(&full);
            let mut model = AnnotationModel::default();
            if compact_first { model.install_frame(frame.clone()).unwrap(); }
            model.install_snapshot(full.clone()).unwrap();
            assert!(model.install_frame(frame).is_ok());
            assert_eq!(model.snapshot.as_ref(), Some(&full));
        }
    }
}
