use crate::core::alignment;
use crate::core::layout;
use crate::core::mouse;
use crate::core::overlay;
use crate::core::renderer;
use crate::core::text;
use crate::core::theme::palette;
use crate::core::touch;
use crate::core::widget;
use crate::core::widget::tree::{self, Tree};
use crate::core::window;
use crate::core::{
    Background, Border, Color, Element, Event, Layout, Length, Pixels, Rectangle, Shell, Size,
    Theme, Vector, Widget,
};

pub struct Checkbox<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer>
where
    Renderer: text::Renderer,
    Theme: Catalog,
{
    is_checked: bool,
    on_toggle: Option<Box<dyn Fn(bool) -> Message + 'a>>,
    label: Option<text::Fragment<'a>>,
    width: Length,
    size: f32,
    spacing: f32,
    text_size: Option<Pixels>,
    line_height: text::LineHeight,
    shaping: text::Shaping,
    wrapping: text::Wrapping,
    font: Option<Renderer::Font>,
    icon: Icon<Renderer::Font>,
    class: Theme::Class<'a>,
    last_status: Option<Status>,
}

struct State<P: text::Paragraph> {
    label: widget::text::State<P>,
    pressed: bool,
}

impl<P: text::Paragraph> Default for State<P> {
    fn default() -> Self {
        Self {
            label: widget::text::State::default(),
            pressed: false,
        }
    }
}

