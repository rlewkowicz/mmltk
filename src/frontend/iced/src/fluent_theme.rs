use iced::{Border, Color, Shadow, Vector};

pub use iced_fluent_theme::Theme;

pub type Element<'a, Message> = iced::Element<'a, Message, Theme>;

pub fn app_theme(dark_mode: bool) -> Theme {
    if dark_mode {
        Theme::dark(None)
    } else {
        Theme::light(None)
    }
}

pub fn button_primary(
    theme: &Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let mut style = iced_fluent_theme::button::rounded::primary(theme, status);
    if status != iced::widget::button::Status::Disabled {
        style.border.color = Color::WHITE;
        style.border.width = 1.0;
        style.shadow = Shadow {
            color: Color::from_rgba8(0xd3, 0xd3, 0xd3, 0.72),
            offset: Vector::new(0.0, 1.0),
            blur_radius: 2.0,
        };
    }
    style
}

pub fn button_workflow_primary(
    _theme: &Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let green = Color::from_rgb8(0x00, 0xc8, 0x53);
    let background = match status {
        iced::widget::button::Status::Hovered => green.mix(Color::WHITE, 0.14),
        iced::widget::button::Status::Pressed => green.mix(Color::BLACK, 0.16),
        iced::widget::button::Status::Active | iced::widget::button::Status::Disabled => green,
    };
    iced::widget::button::Style {
        background: Some(background.into()),
        text_color: Color::WHITE,
        border: Border {
            color: Color::WHITE,
            width: 3.0,
            radius: 10.0.into(),
        },
        ..Default::default()
    }
}

pub fn button_workflow_stop(
    theme: &Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let mut style = button_workflow_primary(theme, status);
    style.background = button_danger(
        theme,
        if status == iced::widget::button::Status::Disabled {
            iced::widget::button::Status::Active
        } else {
            status
        },
    )
    .background;
    style
}

pub fn button_secondary(
    theme: &Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    iced_fluent_theme::button::rounded::secondary(theme, status)
}

pub fn button_selected(
    theme: &Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let tokens = theme.tokens();
    let background = match status {
        iced::widget::button::Status::Active => tokens.neutral_background1_selected,
        iced::widget::button::Status::Hovered => tokens.neutral_background1_hover,
        iced::widget::button::Status::Pressed => tokens.neutral_background1_pressed,
        iced::widget::button::Status::Disabled => tokens.neutral_background_disabled,
    };
    iced::widget::button::Style {
        background: Some(background.into()),
        text_color: if status == iced::widget::button::Status::Disabled {
            tokens.neutral_foreground_disabled
        } else if theme.is_dark() {
            tokens.brand_foreground1.mix(Color::WHITE, 0.30)
        } else {
            tokens.brand_foreground1
        },
        border: Border {
            color: tokens.neutral_stroke_accessible_selected,
            width: 1.0,
            radius: iced_fluent_theme::border_radius::MEDIUM,
        },
        snap: true,
        ..Default::default()
    }
}

pub fn button_danger(
    theme: &Theme,
    status: iced::widget::button::Status,
) -> iced::widget::button::Style {
    let tokens = theme.tokens();
    let background = match status {
        iced::widget::button::Status::Active => tokens.status_danger_background3,
        iced::widget::button::Status::Hovered => {
            tokens.status_danger_background3.mix(Color::WHITE, 0.12)
        }
        iced::widget::button::Status::Pressed => {
            tokens.status_danger_background3.mix(Color::BLACK, 0.14)
        }
        iced::widget::button::Status::Disabled => tokens.neutral_background_disabled,
    };
    iced::widget::button::Style {
        background: Some(background.into()),
        text_color: Color::WHITE,
        border: Border {
            color: background,
            width: 1.0,
            radius: iced_fluent_theme::border_radius::MEDIUM,
        },
        snap: true,
        ..Default::default()
    }
}

pub fn container_shell(theme: &Theme) -> iced::widget::container::Style {
    fill(
        theme.tokens().neutral_background2,
        theme.tokens().neutral_foreground1,
    )
}

