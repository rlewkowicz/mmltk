use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view_model::ApplicationModel;
use iced::widget::{button, checkbox, container, row, space};
use iced::Center;

#[derive(Debug, Clone)]
pub enum Message {
    PreviousRequested,
    NextRequested,
    CloseRequested,
    OpenAnnotationRequested,
    UpscaleRequested(crate::generated::UpscaleKernel),
    DetailSourceSelected(bool),
    FitRequested,
    Overlay(super::overlay::Message),
}

#[derive(Debug, Clone)]
pub(super) enum Outcome {
    PreviousRequested,
    NextRequested,
    CloseRequested,
    OpenAnnotationRequested,
    UpscaleRequested(crate::generated::UpscaleKernel),
    DetailUpdated(crate::generated::ExploreDetailUpdate),
    OverlayUpdated(crate::generated::ExploreOverlay),
}

pub(super) fn update(
    state: &mut super::state::State,
    model: &crate::view_model::ExploreModel,
    message: Message,
) -> Result<Option<Outcome>, String> {
    Ok(Some(match message {
        Message::PreviousRequested => Outcome::PreviousRequested,
        Message::NextRequested => Outcome::NextRequested,
        Message::CloseRequested => Outcome::CloseRequested,
        Message::OpenAnnotationRequested => Outcome::OpenAnnotationRequested,
        Message::UpscaleRequested(kernel) => Outcome::UpscaleRequested(kernel),
        Message::DetailSourceSelected(showoriginaldimensions) => {
            state.choose_detail_original(showoriginaldimensions);
            Outcome::DetailUpdated(crate::generated::ExploreDetailUpdate {
                showoriginaldimensions,
            })
        }
        Message::FitRequested => {
            state.fit_revision = state.fit_revision.wrapping_add(1);
            return Ok(None);
        }
        Message::Overlay(message) => Outcome::OverlayUpdated(super::overlay::update(
            state,
            model.snapshot.as_ref(),
            message,
        )?),
    }))
}

pub(super) fn view<'a>(
    state: &'a super::state::State,
    model: &'a ApplicationModel,
    settings: &'a crate::view::settings::SettingsModel,
    surface: Surface,
    content: crate::presentation_surface::DetailContent,
    input: crate::workspace_input::Binding,
) -> Element<'a, Message> {
    let available = !settings.has_local_edits() && model.explore_mutation_available();
    let original = state.detail_original(&content);
    let selected = content
        .viewer_identity()
        .expect("validated detail viewer")
        .1;
    let overlay = state
        .presented_filter(model.explore.snapshot.as_ref())
        .map_or_else(|| content.overlay().clone(), |request| request.overlay);
    let image = crate::view::image_viewer::image(
        content.configure_surface(surface, original, state.fit_revision),
        crate::presentation_surface::labels::Source::Detail(content.clone(), overlay.showlabels),
        Some(input.for_source(content.frame().source.kind, 0, None)),
        crate::workspace_fps::enabled(settings),
        super::DETAIL_WORKSPACE_ID,
    );
    let active = model.displayed_upscale_kernel();
    let pending = model
        .requested_upscale
        .as_ref()
        .filter(|request| active != Some(request.kernel))
        .map(|request| request.kernel)
        .or_else(|| {
            model
                .upscale_snapshot
                .as_ref()?
                .pending
                .as_ref()
                .map(|request| request.kernel)
        });
    let source = row![
        container(button("Fit").on_press(Message::FitRequested)).id(super::DETAIL_FIT_ID),
        container(
            checkbox(original)
                .label("Original content")
                .on_toggle_maybe(available.then_some(Message::DetailSourceSelected))
        )
        .id(super::DETAIL_ORIGINAL_ID),
        space::horizontal(),
        super::overlay::view(&overlay, available, true).map(Message::Overlay),
    ]
    .spacing(7)
    .align_y(Center);
    crate::view::image_viewer::panel(
        selected, image, source.into(),
        crate::view::image_viewer::PanelIds {
            image: super::DETAIL_WORKSPACE_ID,
            previous: super::DETAIL_PREVIOUS_ID,
            next: super::DETAIL_NEXT_ID,
            close: super::DETAIL_CLOSE_ID,
            annotate: super::DETAIL_ANNOTATE_ID,
            upscale: [super::DETAIL_UPSCALE_BASIC_ID, super::DETAIL_UPSCALE_FAST_ID, super::DETAIL_UPSCALE_NEURAL_ID],
        },
        available.then_some(Message::PreviousRequested),
        available.then_some(Message::NextRequested),
        available.then_some(Message::CloseRequested),
        (model.annotation_import_available() && !settings.has_local_edits()).then_some(Message::OpenAnnotationRequested),
        active, pending, model.upscale_start_available(), Message::UpscaleRequested,
    )
}

