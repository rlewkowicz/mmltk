use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view::shared::status_text;
use crate::view_model::ApplicationModel;
use iced::widget::{button, column};

#[derive(Debug, Clone)]
pub enum Message {
    Output(crate::view::workflow::output::Message),
    StartRequested,
    StopRequested,
    Model(crate::view::workflow::model_card::Message),
    OnnxChanged(bool),
    TensorRtChanged(bool),
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
        let operation = model.workflow.export.as_ref();
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
                "export.card.formats",
                crate::view::shared::card(
                    "Export",
                    "Model artifacts, format, and output destination.",
                    column![
                        crate::view::workflow::fields::toggle(
                            "ONNX",
                            draft.map_or(true, |value| value.exportonnx),
                            settings_edit_available,
                            Message::OnnxChanged
                        ),
                        crate::view::workflow::fields::toggle(
                            "TensorRT",
                            build_tensorrt,
                            settings_edit_available,
                            Message::TensorRtChanged
                        ),
                    ]
                    .spacing(crate::view::workflow::FIELD_SPACING)
                )
            ),
            crate::view::workflow::output::view(
                crate::generated::FeatureId::Export,
                model,
                settings,
                model
                    .workflow
                    .export
                    .as_ref()
                    .map(|operation| &operation.output)
            )
            .map(Message::Output),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Export,
                model.primary_action_active(crate::generated::FeatureId::Export),
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        settings_settled
                            && model
                                .compute_start_available(draft, crate::generated::FeatureId::Export)
                    })
                    .then_some(Message::StartRequested),
                model
                    .compute_stop_available(crate::generated::FeatureId::Export)
                    .then_some(Message::StopRequested),
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
        let branch_advanced = |build_tensorrt| {
            let fields: Element<'a, Message> = if build_tensorrt {
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
            fields
        };
        let branch_advanced = column![
            crate::view::shared::disclosure(
                "export.advanced.tensorrt",
                build_tensorrt,
                branch_advanced(true)
            ),
            crate::view::shared::disclosure(
                "export.advanced.onnx",
                draft.is_some_and(|value| value.exportonnx || value.buildtensorrt),
                branch_advanced(false)
            ),
        ];
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
            status_text(crate::view::workflow::status::compute_status(operation)),
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
        file_dialog: Option<&crate::generated::FileDialogSnapshot>,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let outcome = match message {
            Message::Output(crate::view::workflow::output::Message::Browse(id)) => {
                Outcome::DialogRequested(id)
            }
            Message::Output(crate::view::workflow::output::Message::Auto(value)) => {
                Outcome::SettingsEdited(crate::view::workflow::output::automatic(
                    settings,
                    crate::generated::FeatureId::Export,
                    value,
                )?)
            }
            Message::StartRequested => Outcome::StartRequested,
            Message::StopRequested => Outcome::StopRequested,
            Message::Model(message) => {
                let Some(outcome) = self.model_card.update(message, file_dialog, settings)? else {
                    return Ok(None);
                };
                Outcome::Model(outcome)
            }
            Message::OnnxChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstateexportonnx(draft, value)
                })?,
            ),
            Message::TensorRtChanged(value) => Outcome::SettingsEdited(
                settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
                    crate::generated::edit_workflowsexportstatebuildtensorrt(draft, value)
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
        let id = crate::generated::constraint_workflowsexportstateoutputdirectory().stable_field_id;
        assert!(matches!(
            Component::default().update(
                &mut crate::view::settings::SettingsModel::default(),
                None,
                Message::Output(crate::view::workflow::output::Message::Browse(id))
            ),
            Ok(Some(Outcome::DialogRequested(value))) if value == id
        ));
    }
}

#[cfg(test)]
mod format_tests {
    use super::*;
    use crate::generated::FeatureId;
    use crate::view::settings::installed_settings_model;
    use crate::view_model::test_support::{accepted_model_for, bootstrapped};

    #[test]
    fn format_controls_preserve_prepared_weights_and_reject_an_empty_run() {
        let mut settings = installed_settings_model();
        let mut component = Component::default();
        let mut model = bootstrapped();
        let initial = settings.draft.as_ref().unwrap();
        assert!(initial.workflows.exportstate.exportonnx);
        assert!(initial.workflows.exportstate.buildtensorrt);
        model.model_snapshot = Some(accepted_model_for(&model, initial, FeatureId::Export));
        let selected = model.model_snapshot.clone();
        for onnx in [false, true] {
            for engine in [false, true] {
                component
                    .update(&mut settings, None, Message::OnnxChanged(onnx))
                    .unwrap();
                component
                    .update(&mut settings, None, Message::TensorRtChanged(engine))
                    .unwrap();
                let draft = settings.draft.as_ref().unwrap();
                assert!(model.model_selection_matches(draft, FeatureId::Export));
                assert_eq!(model.model_snapshot, selected);
                if !onnx && !engine {
                    assert!(!model.compute_start_available(draft, FeatureId::Export));
                    assert!(!model.model_selection_available(draft, FeatureId::Export));
                }
            }
        }
    }
}
