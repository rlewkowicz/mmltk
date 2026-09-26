use crate::fluent_theme::Element;
use crate::generated::*;
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view::shared::{CardHeading, card_section};
use crate::view::workflow::fields;
use iced::widget::{button, column, row, text};

#[derive(Debug, Clone, Copy)]
pub enum Message {
    Count(i32),
    ValidationCount(i32),
    Mode(TrainLaneMode),
    Scope(Option<u64>),
    Seed(u64),
    Coefficient(f64),
    Balancing(TrainBalancing),
    RareThreshold(f64),
    RepeatFactor(f64),
    DrawMultiplier(f64),
    Cadence(TrainMergeCadence),
    MergeRounds(u64),
    FinalPolicy(Option<TrainFinalPolicy>),
}

pub fn update(
    settings: &mut SettingsModel,
    message: Message,
) -> Result<Option<EditSchedule>, String> {
    let cadence = EditCadence::Debounced;
    match message {
        Message::Scope(id) => {
            if id.is_some_and(|id| {
                settings.draft.as_ref().is_none_or(|draft| {
                    !draft
                        .workflows
                        .train
                        .request
                        .laneconfiguration
                        .models
                        .iter()
                        .any(|model| model.modelid == id)
                })
            }) {
                return Err("The selected training model is unavailable.".into());
            }
            settings.recipe_model = id;
            return Ok(None);
        }
        Message::Count(value) => {
            if value < 1 {
                return Err("Lane count must be positive.".into());
            }
            let independent = settings.draft.as_ref().is_some_and(|draft| {
                draft.workflows.train.request.laneconfiguration.mode
                    != TrainLaneMode::SharedGradients
            });
            return Ok(Some(if independent {
                settings.resize_training_models(value as u32)?
            } else {
                settings.edit(cadence, |draft| {
                    edit_workflowstrainrequestlanes(draft, value)
                })?
            }));
        }
        Message::ValidationCount(value) => {
            return settings
                .edit(cadence, |draft| {
                    edit_workflowstrainrequestvalidationlanes(draft, value)
                })
                .map(Some);
        }
        Message::Balancing(value) => {
            return settings
                .edit(cadence, |draft| {
                    edit_workflowstrainrequestdatapolicybalancing(draft, value)
                })
                .map(Some);
        }
        Message::RareThreshold(value) => {
            return settings
                .edit(cadence, |draft| {
                    edit_workflowstrainrequestdatapolicyrarethreshold(draft, value)
                })
                .map(Some);
        }
        Message::RepeatFactor(value) => {
            return settings
                .edit(cadence, |draft| {
                    edit_workflowstrainrequestdatapolicymaximumrepeatfactor(draft, value)
                })
                .map(Some);
        }
        Message::DrawMultiplier(value) => {
            return settings
                .edit(cadence, |draft| {
                    edit_workflowstrainrequestdatapolicymaximumdrawmultiplier(draft, value)
                })
                .map(Some);
        }
        _ => {}
    }
    let mut configuration = settings
        .draft
        .as_ref()
        .ok_or("Settings unavailable")?
        .workflows
        .train
        .request
        .laneconfiguration
        .clone();
    match message {
        Message::Mode(mode) => configuration.mode = mode,
        Message::Cadence(value) => configuration.mergecadence = value,
        Message::MergeRounds(value) => configuration.mergerounds = value,
        Message::FinalPolicy(value) => configuration.finalpolicy = value,
        Message::Seed(value) => {
            configuration
                .models
                .iter_mut()
                .find(|model| Some(model.modelid) == settings.recipe_model)
                .ok_or("Select a model")?
                .seed = value
        }
        Message::Coefficient(value) => {
            configuration
                .models
                .iter_mut()
                .find(|model| Some(model.modelid) == settings.recipe_model)
                .ok_or("Select a model")?
                .coefficient = value
        }
        _ => unreachable!(),
    }
    settings.replace_training_lanes(configuration).map(Some)
}

pub fn execution<'a>(
    request: &'a TrainRequest,
    settings: &SettingsModel,
    enabled: bool,
) -> Element<'a, Message> {
    let configuration = &request.laneconfiguration;
    let enabled = enabled && !settings.training_membership_pending();
    let mut modes = row![].spacing(6);
    for mode in TRAIN_LANE_MODE_VALUES {
        let label = match mode {
            TrainLaneMode::SharedGradients => "Shared gradients",
            TrainLaneMode::Independent => "Independent models",
            TrainLaneMode::PeriodicAveraging => "Periodic averaging",
        };
        modes = modes.push(
            button(label)
                .on_press_maybe(enabled.then_some(Message::Mode(*mode)))
                .style(if configuration.mode == *mode {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                }),
        );
    }
    modes.into()
}

