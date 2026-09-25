use crate::Theme;
use iced_widget::text_editor::{Catalog, Status, Style, StyleFn};

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Theme>;
    fn default<'a>() -> Self::Class<'a> { Box::new(default) }
    fn style(&self, class: &Self::Class<'_>, status: Status) -> Style { class(self, status) }
}

pub fn default(theme: &Theme, _status: Status) -> Style {
    let tokens = theme.tokens();
    Style {
        background: tokens.neutral_background1.into(),
        border: iced_core::Border { color: tokens.neutral_stroke1, width: crate::stroke_width::THIN, radius: crate::border_radius::MEDIUM },
        placeholder: tokens.neutral_foreground4,
        value: tokens.neutral_foreground1,
        selection: tokens.brand_background2,
    }
}
