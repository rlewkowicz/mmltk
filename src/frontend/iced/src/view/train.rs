use crate::fluent_theme::Element;
use crate::generated::ProviderOfferIdentity;
use crate::view::settings::{EditSchedule, SettingsModel};
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};

mod advanced;
pub(crate) mod dataset;
pub mod output;
mod progress;

use crate::view_model::ContinuationMode;

pub const DATASET_CARD_ID: &str = "train.card.dataset";
pub const COMPILE_DATASET_ID: &str = "train.compile_dataset";
pub const DATASET_STATUS_ID: &str = "train.dataset.status";
pub const DATASET_SOURCE_ID: &str = "train.dataset.source";
pub const DATASET_BROWSE_ID: &str = "train.dataset.browse";
pub const COMPILED_DIRECTORY_ID: &str = "train.dataset.compiled_directory";
pub const COMPILE_DIMENSIONS_ID: &str = "train.dataset.compile_dimensions";
pub const COMPILE_RESOLUTION_ID: &str = "train.dataset.resolution";
pub const COMPILE_PROGRESS_ID: &str = "train.compile_dataset.progress";
pub const BENCHMARK_OVERRIDE_ID: &str = "train.dataset.benchmark_override";
pub const MATCH_FREE_ASSIGNMENT_ID: &str = "train.advanced.assignment.match_free";

