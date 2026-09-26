use crate::fluent_theme::Element;
use crate::generated::TrainAssignmentKind;
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view::shared::{CardHeading, card_section};
use crate::view::workflow::fields;
use iced::widget::{button, checkbox, column, container, row};

#[derive(Debug, Clone, Copy)]
pub enum Message {
    Assignment(TrainAssignmentKind),
    MatchFreeRho(f32),
    MatchFreeCorrespondenceWeight(f32),
    MatchFreeQueryWeight(f32),
    DenoisingEnabled(bool),
    DenoisingGroups(u16),
    DenoisingLabelNoiseRatio(f32),
    DenoisingCenterNoiseScale(f32),
    DenoisingSizeNoiseScale(f32),
}

const fn assignment_label(assignment: TrainAssignmentKind) -> &'static str {
    match assignment {
        TrainAssignmentKind::Hungarian => "Hungarian",
        TrainAssignmentKind::MatchFree => "Match-Free",
    }
}

const fn match_free_controls_visible(assignment: TrainAssignmentKind) -> bool {
    matches!(assignment, TrainAssignmentKind::MatchFree)
}

pub fn update(model: &mut SettingsModel, message: Message) -> Result<EditSchedule, String> {
    let cadence = EditCadence::Debounced;
    match message {
        Message::Assignment(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisionassignment(draft, value)
        }),
        Message::MatchFreeRho(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisionmatchfreerho(draft, value)
        }),
        Message::MatchFreeCorrespondenceWeight(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisionmatchfreecorrespondenceweight(draft, value)
        }),
        Message::MatchFreeQueryWeight(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisionmatchfreequeryweight(draft, value)
        }),
        Message::DenoisingEnabled(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisiondenoisingenabled(draft, value)
        }),
        Message::DenoisingGroups(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisiondenoisinggroups(draft, value)
        }),
        Message::DenoisingLabelNoiseRatio(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisiondenoisinglabelnoiseratio(draft, value)
        }),
        Message::DenoisingCenterNoiseScale(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisiondenoisingcenternoisescale(draft, value)
        }),
        Message::DenoisingSizeNoiseScale(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequesttrainingsupervisiondenoisingsizenoisescale(draft, value)
        }),
    }
}

