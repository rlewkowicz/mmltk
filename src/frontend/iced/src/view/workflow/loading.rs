use crate::fluent_theme::Element;
use crate::generated::{FeatureId, data_loading_binding as binding};
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use iced::widget::{column, container, space, text};

#[derive(Debug, Clone, Copy)]
pub enum Message {
    H2d(bool),
    NumaNode(i32),
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
            (fields.h2ddataloader.edit)(draft, value)
        }),
        Message::NumaNode(value) => settings.edit(EditCadence::Debounced, |draft| {
            (fields.numanode.edit)(draft, value)
        }),
    }
}

pub fn view<'a>(
    feature: FeatureId,
    settings: &SettingsModel,
    enabled: bool,
) -> Element<'a, Message> {
    if feature != FeatureId::Explore {
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
            fields.h2ddataloader.value,
            enabled,
            Message::H2d
        ))
        .id(fields.h2ddataloader.constraint.stable_field_id.to_string()),
        super::fields::number_i32(
            "NUMA node",
            fields.numanode.value,
            fields.numanode.constraint,
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
            FeatureId::Live,
            FeatureId::Explore,
        ] {
            let mut settings = crate::view::settings::installed_settings_model();
            let before = binding(feature, settings.draft.as_ref().unwrap()).unwrap();
            assert!(before.h2ddataloader.value);
            assert_eq!(
                before.numanode.value,
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
            assert!(!after.h2ddataloader.value);
            assert_eq!(after.numanode.value, 3);
            assert_eq!(
                after.numanode.constraint.stable_field_id,
                before.numanode.constraint.stable_field_id
            );
            if feature == FeatureId::Live {
                let predict = binding(FeatureId::Predict, settings.draft.as_ref().unwrap()).unwrap();
                assert_eq!(after.h2ddataloader.constraint.stable_field_id,
                    predict.h2ddataloader.constraint.stable_field_id);
                assert_eq!(after.numanode.constraint.stable_field_id,
                    predict.numanode.constraint.stable_field_id);
            }
        }
    }
}