#[derive(Debug, Clone)]
pub enum Message {
    Continuation(ContinuationMode),
    AspectSelected(crate::generated::WorkspaceAspectRatio),
    StartRequested,
    TrainingStopRequested,
    QueryOffersRequested,
    ClearOffersRequested,
    OfferSelected(ProviderOfferIdentity),
    StartRemoteRequested,
    StopRemoteRequested,
    RetryReconciliationRequested,
    Dataset(dataset::Message),
    Advanced(advanced::Message),
    Model(crate::view::workflow::model_card::Message),
    Metrics(crate::view::metrics::Message),
    Output(output::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    Continuation(ContinuationMode),
    // CLEANUP-IGNORE: Train's outcome begins with domain requests parallel to, but distinct from, raw messages.
    Output(output::Message),
    CompileRequested,
    StartRequested,
    DatasetStopRequested,
    TrainingStopRequested,
    QueryOffersRequested,
    ClearOffersRequested,
    OfferSelected(ProviderOfferIdentity),
    StartRemoteRequested,
    StopRemoteRequested,
    RetryReconciliationRequested,
    DialogRequested(u64),
    SettingsEdited(EditSchedule),
    Model(crate::view::workflow::model_card::Outcome),
}

#[derive(Default)]
pub struct Component {
    model_card: crate::view::workflow::model_card::Component,
    metrics: crate::view::metrics::Component,
}

impl Component {
    pub fn reset(&mut self, visible: bool) {
        self.model_card = Default::default();
        self.metrics.reset(visible);
    }
    pub(crate) fn chart_view(
        &self,
        chart: crate::view::metrics::Chart,
    ) -> Option<crate::view::metrics::ChartView> {
        self.metrics.chart_view(chart)
    }
    pub fn sync_metrics(&mut self, model: &ApplicationModel, visible: bool) {
        self.metrics.rebase(model, visible);
    }
    pub fn rebase(&mut self, model: &ApplicationModel) {
        self.model_card
            .rebase(model, crate::generated::FeatureId::Train);
    }
}

fn continuation_controls<'a>(
    model: &'a ApplicationModel,
    train: Option<&crate::generated::TrainViewState>,
    enabled: bool,
) -> Element<'a, Message> {
    let current = train.is_some_and(|train| model.workflow.train_continuation.matches(train));
    let selected = if current {
        model.workflow.train_continuation.mode
    } else {
        ContinuationMode::Transfer
    };
    let radio = |label, mode, available| {
        iced::widget::radio(label, mode, Some(selected), Message::Continuation).style(
            move |theme, status| {
                if available {
                    iced_fluent_theme::radio::default(theme, status)
                } else {
                    iced_fluent_theme::radio::disabled(theme, status)
                }
            },
        )
    };
    let mut controls = column![
        radio("Transfer", ContinuationMode::Transfer, enabled),
        radio(
            "Resume",
            ContinuationMode::Resume,
            enabled
                && current
                && model
                    .workflow
                    .train_continuation
                    .checkpoint()
                    .is_some_and(|checkpoint| checkpoint.resumable)
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    if current {
        match &model.workflow.train_continuation.capability {
            crate::view_model::CheckpointCapability::Pending { .. } => {
                controls = controls.push(text("Inspecting checkpoint…").size(12))
            }
            crate::view_model::CheckpointCapability::Failed(detail) => {
                controls = controls.push(text(detail).size(12))
            }
            _ => {}
        }
    }
    controls.into()
}

pub(crate) fn offer_identity(offer: &crate::generated::ProviderOffer) -> ProviderOfferIdentity {
    ProviderOfferIdentity {
        offerid: offer.offerid,
    }
}

impl Component {
    pub fn update(
        &mut self,
        model: &mut SettingsModel,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let outcome = match message {
            Message::Continuation(mode) => Outcome::Continuation(mode),
            Message::AspectSelected(aspect) => {
                Outcome::SettingsEdited(crate::view::workspace::edit_aspect(model, aspect)?)
            }
            Message::StartRequested => Outcome::StartRequested,
            Message::TrainingStopRequested => Outcome::TrainingStopRequested,
            Message::QueryOffersRequested => Outcome::QueryOffersRequested,
            Message::ClearOffersRequested => Outcome::ClearOffersRequested,
            Message::OfferSelected(identity) => Outcome::OfferSelected(identity),
            Message::StartRemoteRequested => Outcome::StartRemoteRequested,
            Message::StopRemoteRequested => Outcome::StopRemoteRequested,
            Message::RetryReconciliationRequested => Outcome::RetryReconciliationRequested,
            Message::Dataset(message) => match dataset::update(model, message)? {
                dataset::Outcome::Ignored => return Ok(None),
                dataset::Outcome::SettingsEdited(schedule) => Outcome::SettingsEdited(schedule),
                dataset::Outcome::Browse(id) => Outcome::DialogRequested(id),
                dataset::Outcome::Compile => Outcome::CompileRequested,
                dataset::Outcome::Stop => Outcome::DatasetStopRequested,
            },
            Message::Advanced(message) => {
                Outcome::SettingsEdited(advanced::update(model, message)?)
            }
            Message::Model(message) => {
                let Some(outcome) = self.model_card.update(message, model)? else {
                    return Ok(None);
                };
                Outcome::Model(outcome)
            }
            Message::Metrics(message) => {
                self.metrics.update(message);
                return Ok(None);
            }
            Message::Output(message) => Outcome::Output(message),
        };
        Ok(Some(outcome))
    }
}

impl Component {
    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a crate::view::settings::SettingsModel,
        width: f32,
        body_height: f32,
    ) -> Element<'a, Message> {
        let installed_train = settings
            .draft
            .as_ref()
            .map(|settings| &settings.workflows.train);
        let settings_edit_available = settings.draft.is_some() && model.settings_edit_available();
        let settings_settled = !settings.has_local_edits();
        let offers = model
            .workflow
            .training
            .as_ref()
            .into_iter()
            .flat_map(|training| training.offers.offers.iter())
            .fold(column![].spacing(6), |column, offer| {
                column.push(
                    button(text(format!(
                        "{} ×{} · ${:.2}/hr · {}",
                        offer.gpuname, offer.gpucount, offer.hourlyprice, offer.location
                    )))
                    .on_press_maybe(
                        (settings_settled
                            && model.provider_select_available(&offer_identity(offer)))
                        .then_some(Message::OfferSelected(offer_identity(offer))),
                    ),
                )
            });
        let dataset = model.workflow.dataset.as_ref();
        let training = model
            .workflow
            .training
            .as_ref()
            .map(|training| &training.local);
        let setup: Element<'a, Message> = column![
            self.model_card.view_with(
                crate::view::workflow::model_card::State::from_settings(
                    crate::generated::FeatureId::Train,
                    settings.draft.as_ref(),
                    model.model_snapshot.as_ref(),
                    model.file_dialog.as_ref(),
                    settings_edit_available,
                    settings_settled
                        && settings.draft.as_ref().is_some_and(|draft| {
                            model.model_selection_available(
                                draft,
                                crate::generated::FeatureId::Train,
                            )
                        }),
                    model.model_stop_available(),
                ),
                continuation_controls(model, installed_train, settings_edit_available),
                Message::Model
            ),
            dataset::view(
                installed_train,
                model,
                settings_edit_available,
                settings_settled,
            )
            .map(Message::Dataset),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Train,
                model.primary_action_active(crate::generated::FeatureId::Train),
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        model.compute_start_available(draft, crate::generated::FeatureId::Train)
                    })
                    .then_some(Message::StartRequested),
                model
                    .training_stop_available()
                    .then_some(Message::TrainingStopRequested),
                crate::view::workflow::progress::compute(training),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let chart_width =
            crate::view::workflow::Composition::new(crate::generated::FeatureId::Train, width)
                .center_width()
                - 2.0 * crate::view::workflow::CARD_PADDING;
        let aspect = settings.draft.as_ref().map_or(
            crate::generated::WorkspaceAspectRatio::Widescreen,
            |draft| draft.ui.workspaceaspectratio,
        );
        let active = progress::active(model);
        let available_height = (body_height
            - self.metrics.controls_height()
            - 64.0
            - if active { 220.0 } else { 0.0 })
        .max(1.0);
        let (chart_width, chart_height) =
            crate::view::aspect_ratio::fit_extent(chart_width, available_height, aspect);
        let mut center = column![
            crate::view::aspect_ratio::selector(
                aspect,
                settings_edit_available,
                Message::AspectSelected
            ),
            self.metrics
                .view(chart_width, chart_height)
                .map(Message::Metrics),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .align_x(iced::Center);
        if active {
            center = center.push(progress::view(model));
        }
        let workspace = center.into();
        let advanced = crate::view::shared::identified(
            "train.card.advanced",
            advanced::view(installed_train, settings, settings_edit_available)
                .map(Message::Advanced),
        );
        let diagnostics = crate::view::shared::card(
            "Training status",
            "Native dataset, local training, and provider facts.",
            column![
                container(text(crate::view::workflow::status::artifact_status(
                    dataset
                )))
                .id(DATASET_STATUS_ID),
                container(text(crate::view::workflow::status::compute_status(
                    training
                )))
                .id("train.progress"),
                button("Stop training").on_press_maybe(
                    model
                        .training_stop_available()
                        .then_some(Message::TrainingStopRequested)
                ),
                crate::view::shared::identified(
                    "train.card.remote",
                    crate::view::shared::card(
                        "Remote training",
                        "Offers and selected identities are retained from generated replies.",
                        column![
                            button("Find offers").on_press_maybe(
                                (settings_settled && model.provider_query_available())
                                    .then_some(Message::QueryOffersRequested)
                            ),
                            container(
                                button("Clear offers").on_press_maybe(
                                    model
                                        .provider_clear_available()
                                        .then_some(Message::ClearOffersRequested)
                                )
                            )
                            .id("train.offers.clear"),
                            offers,
                            button("Start remote").on_press_maybe(
                                (settings_settled && model.remote_start_available())
                                    .then_some(Message::StartRemoteRequested)
                            ),
                            button("Stop remote").on_press_maybe(
                                model
                                    .remote_stop_available()
                                    .then_some(Message::StopRemoteRequested)
                            ),
                            container(
                                button("Retry reconciliation").on_press_maybe(
                                    model
                                        .remote_retry_available()
                                        .then_some(Message::RetryReconciliationRequested)
                                )
                            )
                            .id("train.remote.retry_reconciliation"),
                        ]
                        .spacing(crate::view::workflow::FIELD_SPACING),
                    ),
                ),
            ]
            .spacing(crate::view::workflow::FIELD_SPACING),
        );
        crate::view::workflow::Regions::new(
            crate::generated::FeatureId::Train,
            setup,
            workspace,
            advanced,
            column![
                output::view(model, settings, self.metrics.omitted_summaries())
                    .map(Message::Output),
                diagnostics
            ]
            .spacing(crate::view::workflow::SECTION_SPACING)
            .into(),
        )
        .render(width)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::view::settings::installed_settings_model;

    #[test]
    fn only_train_installs_continuation_controls_inside_the_weights_card() {
        let model = crate::view_model::test_support::bootstrapped();
        let settings = crate::view::settings::installed_settings_model();
        let state = |feature| {
            crate::view::workflow::model_card::State::from_settings(
                feature,
                settings.draft.as_ref(),
                None,
                None,
                true,
                true,
                false,
            )
        };
        let component = crate::view::workflow::model_card::Component::default();
        let mut train = component.view_with(
            state(crate::generated::FeatureId::Train),
            continuation_controls(
                &model,
                settings.draft.as_ref().map(|draft| &draft.workflows.train),
                true,
            ),
            Message::Model,
        );
        let mut validate = crate::view::workflow::model_card::view(
            state(crate::generated::FeatureId::Validate),
            0,
        );
        let mut train_tree = iced::advanced::widget::Tree::new(&train);
        let mut validate_tree = iced::advanced::widget::Tree::new(&validate);
        train_tree.diff(&mut train);
        validate_tree.diff(&mut validate);
        let body = |tree: &iced::advanced::widget::Tree| tree.children[2].children.len();
        assert_eq!(body(&train_tree), 2);
        assert_eq!(body(&validate_tree), 1);
        assert_eq!(train_tree.children[2].children[1].children.len(), 2);
    }

    #[test]
    fn train_messages_emit_specific_domain_outcomes() {
        let mut model = installed_settings_model();
        let mut component = Component::default();
        assert!(matches!(
            component.update(&mut model, Message::Dataset(dataset::Message::Compile)),
            Ok(Some(Outcome::CompileRequested))
        ));
        assert!(matches!(
            component.update(
                &mut model,
                Message::Model(crate::view::workflow::model_card::Message::PrepareRequested)
            ),
            Ok(Some(Outcome::Model(
                crate::view::workflow::model_card::Outcome::PrepareRequested
            )))
        ));
        assert!(matches!(
            component.update(&mut model, Message::ClearOffersRequested),
            Ok(Some(Outcome::ClearOffersRequested))
        ));
        assert!(matches!(
            component.update(&mut model, Message::RetryReconciliationRequested),
            Ok(Some(Outcome::RetryReconciliationRequested))
        ));
    }

    #[test]
    fn generated_model_controls_emit_typed_updates_and_keep_draft_local() {
        let mut model = installed_settings_model();
        let mut component = Component::default();
        let preset = crate::generated::RFDETR_PRESET_CATALOG
            .first()
            .expect("generated preset catalog");
        assert!(matches!(
            component.update(
                &mut model,
                Message::Model(crate::view::workflow::model_card::Message::PresetSelected(
                    0,
                )),
            ),
            Ok(Some(Outcome::Model(
                crate::view::workflow::model_card::Outcome::SettingsEdited(EditSchedule::Debounce(
                    _
                ))
            )))
        ));
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .presetname,
            preset.presetname
        );

        let draft = "/tmp/selected-model.safetensors".to_owned();
        component
            .update(
                &mut model,
                Message::Model(crate::view::workflow::model_card::Message::SourceSelected(
                    crate::generated::ModelSelectionSource::Custom,
                )),
            )
            .unwrap();
        component
            .update(
                &mut model,
                Message::Model(
                    crate::view::workflow::model_card::Message::ConfirmArtifact {
                        path: draft.clone(),
                        generation: 1,
                    },
                ),
            )
            .unwrap();
        assert_eq!(
            model
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .train
                .request
                .weightspath,
            draft
        );
    }
}
