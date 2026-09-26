use crate::fluent_theme::Element;
use iced::widget::{checkbox, column, row, space, text, text_input};
use iced::{Fill, Length};

const NUMERIC_INPUT_PORTION: u16 = 2;
const NUMERIC_TRAILING_PORTION: u16 = 3;

pub fn numeric_grid<'a, Message>() -> iced::widget::Grid<'a, Message, crate::fluent_theme::Theme> {
    iced::widget::Grid::new()
        .columns(4)
        .height(iced::Length::Shrink)
        .spacing(10)
}

fn labeled_field<'a, Message: 'a>(
    label: &'static str,
    control: Element<'a, Message>,
) -> Element<'a, Message> {
    column![text(label).size(12), control]
        .spacing(super::FIELD_SPACING)
        .into()
}

fn labeled_numeric_field<'a, Message: 'a>(
    label: &'static str,
    control: Element<'a, Message>,
) -> Element<'a, Message> {
    column![
        text(label).size(12),
        row![
            control,
            space::horizontal().width(Length::FillPortion(NUMERIC_TRAILING_PORTION)),
        ]
        .width(Fill),
    ]
    .spacing(super::FIELD_SPACING)
    .into()
}

pub fn text_field<'a, Message: Clone + 'a>(
    label: &'static str,
    stable_field_id: u64,
    value: &'a str,
    enabled: bool,
    on_input: impl Fn(String) -> Message + 'a,
) -> Element<'a, Message> {
    labeled_field(
        label,
        text_input(label, value)
            .id(stable_field_id.to_string())
            .on_input_maybe(enabled.then_some(on_input))
            .width(Fill)
            .into(),
    )
}

