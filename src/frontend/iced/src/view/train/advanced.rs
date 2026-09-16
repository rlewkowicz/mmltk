use crate::fluent_theme::Element;
use crate::generated::{TrainLrSchedulerKind, TrainOptimizerKind};
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view::workflow::fields;
use iced::widget::{button, column, row, text};

mod supervision;

#[derive(Debug, Clone, Copy)]
pub enum Message {
    Loading(crate::view::workflow::loading::Message),
    BatchSize(u64),
    ValidationBatchSize(u64),
    Epochs(i32),
    GradientAccumulation(i32),
    Optimizer(TrainOptimizerKind),
    DecoderLearningRate(f64),
    EncoderLearningRate(f64),
    Scheduler(TrainLrSchedulerKind),
    WeightDecay(f64),
    Momentum(f64),
    Amp(bool),
    Ema(bool),
    FreezeEncoder(bool),
    Supervision(supervision::Message),
    UseOptimizerDefaults,
}

#[cfg(test)]
fn recipe(optimizer: TrainOptimizerKind) -> &'static crate::generated::TrainRecipeCatalogEntry {
    crate::generated::TRAIN_RECIPE_CATALOG
        .iter()
        .find(|recipe| recipe.optimizer == optimizer)
        .unwrap_or_else(|| &crate::generated::TRAIN_RECIPE_CATALOG[0])
}

fn effective_scheduler(request: &crate::generated::TrainRequest) -> TrainLrSchedulerKind {
    crate::generated::effective_workflowstrainrequestlrscheduler(request)
}

const fn optimizer_label(optimizer: TrainOptimizerKind) -> &'static str {
    match optimizer {
        TrainOptimizerKind::AdamW => "AdamW",
        TrainOptimizerKind::Muon => "Muon",
    }
}

const fn scheduler_label(scheduler: TrainLrSchedulerKind) -> &'static str {
    match scheduler {
        TrainLrSchedulerKind::Step => "Step",
        TrainLrSchedulerKind::Cosine => "Cosine",
    }
}

pub fn update(model: &mut SettingsModel, message: Message) -> Result<EditSchedule, String> {
    let cadence = EditCadence::Debounced;
    match message {
        Message::Loading(message) => crate::view::workflow::loading::update(
            crate::generated::FeatureId::Train,
            model,
            message,
        ),
        Message::BatchSize(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestbatchsize(draft, value)
        }),
        Message::ValidationBatchSize(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestvalbatchsize(draft, value)
        }),
        Message::Epochs(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestepochs(draft, value)
        }),
        Message::GradientAccumulation(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestgradaccumsteps(draft, value)
        }),
        Message::Optimizer(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestoptimizer(draft, value)
        }),
        Message::DecoderLearningRate(value) => model.edit(cadence, |draft| {
            crate::generated::edit_relation_workflowstrainrequestlr(draft, value)
        }),
        Message::EncoderLearningRate(value) => model.edit(cadence, |draft| {
            crate::generated::edit_relation_workflowstrainrequestlrencoder(draft, value)
        }),
        Message::Scheduler(value) => model.edit(cadence, |draft| {
            crate::generated::edit_relation_workflowstrainrequestlrscheduler(draft, value)
        }),
        Message::WeightDecay(value) => model.edit(cadence, |draft| {
            crate::generated::edit_relation_workflowstrainrequestweightdecay(draft, value)
        }),
        Message::Momentum(value) => model.edit(cadence, |draft| {
            crate::generated::edit_relation_workflowstrainrequestmomentum(draft, value)
        }),
        Message::Amp(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestamp(draft, value)
        }),
        Message::Ema(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestuseema(draft, value)
        }),
        Message::FreezeEncoder(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestfreezeencoder(draft, value)
        }),
        Message::Supervision(message) => supervision::update(model, message),
        Message::UseOptimizerDefaults => model.edit_group(cadence, |draft| {
            [
                crate::generated::clear_relation_workflowstrainrequestlr(draft),
                crate::generated::clear_relation_workflowstrainrequestlrencoder(draft),
                crate::generated::clear_relation_workflowstrainrequestlrscheduler(draft),
                crate::generated::clear_relation_workflowstrainrequestweightdecay(draft),
                crate::generated::clear_relation_workflowstrainrequestmomentum(draft),
            ]
        }),
    }
}