#[cfg(test)]
use crate::view::image_viewer::upscale_label;

#[cfg(test)]
fn upscale_id(kernel: crate::generated::UpscaleKernel) -> &'static str {
    match kernel {
        crate::generated::UpscaleKernel::Default => super::DETAIL_UPSCALE_BASIC_ID,
        crate::generated::UpscaleKernel::ShiftLut => super::DETAIL_UPSCALE_FAST_ID,
        crate::generated::UpscaleKernel::RealPlksr => super::DETAIL_UPSCALE_NEURAL_ID,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detail_source_and_upscale_actions_remain_distinct() {
        let mut state = super::super::state::State::default();
        let model = crate::view_model::ExploreModel {
            snapshot: Some(crate::view_model::test_support::explore_snapshot()),
            ..Default::default()
        };
        assert!(matches!(
            update(&mut state, &model, Message::DetailSourceSelected(true)).unwrap(),
            Some(Outcome::DetailUpdated(
                crate::generated::ExploreDetailUpdate {
                    showoriginaldimensions: true
                }
            ))
        ));
        let detail = crate::generated::encode_explore_UpdateDetail(
            11,
            crate::generated::ExploreDetailUpdate {
                showoriginaldimensions: true,
            },
        );
        assert_eq!(
            detail.endpoint,
            crate::generated::ApplicationIntentEndpoint::ExploreUpdateDetail
        );
        assert_eq!(detail.record.fields.len(), 1);
        assert!(
            update(&mut state, &model, Message::FitRequested)
                .unwrap()
                .is_none()
        );
        assert_eq!(state.fit_revision, 1);
        for kernel in crate::generated::UPSCALE_KERNEL_VALUES.iter().copied() {
            assert!(matches!(
                update(&mut state, &model, Message::UpscaleRequested(kernel)).unwrap(),
                Some(Outcome::UpscaleRequested(actual)) if actual == kernel
            ));
            assert!(!upscale_label(kernel).is_empty());
            assert!(!upscale_id(kernel).is_empty());
        }
    }

    #[test]
    fn independent_overlay_edits_merge_before_native_admission() {
        let mut state = super::super::state::State::default();
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.ready = true;
        let model = crate::view_model::ExploreModel {
            snapshot: Some(snapshot),
            ..Default::default()
        };
        update(
            &mut state,
            &model,
            Message::Overlay(super::super::overlay::Message::BoxesToggled(false)),
        )
        .unwrap();
        update(
            &mut state,
            &model,
            Message::Overlay(super::super::overlay::Message::MasksToggled(false)),
        )
        .unwrap();
        let Some(Outcome::OverlayUpdated(overlay)) = update(
            &mut state,
            &model,
            Message::Overlay(super::super::overlay::Message::LabelsToggled(true)),
        )
        .unwrap() else {
            panic!("typed semantic outcome");
        };
        assert!(!overlay.showboxes && !overlay.showmasks && overlay.showlabels);
    }
}
