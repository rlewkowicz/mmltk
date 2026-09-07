pub mod item;
use crate::Theme;
use iced_core::{Border, Shadow};
use iced_widget_kit::selector_bar::{Catalog, Style, StyleFn};


impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(transparent)
    }

    fn style(&self, class: &Self::Class<'_>) -> Style {
        class(self)
    }
}

pub fn transparent(_theme: &Theme) -> Style {
    Style {
        background: None,
        border: Border::default(),
        shadow: Shadow::default(),
        snap: true,
    }
}
