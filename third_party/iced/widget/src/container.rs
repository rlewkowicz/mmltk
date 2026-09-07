use crate::core::alignment::{self, Alignment};
use crate::core::border::{self, Border};
use crate::core::gradient::{self, Gradient};
use crate::core::layout;
use crate::core::mouse;
use crate::core::overlay;
use crate::core::renderer;
use crate::core::theme;
use crate::core::widget::tree::{self, Tree};
use crate::core::widget::{self, Operation};
use crate::core::{
    self, Background, Color, Element, Event, Layout, Length, Padding, Rectangle, Shadow, Shell,
    Size, Theme, Vector, Widget, color,
};

pub struct Container<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer>
where
    Theme: Catalog,
    Renderer: core::Renderer,
{
    id: Option<widget::Id>,
    padding: Padding,
    width: Length,
    height: Length,
    horizontal_alignment: alignment::Horizontal,
    vertical_alignment: alignment::Vertical,
    clip: bool,
    content: Element<'a, Message, Theme, Renderer>,
    class: Theme::Class<'a>,
}

impl<'a, Message, Theme, Renderer> Container<'a, Message, Theme, Renderer>
where
    Theme: Catalog,
    Renderer: core::Renderer,
{
        pub fn new(content: impl Into<Element<'a, Message, Theme, Renderer>>) -> Self {
        let content = content.into();

        Container {
            id: None,
            padding: Padding::ZERO,
            width: Length::Fit,
            height: Length::Fit,
            horizontal_alignment: alignment::Horizontal::Left,
            vertical_alignment: alignment::Vertical::Top,
            clip: false,
            class: Theme::default(),
            content,
        }
    }

        pub fn id(mut self, id: impl Into<widget::Id>) -> Self {
        self.id = Some(id.into());
        self
    }

        pub fn padding<P: Into<Padding>>(mut self, padding: P) -> Self {
        self.padding = padding.into();
        self
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.width = width.into();
        self
    }

        pub fn height(mut self, height: impl Into<Length>) -> Self {
        self.height = height.into();
        self
    }

        pub fn center_x(self, width: impl Into<Length>) -> Self {
        self.width(width).align_x(alignment::Horizontal::Center)
    }

        pub fn center_y(self, height: impl Into<Length>) -> Self {
        self.height(height).align_y(alignment::Vertical::Center)
    }

                                pub fn center(self, length: impl Into<Length>) -> Self {
        let length = length.into();

        self.center_x(length).center_y(length)
    }

        pub fn align_left(self, width: impl Into<Length>) -> Self {
        self.width(width).align_x(alignment::Horizontal::Left)
    }

        pub fn align_right(self, width: impl Into<Length>) -> Self {
        self.width(width).align_x(alignment::Horizontal::Right)
    }

        pub fn align_top(self, height: impl Into<Length>) -> Self {
        self.height(height).align_y(alignment::Vertical::Top)
    }

        pub fn align_bottom(self, height: impl Into<Length>) -> Self {
        self.height(height).align_y(alignment::Vertical::Bottom)
    }

        pub fn align_x(mut self, alignment: impl Into<alignment::Horizontal>) -> Self {
        self.horizontal_alignment = alignment.into();
        self
    }

        pub fn align_y(mut self, alignment: impl Into<alignment::Vertical>) -> Self {
        self.vertical_alignment = alignment.into();
        self
    }

            pub fn clip(mut self, clip: bool) -> Self {
        self.clip = clip;
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

        #[must_use]
    pub fn class(mut self, class: impl Into<Theme::Class<'a>>) -> Self {
        self.class = class.into();
        self
    }
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for Container<'_, Message, Theme, Renderer>
where
    Theme: Catalog,
    Renderer: core::Renderer,
{
    fn tag(&self) -> tree::Tag {
        self.content.as_widget().tag()
    }

    fn state(&self) -> tree::State {
        self.content.as_widget().state()
    }

    fn diff(&mut self, tree: &mut Tree) {
        self.content.as_widget_mut().diff(tree);

        let size = self.content.as_widget().size();
        self.width = self.width.stack(size.width);
        self.height = self.height.stack(size.height);
    }

    fn size(&self) -> Size<Length> {
        Size {
            width: self.width,
            height: self.height,
        }
    }

    fn layout(
        &mut self,
        tree: &mut Tree,
        renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        layout(
            limits,
            self.width,
            self.height,
            self.padding,
            self.horizontal_alignment,
            self.vertical_alignment,
            |limits| self.content.as_widget_mut().layout(tree, renderer, limits),
        )
    }

    fn operate(
        &mut self,
        tree: &mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        operation.container(self.id.as_ref(), layout.bounds());
        operation.traverse(&mut |operation| {
            self.content.as_widget_mut().operate(
                tree,
                layout.children().next().unwrap(),
                renderer,
                operation,
            );
        });
    }

    fn update(
        &mut self,
        tree: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        self.content.as_widget_mut().update(
            tree,
            event,
            layout.children().next().unwrap(),
            cursor,
            renderer,
            shell,
            viewport,
        );
    }

    fn mouse_interaction(
        &self,
        tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        self.content.as_widget().mouse_interaction(
            tree,
            layout.children().next().unwrap(),
            cursor,
            viewport,
            renderer,
        )
    }

    fn draw(
        &self,
        tree: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        renderer_style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let bounds = layout.bounds();
        let style = theme.style(&self.class);

        if let Some(clipped_viewport) = bounds.intersection(viewport) {
            draw_background(renderer, &style, bounds);

            self.content.as_widget().draw(
                tree,
                renderer,
                theme,
                &renderer::Style {
                    text_color: style.text_color.unwrap_or(renderer_style.text_color),
                },
                layout.children().next().unwrap(),
                cursor,
                if self.clip {
                    &clipped_viewport
                } else {
                    viewport
                },
            );
        }
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        layout: Layout<'b>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        self.content.as_widget_mut().overlay(
            tree,
            layout.children().next().unwrap(),
            renderer,
            viewport,
            translation,
        )
    }
}

impl<'a, Message, Theme, Renderer> From<Container<'a, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: Catalog + 'a,
    Renderer: core::Renderer + 'a,
{
    fn from(
        container: Container<'a, Message, Theme, Renderer>,
    ) -> Element<'a, Message, Theme, Renderer> {
        Element::new(container)
    }
}

