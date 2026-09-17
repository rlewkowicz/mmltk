use crate::fluent_theme::Element;
use crate::presentation_surface::{Surface, SurfaceGesture};
use iced::widget::{container, text};
use iced::{Center, Fill, Length};

pub const STABLE_ID: &str = "workflow.visual.workspace";

#[cfg(test)]
pub fn aspect_id(aspect: crate::generated::WorkspaceAspectRatio) -> &'static str {
    crate::view::aspect_ratio::option_id(aspect)
}

#[derive(Debug, Clone)]
pub enum Message {
    Gesture(SurfaceGesture),
    AspectSelected(crate::generated::WorkspaceAspectRatio),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    Gesture(SurfaceGesture),
    AspectSelected(crate::generated::WorkspaceAspectRatio),
}

pub fn update(message: Message) -> Outcome {
    match message {
        Message::Gesture(gesture) => Outcome::Gesture(gesture),
        Message::AspectSelected(aspect) => Outcome::AspectSelected(aspect),
    }
}

pub fn edit_aspect(
    settings: &mut crate::view::settings::SettingsModel,
    aspect: crate::generated::WorkspaceAspectRatio,
) -> Result<crate::view::settings::EditSchedule, String> {
    settings.edit(crate::view::settings::EditCadence::Debounced, |draft| {
        crate::generated::edit_uiworkspaceaspectratio(draft, aspect)
    })
}

pub fn surface_extent(
    center_width: f32,
    aspect: crate::generated::WorkspaceAspectRatio,
) -> (f32, f32) {
    let width = (center_width - crate::view::workflow::CARD_PADDING * 2.0).max(1.0);
    crate::view::aspect_ratio::extent_for_width(width, aspect)
}

pub fn view(
    surface: Option<Surface>,
    labels: crate::presentation_surface::labels::Source,
    selected: crate::generated::WorkspaceAspectRatio,
    settings_edit_available: bool,
    center_width: f32,
    input: crate::workspace_input::Binding,
    show_fps: bool,
) -> Element<'static, Message> {
    let (surface_width, surface_height) = surface_extent(center_width, selected);
    let content: Element<'static, Message> = crate::presentation_surface::labels::view(
        crate::presentation_surface::Program {
            show_fps,
            input: Some(input),
            local: None,
            surface: surface.unwrap_or_else(Surface::empty),
            publish: Some(Message::Gesture),
            placement: crate::presentation_surface::Placement::Contain,
            control_id: STABLE_ID,
        },
        labels,
    );
    let content = if surface.is_none() {
        iced::widget::stack![
            content,
            container(text("Waiting for a completed native frame"))
                .center(Fill)
                .width(Fill)
                .height(Fill)
        ]
        .into()
    } else {
        content
    };
    iced::widget::column![
        container(crate::view::aspect_ratio::selector(
            selected,
            settings_edit_available,
            Message::AspectSelected,
        ))
        .width(Length::Fixed(surface_width)),
        container(content)
            .id(STABLE_ID)
            .width(Length::Fixed(surface_width))
            .height(Length::Fixed(surface_height))
            .style(crate::fluent_theme::container_workspace),
    ]
    .spacing(crate::view::workflow::SECTION_SPACING)
    .width(Fill)
    .align_x(Center)
    .into()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_generated_aspects_project_exact_surface_geometry() {
        let generated = crate::generated::WORKSPACE_ASPECT_RATIO_VALUES;
        let expected = [
            crate::generated::WorkspaceAspectRatio::Widescreen,
            crate::generated::WorkspaceAspectRatio::Portrait,
            crate::generated::WorkspaceAspectRatio::Standard,
            crate::generated::WorkspaceAspectRatio::Photo,
            crate::generated::WorkspaceAspectRatio::Square,
            crate::generated::WorkspaceAspectRatio::SixteenTen,
        ];
        assert_eq!(generated.len(), expected.len());
        for expected in expected {
            assert_eq!(
                generated
                    .iter()
                    .filter(|aspect| **aspect == expected)
                    .count(),
                1
            );
        }
        for page_width in [1200.0, 1500.0] {
            let center_width = crate::view::workflow::Composition::new(
                crate::generated::FeatureId::Train,
                page_width,
            )
            .center_width();
            for aspect in generated.iter().copied() {
                let (width, height) = surface_extent(center_width, aspect);
                assert!((width - (page_width * 0.62 - 20.0)).abs() < f32::EPSILON);
                assert!(
                    (height - width * crate::view::aspect_ratio::height_factor(aspect)).abs()
                        < f32::EPSILON
                );
                assert!(!crate::view::aspect_ratio::label(aspect).is_empty());
                assert!(!aspect_id(aspect).is_empty());
            }
        }
    }
}