pub fn container_card(theme: &Theme) -> iced::widget::container::Style {
    let mut style = container_bordered_box(theme);
    style.shadow = Shadow {
        color: Color::BLACK.scale_alpha(if theme.is_dark() { 0.24 } else { 0.10 }),
        offset: Vector::new(0.0, 1.0),
        blur_radius: 3.0,
    };
    style
}

pub fn container_bordered_box(theme: &Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        text_color: Some(theme.tokens().neutral_foreground1),
        background: Some(theme.tokens().neutral_background1.into()),
        border: Border {
            color: theme.tokens().neutral_stroke1,
            width: 1.0,
            radius: iced_fluent_theme::border_radius::MEDIUM,
        },
        ..Default::default()
    }
}

pub fn container_sidebar(theme: &Theme) -> iced::widget::container::Style {
    fill(
        theme.tokens().neutral_background2,
        theme.tokens().neutral_foreground1,
    )
}

pub fn container_header_shadow(theme: &Theme) -> iced::widget::container::Style {
    let mut style = container_header(theme);
    style.shadow = Shadow {
        color: Color::BLACK.scale_alpha(0.16),
        offset: Vector::new(0.0, 2.0),
        blur_radius: 2.0,
    };
    style
}

pub fn container_primary_frame(_theme: &Theme) -> iced::widget::container::Style {
    iced::widget::container::Style {
        border: Border {
            color: Color::from_rgba8(0xd3, 0xd3, 0xd3, 0.5),
            width: 1.0,
            radius: 11.0.into(),
        },
        shadow: Shadow {
            color: Color::BLACK.scale_alpha(0.18),
            offset: Vector::new(0.0, 2.0),
            blur_radius: 2.0,
        },
        ..Default::default()
    }
}

pub fn scrollable_default(
    theme: &Theme,
    status: iced::widget::scrollable::Status,
) -> iced::widget::scrollable::Style {
    iced_fluent_theme::scrollable::default(theme, status)
}

pub fn container_header(theme: &Theme) -> iced::widget::container::Style {
    fill(
        theme.tokens().neutral_background3,
        theme.tokens().neutral_foreground1,
    )
}

pub fn container_modal(theme: &Theme) -> iced::widget::container::Style {
    let mut style = container_card(theme);
    style.shadow = Shadow {
        color: Color::from_rgba(0.0, 0.0, 0.0, 0.30),
        offset: Vector::new(0.0, 8.0),
        blur_radius: 24.0,
    };
    style
}

pub fn container_workspace(_theme: &Theme) -> iced::widget::container::Style {
    fill(Color::from_rgb8(0x11, 0x11, 0x11), Color::WHITE)
}

pub fn container_error(theme: &Theme) -> iced::widget::container::Style {
    fill(theme.tokens().status_danger_background3, Color::WHITE)
}

pub fn text_secondary(theme: &Theme) -> iced::widget::text::Style {
    iced::widget::text::Style {
        color: Some(theme.tokens().neutral_foreground2),
    }
}

pub fn text_success(theme: &Theme) -> iced::widget::text::Style {
    iced::widget::text::Style {
        color: Some(theme.tokens().status_success_foreground1),
    }
}

pub fn text_warning(theme: &Theme) -> iced::widget::text::Style {
    iced::widget::text::Style {
        color: Some(theme.tokens().status_warning_foreground1),
    }
}

pub fn checkbox_benchmark(
    theme: &Theme,
    status: iced::widget::checkbox::Status,
) -> iced::widget::checkbox::Style {
    const PURPLE: Color = Color::from_rgb8(0x8a, 0x2b, 0xe2);
    let (is_checked, disabled, hovered) = match status {
        iced::widget::checkbox::Status::Active { is_checked } => (is_checked, false, false),
        iced::widget::checkbox::Status::Hovered { is_checked } => (is_checked, false, true),
        iced::widget::checkbox::Status::Disabled { is_checked } => (is_checked, true, false),
    };
    let mut style = iced_fluent_theme::checkbox::default(theme, status);
    let purple = if disabled {
        PURPLE.mix(theme.tokens().neutral_foreground_disabled, 0.55)
    } else if hovered {
        PURPLE.mix(Color::WHITE, if theme.is_dark() { 0.20 } else { 0.08 })
    } else {
        PURPLE
    };
    style.text_color = Some(purple);
    style.border.color = purple;
    if is_checked {
        style.background = purple.into();
        style.icon_color = Color::WHITE;
    }
    style
}

