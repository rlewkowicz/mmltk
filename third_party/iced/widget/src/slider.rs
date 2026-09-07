use crate::core::border::{self, Border};
use crate::core::keyboard;
use crate::core::keyboard::key::{self, Key};
use crate::core::layout;
use crate::core::mouse;
use crate::core::renderer;
use crate::core::touch;
use crate::core::widget::tree::{self, Tree};
use crate::core::window;
use crate::core::{
    self, Background, Color, Element, Event, Layout, Length, Pixels, Point, Rectangle, Shell, Size,
    Theme, Widget,
};

use std::ops::RangeInclusive;

pub struct Slider<'a, T, Message, Theme = crate::Theme>
where
    Theme: Catalog,
{
    range: RangeInclusive<T>,
    step: f64,
    shift_step: Option<f64>,
    value: T,
    default: Option<T>,
    on_change: Box<dyn Fn(T) -> Message + 'a>,
    on_release: Option<Message>,
    width: Length,
    height: f32,
    class: Theme::Class<'a>,
    status: Option<Status>,
}

impl<'a, T, Message, Theme> Slider<'a, T, Message, Theme>
where
    T: Copy + PartialOrd,
    Message: Clone,
    Theme: Catalog,
{
        pub const DEFAULT_HEIGHT: f32 = 16.0;

                                    pub fn new<F>(range: RangeInclusive<T>, value: T, on_change: F) -> Self
    where
        F: 'a + Fn(T) -> Message,
    {
        let value = if value >= *range.start() {
            value
        } else {
            *range.start()
        };

        let value = if value <= *range.end() {
            value
        } else {
            *range.end()
        };

        Slider {
            value,
            default: None,
            range,
            step: 1.0,
            shift_step: None,
            on_change: Box::new(on_change),
            on_release: None,
            width: Length::Fill,
            height: Self::DEFAULT_HEIGHT,
            class: Theme::default(),
            status: None,
        }
    }

                pub fn default(mut self, default: impl Into<T>) -> Self {
        self.default = Some(default.into());
        self
    }

                            pub fn on_release(mut self, on_release: Message) -> Self {
        self.on_release = Some(on_release);
        self
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.width = width.into();
        self
    }

        pub fn height(mut self, height: impl Into<Pixels>) -> Self {
        self.height = height.into().0;
        self
    }

        pub fn step(mut self, step: impl num_traits::AsPrimitive<f64>) -> Self {
        self.step = step.as_();
        self
    }

                pub fn shift_step(mut self, shift_step: impl num_traits::AsPrimitive<f64>) -> Self {
        self.shift_step = Some(shift_step.as_());
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

impl<T, Message, Theme, Renderer> Widget<Message, Theme, Renderer> for Slider<'_, T, Message, Theme>
where
    T: Copy + num_traits::AsPrimitive<f64> + num_traits::FromPrimitive,
    Message: Clone,
    Theme: Catalog,
    Renderer: core::Renderer,
{
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<State>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(State::default())
    }

    fn size(&self) -> Size<Length> {
        Size {
            width: self.width,
            height: Length::Shrink,
        }
    }

    fn layout(
        &mut self,
        _tree: &mut Tree,
        _renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        layout::atomic(limits, self.width, self.height)
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
        let state = tree.state.downcast_mut::<State>();

        let mut update = || {
            let current_value = self.value;

            let locate = |cursor_position: Point| -> Option<T> {
                let bounds = layout.bounds();

                if cursor_position.x <= bounds.x {
                    Some(*self.range.start())
                } else if cursor_position.x >= bounds.x + bounds.width {
                    Some(*self.range.end())
                } else {
                    let step = if state.keyboard_modifiers.shift() {
                        self.shift_step.unwrap_or(self.step)
                    } else {
                        self.step
                    };

                    let start = (*self.range.start()).as_();
                    let end = (*self.range.end()).as_();

                    let percent = f64::from(cursor_position.x - bounds.x) / f64::from(bounds.width);

                    let steps = (percent * (end - start) / step).round();
                    let value = steps * step + start;

                    T::from_f64(value.min(end))
                }
            };

            let increment = |value: T| -> Option<T> {
                let step = if state.keyboard_modifiers.shift() {
                    self.shift_step.unwrap_or(self.step)
                } else {
                    self.step
                };

                let steps = (value.as_() / step).round();
                let new_value = step * (steps + 1.0);

                if new_value > (*self.range.end()).as_() {
                    return Some(*self.range.end());
                }

                T::from_f64(new_value)
            };

            let decrement = |value: T| -> Option<T> {
                let step = if state.keyboard_modifiers.shift() {
                    self.shift_step.unwrap_or(self.step)
                } else {
                    self.step
                };

                let steps = (value.as_() / step).round();
                let new_value = step * (steps - 1.0);

                if new_value < (*self.range.start()).as_() {
                    return Some(*self.range.start());
                }

                T::from_f64(new_value)
            };

            let change = |new_value: T| {
                if (self.value.as_() - new_value.as_()).abs() > f64::EPSILON {
                    shell.publish((self.on_change)(new_value));

                    self.value = new_value;
                }
            };

            match &event {
                Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
                | Event::Touch(touch::Event::FingerPressed { .. }) => {
                    if let Some(cursor_position) = cursor.position_over(layout.bounds()) {
                        if state.keyboard_modifiers.command() {
                            let _ = self.default.map(change);
                            state.is_dragging = false;
                        } else {
                            let _ = locate(cursor_position).map(change);
                            state.is_dragging = true;
                        }

                        shell.capture_event();
                    }
                }
                Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
                | Event::Touch(touch::Event::FingerLifted { .. })
                | Event::Touch(touch::Event::FingerLost { .. })
                    if state.is_dragging =>
                {
                    if let Some(on_release) = self.on_release.clone() {
                        shell.publish(on_release);
                    }
                    state.is_dragging = false;
                }
                Event::Mouse(mouse::Event::CursorMoved { .. })
                | Event::Touch(touch::Event::FingerMoved { .. })
                    if state.is_dragging =>
                {
                    let _ = cursor.land().position().and_then(locate).map(change);

                    shell.capture_event();
                }
                Event::Mouse(mouse::Event::WheelScrolled { delta })
                    if state.keyboard_modifiers.control() && cursor.is_over(layout.bounds()) =>
                {
                    let delta = match delta {
                        mouse::ScrollDelta::Lines { x: _, y } => y,
                        mouse::ScrollDelta::Pixels { x: _, y } => y,
                    };

                    if *delta < 0.0 {
                        let _ = decrement(current_value).map(change);
                    } else {
                        let _ = increment(current_value).map(change);
                    }

                    shell.capture_event();
                }
                Event::Keyboard(keyboard::Event::KeyPressed { key, .. })
                    if cursor.is_over(layout.bounds()) =>
                {
                    match key {
                        Key::Named(key::Named::ArrowUp) => {
                            let _ = increment(current_value).map(change);
                            shell.capture_event();
                        }
                        Key::Named(key::Named::ArrowDown) => {
                            let _ = decrement(current_value).map(change);
                            shell.capture_event();
                        }
                        _ => (),
                    }
                }
                Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers)) => {
                    state.keyboard_modifiers = *modifiers;
                }
                _ => {}
            }
        };

        update();

        let current_status = if state.is_dragging {
            Status::Dragged
        } else if cursor.is_over(layout.bounds()) {
            Status::Hovered
        } else {
            Status::Active
        };

        if let Event::Window(window::Event::RedrawRequested(_now)) = event {
            self.status = Some(current_status);
        } else if self.status.is_some_and(|status| status != current_status) {
            shell.request_redraw();
        }
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

        let style = theme.style(&self.class, self.status.unwrap_or(Status::Active));

        let (handle_width, handle_height, handle_border_radius) = match style.handle.shape {
            HandleShape::Circle { radius } => (radius * 2.0, radius * 2.0, radius.into()),
            HandleShape::Rectangle {
                width,
                border_radius,
            } => (f32::from(width), bounds.height, border_radius),
        };

        let value = self.value.as_() as f32;
        let (range_start, range_end) = {
            let (start, end) = self.range.clone().into_inner();

            (start.as_() as f32, end.as_() as f32)
        };

        let offset = if range_start >= range_end {
            0.0
        } else {
            (bounds.width - handle_width) * (value - range_start) / (range_end - range_start)
        };

        let rail_y = bounds.y + bounds.height / 2.0;

        renderer.fill_quad(
            renderer::Quad {
                bounds: Rectangle {
                    x: bounds.x,
                    y: rail_y - style.rail.width / 2.0,
                    width: offset + handle_width / 2.0,
                    height: style.rail.width,
                },
                border: style.rail.border,
                ..renderer::Quad::default()
            },
            style.rail.backgrounds.0,
        );

        renderer.fill_quad(
            renderer::Quad {
                bounds: Rectangle {
                    x: bounds.x + offset + handle_width / 2.0,
                    y: rail_y - style.rail.width / 2.0,
                    width: bounds.width - offset - handle_width / 2.0,
                    height: style.rail.width,
                },
                border: style.rail.border,
                ..renderer::Quad::default()
            },
            style.rail.backgrounds.1,
        );

        renderer.fill_quad(
            renderer::Quad {
                bounds: Rectangle {
                    x: bounds.x + offset,
                    y: rail_y - handle_height / 2.0,
                    width: handle_width,
                    height: handle_height,
                },
                border: Border {
                    radius: handle_border_radius,
                    width: style.handle.border_width,
                    color: style.handle.border_color,
                },
                ..renderer::Quad::default()
            },
            style.handle.background,
        );
    }

    fn mouse_interaction(
        &self,
        tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _viewport: &Rectangle,
        _renderer: &Renderer,
    ) -> mouse::Interaction {
        let state = tree.state.downcast_ref::<State>();

        if state.is_dragging {
            if cfg!(target_os = "windows") {
                mouse::Interaction::Pointer
            } else {
                mouse::Interaction::Grabbing
            }
        } else if cursor.is_over(layout.bounds()) {
            if cfg!(target_os = "windows") {
                mouse::Interaction::Pointer
            } else {
                mouse::Interaction::Grab
            }
        } else {
            mouse::Interaction::default()
        }
    }
}

