use crate::core::alignment;
use crate::core::border::{self, Border};
use crate::core::layout;
use crate::core::mouse;
use crate::core::renderer;
use crate::core::text;
use crate::core::touch;
use crate::core::widget;
use crate::core::widget::tree::{self, Tree};
use crate::core::window;
use crate::core::{
    Background, Color, Element, Event, Layout, Length, Pixels, Rectangle, Shell, Size, Theme,
    Widget,
};

pub struct Radio<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer>
where
    Theme: Catalog,
    Renderer: text::Renderer,
{
    is_selected: bool,
    on_click: Message,
    label: String,
    width: Length,
    size: f32,
    spacing: f32,
    text_size: Option<Pixels>,
    line_height: text::LineHeight,
    shaping: text::Shaping,
    wrapping: text::Wrapping,
    font: Option<Renderer::Font>,
    class: Theme::Class<'a>,
    last_status: Option<Status>,
}

impl<'a, Message, Theme, Renderer> Radio<'a, Message, Theme, Renderer>
where
    Message: Clone,
    Theme: Catalog,
    Renderer: text::Renderer,
{
        pub const DEFAULT_SIZE: f32 = 16.0;

        pub const DEFAULT_SPACING: f32 = 8.0;

                                    pub fn new<F, V>(label: impl Into<String>, value: V, selected: Option<V>, f: F) -> Self
    where
        V: Eq + Copy,
        F: FnOnce(V) -> Message,
    {
        Radio {
            is_selected: Some(value) == selected,
            on_click: f(value),
            label: label.into(),
            width: Length::Shrink,
            size: Self::DEFAULT_SIZE,
            spacing: Self::DEFAULT_SPACING,
            text_size: None,
            line_height: text::LineHeight::default(),
            shaping: text::Shaping::default(),
            wrapping: text::Wrapping::default(),
            font: None,
            class: Theme::default(),
            last_status: None,
        }
    }

        pub fn size(mut self, size: impl Into<Pixels>) -> Self {
        self.size = size.into().0;
        self
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.width = width.into();
        self
    }

        pub fn spacing(mut self, spacing: impl Into<Pixels>) -> Self {
        self.spacing = spacing.into().0;
        self
    }

        pub fn text_size(mut self, text_size: impl Into<Pixels>) -> Self {
        self.text_size = Some(text_size.into());
        self
    }

        pub fn line_height(mut self, line_height: impl Into<text::LineHeight>) -> Self {
        self.line_height = line_height.into();
        self
    }

        pub fn shaping(mut self, shaping: text::Shaping) -> Self {
        self.shaping = shaping;
        self
    }

        pub fn wrapping(mut self, wrapping: text::Wrapping) -> Self {
        self.wrapping = wrapping;
        self
    }

        pub fn font(mut self, font: impl Into<Renderer::Font>) -> Self {
        self.font = Some(font.into());
        self
    }

        #[must_use]
    pub fn style(mut self, style: impl Fn(&Theme, Status) -> Style + 'a) -> Self
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

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for Radio<'_, Message, Theme, Renderer>
where
    Message: Clone,
    Theme: Catalog,
    Renderer: text::Renderer,
{
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<widget::text::State<Renderer::Paragraph>>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(widget::text::State::<Renderer::Paragraph>::default())
    }

    fn size(&self) -> Size<Length> {
        Size {
            width: self.width,
            height: Length::Shrink,
        }
    }

    fn layout(
        &mut self,
        tree: &mut Tree,
        renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        layout::next_to_each_other(
            &limits.width(self.width),
            self.spacing,
            |_| layout::Node::new(Size::new(self.size, self.size)),
            |limits| {
                let state = tree
                    .state
                    .downcast_mut::<widget::text::State<Renderer::Paragraph>>();

                widget::text::layout(
                    state,
                    renderer,
                    limits,
                    &self.label,
                    widget::text::Format {
                        width: self.width,
                        height: Length::Shrink,
                        line_height: self.line_height,
                        size: self.text_size,
                        font: self.font,
                        align_x: text::Alignment::Default,
                        align_y: alignment::Vertical::Top,
                        shaping: self.shaping,
                        wrapping: self.wrapping,
                        ellipsis: text::Ellipsis::default(),
                    },
                )
            },
        )
    }

    fn update(
        &mut self,
        _tree: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        _viewport: &Rectangle,
    ) {
        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
            | Event::Touch(touch::Event::FingerPressed { .. })
                if cursor.is_over(layout.bounds()) =>
            {
                shell.publish(self.on_click.clone());
                shell.capture_event();
            }
            _ => {}
        }

        let current_status = {
            let is_mouse_over = cursor.is_over(layout.bounds());
            let is_selected = self.is_selected;

            if is_mouse_over {
                Status::Hovered { is_selected }
            } else {
                Status::Active { is_selected }
            }
        };

        if let Event::Window(window::Event::RedrawRequested(_now)) = event {
            self.last_status = Some(current_status);
        } else if self
            .last_status
            .is_some_and(|last_status| last_status != current_status)
        {
            shell.request_redraw();
        }
    }

    fn mouse_interaction(
        &self,
        _tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _viewport: &Rectangle,
        _renderer: &Renderer,
    ) -> mouse::Interaction {
        if cursor.is_over(layout.bounds()) {
            mouse::Interaction::Pointer
        } else {
            mouse::Interaction::default()
        }
    }

    fn draw(
        &self,
        tree: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        defaults: &renderer::Style,
        layout: Layout<'_>,
        _cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let mut children = layout.children();

        let style = theme.style(
            &self.class,
            self.last_status.unwrap_or(Status::Active {
                is_selected: self.is_selected,
            }),
        );

        {
            let layout = children.next().unwrap();
            let bounds = layout.bounds();

            let size = bounds.width;
            let dot_size = size / 2.0;

            renderer.fill_quad(
                renderer::Quad {
                    bounds,
                    border: Border {
                        radius: (size / 2.0).into(),
                        width: style.border_width,
                        color: style.border_color,
                    },
                    ..renderer::Quad::default()
                },
                style.background,
            );

            if self.is_selected {
                renderer.fill_quad(
                    renderer::Quad {
                        bounds: Rectangle {
                            x: bounds.x + dot_size / 2.0,
                            y: bounds.y + dot_size / 2.0,
                            width: bounds.width - dot_size,
                            height: bounds.height - dot_size,
                        },
                        border: border::rounded(dot_size / 2.0),
                        ..renderer::Quad::default()
                    },
                    style.dot_color,
                );
            }
        }

        {
            let label_layout = children.next().unwrap();
            let state: &widget::text::State<Renderer::Paragraph> = tree.state.downcast_ref();

            crate::text::draw(
                renderer,
                defaults,
                label_layout.bounds(),
                state.raw(),
                crate::text::Style {
                    color: style.text_color,
                },
                viewport,
            );
        }
    }
}