pub fn number_i32<'a, Message: Clone + 'a>(
    label: &'static str,
    value: i32,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(i32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.map_or(i32::MIN, |value| value as i32);
    let maximum = constraint.maximum.map_or(i32::MAX, |value| value as i32);
    // CLEANUP-IGNORE: Integer adapters retain their generated field type and callback without extra numeric traits.
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn number_u64<'a, Message: Clone + 'a>(
    label: &'static str,
    value: u64,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(u64) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.map_or(u64::MIN, |value| value as u64);
    let maximum = constraint.maximum.map_or(u64::MAX, |value| value as u64);
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn number_f32<'a, Message: Clone + 'a>(
    label: &'static str,
    value: f32,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(f32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    number_f32_input(label, value, constraint, enabled, false, on_input)
}

pub fn decimal_f32<'a, Message: Clone + 'a>(
    label: &'static str,
    value: f32,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(f32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    number_f32_input(label, value, constraint, enabled, true, on_input)
}

fn number_f32_input<'a, Message: Clone + 'a>(
    label: &'static str,
    value: f32,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    typed_only: bool,
    on_input: impl Fn(f32) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.map_or(f32::MIN, |value| value as f32);
    let maximum = constraint.maximum.map_or(f32::MAX, |value| value as f32);
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .step(0.01)
            .typed_only(typed_only)
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn number_f64<'a, Message: Clone + 'a>(
    label: &'static str,
    value: f64,
    constraint: crate::generated::SettingsLeafConstraint,
    enabled: bool,
    on_input: impl Fn(f64) -> Message + Copy + 'a,
) -> Element<'a, Message> {
    let stable_field_id = constraint.stable_field_id;
    let minimum = constraint.minimum.unwrap_or(f64::MIN);
    let maximum = constraint.maximum.unwrap_or(f64::MAX);
    labeled_numeric_field(
        label,
        iced_aw::number_input(&value, minimum..=maximum, on_input)
            .id(stable_field_id.to_string())
            .step(0.0001)
            .ignore_scroll(true)
            .ignore_buttons(true)
            .on_input_maybe(enabled.then_some(on_input))
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

/// Read-only product facts retain ordinary input styling and have no edit callback.
pub fn read_only<'a, Message: Clone + 'a>(
    label: &'static str,
    id: impl Into<String>,
    value: String,
) -> Element<'a, Message> {
    labeled_numeric_field(
        label,
        text_input("", &value)
            .id(id.into())
            .style(|theme, _| {
                iced_fluent_theme::text_input::default(
                    theme,
                    iced::widget::text_input::Status::Active,
                )
            })
            .width(Length::FillPortion(NUMERIC_INPUT_PORTION))
            .into(),
    )
}

pub fn execution_facts<'a>(
    feature: crate::generated::FeatureId,
    training_validation: bool,
    model: &'a crate::view_model::ApplicationModel,
    settings: &crate::view::settings::SettingsModel,
) -> Option<&'a crate::generated::ExecutionFacts> {
    use crate::generated::FeatureId;
    let accepted = model.settings_snapshot.as_ref()?;
    let draft = settings.draft.as_ref()?;
    let matching = match feature {
        FeatureId::Train => {
            draft.workflows.train.request == accepted.settingsstate.workflows.train.request
        }
        FeatureId::Validate => {
            draft.workflows.validate.request == accepted.settingsstate.workflows.validate.request
        }
        FeatureId::Predict => {
            draft.workflows.predict.request == accepted.settingsstate.workflows.predict.request
        }
        _ => false,
    };
    if !matching || (feature == FeatureId::Train && settings.training_membership_pending()) {
        return None;
    }
    if let Some(operation) = model.current_native_operation(feature) {
        let facts = match feature {
            FeatureId::Train => {
                let state = model.workflow.training.as_ref()?;
                let execution = state.sources.execution.as_ref()?;
                if training_validation {
                    &execution.validation
                } else {
                    &execution.training
                }
            }
            FeatureId::Validate => {
                let state = model.workflow.validation.as_ref()?;
                &state.execution
            }
            FeatureId::Predict => {
                let state = model.predict_snapshot.as_ref()?;
                &state.execution
            }
            _ => return None,
        };
        return (facts.operationgeneration == operation.generationfrontier
            && facts.admittedcapacity > 0)
            .then_some(facts);
    }
    if model.primary_action_active(feature) {
        return None;
    }
    let facts = match feature {
        FeatureId::Train if training_validation => &accepted.trainingvalidationexecution,
        FeatureId::Train => &accepted.trainexecution,
        FeatureId::Validate => &accepted.validationexecution,
        FeatureId::Predict => &accepted.predictionexecution,
        _ => return None,
    };
    (facts.settingsrevision == accepted.revision).then_some(facts)
}

pub fn effective_batch<'a, Message: Clone + 'a>(
    feature: crate::generated::FeatureId,
    validation: bool,
    model: &crate::view_model::ApplicationModel,
    settings: &crate::view::settings::SettingsModel,
) -> Element<'a, Message> {
    let facts = execution_facts(feature, validation, model, settings);
    let label = if feature == crate::generated::FeatureId::Train && !validation {
        "Effective batch / model"
    } else if facts.is_some_and(|facts| facts.admittedcapacity > 0) {
        "Effective batch (admitted)"
    } else {
        "Effective batch (pre-admission)"
    };
    read_only(
        label,
        format!("{feature:?}.{validation}.effective_batch"),
        facts.map_or_else(
            || "Updating".into(),
            |facts| facts.effectivebatchpermodel.to_string(),
        ),
    )
}

pub fn toggle<'a, Message: Clone + 'a>(
    label: &'static str,
    value: bool,
    enabled: bool,
    on_toggle: impl Fn(bool) -> Message + 'a,
) -> Element<'a, Message> {
    let control = checkbox(value).label(label);
    if enabled {
        control.on_toggle(on_toggle).into()
    } else {
        control.into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_numeric_adapter_uses_the_uniform_compact_layout_policy() {
        assert_eq!(NUMERIC_INPUT_PORTION, 2);
        assert_eq!(NUMERIC_TRAILING_PORTION, 3);
        assert_eq!(
            NUMERIC_INPUT_PORTION + NUMERIC_TRAILING_PORTION,
            5,
            "integer and floating adapters share one two-fifths input width"
        );
    }
    #[test]
    fn effective_facts_require_matching_drafts_revisions_and_admitted_generation() {
        use crate::generated::*;
        let mut model = crate::view_model::test_support::bootstrapped();
        let snapshot = model.settings_snapshot.as_mut().unwrap();
        assert_eq!(
            [
                snapshot.settingsstate.workflows.train.request.lanes,
                snapshot
                    .settingsstate
                    .workflows
                    .train
                    .request
                    .validationlanes,
                snapshot.settingsstate.workflows.validate.request.lanes,
                snapshot.settingsstate.workflows.predict.request.lanes
            ],
            [1; 4]
        );
        snapshot.trainexecution.settingsrevision = snapshot.revision;
        snapshot.trainexecution.effectivebatchpermodel = 24;
        snapshot.trainexecution.aggregateroundimages = 72;
        let mut settings = crate::view::settings::SettingsModel::default();
        settings.install(snapshot);
        assert_eq!(
            execution_facts(FeatureId::Train, false, &model, &settings)
                .unwrap()
                .effectivebatchpermodel,
            24
        );
        settings
            .draft
            .as_mut()
            .unwrap()
            .workflows
            .train
            .request
            .batchsize += 1;
        assert!(execution_facts(FeatureId::Train, false, &model, &settings).is_none());
        settings.install(model.settings_snapshot.as_ref().unwrap());
        model
            .settings_snapshot
            .as_mut()
            .unwrap()
            .trainexecution
            .settingsrevision += 1;
        assert!(execution_facts(FeatureId::Train, false, &model, &settings).is_none());
        let state = model.workflow.training.as_mut().unwrap();
        state.local.active = true;
        state.local.generationfrontier = 7;
        let mut execution = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .trainexecution
            .clone();
        execution.operationgeneration = 6;
        execution.admittedcapacity = 2;
        let mut retained = crate::view_model::test_support::saved_training_run(
            settings
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .clone(),
        )
        .run
        .unwrap()
        .execution;
        retained.training = execution.clone();
        retained.validation = execution;
        state.sources.execution = Some(retained);
        assert!(execution_facts(FeatureId::Train, false, &model, &settings).is_none());
        model
            .workflow
            .training
            .as_mut()
            .unwrap()
            .sources
            .execution
            .as_mut()
            .unwrap()
            .training
            .operationgeneration = 7;
        assert_eq!(
            execution_facts(FeatureId::Train, false, &model, &settings)
                .unwrap()
                .admittedcapacity,
            2
        );
        model
            .workflow
            .training
            .as_mut()
            .unwrap()
            .sources
            .execution
            .as_mut()
            .unwrap()
            .training
            .admittedcapacity = 0;
        assert!(execution_facts(FeatureId::Train, false, &model, &settings).is_none());
    }

    #[test]
    fn new_preparation_cannot_display_a_completed_operations_retained_capacity() {
        use crate::generated::*;
        use crate::view_model::{PendingStart, StartInputs, StartPreparation};
        for feature in [FeatureId::Train, FeatureId::Validate, FeatureId::Predict] {
            let mut model = crate::view_model::test_support::bootstrapped();
            let snapshot = model.settings_snapshot.as_mut().unwrap();
            for facts in [
                &mut snapshot.trainexecution,
                &mut snapshot.trainingvalidationexecution,
                &mut snapshot.validationexecution,
                &mut snapshot.predictionexecution,
            ] {
                facts.settingsrevision = snapshot.revision;
                facts.effectivebatchpermodel = 11;
            }
            let mut settings = crate::view::settings::SettingsModel::default();
            settings.install(snapshot);
            let retained = crate::view_model::test_support::saved_training_run(
                snapshot.settingsstate.workflows.train.request.clone(),
            )
            .run
            .unwrap()
            .execution;
            model.workflow.training.as_mut().unwrap().sources.execution = Some(retained);
            fn runtime(
                model: &mut crate::view_model::ApplicationModel,
                feature: FeatureId,
            ) -> (&mut ComputeUiState, &mut ExecutionFacts) {
                match feature {
                    FeatureId::Train => {
                        let state = model.workflow.training.as_mut().unwrap();
                        (
                            &mut state.local,
                            &mut state.sources.execution.as_mut().unwrap().training,
                        )
                    }
                    FeatureId::Validate => {
                        let state = model.workflow.validation.as_mut().unwrap();
                        (&mut state.operation, &mut state.execution)
                    }
                    FeatureId::Predict => {
                        let state = model.predict_snapshot.as_mut().unwrap();
                        (&mut state.operation, &mut state.execution)
                    }
                    _ => unreachable!(),
                }
            }
            let (operation, facts) = runtime(&mut model, feature);
            operation.active = false;
            operation.generationfrontier = 7;
            facts.operationgeneration = 7;
            facts.admittedcapacity = 3;
            facts.effectivebatchpermodel = 99;
            assert_eq!(
                execution_facts(feature, false, &model, &settings)
                    .unwrap()
                    .effectivebatchpermodel,
                11
            );
            model.workflow.pending_start = Some(PendingStart {
                feature,
                inputs: StartInputs::capture(settings.draft.as_ref().unwrap(), feature).unwrap(),
                preparation: StartPreparation::Waiting,
                resume_checkpoint: None,
                validation_preview: default_request_validationStartpreview().unwrap(),
                prediction_preview: default_request_predictStartpreview().unwrap(),
                prediction_saving: default_request_predictStartsaving().unwrap(),
                prediction_population: 0,
            });
            assert!(execution_facts(feature, false, &model, &settings).is_none());
            let (operation, _) = runtime(&mut model, feature);
            operation.active = true;
            operation.generationfrontier = 8;
            assert!(execution_facts(feature, false, &model, &settings).is_none());
            let (_, facts) = runtime(&mut model, feature);
            facts.operationgeneration = 8;
            facts.admittedcapacity = 0;
            assert!(execution_facts(feature, false, &model, &settings).is_none());
            runtime(&mut model, feature).1.admittedcapacity = 2;
            assert_eq!(
                execution_facts(feature, false, &model, &settings)
                    .unwrap()
                    .effectivebatchpermodel,
                99
            );
            if feature == FeatureId::Train {
                assert!(execution_facts(feature, true, &model, &settings).is_none());
                let facts = &mut model
                    .workflow
                    .training
                    .as_mut()
                    .unwrap()
                    .sources
                    .execution
                    .as_mut()
                    .unwrap()
                    .validation;
                facts.operationgeneration = 8;
                facts.admittedcapacity = 1;
                facts.effectivebatchpermodel = 5;
                assert_eq!(
                    execution_facts(feature, true, &model, &settings)
                        .unwrap()
                        .effectivebatchpermodel,
                    5
                );
                settings.resize_training_models(2).unwrap();
                assert!(execution_facts(feature, false, &model, &settings).is_none());
                assert!(execution_facts(feature, true, &model, &settings).is_none());
            }
        }
    }
}
