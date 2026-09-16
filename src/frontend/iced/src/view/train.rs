use crate::fluent_theme::Element;
use crate::generated::ProviderOfferIdentity;
use crate::view::settings::{EditSchedule, SettingsModel};
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};

mod advanced;
pub(crate) mod dataset;
pub mod output;

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
    pub fn sync_metrics(&mut self, model: &ApplicationModel, visible: bool) {
        self.metrics.rebase(model, visible);
    }
    pub fn rebase(&mut self, model: &ApplicationModel) {
        self.model_card
            .rebase(model, crate::generated::FeatureId::Train);
    }
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
            Message::StartRequested => Outcome::StartRequested,
            Message::TrainingStopRequested => Outcome::TrainingStopRequested,
            Message::QueryOffersRequested => Outcome::QueryOffersRequested,
            Message::ClearOffersRequested => Outcome::ClearOffersRequested,
            Message::OfferSelected(identity) => Outcome::OfferSelected(identity),
            Message::StartRemoteRequested => Outcome::StartRemoteRequested,
            Message::StopRemoteRequested => Outcome::StopRemoteRequested,
            Message::RetryReconciliationRequested => Outcome::RetryReconciliationRequested,
            Message::Dataset(message) => match dataset::update(model, message)? {
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
            self.model_card
                .view(crate::view::workflow::model_card::State::from_settings(
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
                ))
                .map(Message::Model),
            dataset::view(
                installed_train,
                model,
                settings_edit_available,
                settings_settled,
            )
            .map(Message::Dataset),
            text(
                model
                    .workflow
                    .start_detail(crate::generated::FeatureId::Train)
            ),
            crate::view::workflow::primary_action(
                crate::generated::FeatureId::Train,
                "Start training",
                settings
                    .draft
                    .as_ref()
                    .is_some_and(|draft| {
                        model.compute_start_available(draft, crate::generated::FeatureId::Train)
                    })
                    .then_some(Message::StartRequested),
                crate::view::workflow::progress::compute(training),
            ),
        ]
        .spacing(crate::view::workflow::SECTION_SPACING)
        .into();
        let chart_width =
            crate::view::workflow::Composition::new(crate::generated::FeatureId::Train, width)
                .center_width()
                - 2.0 * crate::view::workflow::CARD_PADDING;
        let workspace = self.metrics.view(chart_width).map(Message::Metrics);
        let advanced = crate::view::shared::identified(
            "train.card.advanced",
            advanced::view(installed_train, settings, settings_edit_available)
                .map(Message::Advanced),
        );
        let diagnostics = crate::view::shared::card(
            "Training status",
            "Native dataset, local training, and provider facts.",
            column![
                container(text(crate::view::shared::artifact_status(dataset)))
                    .id(DATASET_STATUS_ID),
                container(text(crate::view::shared::compute_status(training))).id("train.progress"),
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
                output::view(model, settings).map(Message::Output),
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

        let draft = crate::generated::default_workflowstrainrequestweightspath().unwrap();
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