pub fn view<'a>(
    train: Option<&'a crate::generated::TrainViewState>,
    settings: &'a SettingsModel,
    enabled: bool,
) -> Element<'a, Message> {
    let Some(train) = train else {
        return crate::view::shared::card(
            "Advanced",
            "Optimizer, schedule, precision, and supervision controls.",
            iced::widget::text("Training settings unavailable"),
        );
    };
    let request = &train.request;
    let lr = crate::generated::effective_workflowstrainrequestlr(request);
    let lr_encoder = crate::generated::effective_workflowstrainrequestlrencoder(request);
    let scheduler = effective_scheduler(request);
    let weight_decay = crate::generated::effective_workflowstrainrequestweightdecay(request);
    let momentum = crate::generated::effective_workflowstrainrequestmomentum(request);
    let optimizer_choices =
        crate::generated::TRAIN_RECIPE_CATALOG
            .iter()
            .fold(row![].spacing(6), |choices, recipe| {
                choices.push(
                    button(optimizer_label(recipe.optimizer))
                        .on_press_maybe(enabled.then_some(Message::Optimizer(recipe.optimizer)))
                        .style(if request.optimizer == recipe.optimizer {
                            crate::fluent_theme::button_selected
                        } else {
                            crate::fluent_theme::button_secondary
                        }),
                )
            });
    let scheduler_choices = crate::generated::TRAIN_LR_SCHEDULER_KIND_VALUES
        .iter()
        .copied()
        .fold(row![].spacing(6), |choices, choice| {
            choices.push(
                button(scheduler_label(choice))
                    .on_press_maybe(enabled.then_some(Message::Scheduler(choice)))
                    .style(if scheduler == choice {
                        crate::fluent_theme::button_selected
                    } else {
                        crate::fluent_theme::button_secondary
                    }),
            )
        });
    crate::view::shared::card(
        "Advanced",
        "Optimizer, schedule, precision, and supervision controls.",
        column![
            crate::view::workflow::loading::view(
                crate::generated::FeatureId::Train,
                settings,
                enabled
            )
            .map(Message::Loading),
            fields::numeric_grid()
                .push(fields::number_u64(
                    "Train batch size",
                    request.batchsize,
                    crate::generated::constraint_workflowstrainrequestbatchsize(),
                    enabled,
                    Message::BatchSize,
                ))
                .push(fields::number_u64(
                    "Validation batch size",
                    request.valbatchsize,
                    crate::generated::constraint_workflowstrainrequestvalbatchsize(),
                    enabled,
                    Message::ValidationBatchSize,
                ))
                .push(fields::number_i32(
                    "Epochs",
                    request.epochs,
                    crate::generated::constraint_workflowstrainrequestepochs(),
                    enabled,
                    Message::Epochs,
                ))
                .push(fields::number_i32(
                    "Gradient accumulation",
                    request.gradaccumsteps,
                    crate::generated::constraint_workflowstrainrequestgradaccumsteps(),
                    enabled,
                    Message::GradientAccumulation,
                )),
            text("Optimizer"),
            optimizer_choices,
            fields::numeric_grid()
                .push(fields::number_f64(
                    "Decoder learning rate",
                    lr,
                    crate::generated::constraint_workflowstrainrequestlr(),
                    enabled,
                    Message::DecoderLearningRate,
                ))
                .push(fields::number_f64(
                    "Encoder learning rate",
                    lr_encoder,
                    crate::generated::constraint_workflowstrainrequestlrencoder(),
                    enabled,
                    Message::EncoderLearningRate,
                ))
                .push(fields::number_f64(
                    "Weight decay",
                    weight_decay,
                    crate::generated::constraint_workflowstrainrequestweightdecay(),
                    enabled,
                    Message::WeightDecay,
                ))
                .push(fields::number_f64(
                    "Momentum",
                    momentum,
                    crate::generated::constraint_workflowstrainrequestmomentum(),
                    enabled,
                    Message::Momentum,
                )),
            text("Learning-rate scheduler"),
            scheduler_choices,
            fields::toggle(
                "Automatic mixed precision",
                request.amp,
                enabled,
                Message::Amp
            ),
            fields::toggle(
                "Exponential moving average",
                request.useema,
                enabled,
                Message::Ema
            ),
            fields::toggle(
                "Freeze encoder",
                request.freezeencoder,
                enabled,
                Message::FreezeEncoder,
            ),
            supervision::view(train, enabled).map(Message::Supervision),
            button("Use optimizer defaults")
                .on_press_maybe(enabled.then_some(Message::UseOptimizerDefaults)),
        ]
        .spacing(crate::view::workflow::FIELD_SPACING),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::generated::TrainRecipeCatalogRelationField;
    use crate::view::settings::installed_settings_model;

    #[test]
    fn ema_uses_the_canonical_false_default_and_round_trips_edits() {
        let mut model = installed_settings_model();
        assert!(!model.draft.as_ref().unwrap().workflows.train.request.useema);
        for enabled in [true, false] {
            update(&mut model, Message::Ema(enabled)).unwrap();
            assert_eq!(model.draft.as_ref().unwrap().workflows.train.request.useema, enabled);
        }
    }

    #[test]
    fn recipe_edits_pin_values_and_reset_clears_the_basic_override_set() {
        let mut model = installed_settings_model();
        update(&mut model, Message::DecoderLearningRate(0.002)).unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(request.lr, 0.002);
        assert!(
            request
                .recipeoverrides
                .overridden(TrainRecipeCatalogRelationField::Lr)
        );

        update(&mut model, Message::UseOptimizerDefaults).unwrap();
        let overrides = &model
            .draft
            .as_ref()
            .unwrap()
            .workflows
            .train
            .request
            .recipeoverrides;
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::Lr));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::LrEncoder));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::LrScheduler));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::WeightDecay));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::Momentum));
    }

    #[test]
    fn scheduler_selection_tracks_optimizer_recipe_until_explicitly_overridden() {
        let mut model = installed_settings_model();
        update(&mut model, Message::Optimizer(TrainOptimizerKind::Muon)).unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(
            effective_scheduler(request),
            recipe(TrainOptimizerKind::Muon).lrscheduler
        );

        update(&mut model, Message::Scheduler(TrainLrSchedulerKind::Cosine)).unwrap();
        update(&mut model, Message::Optimizer(TrainOptimizerKind::AdamW)).unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert!(
            request
                .recipeoverrides
                .overridden(TrainRecipeCatalogRelationField::LrScheduler)
        );
        assert_eq!(effective_scheduler(request), TrainLrSchedulerKind::Cosine);
    }

    #[test]
    fn advanced_choice_sources_are_the_generated_recipe_and_enum_inventories() {
        for recipe in crate::generated::TRAIN_RECIPE_CATALOG {
            assert!(!optimizer_label(recipe.optimizer).is_empty());
        }
        for scheduler in crate::generated::TRAIN_LR_SCHEDULER_KIND_VALUES {
            assert!(!scheduler_label(*scheduler).is_empty());
        }
    }
}
