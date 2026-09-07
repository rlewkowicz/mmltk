use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, text};

#[derive(Debug, Clone)]
pub enum Message {
    Loading(crate::view::workflow::loading::Message),
    StartRequested,
    StopRequested,
    Model(crate::view::workflow::model_card::Message),
    OutputPathChanged(String),
    ThresholdChanged(f32),
    BatchSizeChanged(u64),
    Workspace(crate::view::workspace::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    StartRequested,
    // CLEANUP-IGNORE: Predict retains its local domain outcome and workflow-bound child component ownership.
    StopRequested,
    // CLEANUP-IGNORE: Predict's local settings outcome precedes its distinct model-card outcomes.
    SettingsEdited(crate::view::settings::EditSchedule),
    Model(crate::view::workflow::model_card::Outcome),
}

pub struct Component {
    model_card: crate::view::workflow::model_card::Component,
}

impl Default for Component {
    fn default() -> Self {
        Self {
            model_card: crate::view::workflow::model_card::Component::new(
                // CLEANUP-IGNORE: Predict binds the child owner to its generated feature.
                crate::generated::FeatureId::Predict,
            ),
        }
    }
}

impl Component {
    // CLEANUP-IGNORE: Predict owns this local view entry point.
    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a crate::view::settings::SettingsModel,
        surface: Option<Surface>,
        width: f32,
    ) -> Element<'a, Message> {
        let settings_edit_available = settings.draft.is_some() && model.settings_edit_available();
        let settings_settled = !settings.has_local_edits();
        let draft = settings
            .draft
            .as_ref()
            .map(|settings| &settings.workflows.predict);
        let operation = model
            .predict_snapshot
            .as_ref()
            .map(|snapshot| &snapshot.operation);
        let setup: Element<'a, Message> = column![
            self.model_card
                .view(crate::view::workflow::model_card::State::from_settings(
                    crate::generated::FeatureId::Predict,
                    settings.draft.as_ref(),
                    model.model_snapshot.as_ref(),
                    model.file_dialog.as_ref(),
                    settings_edit_available,
                    settings_settled
                        && settings.draft.as_ref().is_some_and(|draft| {
                            model.model_selection_available(
                                draft,
                                crate::generated::FeatureId::Predict,
                            )
                        }),
                    model.model_stop_available(),
                ))
                .map(Message::Model),
            crate::view::shared::identified(
                "predict.card.inputs",
                crate::view::shared::card(
                    "Prediction",
                    "The direct predictor consumes the current compiled training artifact.",
                    column![
                        text(settings.draft.as_ref().map_or(
                            "Compiled training input unavailable",
                            |settings| {
                                settings.workflows.train.request.traincompiledpath.as_str()
                            },
                        )),
                        crate::view::workflow::fields::text_field(
                            "Output JSON",
                            crate::generated::constraint_workflowspredictrequestoutputpath()
                                .stable_field_id,
                            draft.map_or("", |value| value.request.outputpath.as_str()),
                            settings_edit_available,
                            Message::OutputPathChanged,
                        ),
                    ]
                    .spacing(crate::view::workflow::FIELD_SPACING),
                )
            ),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Predict,
                "Run prediction",
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        settings_settled
                            && model.compute_start_available(
                                draft,
                                crate::generated::FeatureId::Predict,
                            )
                    })
                    .then_some(Message::StartRequested),
                crate::view::workflow::progress::compute(operation),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let workspace = crate::view::workflow::workspace(
            surface,
            settings,
            settings_edit_available,
            crate::generated::FeatureId::Predict,
            width,
            Message::Workspace,
        );
        let advanced = crate::view::shared::card(
            "Advanced",
            "Prediction thresholds and preview behavior.",
            column![
                crate::view::workflow::loading::view(
                    crate::generated::FeatureId::Predict,
                    settings,
                    settings_edit_available
                )
                .map(Message::Loading),
                crate::view::workflow::fields::number_f32(
                    "Preview threshold",
                    draft.map_or(0.0, |value| value.request.threshold),
                    crate::generated::constraint_workflowspredictrequestthreshold(),
                    settings_edit_available,
                    Message::ThresholdChanged,
                ),
                crate::view::workflow::fields::number_u64(
                    "Batch size",
                    draft.map_or(0, |value| value.request.batchsize),
                    crate::generated::constraint_workflowspredictrequestbatchsize(),
                    settings_edit_available,
                    Message::BatchSizeChanged,
                ),
                button("Stop").on_press_maybe(
                    model
                        .compute_stop_available(crate::generated::FeatureId::Predict)
                        .then_some(Message::StopRequested),
                ),
            ]
            .spacing(crate::view::workflow::FIELD_SPACING),
        );
        let diagnostics = crate::view::shared::card(
            "Prediction status",
            "Canonical result and frame activity.",
            text(crate::view::shared::compute_status(operation)),
        );
        crate::view::workflow::Regions::new(
            // CLEANUP-IGNORE: Predict supplies its generated page identity to the shared compositor.
            crate::generated::FeatureId::Predict,
            // CLEANUP-IGNORE: Predict supplies local regions through the shared workflow compositor.
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
            Message::Loading(message) => {
                Outcome::SettingsEdited(crate::view::workflow::loading::update(
                    crate::generated::FeatureId::Predict,
                    settings,
                    message,
                )?)
            }
            Message::StartRequested => Outcome::StartRequested,
            // CLEANUP-IGNORE: Predict's Stop arm precedes its distinct child-domain reduction.
            Message::StopRequested => Outcome::StopRequested,
            // CLEANUP-IGNORE: Predict maps the model-card child outcome into its local domain.
            Message::Model(message) => {
                let Some(outcome) = self.model_card.update(message, settings)? else {
                    return Ok(None);
                };
                Outcome::Model(outcome)
            }
            Message::OutputPathChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowspredictrequestoutputpath(draft, value)
                })?,
            ),
            Message::ThresholdChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Immediate, |draft| {
                    crate::generated::edit_workflowspredictrequestthreshold(draft, value)
                })?,
            ),
            Message::BatchSizeChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    // CLEANUP-IGNORE: Predict applies its generated batch-size edit before workspace routing.
                    crate::generated::edit_workflowspredictrequestbatchsize(draft, value)
                })?,
                // CLEANUP-IGNORE: Predict closes its local settings outcome before workspace routing.
            ),
            // CLEANUP-IGNORE: Predict alone converts its child workspace result into its local outcome.
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
    fn prediction_stop_is_a_domain_outcome() {
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                Message::StopRequested
            ),
            Ok(Some(Outcome::StopRequested))
        ));
    }
}
