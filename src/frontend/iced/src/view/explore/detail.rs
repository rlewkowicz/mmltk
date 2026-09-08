use crate::fluent_theme::Element;
use crate::presentation_surface::Surface;
use crate::view_model::ApplicationModel;
use iced::widget::{button, checkbox, column, container, opaque, row, space, text};
use iced::{Center, Fill, Length, Padding};

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
            state.desired_original = Some(showoriginaldimensions);
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
    snapshot: &'a crate::generated::ExploreSnapshot,
    surface: Option<Surface>,
) -> Element<'a, Message> {
    let available = !settings.has_local_edits() && model.explore_mutation_available();
    let shown = model.viewed_explore_frame();
    let original = state
        .desired_original
        .unwrap_or(snapshot.detail.showoriginaldimensions);
    let surface = surface.map(|mut surface| {
        if !shown.as_ref().is_some_and(|shown| {
            surface.frame.is_some_and(|frame| {
                model
                    .presentation
                    .as_ref()
                    .is_some_and(|presentation| frame.matches_completed(presentation))
                    && frame.matches_content(shown)
            })
        }) {
            surface.frame = None;
        }
        surface.viewer_identity = snapshot
            .selectedimage
            .map(|image| (snapshot.dataset.identity, image));
        surface.fit_revision = state.fit_revision;
        surface.crop = shown.as_ref().filter(|_| original).map(|frame| {
            [
                frame.content.x,
                frame.content.y,
                frame.content.width,
                frame.content.height,
            ]
        });
        surface
    });
    let image: Element<'a, Message> = surface.map_or_else(
        || {
            container(text("Preparing selected GPU detail"))
                .center(Fill)
                .width(Fill)
                .height(Fill)
                .into()
        },
        |surface| {
            crate::presentation_surface::labels::view(
                crate::presentation_surface::Program {
                    surface,
                    publish: None,
                    placement: crate::presentation_surface::Placement::Contain,
                    control_id: super::DETAIL_WORKSPACE_ID,
                },
                crate::presentation_surface::retained_detail()
                    .filter(|(retained, _)| retained.viewer_identity == surface.viewer_identity)
                    .map_or(
                        crate::presentation_surface::labels::Source::Hidden,
                        |(_, content)| crate::presentation_surface::labels::Source::Detail(content),
                    ),
            )
        },
    );
    let active = model.displayed_upscale_kernel();
    let pending = model
        .explore
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
    let upscale = crate::generated::UPSCALE_KERNEL_VALUES
        .iter()
        .copied()
        .fold(row![].spacing(5), |row, kernel| {
            row.push(
                container(
                    button(text(if pending == Some(kernel) {
                        format!("{}…", upscale_label(kernel))
                    } else {
                        upscale_label(kernel).to_owned()
                    }))
                    .style(if active == Some(kernel) {
                        crate::fluent_theme::button_primary
                    } else {
                        crate::fluent_theme::button_secondary
                    })
                    .on_press_maybe(
                        model
                            .upscale_start_available()
                            .then_some(Message::UpscaleRequested(kernel)),
                    ),
                )
                .id(upscale_id(kernel)),
            )
        });
    let overlay = state
        .presented_filter(Some(snapshot))
        .map(|request| request.overlay)
        .unwrap_or_else(|| snapshot.overlay.clone());
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
    let panel = container(
        column![
            row![
                text(format!(
                    "Sample #{}",
                    snapshot.selectedimage.unwrap_or_default()
                ))
                .size(20),
                space::horizontal(),
                container(
                    button("Previous")
                        .on_press_maybe(available.then_some(Message::PreviousRequested))
                )
                .id(super::DETAIL_PREVIOUS_ID),
                container(
                    button("Next").on_press_maybe(available.then_some(Message::NextRequested))
                )
                .id(super::DETAIL_NEXT_ID),
                container(
                    button("×")
                        .on_press_maybe(available.then_some(Message::CloseRequested))
                        .padding([2, 9])
                )
                .id(super::DETAIL_CLOSE_ID),
            ]
            .spacing(7)
            .align_y(Center),
            source,
            container(image)
                .id(super::DETAIL_WORKSPACE_ID)
                .width(Fill)
                .height(Fill)
                .style(crate::fluent_theme::container_workspace),
            row![
                text("Wheel to zoom · right-drag to pan")
                    .size(12)
                    .style(crate::fluent_theme::text_secondary),
                space::horizontal(),
                text("Upscale"),
                upscale,
            ]
            .spacing(7)
            .align_y(Center),
            container(
                button("Open in Annotation")
                    .on_press_maybe(
                        (model.annotation_open_available() && !settings.has_local_edits())
                            .then_some(Message::OpenAnnotationRequested),
                    )
                    .style(crate::fluent_theme::button_primary)
            )
            .id(super::DETAIL_ANNOTATE_ID),
        ]
        .spacing(10),
    )
    .padding(Padding::from([14, 14]))
    .width(Length::FillPortion(4))
    .height(Length::FillPortion(4))
    .style(crate::fluent_theme::container_modal);

    opaque(
        container(panel)
            .padding(32)
            .center(Fill)
            .width(Fill)
            .height(Fill)
            .style(|_theme| iced::widget::container::Style {
                background: Some(iced::Color::BLACK.into()),
                ..Default::default()
            }),
    )
    .into()
}

fn upscale_label(kernel: crate::generated::UpscaleKernel) -> &'static str {
    match kernel {
        crate::generated::UpscaleKernel::Default => "Basic",
        crate::generated::UpscaleKernel::ShiftLut => "Fast",
        crate::generated::UpscaleKernel::RealPlksr => "Neural",
    }
}

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
