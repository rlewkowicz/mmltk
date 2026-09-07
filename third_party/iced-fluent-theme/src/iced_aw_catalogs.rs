use crate::{Theme, border_radius};
use iced_aw::style::{Status, card, color_picker, context_menu, number_input, selection_list};
use iced_core::{Background, Color, Shadow, Vector};

impl card::Catalog for Theme {
    type Class<'a> = Box<dyn Fn(&Self, Status) -> card::Style + 'a>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(card_style)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> card::Style {
        class(self, status)
    }
}

fn card_style(theme: &Theme, _status: Status) -> card::Style {
    let tokens = theme.tokens();
    card::Style {
        background: tokens.neutral_background1.into(),
        border_radius: border_radius::MEDIUM.top_left,
        border_width: 1.0,
        border_color: tokens.neutral_stroke1,
        shadow: Shadow {
            color: Color::BLACK.scale_alpha(0.18),
            offset: Vector::new(0.0, 2.0),
            blur_radius: 2.0,
        },
        head_background: tokens.neutral_background3.into(),
        head_text_color: tokens.neutral_foreground1,
        body_background: tokens.neutral_background1.into(),
        body_text_color: tokens.neutral_foreground1,
        foot_background: tokens.neutral_background2.into(),
        foot_text_color: tokens.neutral_foreground2,
        close_color: tokens.neutral_foreground1,
    }
}

impl number_input::Catalog for Theme {
    type Class<'a> = Box<dyn Fn(&Self, Status) -> number_input::Style + 'a>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(number_input_style)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> number_input::Style {
        class(self, status)
    }
}

impl number_input::ExtendedCatalog for Theme {
    fn style(
        &self,
        class: &<Self as number_input::Catalog>::Class<'_>,
        status: Status,
    ) -> number_input::Style {
        class(self, status)
    }
}

fn number_input_style(theme: &Theme, status: Status) -> number_input::Style {
    let tokens = theme.tokens();
    let (background, icon) = if status == Status::Disabled {
        (
            tokens.neutral_background_disabled,
            tokens.neutral_foreground_disabled,
        )
    } else {
        (tokens.brand_background, tokens.neutral_foreground_on_brand)
    };
    number_input::Style {
        button_background: Some(background.into()),
        icon_color: icon,
    }
}

impl context_menu::Catalog for Theme {
    type Class<'a> = Box<dyn Fn(&Self, Status) -> context_menu::Style + 'a>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(context_menu_style)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> context_menu::Style {
        class(self, status)
    }
}

fn context_menu_style(_theme: &Theme, _status: Status) -> context_menu::Style {
    context_menu::Style {
        background: Background::Color(Color::TRANSPARENT),
    }
}

impl color_picker::Catalog for Theme {
    type Class<'a> = Box<dyn Fn(&Self, Status) -> color_picker::Style + 'a>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(color_picker_style)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> color_picker::Style {
        class(self, status)
    }
}

fn color_picker_style(theme: &Theme, status: Status) -> color_picker::Style {
    let tokens = theme.tokens();
    let border_color = if status == Status::Focused {
        tokens.brand_stroke1
    } else {
        tokens.neutral_stroke1
    };
    color_picker::Style {
        background: tokens.neutral_background1.into(),
        border_radius: border_radius::LARGE.top_left,
        border_width: 1.0,
        border_color,
        bar_border_radius: border_radius::MEDIUM.top_left,
        bar_border_width: 1.0,
        bar_border_color: border_color,
    }
}

impl selection_list::Catalog for Theme {
    type Class<'a> = Box<dyn Fn(&Self, Status) -> selection_list::Style + 'a>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(selection_list_style)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> selection_list::Style {
        class(self, status)
    }
}

pub fn selection_list_style(theme: &Theme, status: Status) -> selection_list::Style {
    let tokens = theme.tokens();
    let (text_color, background) = match status {
        Status::Hovered => (
            tokens.neutral_foreground1_hover,
            tokens.neutral_background1_hover,
        ),
        Status::Selected => (
            tokens.neutral_foreground_on_brand,
            tokens.brand_background,
        ),
        _ => (tokens.neutral_foreground1, tokens.neutral_background1),
    };
    selection_list::Style {
        text_color,
        background: background.into(),
        border_width: 1.0,
        border_color: tokens.neutral_stroke1,
    }
}
