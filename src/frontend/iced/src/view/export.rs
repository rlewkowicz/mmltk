use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};

#[derive(Debug, Clone)]
pub enum Message {
    StartRequested,
    StopRequested,
    DialogRequested(u64),
    Model(crate::view::workflow::model_card::Message),
    OnnxOutputPathChanged(String),
    OutputPathChanged(String),
    OpsetChanged(i32),
    Fp16Changed(bool),
    SimplifyChanged(bool),
    Workspace(crate::view::workspace::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    StartRequested,
    StopRequested,
    DialogRequested(u64),
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
                crate::generated::FeatureId::Export,
            ),
        }
    }
}

impl Component {
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
            .map(|settings| &settings.workflows.exportstate);
        let build_tensorrt = draft.map_or_else(
            || crate::generated::default_workflowsexportstatebuildtensorrt().unwrap_or_default(),
            |value| value.buildtensorrt,
        );
        let relevant_dialogs = if build_tensorrt {
            &[crate::generated::constraint_workflowsexportstateoutputpath().stable_field_id][..]
        } else {
            &[crate::generated::constraint_workflowsexportstateonnxoutputpath().stable_field_id][..]
        };
        let dialogs = model
            .workflow
            .dialogs(crate::generated::FeatureId::Export)
            .filter(|fact| relevant_dialogs.contains(&fact.stable_field_id))
            .fold(column![].spacing(6), |column, fact| {
                column.push(
                    container(
                        button(fact.title).on_press_maybe(
                            model
                                .file_dialog_open_available(
                                    fact,
                                    crate::generated::FeatureId::Export,
                                )
                                .then_some(Message::DialogRequested(fact.stable_field_id)),
                        ),
                    )
                    .id(format!("dialog.{}", fact.stable_field_id)),
                )
            });
        let operation = model.workflow.export.as_ref();
        let branch_fields: Element<'a, Message> = if build_tensorrt {
            column![
                crate::view::workflow::fields::text_field(
                    "TensorRT output",
                    crate::generated::constraint_workflowsexportstateoutputpath().stable_field_id,
                    draft.map_or("", |value| value.outputpath.as_str()),
                    settings_edit_available,
                    Message::OutputPathChanged,
                ),
                dialogs,
            ]
            .spacing(crate::view::workflow::FIELD_SPACING)
            .into()
        } else {
            column![
                crate::view::workflow::fields::text_field(
                    "ONNX output",
                    crate::generated::constraint_workflowsexportstateonnxoutputpath()
                        .stable_field_id,
                    draft.map_or("", |value| value.onnxoutputpath.as_str()),
                    settings_edit_available,
                    Message::OnnxOutputPathChanged,
                ),
                dialogs,
            ]
            .spacing(crate::view::workflow::FIELD_SPACING)
            .into()
        };
        let setup: Element<'a, Message> = column![
            self.model_card
                .view(crate::view::workflow::model_card::State::from_settings(
                    crate::generated::FeatureId::Export,
                    settings.draft.as_ref(),
                    model.model_snapshot.as_ref(),
                    model.file_dialog.as_ref(),
                    settings_edit_available,
                    settings_settled
                        && settings.draft.as_ref().is_some_and(|draft| {
                            model.model_selection_available(
                                draft,
                                crate::generated::FeatureId::Export,
                            )
                        }),
                    model.model_stop_available(),
                ))
                .map(Message::Model),
            crate::view::shared::identified(
                "export.card.output",
                crate::view::shared::card(
                    "Export",
                    "Model artifacts, format, and output destination.",
                    column![
                        crate::view::workflow::fields::toggle(
                            "Build TensorRT",
                            build_tensorrt,
                            settings_edit_available,
                            |value| Message::Model(
                                crate::view::workflow::model_card::Message::ExportBuildChanged(
                                    value
                                )
                            ),
                        ),
                        branch_fields,
                    ]
                    .spacing(crate::view::workflow::FIELD_SPACING)
                )
            ),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Export,
                "Export model",
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        settings_settled
                            && model
                                .compute_start_available(draft, crate::generated::FeatureId::Export)
                    })
                    .then_some(Message::StartRequested),
                crate::view::workflow::progress::compute(operation),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let workspace = crate::view::workflow::workspace(
            surface,
            crate::presentation_surface::labels::Source::Hidden,
            settings,
            settings_edit_available,
            crate::generated::FeatureId::Export,
            width,
            Message::Workspace,
            crate::workspace_input::Binding::default(),
        );
        let branch_advanced: Element<'a, Message> = if build_tensorrt {
            crate::view::workflow::fields::toggle(
                "Allow FP16",
                draft.map_or_else(
                    || {
                        crate::generated::default_workflowsexportstateallowfp16()
                            .unwrap_or_default()
                    },
                    |value| value.allowfp16,
                ),
                settings_edit_available,
                Message::Fp16Changed,
            )
        } else {
            column![
                crate::view::workflow::fields::number_i32(
                    "ONNX opset",
                    draft.map_or_else(
                        || {
                            crate::generated::default_workflowsexportstateopsetversion()
                                .unwrap_or_default()
                        },
                        |value| value.opsetversion,
                    ),
                    crate::generated::constraint_workflowsexportstateopsetversion(),
                    settings_edit_available,
                    Message::OpsetChanged,
                ),
                crate::view::workflow::fields::toggle(
                    "Simplify ONNX",
                    draft.map_or_else(
                        || {
                            crate::generated::default_workflowsexportstatesimplify()
                                .unwrap_or_default()
                        },
                        |value| value.simplify,
                    ),
                    settings_edit_available,
                    Message::SimplifyChanged,
                ),
            ]
            .spacing(crate::view::workflow::FIELD_SPACING)
            .into()
        };
        let advanced = crate::view::shared::card(
            "Advanced",
            "Export precision, optimization, and packaging.",
            column![
                branch_advanced,
                button("Stop").on_press_maybe(
                    model
                        .compute_stop_available(crate::generated::FeatureId::Export)
                        .then_some(Message::StopRequested),
                ),
            ]
            .spacing(crate::view::workflow::FIELD_SPACING),
        );
        let diagnostics = crate::view::shared::card(
            "Export status",
            "Canonical artifact outcome.",
            text(crate::view::workflow::status::compute_status(operation)),
        );
        crate::view::workflow::Regions::new(
            crate::generated::FeatureId::Export,
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
            Message::StopRequested => Outcome::StopRequested,
            Message::DialogRequested(id) => Outcome::DialogRequested(id),
            Message::Model(message) => {
                let Some(outcome) = self.model_card.update(message, settings)? else {
                    return Ok(None);
                };
                Outcome::Model(outcome)
            }
            Message::OutputPathChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstateoutputpath(draft, value)
                })?,
            ),
            Message::OnnxOutputPathChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstateonnxoutputpath(draft, value)
                })?,
            ),
            Message::OpsetChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstateopsetversion(draft, value)
                })?,
            ),
            Message::Fp16Changed(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstateallowfp16(draft, value)
                })?,
            ),
            Message::SimplifyChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstatesimplify(draft, value)
                })?,
            ),
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
    fn export_dialog_retains_generated_identity() {
        let id = crate::generated::FILE_DIALOGS
            .iter()
            .find(|fact| {
                fact.workflows
                    .contains(&crate::generated::FeatureId::Export)
            })
            .expect("generated Export dialog")
            .stable_field_id;
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                Message::DialogRequested(id)
            ),
            Ok(Some(Outcome::DialogRequested(value))) if value == id
        ));
    }
}