pub fn view(train: &crate::generated::TrainViewState, enabled: bool) -> Element<'_, Message> {
    let supervision = &train.request.trainingsupervision;
    let assignment_choices = crate::generated::TRAIN_ASSIGNMENT_KIND_VALUES
        .iter()
        .copied()
        .fold(row![].spacing(6), |choices, assignment| {
            let available = enabled;
            let choice = button(assignment_label(assignment))
                .on_press_maybe(available.then_some(Message::Assignment(assignment)))
                .style(if supervision.assignment == assignment {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                });
            if assignment == TrainAssignmentKind::MatchFree {
                choices.push(container(choice).id(super::super::MATCH_FREE_ASSIGNMENT_ID))
            } else {
                choices.push(choice)
            }
        });
    let denoising_toggle = checkbox(supervision.denoising.enabled).label("Denoising supervision");
    let denoising_toggle: Element<'_, Message> = if enabled {
        denoising_toggle.on_toggle(Message::DenoisingEnabled).into()
    } else {
        denoising_toggle.into()
    };
    let mut controls = column![
        container(assignment_choices).id(
            crate::generated::constraint_workflowstrainrequesttrainingsupervisionassignment()
                .stable_field_id
                .to_string()
        ),
        container(denoising_toggle).id(
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled()
                .stable_field_id
                .to_string()
        )
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);

    if match_free_controls_visible(supervision.assignment) {
        controls = controls.push(
            fields::numeric_grid()
                .push(fields::number_f32(
                    "Match-Free rho",
                    supervision.matchfree.rho,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreerho(),
                    enabled,
                    Message::MatchFreeRho,
                ))
                .push(fields::number_f32(
                    "Correspondence weight",
                    supervision.matchfree.correspondenceweight,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreecorrespondenceweight(),
                    enabled,
                    Message::MatchFreeCorrespondenceWeight,
                ))
                .push(fields::number_f32(
                    "Query weight",
                    supervision.matchfree.queryweight,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreequeryweight(),
                    enabled,
                    Message::MatchFreeQueryWeight,
                )),
        );
    }
    if supervision.denoising.enabled {
        controls = controls.push(
            fields::numeric_grid()
                .push(fields::number_u64(
                    "DN groups",
                    u64::from(supervision.denoising.groups),
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinggroups(),
                    enabled,
                    |value| Message::DenoisingGroups(value as u16),
                ))
                .push(fields::number_f32(
                    "Label noise ratio",
                    supervision.denoising.labelnoiseratio,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinglabelnoiseratio(),
                    enabled,
                    Message::DenoisingLabelNoiseRatio,
                ))
                .push(fields::number_f32(
                    "Center noise scale",
                    supervision.denoising.centernoisescale,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingcenternoisescale(),
                    enabled,
                    Message::DenoisingCenterNoiseScale,
                ))
                .push(fields::number_f32(
                    "Size noise scale",
                    supervision.denoising.sizenoisescale,
                    crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingsizenoisescale(),
                    enabled,
                    Message::DenoisingSizeNoiseScale,
                )),
        );
    }
    card_section("Assignment", CardHeading::H2, controls)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application_codec::{FromApplicationValue, Value};
    use crate::view::settings::installed_settings_model;

    fn decode_match_free_rho(
        rho: f32,
    ) -> Result<crate::generated::MatchFreeSupervisionConfig, String> {
        crate::generated::MatchFreeSupervisionConfig::from_application_value(Value::Object(vec![
            ("rho".into(), Value::Float(rho.into())),
            ("correspondence_weight".into(), Value::Float(1.0)),
            ("query_weight".into(), Value::Float(1.0)),
        ]))
    }

    fn decode_denoising_scales(
        center: f32,
        size: f32,
    ) -> Result<crate::generated::DenoisingSupervisionConfig, String> {
        crate::generated::DenoisingSupervisionConfig::from_application_value(Value::Object(vec![
            ("enabled".into(), Value::Bool(true)),
            ("groups".into(), Value::Unsigned(5)),
            ("label_noise_ratio".into(), Value::Float(0.2)),
            ("center_noise_scale".into(), Value::Float(center.into())),
            ("size_noise_scale".into(), Value::Float(size.into())),
        ]))
    }

    #[test]
    fn generated_inventory_constraints_and_debounced_local_edits_drive_the_component() {
        assert_eq!(crate::generated::TRAIN_ASSIGNMENT_KIND_VALUES.len(), 2);
        for open_unit in [
            crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreerho(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingcenternoisescale(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingsizenoisescale(),
        ] {
            assert!(open_unit.minimum.is_some_and(|value| value > 0.0));
            assert!(open_unit.maximum.is_some_and(|value| value < 1.0));
            assert_eq!(
                open_unit.minimum.map(|value| value as f32),
                Some(f32::from_bits(1))
            );
            assert_eq!(
                open_unit.maximum.map(|value| value as f32),
                Some(f32::from_bits(1.0_f32.to_bits() - 1))
            );
        }
        let predecessor = f32::from_bits(1.0_f32.to_bits() - 1);
        assert!(decode_match_free_rho(predecessor).is_ok());
        assert!(decode_match_free_rho(1.0).is_err());
        assert!(decode_denoising_scales(f32::from_bits(1), predecessor).is_ok());
        assert!(decode_denoising_scales(1.0, predecessor).is_err());
        assert!(decode_denoising_scales(predecessor, 1.0).is_err());

        let mut model = installed_settings_model();
        let schedule = update(
            &mut model,
            Message::Assignment(TrainAssignmentKind::MatchFree),
        )
        .unwrap();
        assert!(matches!(schedule, EditSchedule::Debounce(_)));
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .trainingsupervision
                .assignment,
            TrainAssignmentKind::MatchFree
        );
        assert!(match_free_controls_visible(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .trainingsupervision
                .assignment
        ));
        let schedule = update(&mut model, Message::DenoisingEnabled(true)).unwrap();
        assert!(matches!(schedule, EditSchedule::Debounce(_)));
        assert!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .trainingsupervision
                .denoising
                .enabled
        );
    }

    #[test]
    fn every_visible_control_has_a_stable_identity() {
        for constraint in [
            crate::generated::constraint_workflowstrainrequesttrainingsupervisionassignment(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreerho(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreecorrespondenceweight(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisionmatchfreequeryweight(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingenabled(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinggroups(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisinglabelnoiseratio(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingcenternoisescale(),
            crate::generated::constraint_workflowstrainrequesttrainingsupervisiondenoisingsizenoisescale(),
        ] {
            assert_ne!(constraint.stable_field_id, 0);
        }
    }
}
