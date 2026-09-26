use crate::fluent_theme::Element;
use crate::generated::{TrainLrSchedulerKind, TrainOptimizerKind, TrainRecipeSettingsEdit};
use crate::view::settings::{EditCadence, EditSchedule, SettingsModel};
use crate::view::workflow::fields;
use iced::widget::{button, column, container, row, text};

mod supervision;

#[derive(Debug, Clone)]
pub enum Message {
    Loading(crate::view::workflow::loading::Message),
    BatchSize(u64),
    ValidationBatchSize(u64),
    Epochs(i32),
    GradientAccumulation(i32),
    Recipe(TrainRecipeSettingsEdit),
    Amp(bool),
    Ema(bool),
    Augmentation(bool),
    PerceptualDownscale(bool),
    FreezeEncoder(bool),
    UnfreezeLast(i32),
    DisableAugmentationLast(i32),
    Supervision(supervision::Message),
}

#[cfg(test)]
fn recipe(optimizer: TrainOptimizerKind) -> &'static crate::generated::TrainRecipeCatalogEntry {
    crate::generated::TRAIN_RECIPE_CATALOG
        .iter()
        .find(|recipe| recipe.optimizer == optimizer)
        .unwrap_or_else(|| &crate::generated::TRAIN_RECIPE_CATALOG[0])
}

#[cfg(test)]
fn effective_scheduler(request: &crate::generated::TrainRequest) -> TrainLrSchedulerKind {
    crate::generated::effective_trainrecipesettings_lrscheduler(&request.recipe)
}

const fn optimizer_label(optimizer: TrainOptimizerKind) -> &'static str {
    match optimizer {
        TrainOptimizerKind::AdamW => "AdamW",
        TrainOptimizerKind::Muon => "Muon",
        TrainOptimizerKind::SGD => "SGD",
    }
}

const fn scheduler_label(scheduler: TrainLrSchedulerKind) -> &'static str {
    match scheduler {
        TrainLrSchedulerKind::Step => "Step",
        TrainLrSchedulerKind::Cosine => "Cosine",
        TrainLrSchedulerKind::UltralyticsLinear => "Ultralytics linear",
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
        Message::Recipe(edit) => model.edit_recipe(edit),
        Message::Amp(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestamp(draft, value)
        }),
        Message::Augmentation(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestgpuaugmentationenabled(draft, value)
        }),
        Message::PerceptualDownscale(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestgpuaugmentationperceptualdownscale(
                draft, value,
            )
        }),
        Message::Ema(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestuseema(draft, value)
        }),
        Message::FreezeEncoder(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestfreezeencoder(draft, value)
        }),
        Message::UnfreezeLast(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestunfreezeencoderlastepochs(draft, value)
        }),
        Message::DisableAugmentationLast(value) => model.edit(cadence, |draft| {
            crate::generated::edit_workflowstrainrequestdisableaugmentationlastepochs(draft, value)
        }),
        Message::Supervision(message) => supervision::update(model, message),
    }
}

