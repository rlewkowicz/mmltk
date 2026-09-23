use crate::fluent_theme::Element;
use crate::generated::{self, FeatureId, WorkflowOutputFacts};
use crate::view::shared::status_text;
use crate::view_model::ApplicationModel;
use iced::widget::{button, checkbox, column, container, Column};

#[derive(Debug, Clone)]
pub enum Message {
    Auto(bool),
    Browse(u64),
}

pub fn content<'a>(
    workflow: FeatureId,
    model: &'a ApplicationModel,
    settings: &'a crate::view::settings::SettingsModel,
    facts: Option<&'a WorkflowOutputFacts>,
) -> Column<'a, Message> {
    let selection = settings.draft.as_ref().map(|draft| match workflow {
        FeatureId::Train => &draft.workflows.train.output,
        FeatureId::Validate => &draft.workflows.validate.output,
        FeatureId::Predict => &draft.workflows.predict.output,
        FeatureId::Export => &draft.workflows.exportstate.output,
        _ => unreachable!(),
    });
    let id = match workflow {
        FeatureId::Train => generated::constraint_workflowstrainoutputdirectory().stable_field_id,
        FeatureId::Validate => generated::constraint_workflowsvalidateoutputdirectory().stable_field_id,
        FeatureId::Predict => generated::constraint_workflowspredictoutputdirectory().stable_field_id,
        FeatureId::Export => generated::constraint_workflowsexportstateoutputdirectory().stable_field_id,
        _ => unreachable!(),
    };
    let automatic = selection.is_none_or(|value| value.automatic);
    let directory = facts.filter(|facts| !facts.directory.is_empty()).map(|facts| facts.directory.as_str())
        .unwrap_or_else(|| selection.filter(|value| !value.automatic).map_or("", |value| value.directory.as_str()));
    let dialog = model.workflow.dialogs(workflow).find(|fact| fact.stable_field_id == id);
    let mut content = column![
        checkbox(automatic).label("Auto Output")
            .on_toggle_maybe(model.settings_edit_available().then_some(Message::Auto)),
        container(button("Browse Output").style(crate::fluent_theme::button_primary).width(iced::Fill)
            .on_press_maybe(dialog.filter(|fact| !settings.has_local_edits() && model.file_dialog_open_available(fact, workflow))
                .map(|fact| Message::Browse(fact.stable_field_id))))
            .id(format!("{}.output.browse", workflow_name(workflow))),
        status_text(directory).size(12),
    ].spacing(super::FIELD_SPACING);
    if let Some(selection) = selection.filter(|value| !value.automatic && !value.directory.is_empty() && value.directory != directory) {
        content = content.push(status_text(format!("Next output: {}", selection.directory)).size(12));
    }
    if let Some(facts) = facts {
        for path in &facts.artifacts {
            content = content.push(status_text(path.as_str()).size(12));
        }
        if facts.completedsamples > 0 {
            content = content.push(status_text(format!("{} samples: {}", facts.completedsamples, facts.samplesdirectory)).size(12));
        }
        if !facts.recentsample.is_empty() {
            content = content.push(status_text(facts.recentsample.as_str()).size(12));
        }
        if !facts.partialvideo.is_empty() {
            content = content.push(status_text(format!("Recoverable video: {}", facts.partialvideo)).size(12));
        }
    }
    content
}

fn workflow_name(workflow: FeatureId) -> &'static str {
    match workflow {
        FeatureId::Train => "train",
        FeatureId::Validate => "validate",
        FeatureId::Predict => "predict",
        FeatureId::Export => "export",
        _ => unreachable!(),
    }
}

pub fn view<'a>(workflow: FeatureId, model: &'a ApplicationModel,
    settings: &'a crate::view::settings::SettingsModel, facts: Option<&'a WorkflowOutputFacts>) -> Element<'a, Message> {
    let id = match workflow {
        FeatureId::Train => "train.card.output",
        FeatureId::Validate => "validate.card.output",
        FeatureId::Predict => "predict.card.output",
        FeatureId::Export => "export.card.output",
        _ => unreachable!(),
    };
    crate::view::shared::identified(id,
        crate::view::shared::card("Output", "", content(workflow, model, settings, facts)))
}

pub fn automatic(settings: &mut crate::view::settings::SettingsModel, workflow: FeatureId, value: bool)
    -> Result<crate::view::settings::EditSchedule, String> {
    settings.edit(crate::view::settings::EditCadence::Immediate, |draft| match workflow {
        FeatureId::Train => generated::edit_workflowstrainoutputautomatic(draft, value),
        FeatureId::Validate => generated::edit_workflowsvalidateoutputautomatic(draft, value),
        FeatureId::Predict => generated::edit_workflowspredictoutputautomatic(draft, value),
        FeatureId::Export => generated::edit_workflowsexportstateoutputautomatic(draft, value),
        _ => unreachable!(),
    })
}
