use crate::core;
use crate::core::border;
use crate::core::layout;
use crate::core::mouse;
use crate::core::renderer;
use crate::core::widget::Tree;
use crate::core::{Color, Element, Layout, Length, Pixels, Rectangle, Size, Theme, Widget};

pub fn horizontal<'a, Theme>(height: impl Into<Pixels>) -> Rule<'a, Theme>
where
    Theme: Catalog,
{
    Rule {
        thickness: Length::Fixed(height.into().0),
        is_vertical: false,
        class: Theme::default(),
    }
}

pub fn vertical<'a, Theme>(width: impl Into<Pixels>) -> Rule<'a, Theme>
where
    Theme: Catalog,
{
    Rule {
        thickness: Length::Fixed(width.into().0),
        is_vertical: true,
        class: Theme::default(),
    }
}

pub struct Rule<'a, Theme = crate::Theme>
where
    Theme: Catalog,
{
    thickness: Length,
    is_vertical: bool,
    class: Theme::Class<'a>,
}

impl<'a, Theme> Rule<'a, Theme>
where
    Theme: Catalog,
{
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
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer> for Rule<'_, Theme>
where
    Renderer: core::Renderer,
    Theme: Catalog,
{
    fn size(&self) -> Size<Length> {
        if self.is_vertical {
            Size {
                width: self.thickness,
                height: Length::Fill,
            }
        } else {
            Size {
                width: Length::Fill,
                height: self.thickness,
            }
        }
    }

    fn layout(
        &mut self,
        _tree: &mut Tree,
        _renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        let size = <Self as Widget<(), Theme, Renderer>>::size(self);

        layout::atomic(limits, size.width, size.height)
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
        let style = theme.style(&self.class);

        let mut bounds = if self.is_vertical {
            let line_x = bounds.x;

            let (offset, line_height) = style.fill_mode.fill(bounds.height);
            let line_y = bounds.y + offset;

            Rectangle {
                x: line_x,
                y: line_y,
                width: bounds.width,
                height: line_height,
            }
        } else {
            let line_y = bounds.y;

            let (offset, line_width) = style.fill_mode.fill(bounds.width);
            let line_x = bounds.x + offset;

            Rectangle {
                x: line_x,
                y: line_y,
                width: line_width,
                height: bounds.height,
            }
        };

        if style.snap {
            let unit = 1.0 / renderer.scale_factor().unwrap_or(1.0);

            bounds.width = bounds.width.max(unit);
            bounds.height = bounds.height.max(unit);
        }

        renderer.fill_quad(
            renderer::Quad {
                bounds,
                border: border::rounded(style.radius),
                snap: style.snap,
                ..renderer::Quad::default()
            },
            style.color,
        );
    }
}

impl<'a, Message, Theme, Renderer> From<Rule<'a, Theme>> for Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: 'a + Catalog,
    Renderer: 'a + core::Renderer,
{
    fn from(rule: Rule<'a, Theme>) -> Element<'a, Message, Theme, Renderer> {
        Element::new(rule)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub color: Color,
        pub radius: border::Radius,
        pub fill_mode: FillMode,
        pub snap: bool,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum FillMode {
        Full,
                    Percent(f32),
        Padded(u16),
            AsymmetricPadding(u16, u16),
}

impl FillMode {
                                pub fn fill(&self, space: f32) -> (f32, f32) {
        match *self {
            FillMode::Full => (0.0, space),
            FillMode::Percent(percent) => {
                if percent >= 100.0 {
                    (0.0, space)
                } else {
                    let percent_width = (space * percent / 100.0).round();

                    (((space - percent_width) / 2.0).round(), percent_width)
                }
            }
            FillMode::Padded(padding) => {
                if padding == 0 {
                    (0.0, space)
                } else {
                    let padding = padding as f32;
                    let mut line_width = space - (padding * 2.0);
                    if line_width < 0.0 {
                        line_width = 0.0;
                    }

                    (padding, line_width)
                }
            }
            FillMode::AsymmetricPadding(first_pad, second_pad) => {
                let first_pad = first_pad as f32;
                let second_pad = second_pad as f32;
                let mut line_width = space - first_pad - second_pad;
                if line_width < 0.0 {
                    line_width = 0.0;
                }

                (first_pad, line_width)
            }
        }
    }
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
        Box::new(default)
    }

    fn style(&self, class: &Self::Class<'_>) -> Style {
        class(self)
    }
}

pub fn default(theme: &Theme) -> Style {
    let palette = theme.palette();

    Style {
        color: palette.background.strong.color,
        radius: 0.0.into(),
        fill_mode: FillMode::Full,
        snap: true,
    }
}

pub fn weak(theme: &Theme) -> Style {
    let palette = theme.palette();

    Style {
        color: palette.background.weak.color,
        radius: 0.0.into(),
        fill_mode: FillMode::Full,
        snap: true,
    }
}
