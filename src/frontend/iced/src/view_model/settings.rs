use super::*;

impl crate::generated::SettingsApplicationProjection<UiError> for ApplicationModel {
    fn project_settings_snapshot(&mut self, value: SettingsUiState) -> Result<(), UiError> {
        self.install_settings_snapshot(value).map(|_| ())
    }

    fn project_settings_event(&mut self, event: ApplicationEvent) {
        let ApplicationEvent::SettingsSettingsChanged(value) = event else {
            unreachable!("generated Settings dispatch supplied another system event");
        };
        if let Err(error) = self.install_settings_snapshot(value.snapshot) {
            self.error = Some(error);
        }
    }

    fn project_settings_reply(&mut self, _correlation: u64, reply: ApplicationReply) {
        let snapshot = match reply {
            ApplicationReply::SettingsUpdate(snapshot)
            | ApplicationReply::SettingsReset(snapshot) => snapshot,
            _ => unreachable!("generated Settings dispatch supplied another system reply"),
        };
        if let Err(error) = self.install_settings_snapshot(snapshot) {
            self.error = Some(error);
        }
    }
}

impl crate::generated::FileDialogApplicationProjection<UiError> for ApplicationModel {
    fn project_filedialog_snapshot(&mut self, value: FileDialogSnapshot) -> Result<(), UiError> {
        merge_dialog_snapshot(&mut self.file_dialog, value).map(|_| ())
    }

    fn project_filedialog_event(&mut self, event: ApplicationEvent) {
        match event {
            ApplicationEvent::FileDialogFileDialogCompleted(value) => {
                let target = value.snapshot.target.clone();
                if let Err(error) = self.install_dialog_terminal(value.snapshot) {
                    self.clear_dialog_target_if_matches(&target);
                    self.error = Some(error);
                }
            }
            ApplicationEvent::FileDialogFileDialogFailed(value) => {
                let target = value.snapshot.target.clone();
                match self.install_dialog_terminal(value.snapshot) {
                    Err(error) => {
                        self.clear_dialog_target_if_matches(&target);
                        self.error = Some(error);
                    }
                    Ok(Observation::Installed | Observation::Current) => {
                        self.failed(value.detail);
                    }
                    Ok(Observation::Stale) => {}
                }
            }
            _ => unreachable!("generated FileDialog dispatch supplied another system event"),
        }
    }

    fn project_filedialog_reply(&mut self, correlation: u64, reply: ApplicationReply) {
        match reply {
            ApplicationReply::FileDialogOpen(snapshot) => self.install_dialog_reply(
                correlation,
                ApplicationIntentEndpoint::FileDialogOpen,
                snapshot,
            ),
            ApplicationReply::FileDialogStop(snapshot) => self.install_dialog_reply(
                correlation,
                ApplicationIntentEndpoint::FileDialogStop,
                snapshot,
            ),
            _ => unreachable!("generated FileDialog dispatch supplied another system reply"),
        }
    }
}
