use super::UiError;

#[derive(Debug, Clone, Default)]
pub struct AnnotationModel {
    pub snapshot: Option<crate::generated::AnnotationSnapshot>,
}

impl AnnotationModel {
    pub(super) fn install_snapshot(
        &mut self,
        incoming: crate::generated::AnnotationSnapshot,
    ) -> Result<Observation, UiError> {
        match self.snapshot.as_ref() {
            Some(installed) if incoming.revision < installed.revision => Ok(Observation::Stale),
            Some(installed)
                if incoming.revision == installed.revision && incoming != *installed =>
            {
                Err(UiError::protocol(
                    "inconsistent Annotation snapshot revision",
                ))
            }
            Some(installed) if incoming == *installed => Ok(Observation::Current),
            _ => {
                self.snapshot = Some(incoming);
                Ok(Observation::Installed)
            }
        }
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
        match event {
            ApplicationEvent::AnnotationAnnotationChanged(value) => {
                match self.annotation.install_snapshot(value.snapshot) {
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
            ApplicationEvent::AnnotationAnnotationFailed(value) => {
                match self.annotation.install_snapshot(value.snapshot) {
                    Err(error) => self.error = Some(error),
                    Ok(Observation::Stale) => {}
                    Ok(Observation::Installed | Observation::Current) => {
                        self.failed(value.detail);
                    }
                }
            }
            _ => unreachable!("generated Annotation dispatch supplied another system event"),
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
