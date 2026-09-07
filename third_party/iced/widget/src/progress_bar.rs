use crate::core::border::{self, Border};
use crate::core::layout;
use crate::core::mouse;
use crate::core::renderer;
use crate::core::widget::Tree;
use crate::core::{
    self, Background, Color, Element, Layout, Length, Rectangle, Size, Theme, Widget,
};

use std::ops::RangeInclusive;

pub struct ProgressBar<'a, Theme = crate::Theme>
where
    Theme: Catalog,
{
    range: RangeInclusive<f32>,
    value: f32,
    length: Length,
    girth: Length,
    is_vertical: bool,
    class: Theme::Class<'a>,
}

impl<'a, Theme> ProgressBar<'a, Theme>
where
    Theme: Catalog,
{
        pub const DEFAULT_GIRTH: f32 = 30.0;

                        pub fn new(range: RangeInclusive<f32>, value: f32) -> Self {
        ProgressBar {
            value: value.clamp(*range.start(), *range.end()),
            range,
            length: Length::Fill,
            girth: Length::from(Self::DEFAULT_GIRTH),
            is_vertical: false,
            class: Theme::default(),
        }
    }

        pub fn length(mut self, length: impl Into<Length>) -> Self {
        self.length = length.into();
        self
    }

        pub fn girth(mut self, girth: impl Into<Length>) -> Self {
        self.girth = girth.into();
        self
    }

                pub fn vertical(mut self, vertical: bool) -> Self {
        self.is_vertical = vertical;
        self
    }

        #[must_use]
    pub fn style(mut self, style: impl Fn(&Theme) -> Style + 'a) -> Self
    where
        Theme::Class<'a>: From<StyleFn<'a, Theme>>,
    {
        self.class = (Box::new(style) as StyleFn<'a, Theme>).into();
        self
    }

        #[cfg(feature = "advanced")]
    #[must_use]
    pub fn class(mut self, class: impl Into<Theme::Class<'a>>) -> Self {
        self.class = class.into();
        self
    }

    fn width(&self) -> Length {
        if self.is_vertical {
            self.girth
        } else {
            self.length
        }
    }

    fn height(&self) -> Length {
        if self.is_vertical {
            self.length
        } else {
            self.girth
        }
    }
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer> for ProgressBar<'_, Theme>
where
    Theme: Catalog,
    Renderer: core::Renderer,
{
    fn size(&self) -> Size<Length> {
        Size {
            width: self.width(),
            height: self.height(),
        }
    }

    fn layout(
        &mut self,
        _tree: &mut Tree,
        _renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        layout::atomic(limits, self.width(), self.height())
    }

    fn draw(
        &self,
        _tree: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        _style: &renderer::Style,
        layout: Layout<'_>,
        _cursor: mouse::Cursor,
        _viewport: &Rectangle,
    ) {
        let bounds = layout.bounds();
        let (range_start, range_end) = self.range.clone().into_inner();

        let length = if self.is_vertical {
            bounds.height
        } else {
            bounds.width
        };

        let active_progress_length = if range_start >= range_end {
            0.0
        } else {
            length * (self.value - range_start) / (range_end - range_start)
        };

        let style = theme.style(&self.class);

        renderer.fill_quad(
            renderer::Quad {
                bounds: Rectangle { ..bounds },
                border: style.border,
                ..renderer::Quad::default()
            },
            style.background,
        );

        if active_progress_length > 0.0 {
            let bounds = if self.is_vertical {
                Rectangle {
                    y: bounds.y + bounds.height - active_progress_length,
                    height: active_progress_length,
                    ..bounds
                }
            } else {
                Rectangle {
                    width: active_progress_length,
                    ..bounds
                }
            };

            renderer.fill_quad(
                renderer::Quad {
                    bounds,
                    border: Border {
                        color: Color::TRANSPARENT,
                        ..style.border
                    },
                    ..renderer::Quad::default()
                },
                style.bar,
            );
        }
    }
}

impl<'a, Message, Theme, Renderer> From<ProgressBar<'a, Theme>>
    for Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: 'a + Catalog,
    Renderer: 'a + core::Renderer,
{
    fn from(progress_bar: ProgressBar<'a, Theme>) -> Element<'a, Message, Theme, Renderer> {
        Element::new(progress_bar)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub background: Background,
        pub bar: Background,
        pub border: Border,
}

pub trait Catalog: Sized {
        type Class<'a>;

        fn default<'a>() -> Self::Class<'a>;

        fn style(&self, class: &Self::Class<'_>) -> Style;
}

pub type StyleFn<'a, Theme> = Box<dyn Fn(&Theme) -> Style + 'a>;

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(primary)
    }

    fn style(&self, class: &Self::Class<'_>) -> Style {
        class(self)
    }
}

pub fn primary(theme: &Theme) -> Style {
    let palette = theme.palette();

    styled(palette.background.strong.color, palette.primary.base.color)
}

pub fn secondary(theme: &Theme) -> Style {
    let palette = theme.palette();

    styled(
        palette.background.strong.color,
        palette.secondary.base.color,
    )
}

pub fn success(theme: &Theme) -> Style {
    let palette = theme.palette();

    styled(palette.background.strong.color, palette.success.base.color)
}

pub fn warning(theme: &Theme) -> Style {
    let palette = theme.palette();

    styled(palette.background.strong.color, palette.warning.base.color)
}

pub fn danger(theme: &Theme) -> Style {
    let palette = theme.palette();

    styled(palette.background.strong.color, palette.danger.base.color)
}

fn styled(background: impl Into<Background>, bar: impl Into<Background>) -> Style {
    Style {
        background: background.into(),
        bar: bar.into(),
        border: border::rounded(2),
    }
}