impl<'a, T, Message, Theme, Renderer> From<Slider<'a, T, Message, Theme>>
    for Element<'a, Message, Theme, Renderer>
where
    T: Copy + num_traits::AsPrimitive<f64> + num_traits::FromPrimitive + 'a,
    Message: Clone + 'a,
    Theme: Catalog + 'a,
    Renderer: core::Renderer + 'a,
{
    fn from(slider: Slider<'a, T, Message, Theme>) -> Element<'a, Message, Theme, Renderer> {
        Element::new(slider)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
struct State {
    is_dragging: bool,
    keyboard_modifiers: keyboard::Modifiers,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
        Active,
        Hovered,
        Dragged,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub rail: Rail,
        pub handle: Handle,
}

impl Style {
            pub fn with_circular_handle(mut self, radius: impl Into<Pixels>) -> Self {
        self.handle.shape = HandleShape::Circle {
            radius: radius.into().0,
        };
        self
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Rail {
        pub backgrounds: (Background, Background),
        pub width: f32,
        pub border: Border,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Handle {
        pub shape: HandleShape,
        pub background: Background,
        pub border_width: f32,
        pub border_color: Color,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum HandleShape {
        Circle {
                radius: f32,
    },
        Rectangle {
                width: u16,
                border_radius: border::Radius,
    },
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

    let color = match status {
        Status::Active => palette.primary.base.color,
        Status::Hovered => palette.primary.strong.color,
        Status::Dragged => palette.primary.weak.color,
    };

    Style {
        rail: Rail {
            backgrounds: (color.into(), palette.background.strong.color.into()),
            width: 4.0,
            border: Border {
                radius: 2.0.into(),
                width: 0.0,
                color: Color::TRANSPARENT,
            },
        },
        handle: Handle {
            shape: HandleShape::Circle { radius: 7.0 },
            background: color.into(),
            border_color: Color::TRANSPARENT,
            border_width: 0.0,
        },
    }
}