pub fn view<'a>(
    train: Option<&'a crate::generated::TrainViewState>,
    settings: &'a SettingsModel,
    enabled: bool,
    model: &crate::view_model::ApplicationModel,
) -> Element<'a, Message> {
    let Some(train) = train else {
        return crate::view::shared::card(
            "Advanced",
            "Optimizer, schedule, precision, and supervision controls.",
            iced::widget::text("Training settings unavailable"),
        );
    };
    let request = &train.request;
    let scoped_recipe = settings
        .recipe_model
        .and_then(|id| {
            request
                .laneconfiguration
                .models
                .iter()
                .find(|entry| entry.modelid == id)
        })
        .map_or(&request.recipe, |entry| &entry.recipe);
    let enabled = enabled && !settings.training_membership_pending();
    let lr = crate::generated::effective_trainrecipesettings_lr(scoped_recipe);
    let lr_encoder = crate::generated::effective_trainrecipesettings_lrencoder(scoped_recipe);
    let scheduler = crate::generated::effective_trainrecipesettings_lrscheduler(scoped_recipe);
    let weight_decay = crate::generated::effective_trainrecipesettings_weightdecay(scoped_recipe);
    let momentum = crate::generated::effective_trainrecipesettings_momentum(scoped_recipe);
    let optimizer_choices =
        crate::generated::TRAIN_RECIPE_CATALOG
            .iter()
            .fold(row![].spacing(6), |choices, recipe| {
                choices.push(
                    button(optimizer_label(recipe.optimizer))
                        .on_press_maybe(enabled.then_some(Message::Recipe(
                            TrainRecipeSettingsEdit::Optimizer(recipe.optimizer),
                        )))
                        .style(if scoped_recipe.optimizer == recipe.optimizer {
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
                    .on_press_maybe(
                        (enabled
                            && crate::generated::train_scheduler_available(
                                scoped_recipe.optimizer,
                                choice,
                            ))
                        .then_some(Message::Recipe(
                            TrainRecipeSettingsEdit::LrScheduler(choice),
                        )),
                    )
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
                .push(fields::effective_batch(crate::generated::FeatureId::Train, false, model, settings))
                .push(fields::number_u64(
                    "Validation batch size",
                    request.valbatchsize,
                    crate::generated::constraint_workflowstrainrequestvalbatchsize(),
                    enabled,
                    Message::ValidationBatchSize,
                ))
                .push(fields::effective_batch(crate::generated::FeatureId::Train, true, model, settings))
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
            fields::read_only("Aggregate session round", "train.aggregate_batch", fields::execution_facts(crate::generated::FeatureId::Train, false, model, settings).map_or_else(|| "Updating".into(), |facts| facts.aggregateroundimages.to_string())),
            text(if settings.recipe_model.is_some() { "Model optimizer" } else { "Global optimizer defaults" }),
            optimizer_choices,
            fields::numeric_grid()
                .push(fields::number_f64(
                    "Decoder learning rate",
                    lr,
                    crate::generated::constraint_workflowstrainrequestrecipelr(),
                    enabled,
                    |value| Message::Recipe(TrainRecipeSettingsEdit::Lr(value)),
                ))
                .push(fields::number_f64(
                    "Encoder learning rate",
                    lr_encoder,
                    crate::generated::constraint_workflowstrainrequestrecipelrencoder(),
                    enabled,
                    |value| Message::Recipe(TrainRecipeSettingsEdit::LrEncoder(value)),
                ))
                .push(fields::number_f64(
                    "Weight decay",
                    weight_decay,
                    crate::generated::constraint_workflowstrainrequestrecipeweightdecay(),
                    enabled,
                    |value| Message::Recipe(TrainRecipeSettingsEdit::WeightDecay(value)),
                ))
                .push(fields::number_f64(
                    "Momentum",
                    momentum,
                    crate::generated::constraint_workflowstrainrequestrecipemomentum(),
                    enabled,
                    |value| Message::Recipe(TrainRecipeSettingsEdit::Momentum(value)),
                )),
            fields::numeric_grid()
                .push(fields::number_f64("Component LR factor", crate::generated::effective_trainrecipesettings_lrcomponentdecay(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipelrcomponentdecay(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::LrComponentDecay(value))))
                .push(fields::number_f64("Encoder layer LR factor", crate::generated::effective_trainrecipesettings_encoderlayerdecay(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipeencoderlayerdecay(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::EncoderLayerDecay(value))))
                .push(fields::number_f64("Warmup epochs", crate::generated::effective_trainrecipesettings_warmupepochs(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipewarmupepochs(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::WarmupEpochs(value))))
                .push(fields::number_f64("Warmup momentum", crate::generated::effective_trainrecipesettings_warmupmomentum(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipewarmupmomentum(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::WarmupMomentum(value))))
                .push(fields::number_f64("Minimum LR factor", crate::generated::effective_trainrecipesettings_lrminfactor(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipelrminfactor(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::LrMinFactor(value))))
                .push(fields::number_i32("LR drop epoch", crate::generated::effective_trainrecipesettings_lrdrop(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipelrdrop(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::LrDrop(value))))
                .push(fields::number_f64("Bias warmup LR", crate::generated::effective_trainrecipesettings_warmupbiaslr(scoped_recipe), crate::generated::constraint_workflowstrainrequestrecipewarmupbiaslr(), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::WarmupBiasLr(value)))),
            fields::toggle("Nesterov momentum", crate::generated::effective_trainrecipesettings_nesterov(scoped_recipe), enabled, |value| Message::Recipe(TrainRecipeSettingsEdit::Nesterov(value))),
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
            fields::number_i32("Unfreeze encoder: final epochs", request.unfreezeencoderlastepochs, crate::generated::constraint_workflowstrainrequestunfreezeencoderlastepochs(), enabled, Message::UnfreezeLast),
            container(fields::toggle("GPU augmentation", request.gpuaugmentation.enabled, enabled, Message::Augmentation))
                .id(crate::generated::constraint_workflowstrainrequestgpuaugmentationenabled().stable_field_id.to_string()),
            fields::number_i32("Disable augmentation: final epochs", request.disableaugmentationlastepochs, crate::generated::constraint_workflowstrainrequestdisableaugmentationlastepochs(), enabled, Message::DisableAugmentationLast),
            container(fields::toggle("Perceptual augmentation downscaling", request.gpuaugmentation.perceptualdownscale, enabled, Message::PerceptualDownscale))
                .id(crate::generated::constraint_workflowstrainrequestgpuaugmentationperceptualdownscale().stable_field_id.to_string()),
            supervision::view(train, enabled).map(Message::Supervision),
            button("Use optimizer defaults")
                .on_press_maybe(enabled.then_some(Message::Recipe(TrainRecipeSettingsEdit::Reset))),
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
            assert_eq!(
                model.draft.as_ref().unwrap().workflows.train.request.useema,
                enabled
            );
        }
    }

    #[test]
    fn augmentation_and_perceptual_edits_are_independent() {
        let mut model = installed_settings_model();
        // CLEANUP-IGNORE: Repeated typed field access checks distinct augmentation settings.
        assert!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .gpuaugmentation
                .enabled
        );
        assert!(
            !model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .gpuaugmentation
                .perceptualdownscale
        );
        update(&mut model, Message::Augmentation(false)).unwrap();
        update(&mut model, Message::PerceptualDownscale(true)).unwrap();
        let config = &model
            .draft
            .as_ref()
            .unwrap()
            .workflows
            .train
            .request
            .gpuaugmentation;
        assert!(!config.enabled);
        assert!(config.perceptualdownscale);
        update(&mut model, Message::Augmentation(true)).unwrap();
        assert!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .gpuaugmentation
                .perceptualdownscale
        );
    }

    #[test]
    fn captured_start_detects_independent_resampling_edits() {
        use crate::generated::FeatureId;
        use crate::view_model::StartInputs;
        let mut model = installed_settings_model();
        let captured =
            StartInputs::capture(model.draft.as_ref().unwrap(), FeatureId::Train).unwrap();
        assert!(captured.matches(model.draft.as_ref().unwrap()));
        update(&mut model, Message::PerceptualDownscale(true)).unwrap();
        assert!(!captured.matches(model.draft.as_ref().unwrap()));
        update(&mut model, Message::PerceptualDownscale(false)).unwrap();
        assert!(captured.matches(model.draft.as_ref().unwrap()));
        crate::view::train::dataset::update(
            &mut model,
            crate::view::train::dataset::Message::PerceptualDownscaleChanged(true),
        )
        .unwrap();
        assert!(!captured.matches(model.draft.as_ref().unwrap()));
        let StartInputs::Train(original) = captured else {
            panic!("train capture expected");
        };
        assert!(!original.compileperceptualdownscale);
        assert!(!original.request.gpuaugmentation.perceptualdownscale);
        assert!(original.request.gpuaugmentation.enabled);
    }

    #[test]
    fn recipe_edits_pin_values_and_reset_clears_the_basic_override_set() {
        let mut model = installed_settings_model();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Lr(0.002)),
        )
        .unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(request.recipe.lr, 0.002);
        assert!(
            request
                .recipe
                .overrides
                .overridden(TrainRecipeCatalogRelationField::Lr)
        );

        update(&mut model, Message::Recipe(TrainRecipeSettingsEdit::Reset)).unwrap();
        let overrides = &model
            .draft
            .as_ref()
            .unwrap()
            .workflows
            .train
            .request
            .recipe
            .overrides;
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::Lr));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::LrEncoder));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::LrScheduler));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::WeightDecay));
        assert!(!overrides.overridden(TrainRecipeCatalogRelationField::Momentum));
    }

    #[test]
    fn scheduler_selection_tracks_optimizer_recipe_until_explicitly_overridden() {
        let mut model = installed_settings_model();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Optimizer(TrainOptimizerKind::Muon)),
        )
        .unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(
            effective_scheduler(request),
            recipe(TrainOptimizerKind::Muon).lrscheduler
        );

        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::LrScheduler(
                TrainLrSchedulerKind::Cosine,
            )),
        )
        .unwrap();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Optimizer(
                TrainOptimizerKind::AdamW,
            )),
        )
        .unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert!(
            request
                .recipe
                .overrides
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
    #[test]
    fn model_recipe_scope_preserves_mixed_optimizers_and_resets_only_its_overrides() {
        use crate::generated::*;
        let mut model = installed_settings_model();
        let request = &mut model.draft.as_mut().unwrap().workflows.train.request;
        let global = request.recipe.clone();
        request.laneconfiguration.models = (1..=2)
            .map(|modelid| TrainModelSettings {
                modelid,
                seed: 42,
                recipe: global.clone(),
                coefficient: 1.0,
            })
            .collect();
        request.laneconfiguration.nextmodelid = 3;
        model.recipe_model = Some(1);
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Optimizer(TrainOptimizerKind::SGD)),
        )
        .unwrap();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Momentum(0.8)),
        )
        .unwrap();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::LrScheduler(
                TrainLrSchedulerKind::UltralyticsLinear,
            )),
        )
        .unwrap();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::WarmupBiasLr(0.04)),
        )
        .unwrap();
        let first = model
            .draft
            .as_ref()
            .unwrap()
            .workflows
            .train
            .request
            .laneconfiguration
            .models[0]
            .clone();
        model.recipe_model = Some(2);
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Optimizer(TrainOptimizerKind::Muon)),
        )
        .unwrap();
        update(
            &mut model,
            Message::Recipe(TrainRecipeSettingsEdit::Lr(0.004)),
        )
        .unwrap();
        model.recipe_model = Some(1);
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .laneconfiguration
                .models[0],
            first
        );
        update(&mut model, Message::Recipe(TrainRecipeSettingsEdit::Reset)).unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(request.recipe, global);
        assert_eq!(
            request.laneconfiguration.models[0].recipe.optimizer,
            TrainOptimizerKind::SGD
        );
        assert!(
            !request.laneconfiguration.models[0]
                .recipe
                .overrides
                .overridden(TrainRecipeCatalogRelationField::WarmupBiasLr)
        );
        assert_eq!(
            effective_trainrecipesettings_lr(&request.laneconfiguration.models[1].recipe),
            0.004
        );
        model.recipe_model = None;
        update(&mut model, Message::UnfreezeLast(3)).unwrap();
        update(&mut model, Message::DisableAugmentationLast(5)).unwrap();
        let request = &model.draft.as_ref().unwrap().workflows.train.request;
        assert_eq!(
            (
                request.unfreezeencoderlastepochs,
                request.disableaugmentationlastepochs
            ),
            (3, 5)
        );
        assert!(train_scheduler_available(
            TrainOptimizerKind::SGD,
            TrainLrSchedulerKind::UltralyticsLinear
        ));
        assert!(!train_scheduler_available(
            TrainOptimizerKind::AdamW,
            TrainLrSchedulerKind::UltralyticsLinear
        ));
    }
}
