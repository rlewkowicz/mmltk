use super::*;

impl App {
    pub(super) fn on_annotation(
        &mut self,
        outcome: crate::view::annotation::Outcome,
    ) -> Task<Message> {
        match outcome {
            crate::view::annotation::Outcome::ShortcutRequested(shortcut) => {
                return iced::widget::operation::is_focused(
                    crate::generated::constraint_uiannotationbrushradius()
                        .stable_field_id
                        .to_string(),
                )
                .map(move |focused| {
                    Message::Workspace(crate::view::router::Message::Annotation(
                        crate::view::annotation::Message::ShortcutResolved {
                            shortcut: shortcut.clone(),
                            focused,
                        },
                    ))
                });
            }
            crate::view::annotation::Outcome::OpenRequested => {
                let _ = self.open_annotation();
            }
            crate::view::annotation::Outcome::SaveRequested => {
                if self.settings.has_local_edits() || !self.model.annotation_save_available() {
                    self.model.error = Some(UiError::busy(
                        "Annotation is unavailable or already changing state.",
                    ));
                    return Task::none();
                }
                if self.settings_unsettled() {
                    self.model.error = Some(UiError::busy(
                        "Wait for Annotation settings to finish saving.",
                    ));
                    return Task::none();
                }
                let Some(settings) = self.model.settings_snapshot.as_ref() else {
                    self.model.error = Some(UiError::invalid(
                        "Annotation settings are not installed yet.",
                    ));
                    return Task::none();
                };
                let Some(_snapshot) = self.model.annotation.snapshot.as_ref() else {
                    self.model.error = Some(UiError::invalid(
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
                if self.model.annotation_stop_available() {
                    self.submit_intent(
                        ApplicationIntentEndpoint::AnnotationStop,
                        crate::generated::encode_annotation_Stop,
                    );
                } else {
                    self.model.error = Some(UiError::busy(
                        "Annotation is not running or is already stopping.",
                    ));
                }
            }
            crate::view::annotation::Outcome::DialogRequested(field_id) => {
                self.open_dialog(field_id);
            }
            crate::view::annotation::Outcome::EditRequested(request) => {
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
                    self.model.error = Some(UiError::busy(
                        "Annotation is unavailable or already changing state.",
                    ));
                }
            }
            crate::view::annotation::Outcome::Pointer(pointer) => {
                self.submit_annotation_pointer(pointer);
            }
            crate::view::annotation::Outcome::SettingsEdited(schedule) => {
                return self.handle_settings_schedule(schedule);
            }
        }
        Task::none()
    }

    pub(super) fn open_annotation(&mut self) -> bool {
        self.submit_annotation_open(self.model.selected_detail_source())
    }

    pub(super) fn copy_viewer_to_annotation(&mut self) -> bool {
        let source = self
            .model
            .viewed_explore_frame()
            .filter(|source| crate::presentation_surface::viewer_copy_matches(&self.model, source));
        let originalcontent = self
            .model
            .explore
            .snapshot
            .as_ref()
            .is_some_and(|snapshot| snapshot.detail.showoriginaldimensions);
        self.submit_annotation_open(source.map(|source| AnnotationOpen {
            source,
            originalcontent,
        }))
    }

    fn submit_annotation_open(&mut self, request: Option<AnnotationOpen>) -> bool {
        if self.settings.has_local_edits() || !self.model.annotation_open_available() {
            self.model.error = Some(UiError::busy(
                "Annotation is unavailable or already changing state.",
            ));
            return false;
        }
        let Some(request) = request else {
            self.model.error = Some(UiError::presentation(
                "Annotation requires a current typed visual source",
            ));
            return false;
        };
        self.submit_intent(
            ApplicationIntentEndpoint::AnnotationOpen,
            move |correlation| crate::generated::encode_annotation_Open(correlation, request),
        )
    }

    pub(super) fn submit_annotation_pointer(
        &mut self,
        pointer: crate::generated::AnnotationPointer,
    ) {
        if self.config.integration {
            crate::integration_control::report_annotation_gesture(
                "sending",
                [
                    pointer.interactionid as f64,
                    pointer.sequence as f64,
                    if pointer.phase == crate::generated::AnnotationPointerPhase::Begin {
                        1.0
                    } else if pointer.phase == crate::generated::AnnotationPointerPhase::Update {
                        2.0
                    } else if pointer.phase == crate::generated::AnnotationPointerPhase::End {
                        3.0
                    } else {
                        4.0
                    },
                    0.0,
                ],
            );
        }
        let Some(connection) = self.connection.as_mut() else {
            self.retire_peer(UiError::transport(
                "annotation pointer transport is unavailable",
            ));
            return;
        };
        let result = connection.send_annotation_pointer(pointer);
        if let Err(error) = result {
            self.retire_peer(UiError::transport(error.to_string()));
        }
    }
}