pub fn layout(
    limits: &layout::Limits,
    width: Length,
    height: Length,
    padding: Padding,
    horizontal_alignment: alignment::Horizontal,
    vertical_alignment: alignment::Vertical,
    layout_content: impl FnOnce(&layout::Limits) -> layout::Node,
) -> layout::Node {
    layout::positioned(
        limits,
        width,
        height,
        padding,
        |limits| layout_content(&limits.loose()),
        |content, size| {
            content.align(
                Alignment::from(horizontal_alignment),
                Alignment::from(vertical_alignment),
                size,
            )
        },
    )
}

pub fn draw_background<Renderer>(renderer: &mut Renderer, style: &Style, bounds: Rectangle)
where
    Renderer: core::Renderer,
{
    if style.background.is_some() || style.border.width > 0.0 || style.shadow.color.a > 0.0 {
        renderer.fill_quad(
            renderer::Quad {
                bounds,
                border: style.border,
                shadow: style.shadow,
                snap: style.snap,
            },
            style
                .background
                .unwrap_or(Background::Color(Color::TRANSPARENT)),
        );
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub text_color: Option<Color>,
        pub background: Option<Background>,
        pub border: Border,
        pub shadow: Shadow,
        pub snap: bool,
}

impl Default for Style {
    fn default() -> Self {
        Self {
            text_color: None,
            background: None,
            border: Border::default(),
            shadow: Shadow::default(),
            snap: renderer::CRISP,
        }
    }
}

impl Style {
        pub fn color(self, color: impl Into<Color>) -> Self {
        Self {
            text_color: Some(color.into()),
            ..self
        }
    }

        pub fn border(self, border: impl Into<Border>) -> Self {
        Self {
            border: border.into(),
            ..self
        }
    }

        pub fn background(self, background: impl Into<Background>) -> Self {
        Self {
            background: Some(background.into()),
            ..self
        }
    }

        pub fn shadow(self, shadow: impl Into<Shadow>) -> Self {
        Self {
            shadow: shadow.into(),
            ..self
        }
    }
}

impl From<Color> for Style {
    fn from(color: Color) -> Self {
        Self::default().background(color)
    }
}

impl From<Gradient> for Style {
    fn from(gradient: Gradient) -> Self {
        Self::default().background(gradient)
    }
}

impl From<gradient::Linear> for Style {
    fn from(gradient: gradient::Linear) -> Self {
        Self::default().background(gradient)
    }
}

pub trait Catalog {
        type Class<'a>;

        fn default<'a>() -> Self::Class<'a>;

        fn style(&self, class: &Self::Class<'_>) -> Style;
}

pub type StyleFn<'a, Theme> = Box<dyn Fn(&Theme) -> Style + 'a>;

impl<Theme> From<Style> for StyleFn<'_, Theme> {
    fn from(style: Style) -> Self {
        Box::new(move |_theme| style)
    }
}

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(transparent)
    }

    fn style(&self, class: &Self::Class<'_>) -> Style {
        class(self)
    }
}

pub fn transparent<Theme>(_theme: &Theme) -> Style {
    Style::default()
}

pub fn background(background: impl Into<Background>) -> Style {
    Style::default().background(background)
}

pub fn rounded_box(theme: &Theme) -> Style {
    let palette = theme.palette();

    Style {
        background: Some(palette.background.weak.color.into()),
        text_color: Some(palette.background.weak.text),
        border: border::rounded(2),
        ..Style::default()
    }
}

pub fn bordered_box(theme: &Theme) -> Style {
    let palette = theme.palette();

    Style {
        background: Some(palette.background.weakest.color.into()),
        text_color: Some(palette.background.weakest.text),
        border: Border {
            width: 1.0,
            radius: 5.0.into(),
            color: palette.background.weak.color,
        },
        ..Style::default()
    }
}

pub fn dark(_theme: &Theme) -> Style {
    style(theme::palette::Pair {
        color: color!(0x111111),
        text: Color::WHITE,
    })
}

pub fn primary(theme: &Theme) -> Style {
    let palette = theme.palette();

    style(palette.primary.base)
}

pub fn secondary(theme: &Theme) -> Style {
    let palette = theme.palette();

    style(palette.secondary.base)
}

pub fn success(theme: &Theme) -> Style {
    let palette = theme.palette();

    style(palette.success.base)
}

pub fn warning(theme: &Theme) -> Style {
    let palette = theme.palette();

    style(palette.warning.base)
}

pub fn danger(theme: &Theme) -> Style {
    let palette = theme.palette();

    style(palette.danger.base)
}

fn style(pair: theme::palette::Pair) -> Style {
    Style {
        background: Some(pair.color.into()),
        text_color: Some(pair.text),
        border: border::rounded(2),
        ..Style::default()
    }
}
