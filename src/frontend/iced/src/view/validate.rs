// CLEANUP-IGNORE: Validate declares the concrete dependencies required by its independent Iced component.
use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};

#[derive(Debug, Clone)]
pub enum Message {
    Loading(crate::view::workflow::loading::Message),
    StartRequested,
    StopRequested,
    DialogRequested(u64),
    Model(crate::view::workflow::model_card::Message),
    CompiledPathChanged(String),
    // CLEANUP-IGNORE: Validate retains its generated batch-size message before its workspace child message.
    BatchSizeChanged(u64),
    Workspace(crate::view::workspace::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    StartRequested,
    StopRequested,
    // CLEANUP-IGNORE: Validate retains its local dialog outcome and workflow-bound child component ownership.
    DialogRequested(u64),
    // CLEANUP-IGNORE: Validate's local settings outcome precedes its distinct model-card outcomes.
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
                // CLEANUP-IGNORE: Validate binds the child owner to its generated feature.
                crate::generated::FeatureId::Validate,
            ),
        }
    }
}

impl Component {
    // CLEANUP-IGNORE: Validate owns this local view entry point.
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
            .map(|settings| &settings.workflows.validate);
        let relevant_dialogs =
            [crate::generated::constraint_workflowsvalidaterequestcompiledpath().stable_field_id];
        let dialogs = model
            .workflow
            .dialogs(crate::generated::FeatureId::Validate)
            .filter(|fact| relevant_dialogs.contains(&fact.stable_field_id))
            .fold(column![].spacing(6), |column, fact| {
                column.push(
                    container(
                        button(fact.title).on_press_maybe(
                            model
                                .file_dialog_open_available(
                                    fact,
                                    crate::generated::FeatureId::Validate,
                                )
                                .then_some(Message::DialogRequested(fact.stable_field_id)),
                        ),
                    )
                    .id(format!("dialog.{}", fact.stable_field_id)),
                )
            });
        let progress = crate::view::workflow::progress::compute(model.workflow.validation.as_ref());
        let setup = column![
            self.model_card
                .view(crate::view::workflow::model_card::State::from_settings(
                    crate::generated::FeatureId::Validate,
                    settings.draft.as_ref(),
                    model.model_snapshot.as_ref(),
                    model.file_dialog.as_ref(),
                    settings_edit_available,
                    settings_settled
                        && settings.draft.as_ref().is_some_and(|draft| {
                            model.model_selection_available(
                                draft,
                                crate::generated::FeatureId::Validate,
                            )
                        }),
                    model.model_stop_available(),
                ))
                .map(Message::Model),
            crate::view::shared::identified(
                "validate.card.inputs",
                crate::view::shared::card(
                    "Validation",
                    "Compiled dataset and validation inputs.",
                    column![
                        crate::view::workflow::fields::text_field(
                            "Compiled dataset",
                            crate::generated::constraint_workflowsvalidaterequestcompiledpath()
                                .stable_field_id,
                            draft.map_or("", |value| value.request.compiledpath.as_str()),
                            settings_edit_available,
                            Message::CompiledPathChanged,
                        ),
                        dialogs,
                    ]
                    .spacing(crate::view::workflow::FIELD_SPACING)
                )
            ),
            text(model.workflow.start_detail(crate::generated::FeatureId::Validate)),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Validate,
                "Run validation",
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        model.compute_start_available(
                                draft,
                                crate::generated::FeatureId::Validate,
                            )
                    })
                    .then_some(Message::StartRequested),
                progress,
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let workspace = crate::view::workflow::workspace(
            surface,
            settings,
            settings_edit_available,
            crate::generated::FeatureId::Validate,
            width,
            Message::Workspace,
            crate::workspace_input::Binding::default(),
        );
        let advanced = crate::view::shared::card(
            "Advanced",
            "Validation execution and generated constraints.",
            column![
                crate::view::workflow::loading::view(
                    crate::generated::FeatureId::Validate,
                    settings,
                    settings_edit_available
                )
                .map(Message::Loading),
                crate::view::workflow::fields::number_u64(
                    "Batch size",
                    draft.map_or(0, |value| value.request.batchsize),
                    crate::generated::constraint_workflowsvalidaterequestbatchsize(),
                    settings_edit_available,
                    Message::BatchSizeChanged,
                ),
                button("Stop").on_press_maybe(
                    model
                        .compute_stop_available(crate::generated::FeatureId::Validate)
                        .then_some(Message::StopRequested),
                ),
            ]
            .spacing(crate::view::workflow::FIELD_SPACING),
        );
        let diagnostics = crate::view::shared::card(
            "Validation status",
            "Canonical native operation outcome.",
            text(crate::view::shared::compute_status(
                model.workflow.validation.as_ref(),
            )),
        );
        crate::view::workflow::Regions::new(
            // CLEANUP-IGNORE: Validate supplies its generated page identity to the shared compositor.
            crate::generated::FeatureId::Validate,
            // CLEANUP-IGNORE: Validate supplies local regions through the shared workflow compositor.
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
                    crate::generated::FeatureId::Validate,
                    settings,
                    message,
                )?)
            }
            Message::StartRequested => Outcome::StartRequested,
            Message::StopRequested => Outcome::StopRequested,
            // CLEANUP-IGNORE: Validate maps its dialog identity before the distinct model-card child outcome.
            Message::DialogRequested(id) => Outcome::DialogRequested(id),
            Message::Model(message) => {
                let Some(outcome) = self.model_card.update(message, settings)? else {
                    return Ok(None);
                };
                Outcome::Model(outcome)
            }
            Message::CompiledPathChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsvalidaterequestcompiledpath(draft, value)
                })?,
            ),
            Message::BatchSizeChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    // CLEANUP-IGNORE: Validate applies its generated batch-size edit before workspace routing.
                    crate::generated::edit_workflowsvalidaterequestbatchsize(draft, value)
                })?,
                // CLEANUP-IGNORE: Validate closes its local settings outcome before workspace routing.
            ),
            // CLEANUP-IGNORE: Validate alone converts its child workspace result into its local outcome.
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
    fn validation_start_is_a_domain_outcome() {
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                Message::StartRequested
            ),
            Ok(Some(Outcome::StartRequested))
        ));
    }
}
