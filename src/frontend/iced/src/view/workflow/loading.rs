use crate::fluent_theme::Element;
use crate::generated::{FeatureId, GuiSettingsState, SettingsLeafConstraint, SettingsValueUpdate};
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use iced::widget::{column, container, space, text};

#[derive(Debug, Clone, Copy)]
pub enum Message {
    H2d(bool),
    NumaNode(i32),
}

struct Field<T> {
    value: T,
    constraint: SettingsLeafConstraint,
    edit: fn(&mut GuiSettingsState, T) -> SettingsValueUpdate,
}
struct Binding {
    h2d: Field<bool>,
    node: Field<i32>,
}

fn binding(feature: FeatureId, state: &GuiSettingsState) -> Option<Binding> {
    use crate::generated::*;
    Some(match feature {
        FeatureId::Train => Binding {
            h2d: Field {
                value: state.workflows.train.request.h2ddataloader,
                constraint: constraint_workflowstrainrequesth2ddataloader(),
                edit: edit_workflowstrainrequesth2ddataloader,
            },
            node: Field {
                value: state.workflows.train.request.numanode,
                constraint: constraint_workflowstrainrequestnumanode(),
                edit: edit_workflowstrainrequestnumanode,
            },
        },
        FeatureId::Validate => Binding {
            h2d: Field {
                value: state.workflows.validate.request.h2ddataloader,
                constraint: constraint_workflowsvalidaterequesth2ddataloader(),
                edit: edit_workflowsvalidaterequesth2ddataloader,
            },
            node: Field {
                value: state.workflows.validate.request.numanode,
                constraint: constraint_workflowsvalidaterequestnumanode(),
                edit: edit_workflowsvalidaterequestnumanode,
            },
        },
        FeatureId::Predict => Binding {
            h2d: Field {
                value: state.workflows.predict.request.h2ddataloader,
                constraint: constraint_workflowspredictrequesth2ddataloader(),
                edit: edit_workflowspredictrequesth2ddataloader,
            },
            node: Field {
                value: state.workflows.predict.request.numanode,
                constraint: constraint_workflowspredictrequestnumanode(),
                edit: edit_workflowspredictrequestnumanode,
            },
        },
        FeatureId::Explore => Binding {
            h2d: Field {
                value: state.workflows.explore.h2ddataloader,
                constraint: constraint_workflowsexploreh2ddataloader(),
                edit: edit_workflowsexploreh2ddataloader,
            },
            node: Field {
                value: state.workflows.explore.numanode,
                constraint: constraint_workflowsexplorenumanode(),
                edit: edit_workflowsexplorenumanode,
            },
        },
        _ => return None,
    })
}

pub fn update(
    feature: FeatureId,
    settings: &mut SettingsModel,
    message: Message,
) -> Result<EditSchedule, String> {
    let fields = settings
        .draft
        .as_ref()
        .and_then(|state| binding(feature, state))
        .ok_or_else(|| "Workflow execution settings are unavailable".to_owned())?;
    match message {
        Message::H2d(value) => settings.edit(EditCadence::Immediate, |draft| {
            (fields.h2d.edit)(draft, value)
        }),
        Message::NumaNode(value) => settings.edit(EditCadence::Debounced, |draft| {
            (fields.node.edit)(draft, value)
        }),
    }
}

pub fn view<'a>(
    feature: FeatureId,
    settings: &SettingsModel,
    enabled: bool,
) -> Element<'a, Message> {
    if matches!(feature, FeatureId::Train | FeatureId::Validate | FeatureId::Predict) {
        return space::vertical().height(0).into();
    }
    let Some(fields) = settings
        .draft
        .as_ref()
        .and_then(|state| binding(feature, state))
    else {
        return space::vertical().height(0).into();
    };
    column![
        text("Data loading & execution").size(18),
        container(super::fields::toggle(
            "Use H2D data loading",
            fields.h2d.value,
            enabled,
            Message::H2d
        ))
        .id(fields.h2d.constraint.stable_field_id.to_string()),
        super::fields::number_i32(
            "NUMA node",
            fields.node.value,
            fields.node.constraint,
            enabled,
            Message::NumaNode
        ),
    ]
    .spacing(super::FIELD_SPACING)
    .into()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn generated_execution_controls_preserve_local_drafts_and_edit_cadence() {
        for feature in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Explore,
        ] {
            let mut settings = crate::view::settings::installed_settings_model();
            let before = binding(feature, settings.draft.as_ref().unwrap()).unwrap();
            assert!(before.h2d.value);
            assert_eq!(
                before.node.value,
                crate::generated::default_workflowsexplorenumanode().unwrap()
            );
            assert!(matches!(
                update(feature, &mut settings, Message::H2d(false)).unwrap(),
                EditSchedule::FlushNow
            ));
            assert!(matches!(
                update(feature, &mut settings, Message::NumaNode(3)).unwrap(),
                EditSchedule::Debounce(_)
            ));
            let after = binding(feature, settings.draft.as_ref().unwrap()).unwrap();
            assert!(!after.h2d.value);
            assert_eq!(after.node.value, 3);
            assert_eq!(
                after.node.constraint.stable_field_id,
                before.node.constraint.stable_field_id
            );
        }
    }
}
