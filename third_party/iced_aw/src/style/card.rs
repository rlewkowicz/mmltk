use super::{Status, StyleFn, colors};
use iced_core::{Background, Color, Theme};

#[derive(Clone, Copy, Debug)]
pub struct Style {
        pub background: Background,

        pub border_radius: f32,

        pub border_width: f32,

        pub border_color: Color,

        pub head_background: Background,

        pub head_text_color: Color,

        pub body_background: Background,

        pub body_text_color: Color,

        pub foot_background: Background,

        pub foot_text_color: Color,

        pub close_color: Color,
}

pub trait Catalog {
        type Class<'a>;

        fn default<'a>() -> Self::Class<'a>;

        fn style(&self, class: &Self::Class<'_>, status: Status) -> Style;
}

impl Default for Style {
    fn default() -> Self {
        Self {
            background: Color::WHITE.into(),
            border_radius: 10.0,
            border_width: 1.0,
            border_color: [0.87, 0.87, 0.87].into(),
            head_background: Background::Color([0.87, 0.87, 0.87].into()),
            head_text_color: Color::BLACK,
            body_background: Color::TRANSPARENT.into(),
            body_text_color: Color::BLACK,
            foot_background: Color::TRANSPARENT.into(),
            foot_text_color: Color::BLACK,
            close_color: Color::BLACK,
        }
    }
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
pub fn primary(theme: &Theme, _status: Status) -> Style {
    backing_with_text(theme, colors::PRIMARY, colors::WHITE)
}

#[must_use]
pub fn secondary(theme: &Theme, _status: Status) -> Style {
    backing_with_text(theme, colors::SECONDARY, colors::WHITE)
}

#[must_use]
pub fn success(theme: &Theme, _status: Status) -> Style {
    backing_with_text(theme, colors::SUCCESS, colors::WHITE)
}

#[must_use]
pub fn danger(theme: &Theme, _status: Status) -> Style {
    backing_with_text(theme, colors::DANGER, colors::WHITE)
}

#[must_use]
pub fn warning(theme: &Theme, _status: Status) -> Style {
    backing_only(theme, colors::WARNING)
}

#[must_use]
pub fn info(theme: &Theme, _status: Status) -> Style {
    backing_only(theme, colors::INFO)
}

#[must_use]
pub fn light(theme: &Theme, _status: Status) -> Style {
    backing_only(theme, colors::LIGHT)
}

#[must_use]
pub fn dark(theme: &Theme, _status: Status) -> Style {
    backing_with_text(theme, colors::DARK, colors::WHITE)
}

#[must_use]
pub fn white(theme: &Theme, _status: Status) -> Style {
    backing_only(theme, colors::WHITE)
}

fn backing_with_text(theme: &Theme, color: Color, text_color: Color) -> Style {
    let palette = theme.palette();

    Style {
        border_color: color,
        head_background: color.into(),
        head_text_color: text_color,
        close_color: text_color,
        background: palette.background.base.color.into(),
        body_text_color: palette.background.base.text,
        foot_text_color: palette.background.base.text,
        ..Style::default()
    }
}

fn backing_only(theme: &Theme, color: Color) -> Style {
    let palette = theme.palette();

    Style {
        border_color: color,
        head_background: color.into(),
        background: palette.background.base.color.into(),
        body_text_color: palette.background.base.text,
        foot_text_color: palette.background.base.text,
        ..Style::default()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iced_core::{Background, Color, Theme};

    #[test]
    fn style_default() {
        let style = Style::default();
        assert_eq!(style.background, Background::Color(Color::WHITE));
        assert_eq!(style.border_radius, 10.0);
        assert_eq!(style.border_width, 1.0);
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.head_text_color, Color::BLACK);
        assert_eq!(style.body_background, Background::Color(Color::TRANSPARENT));
        assert_eq!(style.body_text_color, Color::BLACK);
        assert_eq!(style.foot_background, Background::Color(Color::TRANSPARENT));
        assert_eq!(style.foot_text_color, Color::BLACK);
        assert_eq!(style.close_color, Color::BLACK);
    }

    #[test]
    fn backing_with_text_creates_style() {
        let theme = Theme::TokyoNight;
        let color = Color::from_rgb(1.0, 0.0, 0.0);
        let text = Color::WHITE;
        let style = backing_with_text(&theme, color, text);

        assert_eq!(style.border_color, color);
        assert_eq!(style.head_background, Background::Color(color));
        assert_eq!(style.head_text_color, text);
        assert_eq!(style.close_color, text);
        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.body_background, Background::Color(_)));
        assert!(matches!(style.foot_background, Background::Color(_)));
    }

    #[test]
    fn backing_only_creates_style() {
        let theme = Theme::TokyoNight;
        let color = Color::from_rgb(0.0, 1.0, 0.0);
        let style = backing_only(&theme, color);

        assert_eq!(style.border_color, color);
        assert_eq!(style.head_background, Background::Color(color));
        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.body_background, Background::Color(_)));
        assert!(matches!(style.foot_background, Background::Color(_)));
    }

    #[test]
    fn primary_theme() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.head_text_color, colors::WHITE);
        assert_eq!(style.close_color, colors::WHITE);
    }

    #[test]
    fn secondary_theme() {
        let theme = Theme::TokyoNight;
        let style = secondary(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.head_text_color, colors::WHITE);
        assert_eq!(style.close_color, colors::WHITE);
    }

    #[test]
    fn success_theme() {
        let theme = Theme::TokyoNight;
        let style = success(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.head_text_color, colors::WHITE);
        assert_eq!(style.close_color, colors::WHITE);
    }

    #[test]
    fn danger_theme() {
        let theme = Theme::TokyoNight;
        let style = danger(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.head_text_color, colors::WHITE);
        assert_eq!(style.close_color, colors::WHITE);
    }

    #[test]
    fn warning_theme() {
        let theme = Theme::TokyoNight;
        let style = warning(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.border_color, colors::WARNING);
    }

    #[test]
    fn info_theme() {
        let theme = Theme::TokyoNight;
        let style = info(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.border_color, colors::INFO);
    }

    #[test]
    fn light_theme() {
        let theme = Theme::TokyoNight;
        let style = light(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.border_color, colors::LIGHT);
    }

    #[test]
    fn dark_theme() {
        let theme = Theme::TokyoNight;
        let style = dark(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.head_text_color, colors::WHITE);
        assert_eq!(style.close_color, colors::WHITE);
        assert_eq!(style.border_color, colors::DARK);
    }

    #[test]
    fn white_theme() {
        let theme = Theme::TokyoNight;
        let style = white(&theme, Status::Active);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
        assert_eq!(style.border_color, colors::WHITE);
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
        assert!(matches!(style.head_background, Background::Color(_)));
    }

    #[test]
    fn primary_theme_with_hovered_status() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Hovered);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
    }

    #[test]
    fn primary_theme_with_disabled_status() {
        let theme = Theme::TokyoNight;
        let style = primary(&theme, Status::Disabled);

        assert!(matches!(style.background, Background::Color(_)));
        assert!(matches!(style.head_background, Background::Color(_)));
    }
}
