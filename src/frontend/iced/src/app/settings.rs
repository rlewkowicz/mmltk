use super::*;

impl App {
    pub(super) fn settle_settings_reply(
        &mut self,
        endpoint: Option<ApplicationIntentEndpoint>,
        succeeded: bool,
        installed_settings: bool,
    ) {
        if endpoint == Some(ApplicationIntentEndpoint::SettingsUpdate) {
            if succeeded {
                if let Some(authoritative) = self.model.settings_snapshot.clone() {
                    self.settings.settle_success(&authoritative);
                    if let Some(pending) = self.model.workflow.pending_start.as_mut()
                        && pending.preparation == crate::view_model::StartPreparation::Waiting
                        && let Some(inputs) = self.settings.draft().and_then(|draft| crate::view_model::StartInputs::capture(draft, pending.feature))
                    {
                        pending.inputs = inputs;
                    }
                    self.flush_settings_edits();
                } else {
                    self.settings.settle_failure(None);
                    self.model.error = Some(UiError::protocol(
                        "settings reply did not install an authoritative snapshot",
                    ));
                }
            } else {
                let authoritative = self.model.settings_snapshot.clone();
                self.settings.settle_failure(authoritative.as_ref());
                if let Some(authoritative) = authoritative {
                    self.rebase_page(authoritative.settingsstate.currentview);
                }
            }
            self.reconcile_explore_viewport();
        }
        let successful_reset =
            endpoint == Some(ApplicationIntentEndpoint::SettingsReset) && succeeded;
        if successful_reset {
            if let Some(authoritative) = self.model.settings_snapshot.clone() {
                let authoritative_route = authoritative.settingsstate.currentview;
                self.settings.reset(&authoritative);
                self.rebase_page(authoritative_route);
                if let Some(integration) = self.integration.as_mut() {
                    integration.observe_reporting(|reporting| {
                        reporting.observe_authoritative_route(
                            "settings.reply",
                            authoritative_route,
                            self.workspace.active(),
                        );
                    });
                }
            } else {
                self.settings.settle_failure(None);
                self.model.error = Some(UiError::protocol(
                    "settings reset reply did not install an authoritative snapshot",
                ));
            }
            self.reconcile_explore_viewport();
        }
        if installed_settings
            && !successful_reset
            && let Some(authoritative) = self.model.settings_snapshot.as_ref()
        {
            let authoritative_route = authoritative.settingsstate.currentview;
            self.settings.install(authoritative);
            self.rebase_page(authoritative_route);
            if let Some(integration) = self.integration.as_mut() {
                integration.observe_reporting(|reporting| {
                    reporting.observe_authoritative_route(
                        "settings.reply",
                        authoritative_route,
                        self.workspace.active(),
                    );
                });
            }
        }
    }

    pub(super) fn settings_unsettled(&self) -> bool {
        self.settings.has_local_edits() || self.model.native_settings_unsettled()
    }
    pub(super) fn on_file_dialog(&mut self, message: crate::view::file_dialog::Message) {
        match crate::view::file_dialog::update(message) {
            crate::view::file_dialog::Outcome::StopRequested => {
                match self.model.prepare_dialog_stop() {
                    Ok(_) => {
                        self.submit_intent(
                            ApplicationIntentEndpoint::FileDialogStop,
                            crate::generated::encode_filedialog_Stop,
                        );
                    }
                    Err(error) => self.model.error = Some(error),
                }
            }
        }
    }

    pub(super) fn on_settings(&mut self, message: crate::view::settings::Message) -> Task<Message> {
        match self.settings.update(message) {
            Ok(Some(crate::view::settings::Outcome::SettingsEdited(schedule))) => {
                return self.handle_settings_schedule(schedule);
            }
            #[cfg(target_arch = "wasm32")]
            Ok(Some(crate::view::settings::Outcome::DebounceElapsed(generation))) => {
                if self.settings.state().debounce_elapsed(generation) {
                    self.flush_settings_edits();
                }
            }
            Ok(Some(crate::view::settings::Outcome::ResetRequested)) => {
                if self.settings.has_local_edits() || !self.model.settings_reset_available() {
                    self.model.error = Some(UiError::busy(
                        "Wait for the current settings mutation to finish.",
                    ));
                } else {
                    self.submit_intent(ApplicationIntentEndpoint::SettingsReset, |correlation| {
                        crate::generated::encode_settings_Reset(
                            correlation,
                            SettingsResetRequest {},
                        )
                    });
                }
            }
            Ok(Some(crate::view::settings::Outcome::Closed)) | Ok(None) => {}
            Err(detail) => self.model.error = Some(UiError::protocol(detail)),
        }
        Task::none()
    }

