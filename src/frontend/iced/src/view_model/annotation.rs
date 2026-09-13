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
                && (frame.uirevision != incoming.uirevision || frame.frame != incoming.frame)
            {
                return Err(UiError::protocol(
                    "inconsistent Annotation frame and full-state revision",
                ));
            }
            if frame.revision > incoming.revision {
                if frame.uirevision == incoming.uirevision {
                    incoming.frame = frame.frame.clone();
                    incoming.revision = frame.revision;
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
            if incoming.uirevision == installed.uirevision {
                if self
                    .pending_frame
                    .as_ref()
                    .is_some_and(|pending| pending.revision <= incoming.revision)
                {
                    self.pending_frame = None;
                }
                installed.revision = incoming.revision;
                installed.frame = incoming.frame;
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
    #[test]
    fn logical_ui_and_frame_observations_keep_revision_order() {
        let mut model = super::super::test_support::bootstrapped().annotation;
        let mut snapshot = model.snapshot.clone().unwrap();
        snapshot.revision = 4;
        snapshot.uirevision = 3;
        model.install_snapshot(snapshot.clone()).unwrap();
        let newer = crate::generated::AnnotationFrameState {
            revision: 5,
            uirevision: 3,
            frame: snapshot.frame.clone(),
        };
        model.install_frame(newer).unwrap();
        assert_eq!(model.snapshot.as_ref().unwrap().revision, 5);
        assert_eq!(
            model.install_snapshot(snapshot).unwrap(),
            Observation::Stale
        );
    }
}