impl<'a, Message, Theme, Renderer> From<Radio<'a, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    Message: 'a + Clone,
    Theme: 'a + Catalog,
    Renderer: 'a + text::Renderer,
{
    fn from(radio: Radio<'a, Message, Theme, Renderer>) -> Element<'a, Message, Theme, Renderer> {
        Element::new(radio)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
        Active {
                is_selected: bool,
    },
        Hovered {
                is_selected: bool,
    },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub background: Background,
        pub dot_color: Color,
        pub border_width: f32,
        pub border_color: Color,
        pub text_color: Option<Color>,
}

pub trait Catalog {
        type Class<'a>;

        fn default<'a>() -> Self::Class<'a>;

        fn style(&self, class: &Self::Class<'_>, status: Status) -> Style;
}

pub type StyleFn<'a, Theme> = Box<dyn Fn(&Theme, Status) -> Style + 'a>;

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(default)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> Style {
        class(self, status)
    }
}

pub fn default(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    let active = Style {
        background: Color::TRANSPARENT.into(),
        dot_color: palette.primary.strong.color,
        border_width: 1.0,
        border_color: palette.primary.strong.color,
        text_color: None,
    };

    match status {
        Status::Active { .. } => active,
        Status::Hovered { .. } => Style {
            dot_color: palette.primary.strong.color,
            background: palette.primary.weak.color.into(),
            ..active
        },
    }
}
