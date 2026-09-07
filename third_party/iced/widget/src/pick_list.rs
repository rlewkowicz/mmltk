use crate::core::alignment;
use crate::core::keyboard;
use crate::core::layout;
use crate::core::mouse;
use crate::core::overlay;
use crate::core::renderer;
use crate::core::text::paragraph;
use crate::core::text::{self, Text};
use crate::core::touch;
use crate::core::widget::tree::{self, Tree};
use crate::core::window;
use crate::core::{
    Background, Border, Color, Element, Event, Layout, Length, Padding, Pixels, Point, Rectangle,
    Shell, Size, Theme, Vector, Widget,
};
use crate::overlay::menu::{self, Menu};

use std::borrow::Borrow;
use std::f32;

pub struct PickList<'a, T, L, V, Message, Theme = crate::Theme, Renderer = crate::Renderer>
where
    T: PartialEq + Clone,
    L: Borrow<[T]> + 'a,
    V: Borrow<T> + 'a,
    Theme: Catalog,
    Renderer: text::Renderer,
{
    options: L,
    to_string: Box<dyn Fn(&T) -> String + 'a>,
    on_select: Option<Box<dyn Fn(T) -> Message + 'a>>,
    on_open: Option<Message>,
    on_close: Option<Message>,
    placeholder: Option<String>,
    selected: Option<V>,
    width: Length,
    padding: Padding,
    text_size: Option<Pixels>,
    line_height: text::LineHeight,
    shaping: text::Shaping,
    ellipsis: text::Ellipsis,
    font: Option<Renderer::Font>,
    handle: Handle<Renderer::Font>,
    class: <Theme as Catalog>::Class<'a>,
    menu_class: <Theme as menu::Catalog>::Class<'a>,
    last_status: Option<Status>,
    menu_height: Length,
}

