use super::State as TextState;
use crate::clipboard;
use crate::keyboard;
use crate::layout;
use crate::mouse;
use crate::overlay;
use crate::renderer;
use crate::text;
use crate::text::paragraph::{self, Paragraph};
use crate::window;
use crate::{Border, Color, Event, Layout, Pixels, Point, Rectangle, Shell, Size, Vector};

const MENU_WIDTH: f32 = 124.0;
const ROW_HEIGHT: f32 = 28.0;
const MENU_HEIGHT: f32 = ROW_HEIGHT * 2.0;
const HORIZONTAL_PADDING: f32 = 9.0;
const LABELS: &str = "Copy\nSelect All";

pub(super) struct State<P: Paragraph> {
    open: bool,
    position: Point,
    bounds: Rectangle,
    hovered: Option<Action>,
    paragraph: Option<paragraph::Plain<P>>,
}

impl<P: Paragraph> Default for State<P> {
    fn default() -> Self {
        Self {
            open: false,
            position: Point::ORIGIN,
            bounds: Rectangle::with_size(Size::ZERO),
            hovered: None,
            paragraph: None,
        }
    }
}

impl<P: Paragraph> State<P> {
    pub(super) fn is_open(&self) -> bool {
        self.open
    }

    pub(super) fn open(&mut self, position: Point) {
        self.open = true;
        self.position = position;
        self.hovered = None;
    }

