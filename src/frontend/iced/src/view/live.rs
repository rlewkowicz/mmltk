use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view::shared::status_text;
use crate::view::workflow::overlay_controls::{self, KeypointSettingsMessage};
use crate::view_model::ApplicationModel;
use iced::widget::{button, column};

#[derive(Debug, Clone)]
pub enum Message {
    Product(crate::view::image_viewer::ProductAction),
    Model(crate::view::workflow::model_card::Message),
    AnalysisChanged(bool),
    KeypointSettings(KeypointSettingsMessage),
    StartRequested,
    StopRequested,
    Workspace(crate::view::workspace::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    Product(crate::view::image_viewer::ProductAction),
    Model(crate::view::workflow::model_card::Outcome),
    StartRequested,
    StopRequested,
    // CLEANUP-IGNORE: Separate pages own their message type, input binding and child state; the match crosses that ownership boilerplate and a view signature.
    SettingsEdited(crate::view::settings::EditSchedule),
}

pub struct Component {
    pub(crate) input: crate::workspace_input::Binding,
    model_card: crate::view::workflow::model_card::Component,
}

impl Default for Component {
    fn default() -> Self {
        Self {
            input: Default::default(),
            model_card: crate::view::workflow::model_card::Component::new(
                crate::generated::FeatureId::Predict,
            ),
        }
    }
}
// CLEANUP-IGNORE: Live owns its direct page view, input binding and Predict-bound model-card state.
impl Component {
    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a crate::view::settings::SettingsModel,
        surface: Option<Surface>,
        width: f32,
    ) -> Element<'a, Message> {
        let settings_edit_available = settings.draft.is_some()
            && model.settings_edit_available()
            && !model.primary_action_active(crate::generated::FeatureId::Live);
        let draft = settings
            .draft
            .as_ref()
            .map(|draft| &draft.workflows.predict);
        let completed_frames: Element<'a, Message> = model.live_snapshot.as_ref().map_or_else(
            || column![].into(),
            |snapshot| {
                status_text(format!("Completed frames · {}", snapshot.completedframes)).into()
            },
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
                    "Capture runs without analysis by default. Analysis uses the shared Predict model and settings.",
                    column![
                        status_text(format!(
                            "Requested viewport · {} × {} · {} fps",
                            model.window_width, model.window_height, requested_fps
                        )),
                        crate::view::shared::identified("live.analysis.enabled", crate::view::workflow::fields::toggle("Analyze captured frames", draft.is_some_and(|value| value.liveanalysisenabled), settings_edit_available, Message::AnalysisChanged)),
                        status_text("Analysis requires a prepared ONNX or TensorRT model on the capture device."),
                        crate::view::shared::identified("live.keypoints", crate::view::workflow::fields::toggle("Skeletons", draft.is_none_or(|value| value.showkeypoints), settings_edit_available, |value| Message::KeypointSettings(KeypointSettingsMessage::Keypoints(value)))),
                        crate::view::shared::identified("live.uncertainty", crate::view::workflow::fields::toggle("Joint uncertainty", draft.is_some_and(|value| value.showuncertainty), settings_edit_available, |value| Message::KeypointSettings(KeypointSettingsMessage::Uncertainty(value)))),
                        status_text(state_label),
                        completed_frames,
                    ]
                    .spacing(8),
                )
            ),
            self.model_card.view(crate::view::workflow::model_card::State::from_settings(
                crate::generated::FeatureId::Predict, settings.draft.as_ref(), model.model_snapshot.as_ref(), model.file_dialog.as_ref(), settings_edit_available,
                !settings.has_local_edits() && settings.draft.as_ref().is_some_and(|draft| model.model_selection_available(draft, crate::generated::FeatureId::Predict)), model.model_stop_available()
            )).map(Message::Model),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Live,
                model.primary_action_active(crate::generated::FeatureId::Live),
                (!settings.has_local_edits() && model.live_start_available())
                    .then_some(Message::StartRequested),
                model
                    .live_stop_available()
                    .then_some(Message::StopRequested),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let paired = surface.and_then(crate::presentation_surface::drawable_prediction);
        let surface = paired.as_ref().map(|(surface, _)| *surface).or(surface);
        let labels = paired.map_or(
            crate::presentation_surface::labels::Source::Hidden,
            |(_, objects)| crate::presentation_surface::labels::Source::Prediction(objects),
        );
        let workspace = crate::view::workflow::workspace(
            surface,
            labels,
            settings,
            settings_edit_available,
            crate::generated::FeatureId::Live,
            width,
            Message::Workspace,
            self.input
                .for_source(crate::generated::PresentationSourceKind::Live, 0, None),
        );
        let workspace = column![
            workspace,
            crate::view::image_viewer::product_actions(crate::generated::FeatureId::Live, model)
                .map(Message::Product)
        ]
        .spacing(8)
        .into();
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
        crate::view::workflow::Regions::new(
            // CLEANUP-IGNORE: Live supplies its generated page identity to the shared compositor.
            crate::generated::FeatureId::Live,
            // CLEANUP-IGNORE: Live supplies local regions through the shared workflow compositor.
            setup,
            workspace,
            advanced,
            None,
        )
        // CLEANUP-IGNORE: This match crosses the completed shared compositor call and the independent page reducer signature.
        .render(width)
    }

    pub fn update(
        &mut self,
        settings: &mut crate::view::settings::SettingsModel,
        file_dialog: Option<&crate::generated::FileDialogSnapshot>,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let outcome = match message {
            Message::Product(action) => Outcome::Product(action),
            Message::Model(message) => {
                let Some(outcome) = self.model_card.update(message, file_dialog, settings)? else {
                    return Ok(None);
                };
                Outcome::Model(outcome)
            }
            Message::AnalysisChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Immediate, |draft| {
                    crate::generated::edit_workflowspredictliveanalysisenabled(draft, value)
                })?,
            ),
            Message::KeypointSettings(message) => Outcome::SettingsEdited(
                overlay_controls::update_keypoint_settings(settings, message)?,
            ),
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
                None,
                Message::StartRequested
            ),
            Ok(Some(Outcome::StartRequested))
        ));
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                None,
                Message::StopRequested
            ),
            Ok(Some(Outcome::StopRequested))
        ));
    }
}
