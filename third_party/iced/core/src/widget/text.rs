mod context_menu;

use crate::alignment;
use crate::clipboard;
use crate::keyboard;
use crate::keyboard::key;
use crate::layout;
use crate::mouse;
use crate::overlay;
use crate::renderer;
use crate::text;
use crate::text::paragraph::{self, Paragraph};
use crate::widget::tree::{self, Tree};
use crate::window;
use crate::{
    Color, Element, Event, Layout, Length, Pixels, Point, Rectangle, Shell, Size, Theme, Vector,
    Widget,
};

pub use text::{Alignment, Ellipsis, LineHeight, Shaping, Wrapping};

#[must_use]
pub struct Text<'a, Theme, Renderer>
where
    Theme: Catalog,
    Renderer: text::Renderer,
{
    fragment: text::Fragment<'a>,
    format: Format<Renderer::Font>,
    class: Theme::Class<'a>,
}

impl<'a, Theme, Renderer> Text<'a, Theme, Renderer>
where
    Theme: Catalog,
    Renderer: text::Renderer,
{
        pub fn new(fragment: impl text::IntoFragment<'a>) -> Self {
        Text {
            fragment: fragment.into_fragment(),
            format: Format::default(),
            class: Theme::default(),
        }
    }

        pub fn size(mut self, size: impl Into<Pixels>) -> Self {
        self.format.size = Some(size.into());
        self
    }

        pub fn line_height(mut self, line_height: impl Into<LineHeight>) -> Self {
        self.format.line_height = line_height.into();
        self
    }

                pub fn font(mut self, font: impl Into<Renderer::Font>) -> Self {
        self.format.font = Some(font.into());
        self
    }

                pub fn font_maybe(mut self, font: Option<impl Into<Renderer::Font>>) -> Self {
        self.format.font = font.map(Into::into);
        self
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.format.width = width.into();
        self
    }

        pub fn height(mut self, height: impl Into<Length>) -> Self {
        self.format.height = height.into();
        self
    }

        pub fn center(self) -> Self {
        self.align_x(alignment::Horizontal::Center)
            .align_y(alignment::Vertical::Center)
    }

        pub fn align_x(mut self, alignment: impl Into<text::Alignment>) -> Self {
        self.format.align_x = alignment.into();
        self
    }

        pub fn align_y(mut self, alignment: impl Into<alignment::Vertical>) -> Self {
        self.format.align_y = alignment.into();
        self
    }

        pub fn shaping(mut self, shaping: Shaping) -> Self {
        self.format.shaping = shaping;
        self
    }

        pub fn wrapping(mut self, wrapping: Wrapping) -> Self {
        self.format.wrapping = wrapping;
        self
    }

        pub fn ellipsis(mut self, ellipsis: Ellipsis) -> Self {
        self.format.ellipsis = ellipsis;
        self
    }

        pub fn style(mut self, style: impl Fn(&Theme) -> Style + 'a) -> Self
    where
        Theme::Class<'a>: From<StyleFn<'a, Theme>>,
    {
        self.class = (Box::new(style) as StyleFn<'a, Theme>).into();
        self
    }

        pub fn color(self, color: impl Into<Color>) -> Self
    where
        Theme::Class<'a>: From<StyleFn<'a, Theme>>,
    {
        self.color_maybe(Some(color))
    }

        pub fn color_maybe(self, color: Option<impl Into<Color>>) -> Self
    where
        Theme::Class<'a>: From<StyleFn<'a, Theme>>,
    {
        let color = color.map(Into::into);

        self.style(move |_theme| Style { color })
    }

        #[cfg(feature = "advanced")]
    pub fn class(mut self, class: impl Into<Theme::Class<'a>>) -> Self {
        self.class = class.into();
        self
    }
}

pub struct State<P: Paragraph> {
    paragraph: paragraph::Plain<P>,
    selection_bounds: Vec<Rectangle>,
    line_starts: Vec<usize>,
    anchor: usize,
    extent: usize,
    focused: bool,
    dragging: Option<SelectionDrag>,
    last_click: Option<mouse::Click>,
    context_menu: context_menu::State<P>,
}

#[derive(Debug, Clone, Copy)]
enum SelectionDrag {
    Character { anchor: usize },
    Word { start: usize, end: usize },
    Line { start: usize, end: usize },
}

impl<P: Paragraph> Default for State<P> {
    fn default() -> Self {
        Self {
            paragraph: paragraph::Plain::default(),
            selection_bounds: Vec::new(),
            line_starts: Vec::new(),
            anchor: 0,
            extent: 0,
            focused: false,
            dragging: None,
            last_click: None,
            context_menu: context_menu::State::default(),
        }
    }
}

impl<P: Paragraph> State<P> {
        pub fn raw(&self) -> &P {
        self.paragraph.raw()
    }

    fn selection(&self) -> Option<(usize, usize)> {
        let (start, end) = if self.anchor <= self.extent {
            (self.anchor, self.extent)
        } else {
            (self.extent, self.anchor)
        };
        (start < end).then_some((start, end))
    }

    fn clear_selection(&mut self) {
        self.anchor = 0;
        self.extent = 0;
        self.focused = false;
        self.dragging = None;
        self.last_click = None;
        self.context_menu.close();
        self.selection_bounds.clear();
    }

    fn rebuild_selection(&mut self) {
        let Some((start, end)) = self.selection() else {
            self.selection_bounds.clear();
            return;
        };

        if end > self.paragraph.content().len()
            || !self.paragraph.content().is_char_boundary(start)
            || !self.paragraph.content().is_char_boundary(end)
        {
            self.clear_selection();
            return;
        }

        let (start_line, start_index) = self.selection_position(start);
        let (end_line, end_index) = self.selection_position(end);
        self.paragraph.raw().selection_bounds(
            start_line,
            start_index,
            end_line,
            end_index,
            &mut self.selection_bounds,
        );
    }

    fn rebuild_line_starts(&mut self) {
        self.line_starts.clear();
        self.line_starts.push(0);
        self.line_starts.extend(
            self.paragraph
                .content()
                .bytes()
                .enumerate()
                .filter_map(|(index, byte)| (byte == b'\n').then_some(index + 1)),
        );
    }

    fn selection_position(&self, byte_index: usize) -> (usize, usize) {
        let line = self
            .line_starts
            .partition_point(|line_start| *line_start <= byte_index)
            .saturating_sub(1);
        let line_start = self.line_starts.get(line).copied().unwrap_or_default();
        (line, byte_index.saturating_sub(line_start))
    }
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer> for Text<'_, Theme, Renderer>
where
    Theme: Catalog,
    Renderer: text::Renderer,
{
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<State<Renderer::Paragraph>>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(State::<Renderer::Paragraph>::default())
    }

    fn size(&self) -> Size<Length> {
        Size {
            width: self.format.width,
            height: self.format.height,
        }
    }

    fn layout(
        &mut self,
        tree: &mut Tree,
        renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        layout(
            tree.state.downcast_mut::<State<Renderer::Paragraph>>(),
            renderer,
            limits,
            &self.fragment,
            self.format,
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
        update_state(
            tree.state.downcast_mut::<State<Renderer::Paragraph>>(),
            event,
            layout.bounds(),
            cursor,
            shell,
        );
    }

    fn draw(
        &self,
        tree: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        defaults: &renderer::Style,
        layout: Layout<'_>,
        _cursor_position: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_ref::<State<Renderer::Paragraph>>();
        let style = theme.style(&self.class);

        draw_selection(state, renderer, layout.bounds(), viewport);

        draw(
            renderer,
            defaults,
            layout.bounds(),
            state.raw(),
            style,
            viewport,
        );
    }

    fn mouse_interaction(
        &self,
        _tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _viewport: &Rectangle,
        _renderer: &Renderer,
    ) -> mouse::Interaction {
        selection_mouse_interaction(layout.bounds(), cursor)
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        _layout: Layout<'b>,
        renderer: &Renderer,
        _viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        context_menu_overlay(
            tree.state.downcast_mut::<State<Renderer::Paragraph>>(),
            renderer,
            translation,
        )
    }

    fn operate(
        &mut self,
        _tree: &mut Tree,
        layout: Layout<'_>,
        _renderer: &Renderer,
        operation: &mut dyn super::Operation,
    ) {
        operation.text(None, layout.bounds(), &self.fragment);
    }
}

pub fn update_state<P: Paragraph, Message>(
    state: &mut State<P>,
    event: &Event,
    bounds: Rectangle,
    cursor: mouse::Cursor,
    shell: &mut Shell<'_, Message>,
) {
    match event {
        Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) => {
            if cursor.is_over(bounds) {
                state.context_menu.close();
                if let Some(index) = hit_index(state, bounds, cursor, false)
                    && let Some(position) = cursor.position()
                {
                    let click = mouse::Click::new(position, mouse::Button::Left, state.last_click);
                    state.last_click = Some(click);
                    match click.kind() {
                        mouse::click::Kind::Single => {
                            state.anchor = index;
                            state.extent = index;
                            state.dragging = Some(SelectionDrag::Character { anchor: index });
                        }
                        mouse::click::Kind::Double => {
                            let (start, end) = word_range(state.paragraph.content(), index);
                            state.anchor = start;
                            state.extent = end;
                            state.dragging = Some(SelectionDrag::Word { start, end });
                        }
                        mouse::click::Kind::Triple => {
                            let (start, end) = line_range(state.paragraph.content(), index);
                            state.anchor = start;
                            state.extent = end;
                            state.dragging = Some(SelectionDrag::Line { start, end });
                        }
                    }
                    state.focused = true;
                    state.rebuild_selection();
                    shell.request_redraw();
                }
            } else if state.focused {
                state.clear_selection();
                shell.request_redraw();
            }
        }
        Event::Mouse(mouse::Event::CursorMoved { .. }) if state.dragging.is_some() => {
            if let Some(index) = hit_index(state, bounds, cursor, true) {
                let (anchor, extent) = drag_selection(
                    state.paragraph.content(),
                    state.dragging.expect("drag mode exists"),
                    index,
                );
                if state.anchor == anchor && state.extent == extent {
                    return;
                }
                state.anchor = anchor;
                state.extent = extent;
                state.rebuild_selection();
                shell.request_redraw();
                if state.selection().is_some() {
                    shell.capture_event();
                }
            }
        }
        Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) => {
            let was_dragging = state.dragging.take().is_some();
            if was_dragging && state.selection().is_some() {
                shell.capture_event();
            }
        }
        Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)) => {
            if cursor.is_over(bounds)
                && let Some(index) = hit_index(state, bounds, cursor, false)
                && let Some(position) = cursor.position()
            {
                let inside_selection = state
                    .selection()
                    .is_some_and(|(start, end)| index >= start && index < end);
                if !inside_selection {
                    let (start, end) = word_range(state.paragraph.content(), index);
                    state.anchor = start;
                    state.extent = end;
                    state.rebuild_selection();
                }
                state.focused = true;
                state.dragging = None;
                state.context_menu.open(position);
                shell.capture_event();
                shell.invalidate_layout();
                shell.request_redraw();
            } else if state.focused {
                state.clear_selection();
                shell.request_redraw();
            }
        }
        Event::Keyboard(keyboard::Event::KeyPressed {
            key,
            physical_key,
            modifiers,
            ..
        }) if state.focused => match key.to_latin(*physical_key) {
            Some('c') if modifiers.command() => {
                if let Some((start, end)) = state.selection() {
                    shell.write_clipboard_for(
                        clipboard::Content::Text(state.paragraph.content()[start..end].to_owned()),
                        "static.text",
                    );
                    shell.capture_event();
                }
            }
            Some('a') if modifiers.command() => {
                state.anchor = 0;
                state.extent = state.paragraph.content().len();
                state.rebuild_selection();
                shell.request_redraw();
                shell.capture_event();
            }
            _ if matches!(key.as_ref(), keyboard::Key::Named(key::Named::Escape)) => {
                state.clear_selection();
                shell.request_redraw();
                shell.capture_event();
            }
            _ => {}
        },
        Event::Window(window::Event::Unfocused) => {
            state.dragging = None;
            state.context_menu.close();
        }
        _ => {}
    }
}