impl<'a, Message, Theme, Renderer> Checkbox<'a, Message, Theme, Renderer>
where
    Renderer: text::Renderer,
    Theme: Catalog,
{
        const DEFAULT_SIZE: f32 = 16.0;

                    pub fn new(is_checked: bool) -> Self {
        Checkbox {
            is_checked,
            on_toggle: None,
            label: None,
            width: Length::Shrink,
            size: Self::DEFAULT_SIZE,
            spacing: Self::DEFAULT_SIZE / 2.0,
            text_size: None,
            line_height: text::LineHeight::default(),
            shaping: text::Shaping::default(),
            wrapping: text::Wrapping::default(),
            font: None,
            icon: Icon {
                font: Renderer::ICON_FONT,
                code_point: Renderer::CHECKMARK_ICON,
                size: None,
                line_height: text::LineHeight::default(),
                shaping: text::Shaping::Basic,
            },
            class: Theme::default(),
            last_status: None,
        }
    }

        pub fn label(mut self, label: impl text::IntoFragment<'a>) -> Self {
        self.label = Some(label.into_fragment());
        self
    }

                        pub fn on_toggle<F>(mut self, f: F) -> Self
    where
        F: 'a + Fn(bool) -> Message,
    {
        self.on_toggle = Some(Box::new(f));
        self
    }

                    pub fn on_toggle_maybe<F>(mut self, f: Option<F>) -> Self
    where
        F: Fn(bool) -> Message + 'a,
    {
        self.on_toggle = f.map(|f| Box::new(f) as _);
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

        pub fn icon(mut self, icon: Icon<Renderer::Font>) -> Self {
        self.icon = icon;
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
    for Checkbox<'_, Message, Theme, Renderer>
where
    Renderer: text::Renderer,
    Theme: Catalog,
{
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<State<Renderer::Paragraph>>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(State::<Renderer::Paragraph>::default())
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
            if self.label.is_some() {
                self.spacing
            } else {
                0.0
            },
            |_| layout::Node::new(Size::new(self.size, self.size)),
            |limits| {
                if let Some(label) = self.label.as_deref() {
                    let state = tree.state.downcast_mut::<State<Renderer::Paragraph>>();

                    widget::text::layout(
                        &mut state.label,
                        renderer,
                        limits,
                        label,
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
        tree: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        _viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_mut::<State<Renderer::Paragraph>>();
        if self.label.is_some() {
            let label_bounds = layout
                .children()
                .nth(1)
                .expect("checkbox label layout exists")
                .bounds();
            widget::text::update_state(&mut state.label, event, label_bounds, cursor, shell);
        }

        if shell.is_event_captured() {
            if matches!(
                event,
                Event::Mouse(
                    mouse::Event::CursorMoved { .. }
                        | mouse::Event::ButtonReleased(mouse::Button::Left)
                )
            ) {
                state.pressed = false;
            }
            return;
        }

        if matches!(event, Event::Window(window::Event::Unfocused)) {
            state.pressed = false;
        }

        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) => {
                if cursor.is_over(layout.bounds()) && self.on_toggle.is_some() {
                    state.pressed = true;
                    shell.capture_event();
                }
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) => {
                let pressed = std::mem::take(&mut state.pressed);
                if pressed {
                    if cursor.is_over(layout.bounds())
                        && let Some(on_toggle) = &self.on_toggle
                    {
                        shell.publish((on_toggle)(!self.is_checked));
                    }
                    shell.capture_event();
                }
            }
            Event::Touch(touch::Event::FingerPressed { .. }) => {
                if cursor.is_over(layout.bounds())
                    && let Some(on_toggle) = &self.on_toggle
                {
                    shell.publish((on_toggle)(!self.is_checked));
                    shell.capture_event();
                }
            }
            _ => {}
        }

        let current_status = {
            let is_mouse_over = cursor.is_over(layout.bounds());
            let is_disabled = self.on_toggle.is_none();
            let is_checked = self.is_checked;

            if is_disabled {
                Status::Disabled { is_checked }
            } else if is_mouse_over {
                Status::Hovered { is_checked }
            } else {
                Status::Active { is_checked }
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
        if self.label.is_some()
            && let Some(label) = layout.children().nth(1)
            && cursor.is_over(label.bounds())
        {
            return widget::text::selection_mouse_interaction(label.bounds(), cursor);
        }
        if cursor.is_over(layout.bounds()) && self.on_toggle.is_some() {
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
            self.last_status.unwrap_or(Status::Disabled {
                is_checked: self.is_checked,
            }),
        );

        {
            let layout = children.next().unwrap();
            let bounds = layout.bounds();

            renderer.fill_quad(
                renderer::Quad {
                    bounds,
                    border: style.border,
                    ..renderer::Quad::default()
                },
                style.background,
            );

            let Icon {
                font,
                code_point,
                size,
                line_height,
                shaping,
            } = &self.icon;
            let size = size.unwrap_or(Pixels(bounds.height * 0.7));

            if self.is_checked {
                renderer.fill_text(
                    text::Text {
                        content: code_point.to_string(),
                        font: *font,
                        size,
                        line_height: *line_height,
                        bounds: bounds.size(),
                        align_x: text::Alignment::Center,
                        align_y: alignment::Vertical::Center,
                        shaping: *shaping,
                        wrapping: text::Wrapping::default(),
                        ellipsis: text::Ellipsis::default(),
                        hint_factor: None,
                    },
                    bounds.center(),
                    style.icon_color,
                    *viewport,
                );
            }
        }

        if self.label.is_none() {
            return;
        }

        {
            let label_layout = children.next().unwrap();
            let state: &State<Renderer::Paragraph> = tree.state.downcast_ref();

            widget::text::draw_selection(&state.label, renderer, label_layout.bounds(), viewport);

            crate::text::draw(
                renderer,
                defaults,
                label_layout.bounds(),
                state.label.raw(),
                crate::text::Style {
                    color: style.text_color,
                },
                viewport,
            );
        }
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        _layout: Layout<'b>,
        renderer: &Renderer,
        _viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        let state = tree.state.downcast_mut::<State<Renderer::Paragraph>>();
        widget::text::context_menu_overlay(&mut state.label, renderer, translation)
    }

    fn operate(
        &mut self,
        _tree: &mut Tree,
        layout: Layout<'_>,
        _renderer: &Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        if let Some(label) = self.label.as_deref() {
            operation.text(None, layout.bounds(), label);
        }
    }
}

impl<'a, Message, Theme, Renderer> From<Checkbox<'a, Message, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: 'a + Catalog,
    Renderer: 'a + text::Renderer,
{
    fn from(
        checkbox: Checkbox<'a, Message, Theme, Renderer>,
    ) -> Element<'a, Message, Theme, Renderer> {
        Element::new(checkbox)
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
        Active {
                is_checked: bool,
    },
        Hovered {
                is_checked: bool,
    },
        Disabled {
                is_checked: bool,
    },
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub background: Background,
        pub icon_color: Color,
        pub border: Border,
        pub text_color: Option<Color>,
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
        Box::new(primary)
    }

    fn style(&self, class: &Self::Class<'_>, status: Status) -> Style {
        class(self, status)
    }
}

pub fn primary(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    match status {
        Status::Active { is_checked } => styled(
            palette.background.strong.color,
            palette.background.base,
            palette.primary.base.text,
            palette.primary.base,
            is_checked,
        ),
        Status::Hovered { is_checked } => styled(
            palette.background.strong.color,
            palette.background.weak,
            palette.primary.base.text,
            palette.primary.strong,
            is_checked,
        ),
        Status::Disabled { is_checked } => styled(
            palette.background.weak.color,
            palette.background.weaker,
            palette.primary.base.text,
            palette.background.strong,
            is_checked,
        ),
    }
}

pub fn secondary(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    match status {
        Status::Active { is_checked } => styled(
            palette.background.strong.color,
            palette.background.base,
            palette.background.base.text,
            palette.background.strong,
            is_checked,
        ),
        Status::Hovered { is_checked } => styled(
            palette.background.strong.color,
            palette.background.weak,
            palette.background.base.text,
            palette.background.strong,
            is_checked,
        ),
        Status::Disabled { is_checked } => styled(
            palette.background.weak.color,
            palette.background.weak,
            palette.background.base.text,
            palette.background.weak,
            is_checked,
        ),
    }
}

pub fn success(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    match status {
        Status::Active { is_checked } => styled(
            palette.background.weak.color,
            palette.background.base,
            palette.success.base.text,
            palette.success.base,
            is_checked,
        ),
        Status::Hovered { is_checked } => styled(
            palette.background.strong.color,
            palette.background.weak,
            palette.success.base.text,
            palette.success.strong,
            is_checked,
        ),
        Status::Disabled { is_checked } => styled(
            palette.background.weak.color,
            palette.background.weak,
            palette.success.base.text,
            palette.success.weak,
            is_checked,
        ),
    }
}

pub fn danger(theme: &Theme, status: Status) -> Style {
    let palette = theme.palette();

    match status {
        Status::Active { is_checked } => styled(
            palette.background.strong.color,
            palette.background.base,
            palette.danger.base.text,
            palette.danger.base,
            is_checked,
        ),
        Status::Hovered { is_checked } => styled(
            palette.background.strong.color,
            palette.background.weak,
            palette.danger.base.text,
            palette.danger.strong,
            is_checked,
        ),
        Status::Disabled { is_checked } => styled(
            palette.background.weak.color,
            palette.background.weak,
            palette.danger.base.text,
            palette.danger.weak,
            is_checked,
        ),
    }
}

fn styled(
    border_color: Color,
    base: palette::Pair,
    icon_color: Color,
    accent: palette::Pair,
    is_checked: bool,
) -> Style {
    let (background, border) = if is_checked {
        (accent, accent.color)
    } else {
        (base, border_color)
    };

    Style {
        background: Background::Color(background.color),
        icon_color,
        border: Border {
            radius: 2.0.into(),
            width: 1.0,
            color: border,
        },
        text_color: None,
    }
}
