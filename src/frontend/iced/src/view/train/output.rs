use crate::fluent_theme::Element;
use crate::generated::{self, FeatureId};
use crate::view_model::ApplicationModel;
use iced::widget::{button, column, container, text};
#[derive(Debug, Clone)]
pub enum Message {
    DirectoryChanged(String),
    Browse(u64),
    Open(String),
    History(generated::TrainingHistoryQuery),
    Inspect(String),
    Resume(String),
    Live,
}
pub fn view<'a>(
    model: &'a ApplicationModel,
    settings: &'a crate::view::settings::SettingsModel,
) -> Element<'a, Message> {
    let request = settings
        .draft
        .as_ref()
        .map(|value| &value.workflows.train.request);
    let directory = request.map_or("", |value| value.outputdir.as_str());
    let checkpoint = request.map_or("", |value| value.resumepath.as_str());
    let available = !settings.has_local_edits() && model.settings_edit_available();
    let dialogs = model
        .workflow
        .dialogs(FeatureId::Train)
        .filter(|fact| {
            [
                generated::constraint_workflowstrainrequestoutputdir().stable_field_id,
                generated::constraint_workflowstrainrequestresumepath().stable_field_id,
            ]
            .contains(&fact.stable_field_id)
        })
        .fold(column![], |column, fact| {
            column.push(
                container(
                    button(fact.title).on_press_maybe(
                        model
                            .file_dialog_open_available(fact, FeatureId::Train)
                            .then_some(Message::Browse(fact.stable_field_id)),
                    ),
                )
                .id(format!("dialog.{}", fact.stable_field_id)),
            )
        });
    let mut content = column![
        crate::view::workflow::fields::text_field(
            "Output directory",
            generated::constraint_workflowstrainrequestoutputdir().stable_field_id,
            directory,
            model.settings_edit_available(),
            Message::DirectoryChanged,
        ),
        dialogs,
        button("Open saved run").on_press_maybe(
            (available
                && !directory.is_empty()
                && !model.has_pending(generated::ApplicationIntentEndpoint::TrainingOpenRun))
            .then(|| Message::Open(directory.to_owned()))
        ),
        button("Current live run").on_press(Message::Live),
        button("Inspect checkpoint").on_press_maybe(
            (available
                && !checkpoint.is_empty()
                && !model
                    .has_pending(generated::ApplicationIntentEndpoint::TrainingInspectCheckpoint))
            .then(|| Message::Inspect(checkpoint.to_owned()))
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    if !checkpoint.is_empty() {
        content = content.push(text(checkpoint));
    }
    if let Some(opened) = &model.workflow.training_run {
        let page = model
            .workflow
            .training_history
            .as_ref()
            .filter(|page| page.generation == opened.generation);
        content = content
            .push(text(format!(
                "{} · {:?} weights",
                opened.run.runid, opened.run.evaluatedweights
            )))
            .push(
                button(if page.is_none() {
                    "Load history"
                } else {
                    "More history"
                })
                .on_press_maybe(
                    (page.is_none_or(|page| page.more)
                        && !model
                            .has_pending(generated::ApplicationIntentEndpoint::TrainingHistory))
                    .then(|| {
                        Message::History(generated::TrainingHistoryQuery {
                            generation: opened.generation,
                            cursor: page.map_or(0, |page| page.nextcursor),
                            count: 32,
                        })
                    }),
                ),
            );
    }
    if let Some(inspected) = &model.workflow.training_checkpoint {
        content = content.push(text(match &inspected.classlayout {
            Some(layout) => format!(
                "Saved class layout: {} foreground classes · {} outputs · {:?}",
                layout.foreground.names.len(),
                layout.slots.len(),
                layout.provenance.origin
            ),
            None => "Class identity unresolved; inspect or supply the required model class layout."
                .into(),
        }));
        content = content
            .push(text(format!(
                "{} · epoch {} · {}",
                inspected.path,
                inspected.epoch,
                if inspected.resumable {
                    "Full resumable checkpoint"
                } else {
                    "Not resumable"
                }
            )))
            .push(
                button("Resume").on_press_maybe(
                    (available
                        && inspected.resumable
                        && model.workflow.pending_start.is_none()
                        && !model.has_pending(
                            generated::ApplicationIntentEndpoint::TrainingPrepareResume,
                        )
                        && model
                            .workflow
                            .training
                            .as_ref()
                            .is_some_and(|snapshot| !snapshot.local.active))
                    .then(|| Message::Resume(inspected.path.clone())),
                ),
            );
    }
    let record = if let Some(opened) = &model.workflow.training_run {
        model
            .workflow
            .training_history
            .as_ref()
            .filter(|page| page.generation == opened.generation)
            .and_then(|page| page.records.last())
    } else {
        model
            .workflow
            .training
            .as_ref()
            .and_then(|snapshot| snapshot.metrics.as_ref())
    };
    if let Some(record) = record {
        for (label, path) in [
            ("Full checkpoint", &record.progress.fullcheckpointpath),
            ("Selected / epoch weights", &record.progress.checkpointpath),
        ] {
            if !path.is_empty() {
                content = content.push(text(format!("{label}: {path}")));
            }
        }
    }
    if let Some(snapshot) = &model.workflow.training {
        if !snapshot.outputdirectory.is_empty() {
            content = content.push(text(format!(
                "Active output directory: {}", snapshot.outputdirectory
            )));
        }
        if snapshot.persistence.degraded {
            content = content.push(text(format!(
                "History incomplete: {} ({} dropped)",
                snapshot.persistence.error, snapshot.persistence.droppedrecords
            )));
        }
    }
    crate::view::shared::identified(
        "train.card.output",
        crate::view::shared::card(
            "Output",
            "Open and inspect without starting training.",
            content,
        ),
    )
}
