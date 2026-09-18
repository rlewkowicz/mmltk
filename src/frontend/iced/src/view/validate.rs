pub mod results;
pub mod samples;
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
    // CLEANUP-IGNORE: Validate retains its generated batch-size message before its workspace child message.
    BatchSizeChanged(u64),
    Samples(samples::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    Sample(samples::Message),
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
    samples: samples::Component,
    pub(crate) input: crate::workspace_input::Binding,
}

impl Default for Component {
    fn default() -> Self {
        Self {
            samples: Default::default(),
            input: Default::default(),
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
                        button("Open Dataset")
                            .style(crate::fluent_theme::button_primary)
                            .on_press_maybe(
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
        let progress = crate::view::workflow::progress::compute(
            model
                .workflow
                .validation
                .as_ref()
                .map(|snapshot| &snapshot.operation)
                .filter(|operation| {
                    operation.active
                        || operation.terminal.outcome
                            != crate::generated::ComputeOperationOutcome::Succeeded
                }),
        );
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
                    "",
                    column![
                        text(
                            draft
                                .filter(|value| !value.request.compiledpath.is_empty())
                                .map(|value| value.request.compiledpath.as_str())
                                .or_else(|| model
                                    .settings_snapshot
                                    .as_ref()
                                    .map(|value| value.validationsource.as_str()))
                                .filter(|path| !path.is_empty())
                                .unwrap_or("No dataset selected")
                        )
                        .size(12),
                        dialogs,
                    ]
                    .spacing(crate::view::workflow::FIELD_SPACING)
                )
            ),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Validate,
                model.primary_action_active(crate::generated::FeatureId::Validate),
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        model.compute_start_available(draft, crate::generated::FeatureId::Validate)
                    })
                    .then_some(Message::StartRequested),
                model
                    .compute_stop_available(crate::generated::FeatureId::Validate)
                    .then_some(Message::StopRequested),
                progress,
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let center =
            crate::view::workflow::Composition::new(crate::generated::FeatureId::Validate, width)
                .center_width()
                - 2.0 * crate::view::workflow::CARD_PADDING;
        let half = center / 2.0;
        let paired = surface.and_then(crate::presentation_surface::drawable_validation);
        let headings = iced::widget::row![
            container(text("Metrics")).center_x(iced::Fill).width(half),
            container(text("Validation Preview"))
                .center_x(iced::Fill)
                .width(half),
        ]
        .height(crate::view::aspect_ratio::HEADER_HEIGHT)
        .align_y(iced::Center);
        let atlas = self
            .samples
            .atlas(paired.clone(), settings, self.input.clone())
            .map(Message::Samples);
        let controls = if paired
            .as_ref()
            .is_some_and(|(_, content)| content.metadata.detail)
        {
            iced::widget::space::horizontal().height(47).into()
        } else {
            self.samples.controls(model).map(Message::Samples)
        };
        let base = column![
            headings,
            iced::widget::row![
                container(results::view(model)).width(half),
                container(atlas).width(half)
            ]
            .height(center * 9.0 / 16.0),
            controls,
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .width(center);
        let workspace = if let Some((surface, content)) =
            paired.filter(|(_, content)| content.metadata.detail)
        {
            container(iced::widget::stack![
                base,
                self.samples
                    .detail(surface, content, model, settings, self.input.clone())
                    .map(Message::Samples)
            ])
            .clip(true)
            .into()
        } else {
            base.into()
        };
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
            text(crate::view::workflow::status::compute_status(
                model
                    .workflow
                    .validation
                    .as_ref()
                    .map(|snapshot| &snapshot.operation),
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
            Message::BatchSizeChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    // CLEANUP-IGNORE: Validate applies its generated batch-size edit before workspace routing.
                    crate::generated::edit_workflowsvalidaterequestbatchsize(draft, value)
                })?,
                // CLEANUP-IGNORE: Validate closes its local settings outcome before workspace routing.
            ),
            Message::Samples(message) => {
                let Some(message) = self.samples.update(message) else {
                    return Ok(None);
                };
                Outcome::Sample(message)
            }
        };
        Ok(Some(outcome))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn labels_and_fit_are_local_while_sample_selection_keeps_displayed_identity() {
        let mut component = Component::default();
        let mut settings = crate::view::settings::SettingsModel::default();
        for message in [
            samples::Message::Labels(true, false),
            samples::Message::Labels(false, true),
            samples::Message::Fit,
        ] {
            assert!(
                component
                    .update(&mut settings, Message::Samples(message))
                    .unwrap()
                    .is_none()
            );
        }
        let identity = crate::generated::ValidationSampleIdentity {
            generation: 7,
            datasetindex: 42,
        };
        assert!(
            matches!(component.update(&mut settings, Message::Samples(samples::Message::Select(identity.clone()))).unwrap(),
            Some(Outcome::Sample(samples::Message::Select(actual))) if actual == identity)
        );
    }

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
