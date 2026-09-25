use super::*;

impl App {
    pub(super) fn on_annotation(
        &mut self,
        outcome: crate::view::annotation::Outcome,
    ) -> Task<Message> {
        match outcome {
            crate::view::annotation::Outcome::ShortcutRequested(shortcut) => {
                if self.status.open || self.modal_active() { return Task::none(); }
                let interaction_revision = self.interaction_revision;
                return iced::widget::operation::is_focused(
                    crate::generated::constraint_uiannotationbrushradius()
                        .stable_field_id
                        .to_string(),
                )
                .map(move |focused| {
                    Message::Workspace(crate::view::router::Message::Annotation(
                        crate::view::annotation::Message::ShortcutResolved {
                            shortcut: shortcut.clone(),
                            interaction_revision,
                            focused,
                        },
                    ))
                });
            }
            crate::view::annotation::Outcome::OpenRequested => {
                let _ = self.open_annotation();
            }
            crate::view::annotation::Outcome::SaveRequested => {
                self.model.begin_admission(ApplicationIntentEndpoint::AnnotationSave);
                if self.settings.has_local_edits() || !self.model.annotation_save_available() {
                    self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationSave, UiError::busy(
                        "Annotation is unavailable or already changing state.",
                    ));
                    return Task::none();
                }
                if self.settings_unsettled() {
                    self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationSave, UiError::busy(
                        "Wait for Annotation settings to finish saving.",
                    ));
                    return Task::none();
                }
                let Some(settings) = self.model.settings_snapshot.as_ref() else {
                    self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationSave, UiError::invalid(
                        "Annotation settings are not installed yet.",
                    ));
                    return Task::none();
                };
                let Some(_snapshot) = self.model.annotation.snapshot.as_ref() else {
                    self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationSave, UiError::invalid(
                        "Annotation has no current typed workspace.",
                    ));
                    return Task::none();
                };
                let destination = settings.settingsstate.workflows.annotate.outputdir.clone();
                self.submit_intent(
                    ApplicationIntentEndpoint::AnnotationSave,
                    move |correlation| {
                        crate::generated::encode_annotation_Save(
                            correlation,
                            AnnotationSave { destination },
                        )
                    },
                );
            }
            crate::view::annotation::Outcome::StopRequested => {
                self.model.begin_admission(ApplicationIntentEndpoint::AnnotationStop);
                if self.model.annotation_stop_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::AnnotationStop,
                        crate::generated::encode_annotation_Stop,
                    );
                } else {
                    self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationStop, UiError::busy(
                        "Annotation is not running or is already stopping.",
                    ));
                }
            }
            crate::view::annotation::Outcome::DialogRequested(field_id) => {
                self.open_dialog(field_id);
            }
            crate::view::annotation::Outcome::EditRequested(request) => {
                self.model.begin_admission(ApplicationIntentEndpoint::AnnotationEdit);
                if self.model.annotation_edit_available() {
                    if self.submit_intent(
                        ApplicationIntentEndpoint::AnnotationEdit,
                        move |correlation| {
                            crate::generated::encode_annotation_Edit(correlation, request)
                        },
                    ) {
                        self.workspace.rebase_annotation(&self.model);
                    }
                } else {
                    self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationEdit, UiError::busy(
                        "Annotation is unavailable or already changing state.",
                    ));
                }
            }
            crate::view::annotation::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
        }
        Task::none()
    }

    pub(super) fn open_annotation(&mut self) -> bool {
        self.submit_annotation_open(
            self.model.selected_detail_source(),
            self.model.annotation_open_available(),
        )
    }

    pub(super) fn copy_viewer_to_annotation(&mut self) -> bool {
        self.submit_annotation_open(
            crate::presentation_surface::viewer_annotation_request(),
            self.model.annotation_import_available(),
        )
    }

    fn submit_annotation_open(&mut self, request: Option<AnnotationOpen>, available: bool) -> bool {
        self.model.begin_admission(ApplicationIntentEndpoint::AnnotationOpen);
        if self.settings.has_local_edits() || !available {
            self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationOpen, UiError::busy(
                "Annotation is unavailable or already changing state.",
            ));
            return false;
        }
        let Some(request) = request else {
            self.model.report_admission_error(ApplicationIntentEndpoint::AnnotationOpen, UiError::presentation(
                "Annotation requires a current typed visual source",
            ));
            return false;
        };
        let observed = self.integration.as_ref().map(|_| {
            (
                request.clone(),
                self.model
                    .annotation
                    .snapshot
                    .as_ref()
                    .map_or(0, |snapshot| snapshot.inputdocumentepoch),
            )
        });
        let submitted = self.submit_intent(
            ApplicationIntentEndpoint::AnnotationOpen,
            move |correlation| crate::generated::encode_annotation_Open(correlation, request),
        );
        if submitted
            && let (Some(integration), Some((request, epoch))) =
                (self.integration.as_mut(), observed)
        {
            integration.observe_annotation_open(request, epoch);
        }
        submitted
    }
}