pub fn draw_selection<P: Paragraph, Renderer: renderer::Renderer>(
    state: &State<P>,
    renderer: &mut Renderer,
    bounds: Rectangle,
    viewport: &Rectangle,
) {
    if state.selection_bounds.is_empty() {
        return;
    }
    let anchor = bounds.anchor(
        state.paragraph.min_bounds(),
        state.paragraph.align_x(),
        state.paragraph.align_y(),
    );
    let translation = anchor - Point::ORIGIN;
    for bounds in &state.selection_bounds {
        if let Some(bounds) = (*bounds + translation).intersection(viewport) {
            renderer.fill_quad(
                renderer::Quad {
                    bounds,
                    ..renderer::Quad::default()
                },
                Color::from_rgba(0.18, 0.48, 0.95, 0.48),
            );
        }
    }
}

pub fn selection_mouse_interaction(bounds: Rectangle, cursor: mouse::Cursor) -> mouse::Interaction {
    if cursor.is_over(bounds) {
        mouse::Interaction::Text
    } else {
        mouse::Interaction::default()
    }
}

pub fn context_menu_overlay<'a, Message, Theme, Renderer>(
    state: &'a mut State<Renderer::Paragraph>,
    renderer: &Renderer,
    translation: Vector,
) -> Option<overlay::Element<'a, Message, Theme, Renderer>>
where
    Renderer: text::Renderer + 'a,
{
    if state.context_menu.is_open() {
        Some(context_menu::overlay(state, renderer, translation))
    } else {
        None
    }
}

