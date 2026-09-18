use crate::fluent_theme::Element;
use crate::generated::{self, FeatureId};
use crate::view_model::ApplicationModel;
use iced::widget::{button, checkbox, column, container, text};
#[derive(Debug, Clone)]
pub enum Message {
    Auto(bool),
    Browse(u64),
}
pub fn view<'a>(
    model: &'a ApplicationModel,
    settings: &'a crate::view::settings::SettingsModel,
    chart_omissions: u64,
) -> Element<'a, Message> {
    let train = settings.draft.as_ref().map(|value| &value.workflows.train);
    let automatic = train.is_none_or(|train| train.autooutput);
    let selected = train.map_or("", |train| train.request.outputdir.as_str());
    let active = model
        .workflow
        .training
        .as_ref()
        .filter(|snapshot| {
            model.workflow.output.saved().is_none() && !snapshot.outputdirectory.is_empty()
        })
        .map(|snapshot| snapshot.outputdirectory.as_str());
    let directory = active.unwrap_or(if automatic { "" } else { selected });
    let fact = model.workflow.dialogs(FeatureId::Train).find(|fact| {
        fact.stable_field_id
            == generated::constraint_workflowstrainrequestoutputdir().stable_field_id
    });
    let mut content = column![
        checkbox(automatic)
            .label("Auto Output")
            .on_toggle_maybe(model.settings_edit_available().then_some(Message::Auto)),
        container(
            button("Browse Output")
                .style(crate::fluent_theme::button_primary)
                .width(iced::Fill)
                .on_press_maybe(
                    fact.filter(|fact| !settings.has_local_edits()
                        && model.file_dialog_open_available(fact, FeatureId::Train))
                        .map(|fact| Message::Browse(fact.stable_field_id))
                )
        )
        .id("train.output.browse"),
        text(directory).size(12),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    if chart_omissions > 0 {
        content = content.push(text(format!("Charts omit {chart_omissions} older disconnected summaries; saved history remains unchanged.")));
    }
    if let Some(run) = model
        .workflow
        .output
        .run()
        .and_then(|opened| opened.run.as_ref())
        && !run.runid.is_empty()
    {
        content = content.push(text(format!(
            "{} · {:?} weights",
            run.runid, run.evaluatedweights
        )));
    }
    let record = if model.workflow.output.saved().is_some() {
        model
            .workflow
            .output
            .page()
            .and_then(|page| page.records.last())
    } else {
        model
            .workflow
            .training
            .as_ref()
            .and_then(|snapshot| snapshot.metrics.as_ref())
    };
    if let Some(record) = record {
        if record.droppedbefore > 0 {
            content = content.push(text(format!(
                "History incomplete: {} records dropped",
                record.droppedbefore
            )));
        }
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
        if snapshot.persistence.degraded {
            content = content.push(text(format!(
                "History incomplete: {} ({} dropped)",
                snapshot.persistence.error, snapshot.persistence.droppedrecords
            )));
        }
    }
    crate::view::shared::identified(
        "train.card.output",
        crate::view::shared::card("Output", "", content),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn output_nodes(model: &ApplicationModel) -> usize {
        fn count(tree: &iced::advanced::widget::Tree) -> usize {
            1 + tree.children.iter().map(count).sum::<usize>()
        }
        let settings = crate::view::settings::installed_settings_model();
        let output = view(model, &settings, 0);
        count(&iced::advanced::widget::Tree::new(&output))
    }

    #[test]
    fn output_facts_follow_live_and_saved_records_and_omit_empty_paths() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let baseline = output_nodes(&model);
        let mut live = crate::view::metrics::tests::record();
        model.workflow.training.as_mut().unwrap().metrics = Some(live.clone());
        assert_eq!(output_nodes(&model), baseline);
        live.progress.fullcheckpointpath = "/live/full.pt".into();
        model.workflow.training.as_mut().unwrap().metrics = Some(live.clone());
        assert_eq!(output_nodes(&model), baseline + 1);
        live.progress.checkpointpath = "/live/selected.pt".into();
        model.workflow.training.as_mut().unwrap().metrics = Some(live);
        assert_eq!(output_nodes(&model), baseline + 2);
        let configuration = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .train
            .request
            .clone();
        for weights in [
            generated::EvaluatedWeights::Ordinary,
            generated::EvaluatedWeights::Ema,
        ] {
            let mut opened =
                crate::view_model::test_support::saved_training_run(configuration.clone());
            opened.run.as_mut().unwrap().evaluatedweights = weights;
            model.workflow.output.select_saved(opened.directory.clone());
            model.workflow.output.saved_mut().unwrap().run = Some(opened);
            // Only saved identity is visible; live checkpoint paths cannot leak.
            assert_eq!(output_nodes(&model), baseline + 1);
            for (full, selected) in [
                ("", ""),
                ("/saved/full.pt", ""),
                ("", "/saved/selected.pt"),
                ("/saved/full.pt", "/saved/selected.pt"),
            ] {
                let mut record = crate::view::metrics::tests::record();
                record.progress.fullcheckpointpath = full.into();
                record.progress.checkpointpath = selected.into();
                model.workflow.output.saved_mut().unwrap().page =
                    Some(generated::TrainingHistoryPage {
                        generation: 3,
                        nextcursor: 1,
                        more: false,
                        records: vec![record],
                    });
                assert_eq!(
                    output_nodes(&model),
                    baseline
                        + 1
                        + usize::from(!full.is_empty())
                        + usize::from(!selected.is_empty())
                );
            }
            let saved = model.workflow.output.saved_mut().unwrap();
            saved
                .run
                .as_mut()
                .unwrap()
                .run
                .as_mut()
                .unwrap()
                .runid
                .clear();
            saved.page = None;
            assert_eq!(output_nodes(&model), baseline);
        }
        model.workflow.output.live();
        assert_eq!(output_nodes(&model), baseline + 2);
    }
}