    pub(super) fn on_error(&mut self, message: crate::view::error_modal::Message) -> Task<Message> {
        let outcome = crate::view::error_modal::update(self.model.error.as_ref(), message);
        match outcome {
            crate::view::error_modal::Outcome::CopyRequested { source, payload } => {
                iced::clipboard::write(payload)
                    .map(move |result| error_clipboard_message(source.clone(), result))
            }
            crate::view::error_modal::Outcome::CopySucceeded => Task::none(),
            crate::view::error_modal::Outcome::CopyFailed => {
                self.model.error = Some(UiError {
                    kind: crate::view_model::UiErrorKind::Presentation,
                    title: "Cannot copy",
                    detail: "The error details could not be written to the clipboard.".to_owned(),
                });
                Task::none()
            }
            crate::view::error_modal::Outcome::Dismissed => {
                self.model.error = None;
                Task::none()
            }
            crate::view::error_modal::Outcome::Ignored => Task::none(),
        }
    }

    pub(super) fn open_dialog(&mut self, stable_field_id: u64) {
        let active_feature = self.workspace.active();
        let Some(fact) = crate::generated::FILE_DIALOGS.iter().find(|dialog| {
            dialog.stable_field_id == stable_field_id && dialog.workflows.contains(&active_feature)
        }) else {
            self.model.error = Some(UiError {
                kind: crate::view_model::UiErrorKind::InvalidIntent,
                title: "File selection unavailable",
                detail: "The requested file field is not available on the active page.".to_owned(),
            });
            return;
        };
        if self.settings.has_local_edits()
            || !self.model.file_dialog_open_available(fact, active_feature)
        {
            self.model.error = Some(UiError::busy(
                "A file selection request is already active or pending.",
            ));
            return;
        }
        if let Err(error) = self.model.register_dialog(fact, active_feature) {
            self.model.error = Some(error);
            return;
        }
        self.submit_file_dialog_open(crate::generated::FileDialogTarget::SettingsFieldTarget(
            crate::generated::SettingsFieldTarget {
                stableid: stable_field_id,
            },
        ));
    }

    pub(super) fn open_model_dialog(&mut self, target: crate::generated::FileDialogTarget) {
        if self.settings.has_local_edits() {
            self.model.error = Some(UiError::busy(
                "Wait for current settings edits before selecting a model.",
            ));
            return;
        }
        if let Err(error) = self.model.register_model_dialog(&target) {
            self.model.error = Some(error);
            return;
        }
        self.submit_file_dialog_open(target);
    }

    fn submit_file_dialog_open(&mut self, target: crate::generated::FileDialogTarget) {
        self.submit_intent(
            ApplicationIntentEndpoint::FileDialogOpen,
            move |correlation| {
                crate::generated::encode_filedialog_Open(correlation, FileDialogOpen { target })
            },
        );
    }

    pub(super) fn handle_settings_schedule(&mut self, schedule: EditSchedule) -> Task<Message> {
        if self.model.workflow.pending_start.as_ref().is_some_and(|pending| {
            self.settings.draft().is_none_or(|draft| !pending.inputs.matches(draft))
        }) { self.model.workflow.cancel_start("Start cancelled because its inputs changed."); }
        match schedule {
            EditSchedule::Debounce(generation) => settings_persist_task(generation),
            EditSchedule::FlushNow => {
                self.flush_settings_edits();
                Task::none()
            }
        }
    }

