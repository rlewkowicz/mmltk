use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, text};

#[derive(Debug, Clone)]
pub enum Message {
    StartRequested,
    StopRequested,
    Workspace(crate::view::workspace::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    StartRequested,
    StopRequested,
    SettingsEdited(crate::view::settings::EditSchedule),
}

#[derive(Default)]
pub struct Component {
    pub(crate) input: crate::workspace_input::Binding,
}

// CLEANUP-IGNORE: Live owns its direct page view and input binding; Export owns model-card state.
impl Component {
    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a crate::view::settings::SettingsModel,
        surface: Option<Surface>,
        width: f32,
    ) -> Element<'a, Message> {
        let settings_edit_available = settings.draft.is_some() && model.settings_edit_available();
        let completed_frames: Element<'a, Message> = model.live_snapshot.as_ref().map_or_else(
            || column![].into(),
            |snapshot| text(format!("Completed frames · {}", snapshot.completedframes)).into(),
        );
        let state_label = model.live_snapshot.as_ref().map_or_else(
            || "Waiting for native Live state".to_owned(),
            |snapshot| {
                format!(
                    "{} · observation {}",
                    if snapshot.running {
                        "Running"
                    } else {
                        "Stopped"
                    },
                    snapshot.revision
                )
            },
        );
        let requested_fps = crate::generated::default_request_liveStartframespersecond()
            .map_or_else(|_| "unavailable".to_owned(), |value| value.to_string());
        let setup: Element<'a, Message> = column![
            crate::view::shared::identified(
                "live.card.capture",
                crate::view::shared::card(
                    "Live capture",
                    "Start and Stop follow the typed Live snapshot.",
                    column![
                        text(format!(
                            "Requested viewport · {} × {} · {} fps",
                            model.window_width, model.window_height, requested_fps
                        )),
                        text(state_label),
                        completed_frames,
                    ]
                    .spacing(8),
                )
            ),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Live,
                "Start Live",
                (!settings.has_local_edits() && model.live_start_available())
                    .then_some(Message::StartRequested),
                column![].into(),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let workspace = crate::view::workflow::workspace(
            surface,
            settings,
            settings_edit_available,
            crate::generated::FeatureId::Live,
            width,
            Message::Workspace,
            self.input
                .for_source(crate::generated::PresentationSourceKind::Live, 0, None),
        );
        let advanced = crate::view::shared::card(
            "Advanced",
            "Direct Live lifecycle controls.",
            column![
                button("Stop Live").on_press_maybe(
                    model
                        .live_stop_available()
                        .then_some(Message::StopRequested),
                ),
            ]
            .spacing(crate::view::workflow::FIELD_SPACING),
        );
        let diagnostics = crate::view::shared::card(
            "Live status",
            "Capture remains active while this page is backgrounded.",
            text(
                model
                    .live_snapshot
                    .as_ref()
                    .map_or("Unavailable", |snapshot| {
                        if snapshot.running {
                            "Running"
                        } else {
                            "Stopped"
                        }
                    }),
            ),
        );
        crate::view::workflow::Regions::new(
            // CLEANUP-IGNORE: Live supplies its generated page identity to the shared compositor.
            crate::generated::FeatureId::Live,
            // CLEANUP-IGNORE: Live supplies local regions through the shared workflow compositor.
            setup,
            workspace,
            advanced,
            diagnostics,
        )
        .render(width)
    }

    pub fn update(
        &mut self,
        settings: &mut crate::view::settings::SettingsModel,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let outcome = match message {
            Message::StartRequested => Outcome::StartRequested,
            // CLEANUP-IGNORE: Live reduces its Stop lifecycle before its independent workspace child.
            Message::StopRequested => Outcome::StopRequested,
            // CLEANUP-IGNORE: Live alone converts its child workspace result into its local outcome.
            Message::Workspace(message) => {
                let Some(schedule) = crate::view::workflow::update_workspace(settings, message)?
                else {
                    return Ok(None);
                };
                Outcome::SettingsEdited(schedule)
            }
        };
        Ok(Some(outcome))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn live_lifecycle_messages_are_distinct() {
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                Message::StartRequested
            ),
            Ok(Some(Outcome::StartRequested))
        ));
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                Message::StopRequested
            ),
            Ok(Some(Outcome::StopRequested))
        ));
    }
}