    pub(super) fn close(&mut self) {
        self.open = false;
        self.hovered = None;
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Action {
    Copy,
    SelectAll,
}

pub(super) fn overlay<'a, Message, Theme, Renderer>(
    state: &'a mut TextState<Renderer::Paragraph>,
    renderer: &Renderer,
    translation: Vector,
) -> overlay::Element<'a, Message, Theme, Renderer>
where
    Renderer: text::Renderer + 'a,
{
    let font = renderer.default_font();
    let size = renderer.default_size();
    overlay::Element::new(Box::new(MenuOverlay {
        state,
        font,
        size,
        translation,
    }))
}

struct MenuOverlay<'a, Renderer>
where
    Renderer: text::Renderer,
{
    state: &'a mut TextState<Renderer::Paragraph>,
    font: Renderer::Font,
    size: Pixels,
    translation: Vector,
}

impl<Message, Theme, Renderer> overlay::Overlay<Message, Theme, Renderer>
    for MenuOverlay<'_, Renderer>
where
    Renderer: text::Renderer,
{
    fn layout(&mut self, renderer: &Renderer, viewport: Size) -> layout::Node {
        let position = self.state.context_menu.position + self.translation;
        let x = position
            .x
            .min((viewport.width - MENU_WIDTH).max(0.0))
            .max(0.0);
        let y = position
            .y
            .min((viewport.height - MENU_HEIGHT).max(0.0))
            .max(0.0);
        self.state.context_menu.bounds = Rectangle {
            x,
            y,
            width: MENU_WIDTH,
            height: MENU_HEIGHT,
        };

        let paragraph = self
            .state
            .context_menu
            .paragraph
            .get_or_insert_with(paragraph::Plain::default);
        let _ = paragraph.update(text::Text {
            content: LABELS,
            bounds: Size::new(MENU_WIDTH - HORIZONTAL_PADDING * 2.0, MENU_HEIGHT),
            size: self.size,
            line_height: text::LineHeight::Absolute(Pixels(ROW_HEIGHT)),
            font: self.font,
            align_x: text::Alignment::Default,
            align_y: crate::alignment::Vertical::Top,
            shaping: text::Shaping::Advanced,
            wrapping: text::Wrapping::None,
            ellipsis: text::Ellipsis::None,
            hint_factor: renderer.scale_factor(),
        });

        layout::Node::new(Size::new(MENU_WIDTH, MENU_HEIGHT)).move_to(Point::new(x, y))
    }

    fn draw(
        &self,
        renderer: &mut Renderer,
        _theme: &Theme,
        inherited_style: &renderer::Style,
        layout: Layout<'_>,
        _cursor: mouse::Cursor,
    ) {
        let bounds = layout.bounds();
        let text_color = inherited_style.text_color;
        let background = contrasting_background(text_color);
        renderer.fill_quad(
            renderer::Quad {
                bounds,
                border: Border {
                    color: Color {
                        a: 0.28,
                        ..text_color
                    },
                    width: 1.0,
                    radius: 3.0.into(),
                },
                ..renderer::Quad::default()
            },
            background,
        );

        if let Some(action) = self.state.context_menu.hovered {
            renderer.fill_quad(
                renderer::Quad {
                    bounds: row_bounds(bounds, action),
                    ..renderer::Quad::default()
                },
                Color::from_rgba(0.35, 0.55, 0.95, 0.28),
            );
        }

        if let Some(paragraph) = &self.state.context_menu.paragraph {
            renderer.fill_paragraph(
                paragraph.raw(),
                Point::new(bounds.x + HORIZONTAL_PADDING, bounds.y),
                text_color,
                bounds,
            );
        }
    }

    fn update(
        &mut self,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
    ) {
        let bounds = layout.bounds();
        match event {
            Event::Mouse(mouse::Event::CursorMoved { .. }) => {
                let hovered = menu_action(bounds, cursor);
                if hovered != self.state.context_menu.hovered {
                    self.state.context_menu.hovered = hovered;
                    shell.request_redraw();
                }
            }
            Event::Mouse(mouse::Event::ButtonPressed(
                mouse::Button::Left | mouse::Button::Right,
            )) => {
                if !cursor.is_over(bounds) {
                    self.state.context_menu.close();
                    shell.invalidate_layout();
                    shell.request_redraw();
                }
                shell.capture_event();
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) => {
                if let Some(action) = menu_action(bounds, cursor) {
                    self.activate(action, shell);
                } else {
                    self.state.context_menu.close();
                    shell.invalidate_layout();
                    shell.request_redraw();
                }
                shell.capture_event();
            }
            Event::Keyboard(keyboard::Event::KeyPressed { key, .. })
                if *key == keyboard::Key::Named(keyboard::key::Named::Escape) =>
            {
                self.state.context_menu.close();
                shell.invalidate_layout();
                shell.request_redraw();
                shell.capture_event();
            }
            Event::Window(window::Event::Resized { .. }) => {
                self.state.context_menu.close();
                shell.invalidate_layout();
                shell.request_redraw();
                shell.capture_event();
            }
            _ => {}
        }
    }

    fn mouse_interaction(
        &self,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _renderer: &Renderer,
    ) -> mouse::Interaction {
        if cursor.is_over(layout.bounds()) {
            mouse::Interaction::Pointer
        } else {
            mouse::Interaction::default()
        }
    }
}

impl<Renderer> MenuOverlay<'_, Renderer>
where
    Renderer: text::Renderer,
{
    fn activate<Message>(&mut self, action: Action, shell: &mut Shell<'_, Message>) {
        match action {
            Action::Copy => {
                if let Some((start, end)) = self.state.selection() {
                    shell.write_clipboard_for(
                        clipboard::Content::Text(
                            self.state.paragraph.content()[start..end].to_owned(),
                        ),
                        "static.text",
                    );
                }
            }
            Action::SelectAll => {
                self.state.anchor = 0;
                self.state.extent = self.state.paragraph.content().len();
                self.state.rebuild_selection();
            }
        }
        self.state.context_menu.close();
        shell.invalidate_layout();
        shell.request_redraw();
    }
}

fn menu_action(bounds: Rectangle, cursor: mouse::Cursor) -> Option<Action> {
    let position = cursor.position()?;
    if !bounds.contains(position) {
        return None;
    }
    Some(if position.y < bounds.y + ROW_HEIGHT {
        Action::Copy
    } else {
        Action::SelectAll
    })
}

fn row_bounds(bounds: Rectangle, action: Action) -> Rectangle {
    Rectangle {
        x: bounds.x,
        y: bounds.y
            + match action {
                Action::Copy => 0.0,
                Action::SelectAll => ROW_HEIGHT,
            },
        width: bounds.width,
        height: ROW_HEIGHT,
    }
}

fn contrasting_background(text: Color) -> Color {
    let luminance = text.r * 0.2126 + text.g * 0.7152 + text.b * 0.0722;
    if luminance > 0.5 {
        Color::from_rgba(0.08, 0.09, 0.11, 0.98)
    } else {
        Color::from_rgba(0.98, 0.98, 0.99, 0.98)
    }
}
