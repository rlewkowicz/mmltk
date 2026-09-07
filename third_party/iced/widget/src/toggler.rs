use crate::core::alignment;
use crate::core::border;
use crate::core::layout;
use crate::core::mouse;
use crate::core::renderer;
use crate::core::text;
use crate::core::touch;
use crate::core::widget;
use crate::core::widget::tree::{self, Tree};
use crate::core::window;
use crate::core::{
    Background, Border, Color, Element, Event, Layout, Length, Pixels, Rectangle, Shell, Size,
    Theme, Widget,
};

pub struct Toggler<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer>
where
    Theme: Catalog,
    Renderer: text::Renderer,
{
    is_toggled: bool,
    on_toggle: Option<Box<dyn Fn(bool) -> Message + 'a>>,
    label: Option<text::Fragment<'a>>,
    width: Length,
    size: f32,
    text_size: Option<Pixels>,
    line_height: text::LineHeight,
    alignment: text::Alignment,
    text_shaping: text::Shaping,
    wrapping: text::Wrapping,
    spacing: f32,
    font: Option<Renderer::Font>,
    class: Theme::Class<'a>,
    last_status: Option<Status>,
}

impl<'a, Message, Theme, Renderer> Toggler<'a, Message, Theme, Renderer>
where
    Theme: Catalog,
    Renderer: text::Renderer,
{
        pub const DEFAULT_SIZE: f32 = 16.0;

                                    pub fn new(is_toggled: bool) -> Self {
        Toggler {
            is_toggled,
            on_toggle: None,
            label: None,
            width: Length::Shrink,
            size: Self::DEFAULT_SIZE,
            text_size: None,
            line_height: text::LineHeight::default(),
            alignment: text::Alignment::Default,
            text_shaping: text::Shaping::default(),
            wrapping: text::Wrapping::default(),
            spacing: Self::DEFAULT_SIZE / 2.0,
            font: None,
            class: Theme::default(),
            last_status: None,
        }
    }

        pub fn label(mut self, label: impl text::IntoFragment<'a>) -> Self {
        self.label = Some(label.into_fragment());
        self
    }

                    pub fn on_toggle(mut self, on_toggle: impl Fn(bool) -> Message + 'a) -> Self {
        self.on_toggle = Some(Box::new(on_toggle));
        self
    }

                    pub fn on_toggle_maybe(mut self, on_toggle: Option<impl Fn(bool) -> Message + 'a>) -> Self {
        self.on_toggle = on_toggle.map(|on_toggle| Box::new(on_toggle) as _);
        self
    }

        pub fn size(mut self, size: impl Into<Pixels>) -> Self {
        self.size = size.into().0;
        self
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.width = width.into();
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

        pub fn alignment(mut self, alignment: impl Into<text::Alignment>) -> Self {
        self.alignment = alignment.into();
        self
    }

        pub fn shaping(mut self, shaping: text::Shaping) -> Self {
        self.text_shaping = shaping;
        self
    }

        pub fn wrapping(mut self, wrapping: text::Wrapping) -> Self {
        self.wrapping = wrapping;
        self
    }

        pub fn spacing(mut self, spacing: impl Into<Pixels>) -> Self {
        self.spacing = spacing.into().0;
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
    for Toggler<'_, Message, Theme, Renderer>
where
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
        let limits = limits.width(self.width);

        layout::next_to_each_other(
            &limits,
            if self.label.is_some() {
                self.spacing
            } else {
                0.0
            },
            |_| {
                let size = if renderer::CRISP {
                    let scale_factor = renderer.scale_factor().unwrap_or(1.0);

                    (self.size * scale_factor).round() / scale_factor
                } else {
                    self.size
                };

                layout::Node::new(Size::new(2.0 * size, size))
            },
            |limits| {
                if let Some(label) = self.label.as_deref() {
                    let state = tree
                        .state
                        .downcast_mut::<widget::text::State<Renderer::Paragraph>>();

                    widget::text::layout(
                        state,
                        renderer,
                        limits,
                        label,
                        widget::text::Format {
                            width: self.width,
                            height: Length::Shrink,
                            line_height: self.line_height,
                            size: self.text_size,
                            font: self.font,
                            align_x: self.alignment,
                            align_y: alignment::Vertical::Top,
                            shaping: self.text_shaping,
                            wrapping: self.wrapping,
                            ellipsis: text::Ellipsis::None,
                        },
                    )
                } else {
                    layout::Node::new(Size::ZERO)
                }
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
        let Some(on_toggle) = &self.on_toggle else {
            return;
        };

        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
            | Event::Touch(touch::Event::FingerPressed { .. }) => {
                let mouse_over = cursor.is_over(layout.bounds());

                if mouse_over {
                    shell.publish(on_toggle(!self.is_toggled));
                    shell.capture_event();
                }
            }
            _ => {}
        }

        let current_status = if self.on_toggle.is_none() {
            Status::Disabled {
                is_toggled: self.is_toggled,
            }
        } else if cursor.is_over(layout.bounds()) {
            Status::Hovered {
                is_toggled: self.is_toggled,
            }
        } else {
            Status::Active {
                is_toggled: self.is_toggled,
            }
        };

        if let Event::Window(window::Event::RedrawRequested(_now)) = event {
            self.last_status = Some(current_status);
        } else if self
            .last_status
            .is_some_and(|status| status != current_status)
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
            if self.on_toggle.is_some() {
                mouse::Interaction::Pointer
            } else {
                mouse::Interaction::NotAllowed
            }
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
        let toggler_layout = children.next().unwrap();

        let style = theme.style(
            &self.class,
            self.last_status.unwrap_or(Status::Disabled {
                is_toggled: self.is_toggled,
            }),
        );

        if self.label.is_some() {
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

        let scale_factor = renderer.scale_factor().unwrap_or(1.0);
        let bounds = toggler_layout.bounds();

        let border_radius = style
            .border_radius
            .unwrap_or_else(|| border::Radius::new(bounds.height / 2.0));

        renderer.fill_quad(
            renderer::Quad {
                bounds,
                border: Border {
                    radius: border_radius,
                    width: style.background_border_width,
                    color: style.background_border_color,
                },
                ..renderer::Quad::default()
            },
            style.background,
        );

        let toggle_bounds = {
            let bounds = if renderer::CRISP {
                (bounds * scale_factor).round()
            } else {
                bounds
            };

            let padding = (style.padding_ratio * bounds.height).round();

            Rectangle {
                x: bounds.x
                    + if self.is_toggled {
                        bounds.width - bounds.height + padding
                    } else {
                        padding
                    },
                y: bounds.y + padding,
                width: bounds.height - (2.0 * padding),
                height: bounds.height - (2.0 * padding),
            } * (1.0 / scale_factor)
        };

        renderer.fill_quad(
            renderer::Quad {
                bounds: toggle_bounds,
                border: Border {
                    radius: border_radius,
                    width: style.foreground_border_width,
                    color: style.foreground_border_color,
                },
                ..renderer::Quad::default()
            },
            style.foreground,
        );
    }
}

impl<'a, Message, Theme, Renderer> From<Toggler<'a, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: Catalog + 'a,
    Renderer: text::Renderer + 'a,
{
    fn from(
        toggler: Toggler<'a, Message, Theme, Renderer>,
    ) -> Element<'a, Message, Theme, Renderer> {
        Element::new(toggler)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
        Active {
                is_toggled: bool,
    },
        Hovered {
                is_toggled: bool,
    },
        Disabled {
                is_toggled: bool,
    },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub background: Background,
        pub background_border_width: f32,
        pub background_border_color: Color,
        pub foreground: Background,
        pub foreground_border_width: f32,
        pub foreground_border_color: Color,
        pub text_color: Option<Color>,
                pub border_radius: Option<border::Radius>,
        pub padding_ratio: f32,
}

pub trait Catalog: Sized {
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

    let background = match status {
        Status::Active { is_toggled } | Status::Hovered { is_toggled } => {
            if is_toggled {
                palette.primary.base.color
            } else {
                palette.background.strong.color
            }
        }
        Status::Disabled { is_toggled } => {
            if is_toggled {
                palette.background.strong.color
            } else {
                palette.background.weak.color
            }
        }
    };

    let foreground = match status {
        Status::Active { is_toggled } => {
            if is_toggled {
                palette.primary.base.text
            } else {
                palette.background.base.color
            }
        }
        Status::Hovered { is_toggled } => {
            if is_toggled {
                Color {
                    a: 0.5,
                    ..palette.primary.base.text
                }
            } else {
                palette.background.weak.color
            }
        }
        Status::Disabled { .. } => palette.background.weakest.color,
    };

    Style {
        background: background.into(),
        foreground: foreground.into(),
        foreground_border_width: 0.0,
        foreground_border_color: Color::TRANSPARENT,
        background_border_width: 0.0,
        background_border_color: Color::TRANSPARENT,
        text_color: None,
        border_radius: None,
        padding_ratio: 0.1,
    }
}