    pub(super) fn flush_settings_edits(&mut self) {
        if self
            .model
            .has_pending(ApplicationIntentEndpoint::SettingsUpdate)
        {
            return;
        }
        let Some(request) = self.settings.state_mut().take_request() else {
            return;
        };
        if !self.submit_intent(
            ApplicationIntentEndpoint::SettingsUpdate,
            move |correlation| crate::generated::encode_settings_Update(correlation, request),
        ) {
            let authoritative = self.model.settings_snapshot.clone();
            self.settings.settle_failure(authoritative.as_ref());
            if let Some(authoritative) = authoritative {
                self.rebase_page(authoritative.settingsstate.currentview);
            }
            self.reconcile_explore_viewport();
        }
    }
}

#[cfg(target_arch = "wasm32")]
fn settings_persist_task(generation: u64) -> Task<Message> {
    Task::perform(
        async move {
            gloo_timers::future::TimeoutFuture::new(SETTINGS_PERSIST_DEBOUNCE_MS).await;
            generation
        },
        |generation| Message::Settings(crate::view::settings::Message::DebounceElapsed(generation)),
    )
}

#[cfg(not(target_arch = "wasm32"))]
fn settings_persist_task(_generation: u64) -> Task<Message> {
    Task::none()
}

fn error_clipboard_message(source: UiError, result: Result<(), iced::clipboard::Error>) -> Message {
    Message::Error(crate::view::error_modal::Message::CopyResolved { source, result })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clipboard_completion_maps_back_through_the_root_error_message() {
        let source = UiError::protocol("copied error");
        assert!(matches!(
            error_clipboard_message(source.clone(), Ok(())),
            Message::Error(crate::view::error_modal::Message::CopyResolved {
                source: mapped,
                result: Ok(())
            }) if mapped == source
        ));
        assert!(matches!(
            error_clipboard_message(
                source.clone(),
                Err(iced::clipboard::Error::ClipboardUnavailable)
            ),
            Message::Error(crate::view::error_modal::Message::CopyResolved {
                source: mapped,
                result: Err(iced::clipboard::Error::ClipboardUnavailable)
            }) if mapped == source
        ));
    }

    #[test]
    fn clipboard_success_keeps_the_original_modal_and_failure_replaces_it() {
        let (mut app, boot_task) = crate::app::boot();
        drop(boot_task);
        let original = UiError::protocol("invalid field");
        app.model.error = Some(original.clone());

        drop(
            app.on_error(crate::view::error_modal::Message::CopyResolved {
                source: original.clone(),
                result: Ok(()),
            }),
        );
        assert_eq!(app.model.error, Some(original.clone()));

        drop(
            app.on_error(crate::view::error_modal::Message::CopyResolved {
                source: original,
                result: Err(iced::clipboard::Error::ClipboardUnavailable),
            }),
        );
        let error = app.model.error.as_ref().unwrap();
        assert_eq!(error.title, "Cannot copy");
        assert_eq!(
            error.detail,
            "The error details could not be written to the clipboard."
        );
    }

    #[test]
    fn stale_clipboard_failure_cannot_resurrect_or_replace_an_error() {
        let (mut app, boot_task) = crate::app::boot();
        drop(boot_task);
        let copied = UiError::protocol("copied error");
        app.model.error = Some(copied.clone());
        drop(app.on_error(crate::view::error_modal::Message::Dismiss));
        drop(
            app.on_error(crate::view::error_modal::Message::CopyResolved {
                source: copied.clone(),
                result: Err(iced::clipboard::Error::ClipboardUnavailable),
            }),
        );
        assert!(app.model.error.is_none());

        let replacement = UiError::busy("newer error");
        app.model.error = Some(replacement.clone());
        drop(
            app.on_error(crate::view::error_modal::Message::CopyResolved {
                source: copied.clone(),
                result: Err(iced::clipboard::Error::ClipboardUnavailable),
            }),
        );
        assert_eq!(app.model.error, Some(replacement));

        app.retire_peer(UiError::transport("reconnecting"));
        let reconnect_error = app.model.error.clone();
        drop(
            app.on_error(crate::view::error_modal::Message::CopyResolved {
                source: copied,
                result: Err(iced::clipboard::Error::ClipboardUnavailable),
            }),
        );
        assert_eq!(app.model.error, reconnect_error);
    }
}