pub fn recipe_scope<'a>(
    request: &'a TrainRequest,
    settings: &SettingsModel,
    enabled: bool,
) -> Element<'a, Message> {
    let configuration = &request.laneconfiguration;
    let enabled = enabled && !settings.training_membership_pending();
    let mut scopes = row![button("Global defaults").on_press(Message::Scope(None))].spacing(6);
    for model in &configuration.models {
        scopes = scopes.push(
            button(text(format!("Model {}", model.modelid)))
                .on_press(Message::Scope(Some(model.modelid)))
                .style(if settings.recipe_model == Some(model.modelid) {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                }),
        );
    }
    let mut content = column![
        iced::widget::scrollable(scopes).direction(iced::widget::scrollable::Direction::Horizontal(iced::widget::scrollable::Scrollbar::default())),
        text("New models copy the settled global recipe. Existing model recipes and overrides remain independent.").size(12),
    ].spacing(8);
    if let Some(model) = configuration
        .models
        .iter()
        .find(|model| Some(model.modelid) == settings.recipe_model)
    {
        let coefficient = constraint_trainmodelsettingscoefficient();
        content = content.push(
            fields::numeric_grid()
                .push(fields::number_u64(
                    "Model seed",
                    model.seed,
                    constraint_trainmodelsettingsseed(),
                    enabled,
                    Message::Seed,
                ))
                .push(fields::number_f64(
                    "Soup coefficient",
                    model.coefficient,
                    coefficient,
                    enabled,
                    Message::Coefficient,
                )),
        );
    }
    card_section("Recipe scope", CardHeading::H3, content)
}

pub fn balancing<'a>(request: &'a TrainRequest, enabled: bool) -> Element<'a, Message> {
    let mut balancing = row![].spacing(6);
    for choice in TRAIN_BALANCING_VALUES {
        let label = match choice {
            TrainBalancing::Off => "Off",
            TrainBalancing::Stratified => "Stratified",
            TrainBalancing::RareRepeatsStratified => "Rare repeats + Stratified",
        };
        balancing = balancing.push(
            button(label)
                .on_press_maybe(enabled.then_some(Message::Balancing(*choice)))
                .style(if request.datapolicy.balancing == *choice {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                }),
        );
    }
    let mut content = column![text("Class balancing"), balancing,
        text("Balancing changes exposure; it cannot create missing class support. Unique images and repeated draws remain distinct.").size(12)].spacing(crate::view::workflow::FIELD_SPACING);
    if request.datapolicy.balancing == TrainBalancing::RareRepeatsStratified {
        content = content.push(
            fields::numeric_grid()
                .push(fields::number_f64(
                    "Rare threshold",
                    request.datapolicy.rarethreshold,
                    constraint_workflowstrainrequestdatapolicyrarethreshold(),
                    enabled,
                    Message::RareThreshold,
                ))
                .push(fields::number_f64(
                    "Maximum repeat factor",
                    request.datapolicy.maximumrepeatfactor,
                    constraint_workflowstrainrequestdatapolicymaximumrepeatfactor(),
                    enabled,
                    Message::RepeatFactor,
                ))
                .push(fields::number_f64(
                    "Maximum draw multiplier",
                    request.datapolicy.maximumdrawmultiplier,
                    constraint_workflowstrainrequestdatapolicymaximumdrawmultiplier(),
                    enabled,
                    Message::DrawMultiplier,
                )),
        );
    }
    content.into()
}

pub fn merging<'a>(request: &'a TrainRequest, enabled: bool) -> Element<'a, Message> {
    let configuration = &request.laneconfiguration;
    let mut content = column![].spacing(crate::view::workflow::FIELD_SPACING);
    if configuration.mode == TrainLaneMode::PeriodicAveraging {
        let mut cadence = row![].spacing(6);
        for value in TRAIN_MERGE_CADENCE_VALUES {
            cadence = cadence.push(
                button(match value {
                    TrainMergeCadence::Epoch => "Each epoch",
                    TrainMergeCadence::Rounds => "Every N rounds",
                })
                .on_press_maybe(enabled.then_some(Message::Cadence(*value)))
                .style(if configuration.mergecadence == *value {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                }),
            );
        }
        content = content.push(text("Merge cadence")).push(cadence)
            .push(fields::number_u64("Merge rounds", configuration.mergerounds, constraint_trainlaneconfigurationmergerounds(), enabled && configuration.mergecadence == TrainMergeCadence::Rounds, Message::MergeRounds))
            .push(text("Experimental: periodic averaging supports mixed optimizers; optimizer state and EMA remain per model.").size(12));
    }
    let effective = effective_train_final_policy(configuration);
    let mut policies =
        row![button("Mode default").on_press_maybe(enabled.then_some(Message::FinalPolicy(None)))]
            .spacing(6);
    for policy in TRAIN_FINAL_POLICY_VALUES {
        let label = match policy {
            TrainFinalPolicy::Off => "Off",
            TrainFinalPolicy::Uniform => "Uniform",
            TrainFinalPolicy::Explicit => "Explicit",
            TrainFinalPolicy::ValidationGreedy => "Validation-greedy",
        };
        policies = policies.push(
            button(label)
                .on_press_maybe(enabled.then_some(Message::FinalPolicy(Some(*policy))))
                .style(if effective == *policy {
                    crate::fluent_theme::button_selected
                } else {
                    crate::fluent_theme::button_secondary
                }),
        );
    }
    content.push(card_section(
        "Final model soup",
        CardHeading::H2,
        column![policies, text("Experimental: RF-DETR soups select native ordinary or EMA artifacts using validation. Individual models remain available.").size(12)]
            .spacing(crate::view::workflow::FIELD_SPACING),
    )).into()
}
