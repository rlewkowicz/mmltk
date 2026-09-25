use crate::fluent_theme::Element;
use crate::generated::{
    PredictionCompiledSampling as Compiled, PredictionVideoSaving as Video, SourceKind,
};
use crate::view::workflow::fields;
use iced::widget::{button, column, container, radio};
#[derive(Debug, Clone)]
pub enum Message {
    Ignore,
    Single(bool),
    CompiledEnabled(bool),
    CompiledMode(Compiled),
    Percent(u64),
    Total(u64),
    VideoEnabled(bool),
    VideoMode(Video),
    Samples(u64),
    Inspect(String),
}
pub fn matching_count(
    model: &crate::view_model::ApplicationModel,
    draft: &crate::generated::PredictViewState,
) -> Option<u64> {
    model.predict_snapshot.as_ref().and_then(|snapshot| {
        let inspected = &snapshot.inspection;
        (!inspected.active
            && inspected.path == draft.source.compiledpath
            && inspected.count > 0
            && inspected.error.is_empty())
        .then_some(inspected.count)
    })
}
// Called by the ordinary application update path after native state and local
// edits settle. Rendering only reads the canonical draft.
pub fn settle_total(
    model: &crate::view_model::ApplicationModel,
    settings: &mut crate::view::settings::SettingsModel,
) -> Result<Option<crate::view::settings::EditSchedule>, String> {
    let Some(draft) = settings
        .draft
        .as_ref()
        .map(|draft| &draft.workflows.predict)
    else {
        return Ok(None);
    };
    if draft.source.kind != SourceKind::CompiledDataset {
        return Ok(None);
    }
    let Some(count) = matching_count(model, draft) else {
        return Ok(None);
    };
    if draft.saving.compiledtotal <= count {
        return Ok(None);
    }
    update(settings, Message::Total(count)).map(Some)
}
pub fn view<'a>(
    model: &'a crate::view_model::ApplicationModel,
    draft: Option<&'a crate::generated::PredictViewState>,
    enabled: bool,
) -> Element<'a, Message> {
    let Some(draft) = draft else {
        return column![].into();
    };
    let saving = &draft.saving;
    let count = matching_count(model, draft);
    let mut total_constraint = crate::generated::constraint_workflowspredictsavingcompiledtotal();
    total_constraint.maximum = count.map(|value| value as f64);
    let compiled = column![
        container(radio(
            "Percent %",
            Compiled::Percent,
            Some(saving.compiledmode),
            move |value| if enabled {
                Message::CompiledMode(value)
            } else {
                Message::Ignore
            }
        ))
        .id("predict.save.percent"),
        container(radio(
            "Total",
            Compiled::Total,
            Some(saving.compiledmode),
            move |value| if enabled {
                Message::CompiledMode(value)
            } else {
                Message::Ignore
            }
        ))
        .id("predict.save.total"),
        crate::view::shared::disclosure(
            "predict.save.percent.value",
            saving.compiledmode == Compiled::Percent,
            fields::number_u64(
                "Percent %",
                saving.compiledpercent as u64,
                crate::generated::constraint_workflowspredictsavingcompiledpercent(),
                enabled,
                Message::Percent
            )
        ),
        crate::view::shared::disclosure(
            "predict.save.total.value",
            saving.compiledmode == Compiled::Total,
            fields::number_u64(
                "Total",
                saving.compiledtotal,
                total_constraint,
                enabled && count.is_some(),
                Message::Total
            )
        ),
        container(
            button("Inspect dataset count").on_press_maybe(
                (enabled
                    && !draft.source.compiledpath.is_empty()
                    && !model
                        .predict_snapshot
                        .as_ref()
                        .is_some_and(|s| s.inspection.active))
                .then(|| Message::Inspect(draft.source.compiledpath.clone()))
            )
        )
        .id("predict.save.inspect"),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    let video = column![
        container(radio(
            "Samples",
            Video::Samples,
            Some(saving.videomode),
            move |value| if enabled {
                Message::VideoMode(value)
            } else {
                Message::Ignore
            }
        ))
        .id("predict.save.video.samples"),
        container(radio(
            "Full",
            Video::Full,
            Some(saving.videomode),
            move |value| if enabled {
                Message::VideoMode(value)
            } else {
                Message::Ignore
            }
        ))
        .id("predict.save.video.full"),
        crate::view::shared::disclosure(
            "predict.save.video.count",
            saving.videomode == Video::Samples,
            fields::number_u64(
                "Samples",
                saving.videosamples,
                crate::generated::constraint_workflowspredictsavingvideosamples(),
                enabled,
                Message::Samples
            )
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING);
    column![
        crate::view::shared::disclosure(
            "predict.save.image",
            draft.source.kind == SourceKind::SingleImage,
            fields::toggle(
                "Save sample",
                saving.singleenabled,
                enabled,
                Message::Single
            )
        ),
        crate::view::shared::disclosure(
            "predict.save.compiled",
            draft.source.kind == SourceKind::CompiledDataset,
            column![
                fields::toggle(
                    "Save samples",
                    saving.compiledenabled,
                    enabled,
                    Message::CompiledEnabled
                ),
                crate::view::shared::disclosure(
                    "predict.save.compiled.options",
                    saving.compiledenabled,
                    compiled
                )
            ]
            .spacing(crate::view::workflow::FIELD_SPACING)
        ),
        crate::view::shared::disclosure(
            "predict.save.video",
            draft.source.kind == SourceKind::VideoFile,
            column![
                fields::toggle(
                    "Save video",
                    saving.videoenabled,
                    enabled,
                    Message::VideoEnabled
                ),
                crate::view::shared::disclosure(
                    "predict.save.video.options",
                    saving.videoenabled,
                    video
                )
            ]
            .spacing(crate::view::workflow::FIELD_SPACING)
        ),
    ]
    .spacing(crate::view::workflow::FIELD_SPACING)
    .into()
}
pub fn update(
    settings: &mut crate::view::settings::SettingsModel,
    message: Message,
) -> Result<crate::view::settings::EditSchedule, String> {
    if matches!(message, Message::Ignore | Message::Inspect(_)) {
        return Err("Not a prediction saving edit".into());
    }
    settings.edit(
        crate::view::settings::EditCadence::Immediate,
        move |draft| match message {
            Message::Single(value) => {
                crate::generated::edit_workflowspredictsavingsingleenabled(draft, value)
            }
            Message::CompiledEnabled(value) => {
                crate::generated::edit_workflowspredictsavingcompiledenabled(draft, value)
            }
            Message::CompiledMode(value) => {
                crate::generated::edit_workflowspredictsavingcompiledmode(draft, value)
            }
            Message::Percent(value) => {
                crate::generated::edit_workflowspredictsavingcompiledpercent(draft, value as u32)
            }
            Message::Total(value) => {
                crate::generated::edit_workflowspredictsavingcompiledtotal(draft, value)
            }
            Message::VideoEnabled(value) => {
                crate::generated::edit_workflowspredictsavingvideoenabled(draft, value)
            }
            Message::VideoMode(value) => {
                crate::generated::edit_workflowspredictsavingvideomode(draft, value)
            }
            Message::Samples(value) => {
                crate::generated::edit_workflowspredictsavingvideosamples(draft, value)
            }
            Message::Ignore | Message::Inspect(_) => unreachable!("non-edit message was excluded"),
        },
    )
}

#[cfg(test)]
mod tests {
    use super::matching_count;
    #[test]
    fn counts_only_bound_the_exact_settled_source() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut draft = model
            .settings_snapshot
            .as_ref()
            .unwrap()
            .settingsstate
            .workflows
            .predict
            .clone();
        draft.source.compiledpath = "selected.bin".into();
        {
            let inspected = &mut model.predict_snapshot.as_mut().unwrap().inspection;
            inspected.path = "selected.bin".into();
            inspected.count = 3;
        }
        assert_eq!(matching_count(&model, &draft), Some(3));
        draft.source.compiledpath = "replacement.bin".into();
        assert_eq!(matching_count(&model, &draft), None);
        draft.source.compiledpath = "selected.bin".into();
        model.predict_snapshot.as_mut().unwrap().inspection.active = true;
        assert_eq!(matching_count(&model, &draft), None);
        model.predict_snapshot.as_mut().unwrap().inspection.active = false;
        model.predict_snapshot.as_mut().unwrap().inspection.error = "unavailable".into();
        assert_eq!(matching_count(&model, &draft), None);
        model
            .predict_snapshot
            .as_mut()
            .unwrap()
            .inspection
            .error
            .clear();
        model.predict_snapshot.as_mut().unwrap().inspection.count = 0;
        assert_eq!(matching_count(&model, &draft), None);
    }
    #[test]
    fn total_settlement_edits_the_persisted_draft_once_and_never_restores_an_old_larger_value() {
        let mut model = crate::view_model::test_support::bootstrapped();
        let mut settings = crate::view::settings::SettingsModel::default();
        settings.install(model.settings_snapshot.as_ref().unwrap());
        settings
            .draft
            .as_mut()
            .unwrap()
            .workflows
            .predict
            .source
            .compiledpath = "small.bin".into();
        {
            let inspected = &mut model.predict_snapshot.as_mut().unwrap().inspection;
            inspected.path = "small.bin".into();
            inspected.count = 3;
        }
        let before = settings
            .draft
            .as_ref()
            .unwrap()
            .workflows
            .predict
            .saving
            .clone();
        assert!(
            super::settle_total(&model, &mut settings)
                .unwrap()
                .is_some()
        );
        assert_eq!(
            settings
                .draft
                .as_ref()
                .unwrap()
                .workflows
                .predict
                .saving
                .compiledtotal,
            3
        );
        assert!(
            super::settle_total(&model, &mut settings)
                .unwrap()
                .is_none()
        );
        let request = settings.take_request(false).unwrap();
        assert_eq!(request.updates.len(), 1);
        assert_eq!(
            request.updates[0],
            crate::generated::update_workflowspredictsavingcompiledtotal(3)
        );
        settings
            .draft
            .as_mut()
            .unwrap()
            .workflows
            .predict
            .source
            .compiledpath = "large.bin".into();
        assert!(
            super::settle_total(&model, &mut settings)
                .unwrap()
                .is_none()
        );
        {
            let inspected = &mut model.predict_snapshot.as_mut().unwrap().inspection;
            inspected.path = "large.bin".into();
            inspected.count = 20;
        }
        assert!(
            super::settle_total(&model, &mut settings)
                .unwrap()
                .is_none()
        );
        let saved = &settings.draft.as_ref().unwrap().workflows.predict.saving;
        assert_eq!(saved.compiledtotal, 3);
        assert_eq!(saved.singleenabled, before.singleenabled);
        assert_eq!(saved.videomode, before.videomode);
        assert_eq!(saved.videosamples, before.videosamples);
    }
}