#[derive(Debug, Clone, Copy)]
#[allow(missing_docs)]
pub struct Format<Font> {
    pub width: Length,
    pub height: Length,
    pub size: Option<Pixels>,
    pub font: Option<Font>,
    pub line_height: LineHeight,
    pub align_x: text::Alignment,
    pub align_y: alignment::Vertical,
    pub shaping: Shaping,
    pub wrapping: Wrapping,
    pub ellipsis: Ellipsis,
}

impl<Font> Default for Format<Font> {
    fn default() -> Self {
        Self {
            size: None,
            line_height: LineHeight::default(),
            font: None,
            width: Length::Shrink,
            height: Length::Shrink,
            align_x: text::Alignment::Default,
            align_y: alignment::Vertical::Top,
            shaping: Shaping::default(),
            wrapping: Wrapping::default(),
            ellipsis: Ellipsis::default(),
        }
    }
}

pub fn layout<Renderer>(
    state: &mut State<Renderer::Paragraph>,
    renderer: &Renderer,
    limits: &layout::Limits,
    content: &str,
    format: Format<Renderer::Font>,
) -> layout::Node
where
    Renderer: text::Renderer,
{
    layout::sized(limits, format.width, format.height, |limits| {
        let bounds = limits.max();

        let size = format.size.unwrap_or_else(|| renderer.default_size());
        let font = format.font.unwrap_or_else(|| renderer.default_font());

        let content_changed = state.paragraph.content() != content;
        let paragraph_changed = state.paragraph.update(text::Text {
            content,
            bounds,
            size,
            line_height: format.line_height,
            font,
            align_x: format.align_x,
            align_y: format.align_y,
            shaping: format.shaping,
            wrapping: format.wrapping,
            ellipsis: format.ellipsis,
            hint_factor: renderer.scale_factor(),
        });

        if content_changed {
            state.clear_selection();
            state.rebuild_line_starts();
        } else if paragraph_changed && state.selection().is_some() {
            state.rebuild_selection();
        }

        state.paragraph.min_bounds()
    })
}

