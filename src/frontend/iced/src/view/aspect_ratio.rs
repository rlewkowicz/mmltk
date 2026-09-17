use crate::fluent_theme::Element;
use iced::widget::{button, container, row, text};
use iced::{Center, Length};

pub const fn option_id(
    aspect: crate::generated::WorkspaceAspectRatio,
) -> &'static str {
    match aspect {
        crate::generated::WorkspaceAspectRatio::Widescreen => "workflow.aspect.widescreen",
        crate::generated::WorkspaceAspectRatio::Portrait => "workflow.aspect.portrait",
        crate::generated::WorkspaceAspectRatio::Standard => "workflow.aspect.standard",
        crate::generated::WorkspaceAspectRatio::Photo => "workflow.aspect.photo",
        crate::generated::WorkspaceAspectRatio::Square => "workflow.aspect.square",
        crate::generated::WorkspaceAspectRatio::SixteenTen => "workflow.aspect.sixteen_ten",
    }
}

pub fn label(aspect: crate::generated::WorkspaceAspectRatio) -> &'static str {
    match aspect {
        crate::generated::WorkspaceAspectRatio::Widescreen => "16:9",
        crate::generated::WorkspaceAspectRatio::Portrait => "9:16",
        crate::generated::WorkspaceAspectRatio::Standard => "4:3",
        crate::generated::WorkspaceAspectRatio::Photo => "3:2",
        crate::generated::WorkspaceAspectRatio::Square => "1:1",
        crate::generated::WorkspaceAspectRatio::SixteenTen => "16:10",
    }
}

pub fn height_factor(aspect: crate::generated::WorkspaceAspectRatio) -> f32 {
    match aspect {
        crate::generated::WorkspaceAspectRatio::Widescreen => 9.0 / 16.0,
        crate::generated::WorkspaceAspectRatio::Portrait => 16.0 / 9.0,
        crate::generated::WorkspaceAspectRatio::Standard => 3.0 / 4.0,
        crate::generated::WorkspaceAspectRatio::Photo => 2.0 / 3.0,
        crate::generated::WorkspaceAspectRatio::Square => 1.0,
        crate::generated::WorkspaceAspectRatio::SixteenTen => 10.0 / 16.0,
    }
}

pub fn extent_for_width(width: f32, aspect: crate::generated::WorkspaceAspectRatio) -> (f32, f32) {
    let width = width.max(1.0);
    (width, width * height_factor(aspect))
}

pub fn selector<'a, Message: Clone + 'a>(
    selected: crate::generated::WorkspaceAspectRatio,
    enabled: bool,
    message: fn(crate::generated::WorkspaceAspectRatio) -> Message,
) -> Element<'a, Message> {
    let options = crate::generated::WORKSPACE_ASPECT_RATIO_VALUES
        .iter()
        .copied()
        .fold(
            row![text("Aspect ratio").size(12)]
                .spacing(crate::view::workflow::FIELD_SPACING)
                .align_y(Center),
            |row, aspect| {
                row.push(
                    container(
                        button(text(label(aspect)))
                            .on_press_maybe(enabled.then_some(message(aspect)))
                            .style(if aspect == selected {
                                crate::fluent_theme::button_selected
                            } else {
                                crate::fluent_theme::button_secondary
                            }),
                    )
                    .id(option_id(aspect)),
                )
            },
        );
    container(options)
        .id("workflow.workspace.aspect")
        .padding([4, 8])
        .width(Length::Fill)
        .style(crate::fluent_theme::container_header)
        .into()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_generated_ratio_has_exact_width_geometry() {
        let expected = [
            (
                crate::generated::WorkspaceAspectRatio::Widescreen,
                9.0 / 16.0,
            ),
            (crate::generated::WorkspaceAspectRatio::Portrait, 16.0 / 9.0),
            (crate::generated::WorkspaceAspectRatio::Standard, 3.0 / 4.0),
            (crate::generated::WorkspaceAspectRatio::Photo, 2.0 / 3.0),
            (crate::generated::WorkspaceAspectRatio::Square, 1.0),
            (
                crate::generated::WorkspaceAspectRatio::SixteenTen,
                10.0 / 16.0,
            ),
        ];
        assert_eq!(
            crate::generated::WORKSPACE_ASPECT_RATIO_VALUES.len(),
            expected.len()
        );
        for (aspect, factor) in expected {
            assert_eq!(
                crate::generated::WORKSPACE_ASPECT_RATIO_VALUES
                    .iter()
                    .filter(|candidate| **candidate == aspect)
                    .count(),
                1
            );
            assert!((height_factor(aspect) - factor).abs() < f32::EPSILON);
            assert!(!label(aspect).is_empty());
            assert_eq!(extent_for_width(800.0, aspect), (800.0, 800.0 * factor));
        }
    }
}
