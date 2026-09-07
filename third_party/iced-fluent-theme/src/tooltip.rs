use crate::{Theme, border_radius, stroke_width};
use iced_core::Border;
use iced_widget::container::Style;



pub fn default(theme: &Theme) -> Style {
    let tokens = theme.tokens();

    Style {
        text_color: Some(tokens.neutral_foreground1),
        background: Some(tokens.neutral_background1.into()),
        border: Border {
            color: tokens.transparent_stroke,
            width: stroke_width::THIN,
            radius: border_radius::MEDIUM,
        },
        shadow: tokens.shadow8,
        ..Style::default()
    }
}

pub fn inverted(theme: &Theme) -> Style {
    let tokens = theme.tokens();

    Style {
        text_color: Some(tokens.neutral_foreground_static_inverted),
        background: Some(tokens.neutral_background_static.into()),
        border: Border {
            color: tokens.transparent_stroke,
            width: stroke_width::THIN,
            radius: border_radius::MEDIUM,
        },
        shadow: tokens.shadow8,
        ..Style::default()
    }
}