fn drag_selection(content: &str, drag: SelectionDrag, index: usize) -> (usize, usize) {
    match drag {
        SelectionDrag::Character { anchor } => (anchor, index),
        SelectionDrag::Word { start, end } => {
            let (current_start, current_end) = word_range(content, index);
            if current_start < start {
                (end, current_start)
            } else {
                (start, current_end)
            }
        }
        SelectionDrag::Line { start, end } => {
            let (current_start, current_end) = line_range(content, index);
            if current_start < start {
                (end, current_start)
            } else {
                (start, current_end)
            }
        }
    }
}

fn word_range(content: &str, index: usize) -> (usize, usize) {
    if content.is_empty() {
        return (0, 0);
    }
    let mut index = index.min(content.len());
    while !content.is_char_boundary(index) {
        index -= 1;
    }
    let (character_start, character) = if index == content.len() {
        content
            .char_indices()
            .next_back()
            .expect("non-empty text has a final character")
    } else {
        (
            index,
            content[index..]
                .chars()
                .next()
                .expect("character boundary contains a character"),
        )
    };
    if !character.is_alphanumeric() && character != '_' && !character.is_whitespace() {
        return (character_start, character_start + character.len_utf8());
    }
    let same_class = |candidate: char| {
        candidate.is_whitespace() == character.is_whitespace()
            && (character.is_whitespace() || candidate.is_alphanumeric() || candidate == '_')
    };
    let mut start = character_start;
    while let Some((previous_start, previous)) = content[..start].char_indices().next_back() {
        if !same_class(previous) {
            break;
        }
        start = previous_start;
    }
    let mut end = character_start + character.len_utf8();
    while let Some(next) = content[end..].chars().next() {
        if !same_class(next) {
            break;
        }
        end += next.len_utf8();
    }
    (start, end)
}