pub(crate) struct Conformance {
    pub dark: bool,
    pub navigation_border: f32,
    pub card_border: f32,
    pub primary_border: f32,
    pub primary_color: Color,
    pub benchmark_color: Color,
    pub resolved: bool,
}

pub(crate) fn conformance(theme: &Theme) -> Conformance {
    let navigation = button_selected(theme, iced::widget::button::Status::Active);
    let card = container_card(theme);
    let primary = button_primary(theme, iced::widget::button::Status::Active);
    let benchmark = checkbox_benchmark(
        theme,
        iced::widget::checkbox::Status::Active { is_checked: true },
    );
    let primary_color = match &primary.background {
        Some(iced::Background::Color(color)) => *color,
        _ => Color::TRANSPARENT,
    };
    let benchmark_color = benchmark.text_color.unwrap_or(Color::TRANSPARENT);
    Conformance {
        dark: theme.is_dark(),
        navigation_border: navigation.border.width,
        card_border: card.border.width,
        primary_border: primary.border.width,
        primary_color,
        benchmark_color,
        resolved: navigation.background.is_some()
            && card.background.is_some()
            && primary.background.is_some()
            && card.text_color.is_some(),
    }
}

fn fill(background: Color, foreground: Color) -> iced::widget::container::Style {
    iced::widget::container::Style {
        text_color: Some(foreground),
        background: Some(background.into()),
        ..Default::default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn workflow_stop_preserves_geometry_and_danger_interaction_in_both_themes() {
        use iced::widget::button::Status;
        for dark in [false, true] {
            let theme = app_theme(dark);
            for status in [
                Status::Active,
                Status::Hovered,
                Status::Pressed,
                Status::Disabled,
            ] {
                let idle = button_workflow_primary(&theme, status);
                let stop = button_workflow_stop(&theme, status);
                assert_eq!(stop.border, idle.border);
                assert_eq!(stop.shadow, idle.shadow);
                assert_eq!(stop.text_color, Color::WHITE);
                assert_eq!(
                    stop.background,
                    button_danger(
                        &theme,
                        if status == Status::Disabled {
                            Status::Active
                        } else {
                            status
                        }
                    )
                    .background
                );
            }
            let frame = container_primary_frame(&theme);
            assert_eq!(frame.border.width, 1.0);
            assert_eq!(frame.border.radius.top_left, 11.0);
        }
    }

    #[test]
    fn fluent_light_and_dark_themes_preserve_the_requested_mode() {
        assert!(!app_theme(false).is_dark());
        assert!(app_theme(true).is_dark());
    }

    #[test]
    fn light_and_dark_styles_match_the_exact_fluent_product_treatment() {
        for dark in [false, true] {
            let theme = app_theme(dark);
            let facts = conformance(&theme);
            assert_eq!(facts.dark, dark);
            assert_eq!(facts.navigation_border, 1.0);
            assert_eq!(facts.card_border, 1.0);
            assert_eq!(facts.primary_border, 1.0);
            assert!(facts.resolved);

            let selected = button_selected(&theme, iced::widget::button::Status::Active);
            assert_eq!(
                selected.background,
                Some(theme.tokens().neutral_background1_selected.into())
            );
            assert_eq!(
                selected.text_color,
                if dark {
                    theme.tokens().brand_foreground1.mix(Color::WHITE, 0.30)
                } else {
                    theme.tokens().brand_foreground1
                }
            );
            assert_eq!(
                selected.border.color,
                theme.tokens().neutral_stroke_accessible_selected
            );
            let workflow_primary =
                button_workflow_primary(&theme, iced::widget::button::Status::Active);
            assert_eq!(workflow_primary.border.width, 3.0);
            assert_eq!(workflow_primary.border.radius.top_left, 10.0);
            assert_eq!(
                workflow_primary.background,
                Some(Color::from_rgb8(0x00, 0xc8, 0x53).into())
            );
            assert_eq!(container_primary_frame(&theme).shadow.blur_radius, 2.0);
            let benchmark = checkbox_benchmark(
                &theme,
                iced::widget::checkbox::Status::Active { is_checked: true },
            );
            assert_eq!(
                benchmark.text_color,
                Some(Color::from_rgb8(0x8a, 0x2b, 0xe2))
            );
            assert_eq!(benchmark.border.color, Color::from_rgb8(0x8a, 0x2b, 0xe2));
            assert_eq!(
                benchmark.background,
                Color::from_rgb8(0x8a, 0x2b, 0xe2).into()
            );
            assert_eq!(benchmark.icon_color, Color::WHITE);
            assert_eq!(
                text_success(&theme).color,
                Some(theme.tokens().status_success_foreground1)
            );
            assert_eq!(
                text_warning(&theme).color,
                Some(theme.tokens().status_warning_foreground1)
            );
            assert_eq!(
                selected.border.radius,
                iced_fluent_theme::border_radius::MEDIUM
            );
            let primary = button_primary(&theme, iced::widget::button::Status::Active);
            assert_eq!(primary.border.color, Color::WHITE);
            assert_eq!(primary.border.width, 1.0);
            assert_eq!(
                primary.shadow.color,
                Color::from_rgba8(0xd3, 0xd3, 0xd3, 0.72)
            );
            assert_eq!(primary.shadow.offset, Vector::new(0.0, 1.0));
            assert_eq!(primary.shadow.blur_radius, 2.0);
            let card = container_card(&theme);
            assert_eq!(
                card.background,
                Some(theme.tokens().neutral_background1.into())
            );
            assert_eq!(card.border.color, theme.tokens().neutral_stroke1);
            assert_eq!(card.border.radius, iced_fluent_theme::border_radius::MEDIUM);
            assert_eq!(card.shadow.offset, Vector::new(0.0, 1.0));
            assert_eq!(card.shadow.blur_radius, 3.0);
            assert_eq!(
                card.shadow.color,
                Color::BLACK.scale_alpha(if dark { 0.24 } else { 0.10 })
            );
            let header = container_header_shadow(&theme);
            assert_eq!(
                header.background,
                Some(theme.tokens().neutral_background3.into())
            );
            assert_eq!(header.shadow.offset, Vector::new(0.0, 2.0));
            assert_eq!(header.shadow.blur_radius, 2.0);
            assert_eq!(header.shadow.color, Color::BLACK.scale_alpha(0.16));
            let primary = button_workflow_primary(&theme, iced::widget::button::Status::Active);
            assert_eq!(
                primary.background,
                Some(Color::from_rgb8(0x00, 0xc8, 0x53).into())
            );
            assert_eq!(primary.text_color, Color::WHITE);
            assert_eq!(primary.border.color, Color::WHITE);
            assert_eq!(primary.border.width, 3.0);
            assert_eq!(primary.border.radius, iced::border::Radius::new(10.0));
            let outer = container_primary_frame(&theme);
            assert_eq!(outer.border.color, Color::from_rgba8(0xd3, 0xd3, 0xd3, 0.5));
            assert_eq!(outer.border.width, 1.0);
            assert_eq!(outer.border.radius.top_left, 11.0);
            assert_eq!(outer.shadow.offset, Vector::new(0.0, 2.0));
            assert_eq!(outer.shadow.blur_radius, 2.0);
            assert_eq!(outer.shadow.color, Color::BLACK.scale_alpha(0.18));
        }
    }
}

// Neutral spacing shared by ordinary cards and modal surfaces.
pub const CARD_PADDING: f32 = 10.0;
pub const FIELD_SPACING: f32 = 4.0;
pub const MODAL_PADDING: f32 = 24.0;