impl<'a, T, L, V, Message, Theme, Renderer> PickList<'a, T, L, V, Message, Theme, Renderer>
where
    T: PartialEq + Clone,
    L: Borrow<[T]> + 'a,
    V: Borrow<T> + 'a,
    Message: Clone,
    Theme: Catalog,
    Renderer: text::Renderer,
{
            pub fn new(selected: Option<V>, options: L, to_string: impl Fn(&T) -> String + 'a) -> Self {
        Self {
            to_string: Box::new(to_string),
            on_select: None,
            on_open: None,
            on_close: None,
            options,
            placeholder: None,
            selected,
            width: Length::Shrink,
            padding: crate::button::DEFAULT_PADDING,
            text_size: None,
            line_height: text::LineHeight::default(),
            shaping: text::Shaping::default(),
            ellipsis: text::Ellipsis::End,
            font: None,
            handle: Handle::default(),
            class: <Theme as Catalog>::default(),
            menu_class: <Theme as Catalog>::default_menu(),
            last_status: None,
            menu_height: Length::Shrink,
        }
    }

        pub fn placeholder(mut self, placeholder: impl Into<String>) -> Self {
        self.placeholder = Some(placeholder.into());
        self
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.width = width.into();
        self
    }

        pub fn menu_height(mut self, menu_height: impl Into<Length>) -> Self {
        self.menu_height = menu_height.into();
        self
    }

        pub fn padding<P: Into<Padding>>(mut self, padding: P) -> Self {
        self.padding = padding.into();
        self
    }

        pub fn text_size(mut self, size: impl Into<Pixels>) -> Self {
        self.text_size = Some(size.into());
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

        pub fn ellipsis(mut self, ellipsis: text::Ellipsis) -> Self {
        self.ellipsis = ellipsis;
        self
    }

        pub fn font(mut self, font: impl Into<Renderer::Font>) -> Self {
        self.font = Some(font.into());
        self
    }

        pub fn handle(mut self, handle: Handle<Renderer::Font>) -> Self {
        self.handle = handle;
        self
    }

        pub fn on_select(mut self, on_select: impl Fn(T) -> Message + 'a) -> Self {
        self.on_select = Some(Box::new(on_select));
        self
    }

        pub fn on_open(mut self, on_open: Message) -> Self {
        self.on_open = Some(on_open);
        self
    }

        pub fn on_close(mut self, on_close: Message) -> Self {
        self.on_close = Some(on_close);
        self
    }

        #[must_use]
    pub fn style(mut self, style: impl Fn(&Theme, Status) -> Style + 'a) -> Self
    where
        <Theme as Catalog>::Class<'a>: From<StyleFn<'a, Theme>>,
    {
        self.class = (Box::new(style) as StyleFn<'a, Theme>).into();
        self
    }

        #[must_use]
    pub fn menu_style(mut self, style: impl Fn(&Theme) -> menu::Style + 'a) -> Self
    where
        <Theme as menu::Catalog>::Class<'a>: From<menu::StyleFn<'a, Theme>>,
    {
        self.menu_class = (Box::new(style) as menu::StyleFn<'a, Theme>).into();
        self
    }

        #[cfg(feature = "advanced")]
    #[must_use]
    pub fn class(mut self, class: impl Into<<Theme as Catalog>::Class<'a>>) -> Self {
        self.class = class.into();
        self
    }

        #[cfg(feature = "advanced")]
    #[must_use]
    pub fn menu_class(mut self, class: impl Into<<Theme as menu::Catalog>::Class<'a>>) -> Self {
        self.menu_class = class.into();
        self
    }
}

impl<'a, T, L, V, Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for PickList<'a, T, L, V, Message, Theme, Renderer>
where
    T: Clone + PartialEq + 'a,
    L: Borrow<[T]>,
    V: Borrow<T>,
    Message: Clone + 'a,
    Theme: Catalog + 'a,
    Renderer: text::Renderer + 'a,
{
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<State<Renderer::Paragraph>>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(State::<Renderer::Paragraph>::new())
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
        let state = tree.state.downcast_mut::<State<Renderer::Paragraph>>();

        let font = self.font.unwrap_or_else(|| renderer.default_font());
        let text_size = self.text_size.unwrap_or_else(|| renderer.default_size());
        let options = self.options.borrow();

        let option_text = Text {
            content: "",
            bounds: Size::new(
                limits.max().width,
                self.line_height.to_absolute(text_size).into(),
            ),
            size: text_size,
            line_height: self.line_height,
            font,
            align_x: text::Alignment::Default,
            align_y: alignment::Vertical::Center,
            shaping: self.shaping,
            wrapping: text::Wrapping::None,
            ellipsis: self.ellipsis,
            hint_factor: renderer.scale_factor(),
        };

        if let Some(placeholder) = &self.placeholder {
            let _ = state.placeholder.update(Text {
                content: placeholder,
                ..option_text
            });
        }

        let max_width = match self.width {
            Length::Shrink => {
                state.options.resize_with(options.len(), Default::default);

                for (option, paragraph) in options.iter().zip(state.options.iter_mut()) {
                    let label = (self.to_string)(option);

                    let _ = paragraph.update(Text {
                        content: &label,
                        ..option_text
                    });
                }

                let labels_width = state.options.iter().fold(0.0, |width, paragraph| {
                    f32::max(width, paragraph.min_width())
                });

                labels_width.max(
                    self.placeholder
                        .as_ref()
                        .map(|_| state.placeholder.min_width())
                        .unwrap_or(0.0),
                )
            }
            _ => 0.0,
        };

        let size = {
            let intrinsic = Size::new(
                max_width + text_size.0 + self.padding.left,
                f32::from(self.line_height.to_absolute(text_size)),
            );

            limits
                .width(self.width)
                .shrink(self.padding)
                .resolve(self.width, Length::Shrink, intrinsic)
                .expand(self.padding)
        };

        layout::Node::new(size)
    }

    fn update(
        &mut self,
        tree: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        _viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_mut::<State<Renderer::Paragraph>>();

        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
            | Event::Touch(touch::Event::FingerPressed { .. }) => {
                if state.is_open {
                    state.is_open = false;

                    if let Some(on_close) = &self.on_close {
                        shell.publish(on_close.clone());
                    }

                    shell.capture_event();
                } else if cursor.is_over(layout.bounds()) {
                    let selected = self.selected.as_ref().map(Borrow::borrow);

                    state.is_open = true;
                    state.hovered_option = self
                        .options
                        .borrow()
                        .iter()
                        .position(|option| Some(option) == selected);

                    if let Some(on_open) = &self.on_open {
                        shell.publish(on_open.clone());
                    }

                    shell.capture_event();
                }
            }
            Event::Mouse(mouse::Event::WheelScrolled {
                delta: mouse::ScrollDelta::Lines { y, .. },
            }) => {
                let Some(on_select) = &self.on_select else {
                    return;
                };

                if state.keyboard_modifiers.command()
                    && cursor.is_over(layout.bounds())
                    && !state.is_open
                {
                    fn find_next<'a, T: PartialEq>(
                        selected: &'a T,
                        mut options: impl Iterator<Item = &'a T>,
                    ) -> Option<&'a T> {
                        let _ = options.find(|&option| option == selected);

                        options.next()
                    }

                    let options = self.options.borrow();
                    let selected = self.selected.as_ref().map(Borrow::borrow);

                    let next_option = if *y < 0.0 {
                        if let Some(selected) = selected {
                            find_next(selected, options.iter())
                        } else {
                            options.first()
                        }
                    } else if *y > 0.0 {
                        if let Some(selected) = selected {
                            find_next(selected, options.iter().rev())
                        } else {
                            options.last()
                        }
                    } else {
                        None
                    };

                    if let Some(next_option) = next_option {
                        shell.publish(on_select(next_option.clone()));
                    }

                    shell.capture_event();
                }
            }
            Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers)) => {
                state.keyboard_modifiers = *modifiers;
            }
            _ => {}
        };

        let status = {
            let is_hovered = cursor.is_over(layout.bounds());

            if self.on_select.is_none() {
                Status::Disabled
            } else if state.is_open {
                Status::Opened { is_hovered }
            } else if is_hovered {
                Status::Hovered
            } else {
                Status::Active
            }
        };

        if let Event::Window(window::Event::RedrawRequested(_now)) = event {
            self.last_status = Some(status);
        } else if self
            .last_status
            .is_some_and(|last_status| last_status != status)
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
        let bounds = layout.bounds();
        let is_mouse_over = cursor.is_over(bounds);

        if is_mouse_over {
            if self.on_select.is_some() {
                mouse::Interaction::Pointer
            } else {
                mouse::Interaction::Idle
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
        _style: &renderer::Style,
        layout: Layout<'_>,
        _cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let font = self.font.unwrap_or_else(|| renderer.default_font());
        let selected = self.selected.as_ref().map(Borrow::borrow);
        let state = tree.state.downcast_ref::<State<Renderer::Paragraph>>();

        let bounds = layout.bounds();

        let style = Catalog::style(
            theme,
            &self.class,
            self.last_status.unwrap_or(Status::Active),
        );

        renderer.fill_quad(
            renderer::Quad {
                bounds,
                border: style.border,
                ..renderer::Quad::default()
            },
            style.background,
        );

        let handle = match &self.handle {
            Handle::Arrow { size } => Some((
                Renderer::ICON_FONT,
                Renderer::ARROW_DOWN_ICON,
                *size,
                text::LineHeight::default(),
                text::Shaping::Basic,
            )),
            Handle::Static(Icon {
                font,
                code_point,
                size,
                line_height,
                shaping,
            }) => Some((*font, *code_point, *size, *line_height, *shaping)),
            Handle::Dynamic { open, closed } => {
                if state.is_open {
                    Some((
                        open.font,
                        open.code_point,
                        open.size,
                        open.line_height,
                        open.shaping,
                    ))
                } else {
                    Some((
                        closed.font,
                        closed.code_point,
                        closed.size,
                        closed.line_height,
                        closed.shaping,
                    ))
                }
            }
            Handle::None => None,
        };

        if let Some((font, code_point, size, line_height, shaping)) = handle {
            let size = size.unwrap_or_else(|| renderer.default_size());

            renderer.fill_text(
                Text {
                    content: code_point.to_string(),
                    size,
                    line_height,
                    font,
                    bounds: Size::new(bounds.width, f32::from(line_height.to_absolute(size))),
                    align_x: text::Alignment::Right,
                    align_y: alignment::Vertical::Center,
                    shaping,
                    wrapping: text::Wrapping::None,
                    ellipsis: text::Ellipsis::None,
                    hint_factor: None,
                },
                Point::new(
                    bounds.x + bounds.width - self.padding.right,
                    bounds.center_y(),
                ),
                style.handle_color,
                *viewport,
            );
        }

        let label = selected.map(&self.to_string);

        if let Some(label) = label.or_else(|| self.placeholder.clone()) {
            let text_size = self.text_size.unwrap_or_else(|| renderer.default_size());

            renderer.fill_text(
                Text {
                    content: label,
                    size: text_size,
                    line_height: self.line_height,
                    font,
                    bounds: Size::new(
                        bounds.width - self.padding.x(),
                        f32::from(self.line_height.to_absolute(text_size)),
                    ),
                    align_x: text::Alignment::Default,
                    align_y: alignment::Vertical::Center,
                    shaping: self.shaping,
                    wrapping: text::Wrapping::None,
                    ellipsis: self.ellipsis,
                    hint_factor: renderer.scale_factor(),
                },
                Point::new(bounds.x + self.padding.left, bounds.center_y()),
                if selected.is_some() {
                    style.text_color
                } else {
                    style.placeholder_color
                },
                *viewport,
            );
        }
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        let Some(on_select) = &self.on_select else {
            return None;
        };

        let state = tree.state.downcast_mut::<State<Renderer::Paragraph>>();
        let font = self.font.unwrap_or_else(|| renderer.default_font());

        if state.is_open {
            let bounds = layout.bounds();

            let mut menu = Menu::new(
                &mut state.menu,
                self.options.borrow(),
                &mut state.hovered_option,
                &self.to_string,
                |option| {
                    state.is_open = false;

                    (on_select)(option)
                },
                None,
                &self.menu_class,
            )
            .width(bounds.width)
            .padding(self.padding)
            .font(font)
            .ellipsis(self.ellipsis)
            .shaping(self.shaping);

            if let Some(text_size) = self.text_size {
                menu = menu.text_size(text_size);
            }

            Some(menu.overlay(
                layout.position() + translation,
                *viewport,
                bounds.height,
                self.menu_height,
            ))
        } else {
            None
        }
    }
}

impl<'a, T, L, V, Message, Theme, Renderer> From<PickList<'a, T, L, V, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    T: Clone + PartialEq + 'a,
    L: Borrow<[T]> + 'a,
    V: Borrow<T> + 'a,
    Message: Clone + 'a,
    Theme: Catalog + 'a,
    Renderer: text::Renderer + 'a,
{
    fn from(pick_list: PickList<'a, T, L, V, Message, Theme, Renderer>) -> Self {
        Self::new(pick_list)
    }
}

#[derive(Debug)]
struct State<P: text::Paragraph> {
    menu: menu::State,
    keyboard_modifiers: keyboard::Modifiers,
    is_open: bool,
    hovered_option: Option<usize>,
    options: Vec<paragraph::Plain<P>>,
    placeholder: paragraph::Plain<P>,
}

impl<P: text::Paragraph> State<P> {
        fn new() -> Self {
        Self {
            menu: menu::State::default(),
            keyboard_modifiers: keyboard::Modifiers::default(),
            is_open: bool::default(),
            hovered_option: Option::default(),
            options: Vec::new(),
            placeholder: paragraph::Plain::default(),
        }
    }
}

impl<P: text::Paragraph> Default for State<P> {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Handle<Font> {
                Arrow {
                size: Option<Pixels>,
    },
        Static(Icon<Font>),
        Dynamic {
                closed: Icon<Font>,
                open: Icon<Font>,
    },
        None,
}

impl<Font> Default for Handle<Font> {
    fn default() -> Self {
        Self::Arrow { size: None }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct Icon<Font> {
        pub font: Font,
        pub code_point: char,
        pub size: Option<Pixels>,
        pub line_height: text::LineHeight,
        pub shaping: text::Shaping,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
        Active,
        Hovered,
        Opened {
                is_hovered: bool,
    },
        Disabled,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub text_color: Color,
        pub placeholder_color: Color,
        pub handle_color: Color,
        pub background: Background,
        pub border: Border,
}

pub trait Catalog: menu::Catalog {
        type Class<'a>;

        fn default<'a>() -> <Self as Catalog>::Class<'a>;

        fn default_menu<'a>() -> <Self as menu::Catalog>::Class<'a> {
        <Self as menu::Catalog>::default()
    }

        fn style(&self, class: &<Self as Catalog>::Class<'_>, status: Status) -> Style;
}

pub type StyleFn<'a, Theme> = Box<dyn Fn(&Theme, Status) -> Style + 'a>;

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> StyleFn<'a, Self> {
        Box::new(default)
    }

    fn style(&self, class: &StyleFn<'_, Self>, status: Status) -> Style {
        class(self, status)
    }
}

pub fn default(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    let active = Style {
        text_color: palette.background.weak.text,
        background: palette.background.weak.color.into(),
        placeholder_color: palette.secondary.base.color,
        handle_color: palette.background.weak.text,
        border: Border {
            radius: 2.0.into(),
            width: 1.0,
            color: palette.background.strong.color,
        },
    };

    match status {
        Status::Active => active,
        Status::Hovered | Status::Opened { .. } => Style {
            border: Border {
                color: palette.primary.strong.color,
                ..active.border
            },
            ..active
        },
        Status::Disabled => Style {
            text_color: palette.background.strongest.color,
            background: palette.background.weaker.color.into(),
            placeholder_color: palette.background.strongest.color,
            handle_color: palette.background.strongest.color,
            border: Border {
                color: palette.background.weak.color,
                ..active.border
            },
        },
    }
}