fn line_range(content: &str, index: usize) -> (usize, usize) {
    let index = index.min(content.len());
    let start = content[..index]
        .rfind('\n')
        .map_or(0, |position| position + 1);
    let end = content[index..]
        .find('\n')
        .map_or(content.len(), |position| index + position + 1);
    (start, end)
}

fn hit_index<P: Paragraph>(
    state: &State<P>,
    bounds: Rectangle,
    cursor: mouse::Cursor,
    clamp_to_text: bool,
) -> Option<usize> {
    let position = cursor.position()?;
    let anchor = bounds.anchor(
        state.paragraph.min_bounds(),
        state.paragraph.align_x(),
        state.paragraph.align_y(),
    );
    let local = position - (anchor - Point::ORIGIN);
    let hit = if clamp_to_text {
        state.paragraph.raw().hit_test(local)
    } else {
        state.paragraph.raw().hit_test_strict(local)
    };
    hit.map(text::Hit::cursor).or_else(|| {
        clamp_to_text.then(|| {
            let paragraph_bottom = anchor.y + state.paragraph.min_height();
            if position.y < anchor.y || (position.y < paragraph_bottom && position.x < anchor.x) {
                0
            } else {
                state.paragraph.content().len()
            }
        })
    })
}

pub fn draw<Renderer>(
    renderer: &mut Renderer,
    style: &renderer::Style,
    bounds: Rectangle,
    paragraph: &Renderer::Paragraph,
    appearance: Style,
    viewport: &Rectangle,
) where
    Renderer: text::Renderer,
{
    let anchor = bounds.anchor(
        paragraph.min_bounds(),
        paragraph.align_x(),
        paragraph.align_y(),
    );

    renderer.fill_paragraph(
        paragraph,
        anchor,
        appearance.color.unwrap_or(style.text_color),
        *viewport,
    );
}

impl<'a, Message, Theme, Renderer> From<Text<'a, Theme, Renderer>>
    for Element<'a, Message, Theme, Renderer>
where
    Theme: Catalog + 'a,
    Renderer: text::Renderer + 'a,
{
    fn from(text: Text<'a, Theme, Renderer>) -> Element<'a, Message, Theme, Renderer> {
        Element::new(text)
    }
}

impl<'a, Theme, Renderer> From<&'a str> for Text<'a, Theme, Renderer>
where
    Theme: Catalog + 'a,
    Renderer: text::Renderer,
{
    fn from(content: &'a str) -> Self {
        Self::new(content)
    }
}

impl<'a, Message, Theme, Renderer> From<&'a str> for Element<'a, Message, Theme, Renderer>
where
    Theme: Catalog + 'a,
    Renderer: text::Renderer + 'a,
{
    fn from(content: &'a str) -> Self {
        Text::from(content).into()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Style {
                pub color: Option<Color>,
}

pub trait Catalog: Sized {
        type Class<'a>;

        fn default<'a>() -> Self::Class<'a>;

        fn style(&self, item: &Self::Class<'_>) -> Style;
}

pub type StyleFn<'a, Theme> = Box<dyn Fn(&Theme) -> Style + 'a>;

impl Catalog for Theme {
    type Class<'a> = StyleFn<'a, Self>;

    fn default<'a>() -> Self::Class<'a> {
        Box::new(|_theme| Style::default())
    }

    fn style(&self, class: &Self::Class<'_>) -> Style {
        class(self)
    }
}

pub fn default(_theme: &Theme) -> Style {
    Style { color: None }
}

pub fn base(theme: &Theme) -> Style {
    Style {
        color: Some(theme.seed().text),
    }
}

pub fn primary(theme: &Theme) -> Style {
    Style {
        color: Some(theme.seed().primary),
    }
}

pub fn secondary(theme: &Theme) -> Style {
    Style {
        color: Some(theme.palette().secondary.base.color),
    }
}

pub fn success(theme: &Theme) -> Style {
    Style {
        color: Some(theme.seed().success),
    }
}

pub fn warning(theme: &Theme) -> Style {
    Style {
        color: Some(theme.seed().warning),
    }
}

pub fn danger(theme: &Theme) -> Style {
    Style {
        color: Some(theme.seed().danger),
    }
}
