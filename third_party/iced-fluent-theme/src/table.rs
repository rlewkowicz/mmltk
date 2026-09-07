use crate::Theme;
use iced_widget::table::{Catalog, Style, StyleFn};


impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(default)
    }

    fn style(&self, class: &Self::Class<'_>) -> Style {
        class(self)
    }
}

pub fn default(theme: &Theme) -> Style {
    let tokens = theme.tokens();

    Style {
        separator_x: tokens.neutral_foreground1.into(),
        separator_y: tokens.subtle_background.into(),
    }
}
