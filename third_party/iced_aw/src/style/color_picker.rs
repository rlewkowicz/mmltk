use super::{Status, StyleFn};
use iced_core::{Background, Color, Theme};

#[derive(Clone, Copy, Debug)]
pub struct Style {
        pub background: Background,

        pub border_radius: f32,

        pub border_width: f32,

        pub border_color: Color,

        pub bar_border_radius: f32,

        pub bar_border_width: f32,

        pub bar_border_color: Color,
}

pub trait Catalog {
        type Class<'a>;

        fn default<'a>() -> Self::Class<'a>;

        fn style(&self, class: &Self::Class<'_>, status: Status) -> Style;
}

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self, Style>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(primary)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> Style {
        class(self, status)
    }
}

#[must_use]
pub fn primary(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    let base = Style {
        background: palette.background.base.color.into(),
        border_radius: 15.0,
        border_width: 1.0,
        border_color: palette.background.base.text,
        bar_border_radius: 5.0,
        bar_border_width: 1.0,
        bar_border_color: palette.background.base.text,
    };

    match status {
        Status::Focused => Style {
            border_color: palette.background.strong.color,
            bar_border_color: palette.background.strong.color,
            ..base
        },
        _ => base,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iced_core::{Background, Theme};

    #[test]
    fn primary_theme_active() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert_eq!(style.border_radius, 15.0);
        assert_eq!(style.border_width, 1.0);
        assert_eq!(style.bar_border_radius, 5.0);
        assert_eq!(style.bar_border_width, 1.0);
    }

    #[test]
    fn primary_theme_focused() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Focused);

        assert!(matches!(style.background, Background::Color(_)));
        assert_eq!(style.border_radius, 15.0);
        assert_eq!(style.border_width, 1.0);
        assert_eq!(style.bar_border_radius, 5.0);
        assert_eq!(style.bar_border_width, 1.0);
    }

    #[test]
    fn primary_theme_hovered() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Hovered);

        assert!(matches!(style.background, Background::Color(_)));
        assert_eq!(style.border_radius, 15.0);
        assert_eq!(style.border_width, 1.0);
    }

    #[test]
    fn primary_theme_disabled() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Disabled);

        assert!(matches!(style.background, Background::Color(_)));
        assert_eq!(style.border_radius, 15.0);
        assert_eq!(style.border_width, 1.0);
    }

    #[test]
    fn focused_changes_border_colors() {
        let theme = Theme::TokyoNight;
        let base_style = primary(&theme, Status::Active);
        let focused_style = primary(&theme, Status::Focused);

        assert_ne!(base_style.border_color, focused_style.border_color);
        assert_ne!(base_style.bar_border_color, focused_style.bar_border_color);

        assert_eq!(base_style.background, focused_style.background);
        assert_eq!(base_style.border_radius, focused_style.border_radius);
        assert_eq!(base_style.border_width, focused_style.border_width);
        assert_eq!(
            base_style.bar_border_radius,
            focused_style.bar_border_radius
        );
        assert_eq!(base_style.bar_border_width, focused_style.bar_border_width);
    }

    #[test]
    fn catalog_default_class() {
        let _class = <Theme as Catalog>::default();
    }

    #[test]
    fn catalog_style() {
        let theme = Theme::TokyoNight;
        let class = <Theme as Catalog>::default();
        let style = theme.style(&class, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert_eq!(style.border_radius, 15.0);
        assert_eq!(style.border_width, 1.0);
        assert_eq!(style.bar_border_radius, 5.0);
        assert_eq!(style.bar_border_width, 1.0);
    }

    #[test]
    fn catalog_style_focused() {
        let theme = Theme::TokyoNight;
        let class = <Theme as Catalog>::default();
        let style = theme.style(&class, Status::Focused);

        assert!(matches!(style.background, Background::Color(_)));
        assert_eq!(style.border_radius, 15.0);
    }

    #[test]
    fn style_fields_are_valid() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Active);

        assert!(style.border_radius > 0.0);
        assert!(style.border_width > 0.0);
        assert!(style.bar_border_radius > 0.0);
        assert!(style.bar_border_width > 0.0);
    }
}
