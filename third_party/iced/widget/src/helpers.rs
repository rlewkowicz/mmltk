use crate::button::{self, Button};
use crate::checkbox::{self, Checkbox};
use crate::combo_box::{self, ComboBox};
use crate::container::{self, Container};
use crate::core;
use crate::core::theme;
use crate::core::time::Instant;
use crate::core::widget::operation::{self, Operation};
use crate::core::window;
use crate::core::{Element, Length, Size, Widget};
use crate::float::{self, Float};
use crate::keyed;
use crate::overlay;
use crate::pane_grid::{self, PaneGrid};
use crate::pick_list::{self, PickList};
use crate::progress_bar::{self, ProgressBar};
use crate::radio::{self, Radio};
use crate::scrollable::{self, Scrollable};
use crate::slider::{self, Slider};
use crate::text::{self, Text};
use crate::text_editor::{self, TextEditor};
use crate::text_input::{self, TextInput};
use crate::toggler::{self, Toggler};
use crate::tooltip::{self, Tooltip};
use crate::transition::{self, Transition};
use crate::vertical_slider::{self, VerticalSlider};
use crate::{Column, Grid, MouseArea, Pin, Responsive, Row, Sensor, Space, Stack, Themer};

use std::borrow::Borrow;
use std::ops::RangeInclusive;

pub use crate::table::table;

#[macro_export]
macro_rules! column {
    () => (
        $crate::Column::new()
    );
    ($($x:expr),+ $(,)?) => (
        $crate::Column::with_children([$($crate::core::Element::from($x)),+])
    );
}

#[macro_export]
macro_rules! row {
    () => (
        $crate::Row::new()
    );
    ($($x:expr),+ $(,)?) => (
        $crate::Row::with_children([$($crate::core::Element::from($x)),+])
    );
}

#[macro_export]
macro_rules! stack {
    () => (
        $crate::Stack::new()
    );
    ($($x:expr),+ $(,)?) => (
        $crate::Stack::with_children([$($crate::core::Element::from($x)),+])
    );
}

#[macro_export]
macro_rules! grid {
    () => (
        $crate::Grid::new()
    );
    ($($x:expr),+ $(,)?) => (
        $crate::Grid::with_children([$($crate::core::Element::from($x)),+])
    );
}

#[macro_export]
macro_rules! text {
    ($($arg:tt)*) => {
        $crate::Text::new(format!($($arg)*))
    };
}

#[macro_export]
macro_rules! rich_text {
    () => (
        $crate::text::Rich::new()
    );
    ($($x:expr),+ $(,)?) => (
        $crate::text::Rich::from_iter([$($crate::text::Span::from($x)),+])
    );
}

pub fn container<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    Container::new(content)
}

pub fn center<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content).center(Length::Fill)
}

pub fn center_x<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content).center_x(Length::Fill)
}

pub fn center_y<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content).center_y(Length::Fill)
}

pub fn right<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content).align_right(Length::Fill)
}

pub fn right_center<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content)
        .align_right(Length::Fill)
        .center_y(Length::Fill)
}

pub fn bottom<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content).align_bottom(Length::Fill)
}

pub fn bottom_center<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content)
        .center_x(Length::Fill)
        .align_bottom(Length::Fill)
}

pub fn bottom_right<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Container<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::Renderer,
{
    container(content)
        .align_right(Length::Fill)
        .align_bottom(Length::Fill)
}

pub fn pin<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Pin<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    Pin::new(content)
}

pub fn column<'a, Message, Theme, Renderer>(
    children: impl IntoIterator<Item = Element<'a, Message, Theme, Renderer>>,
) -> Column<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    Column::with_children(children)
}

pub fn keyed_column<'a, Key, Message, Theme, Renderer>(
    children: impl IntoIterator<Item = (Key, Element<'a, Message, Theme, Renderer>)>,
) -> keyed::Column<'a, Key, Message, Theme, Renderer>
where
    Key: Copy + PartialEq,
    Renderer: core::Renderer,
{
    keyed::Column::with_children(children)
}

pub fn row<'a, Message, Theme, Renderer>(
    children: impl IntoIterator<Item = Element<'a, Message, Theme, Renderer>>,
) -> Row<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    Row::with_children(children)
}

pub fn grid<'a, Message, Theme, Renderer>(
    children: impl IntoIterator<Item = Element<'a, Message, Theme, Renderer>>,
) -> Grid<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    Grid::with_children(children)
}

pub fn stack<'a, Message, Theme, Renderer>(
    children: impl IntoIterator<Item = Element<'a, Message, Theme, Renderer>>,
) -> Stack<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    Stack::with_children(children)
}

pub fn opaque<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: 'a,
    Renderer: core::Renderer + 'a,
{
    use crate::core::layout::{self, Layout};
    use crate::core::mouse;
    use crate::core::renderer;
    use crate::core::widget::tree::{self, Tree};
    use crate::core::{Event, Rectangle, Shell, Size};

    struct Opaque<'a, Message, Theme, Renderer> {
        content: Element<'a, Message, Theme, Renderer>,
    }

    impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer>
        for Opaque<'_, Message, Theme, Renderer>
    where
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
        }

        fn size(&self) -> Size<Length> {
            self.content.as_widget().size()
        }

        fn layout(
            &mut self,
            tree: &mut Tree,
            renderer: &Renderer,
            limits: &layout::Limits,
        ) -> layout::Node {
            self.content.as_widget_mut().layout(tree, renderer, limits)
        }

        fn draw(
            &self,
            tree: &Tree,
            renderer: &mut Renderer,
            theme: &Theme,
            style: &renderer::Style,
            layout: Layout<'_>,
            cursor: mouse::Cursor,
            viewport: &Rectangle,
        ) {
            self.content
                .as_widget()
                .draw(tree, renderer, theme, style, layout, cursor, viewport);
        }

        fn operate(
            &mut self,
            tree: &mut Tree,
            layout: Layout<'_>,
            renderer: &Renderer,
            operation: &mut dyn operation::Operation,
        ) {
            self.content
                .as_widget_mut()
                .operate(tree, layout, renderer, operation);
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
            let is_mouse_press =
                matches!(event, core::Event::Mouse(mouse::Event::ButtonPressed(_)));

            self.content
                .as_widget_mut()
                .update(tree, event, layout, cursor, renderer, shell, viewport);

            if is_mouse_press && cursor.is_over(layout.bounds()) {
                shell.capture_event();
            }
        }

        fn mouse_interaction(
            &self,
            state: &core::widget::Tree,
            layout: core::Layout<'_>,
            cursor: core::mouse::Cursor,
            viewport: &core::Rectangle,
            renderer: &Renderer,
        ) -> core::mouse::Interaction {
            let interaction = self
                .content
                .as_widget()
                .mouse_interaction(state, layout, cursor, viewport, renderer);

            if interaction == mouse::Interaction::None && cursor.is_over(layout.bounds()) {
                mouse::Interaction::Idle
            } else {
                interaction
            }
        }

        fn overlay<'b>(
            &'b mut self,
            state: &'b mut core::widget::Tree,
            layout: core::Layout<'b>,
            renderer: &Renderer,
            viewport: &Rectangle,
            translation: core::Vector,
        ) -> Option<core::overlay::Element<'b, Message, Theme, Renderer>> {
            self.content
                .as_widget_mut()
                .overlay(state, layout, renderer, viewport, translation)
        }
    }

    Element::new(Opaque {
        content: content.into(),
    })
}

pub fn hover<'a, Message, Theme, Renderer>(
    base: impl Into<Element<'a, Message, Theme, Renderer>>,
    top: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Theme: 'a,
    Renderer: core::Renderer + 'a,
{
    use crate::core::layout::{self, Layout};
    use crate::core::mouse;
    use crate::core::renderer;
    use crate::core::widget::tree::{self, Tree};
    use crate::core::{Event, Rectangle, Shell, Size};

    struct Hover<'a, Message, Theme, Renderer> {
        base: Element<'a, Message, Theme, Renderer>,
        top: Element<'a, Message, Theme, Renderer>,
        is_top_focused: bool,
        is_top_overlay_active: bool,
        is_hovered: bool,
    }

    impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer>
        for Hover<'_, Message, Theme, Renderer>
    where
        Renderer: core::Renderer,
    {
        fn tag(&self) -> tree::Tag {
            struct Tag;
            tree::Tag::of::<Tag>()
        }

        fn diff(&mut self, tree: &mut Tree) {
            tree.diff_children(&mut [&mut self.base, &mut self.top]);
        }

        fn size(&self) -> Size<Length> {
            self.base.as_widget().size()
        }

        fn layout(
            &mut self,
            tree: &mut Tree,
            renderer: &Renderer,
            limits: &layout::Limits,
        ) -> layout::Node {
            let base = self
                .base
                .as_widget_mut()
                .layout(&mut tree.children[0], renderer, limits);

            let top = self.top.as_widget_mut().layout(
                &mut tree.children[1],
                renderer,
                &layout::Limits::new(Size::ZERO, base.size()),
            );

            layout::Node::with_children(base.size(), vec![base, top])
        }

        fn draw(
            &self,
            tree: &Tree,
            renderer: &mut Renderer,
            theme: &Theme,
            style: &renderer::Style,
            layout: Layout<'_>,
            cursor: mouse::Cursor,
            viewport: &Rectangle,
        ) {
            if let Some(bounds) = layout.bounds().intersection(viewport) {
                let mut children = layout.children().zip(&tree.children);

                let (base_layout, base_tree) = children.next().unwrap();

                self.base.as_widget().draw(
                    base_tree,
                    renderer,
                    theme,
                    style,
                    base_layout,
                    cursor,
                    viewport,
                );

                if cursor.is_over(layout.bounds())
                    || self.is_top_focused
                    || self.is_top_overlay_active
                {
                    let (top_layout, top_tree) = children.next().unwrap();

                    renderer.with_layer(bounds, |renderer| {
                        self.top.as_widget().draw(
                            top_tree, renderer, theme, style, top_layout, cursor, viewport,
                        );
                    });
                }
            }
        }

        fn operate(
            &mut self,
            tree: &mut Tree,
            layout: Layout<'_>,
            renderer: &Renderer,
            operation: &mut dyn operation::Operation,
        ) {
            let children = [&mut self.base, &mut self.top]
                .into_iter()
                .zip(layout.children().zip(&mut tree.children));

            for (child, (layout, tree)) in children {
                child
                    .as_widget_mut()
                    .operate(tree, layout, renderer, operation);
            }
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
            let mut children = layout.children().zip(&mut tree.children);
            let (base_layout, base_tree) = children.next().unwrap();
            let (top_layout, top_tree) = children.next().unwrap();

            let is_hovered = cursor.is_over(layout.bounds());

            if matches!(event, Event::Window(window::Event::RedrawRequested(_))) {
                let mut count_focused = operation::focusable::count();

                self.top.as_widget_mut().operate(
                    top_tree,
                    top_layout,
                    renderer,
                    &mut operation::black_box(&mut count_focused),
                );

                self.is_top_focused = match count_focused.finish() {
                    operation::Outcome::Some(count) => count.focused.is_some(),
                    _ => false,
                };

                self.is_hovered = is_hovered;
            } else if is_hovered != self.is_hovered {
                shell.request_redraw();
            }

            let is_visible = is_hovered || self.is_top_focused || self.is_top_overlay_active;

            if matches!(
                event,
                Event::Mouse(mouse::Event::CursorMoved { .. } | mouse::Event::ButtonReleased(_))
            ) || is_visible
            {
                let redraw_request = shell.redraw_request();

                self.top.as_widget_mut().update(
                    top_tree, event, top_layout, cursor, renderer, shell, viewport,
                );

                if !is_visible {
                    Shell::replace_redraw_request(shell, redraw_request);
                }

                if shell.is_event_captured() {
                    return;
                }
            };

            self.base.as_widget_mut().update(
                base_tree,
                event,
                base_layout,
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
            [&self.base, &self.top]
                .into_iter()
                .rev()
                .zip(layout.children().rev().zip(tree.children.iter().rev()))
                .map(|(child, (layout, tree))| {
                    child
                        .as_widget()
                        .mouse_interaction(tree, layout, cursor, viewport, renderer)
                })
                .find(|&interaction| interaction != mouse::Interaction::None)
                .unwrap_or_default()
        }

        fn overlay<'b>(
            &'b mut self,
            tree: &'b mut core::widget::Tree,
            layout: core::Layout<'b>,
            renderer: &Renderer,
            viewport: &Rectangle,
            translation: core::Vector,
        ) -> Option<core::overlay::Element<'b, Message, Theme, Renderer>> {
            let mut overlays = [&mut self.base, &mut self.top]
                .into_iter()
                .zip(layout.children().zip(tree.children.iter_mut()))
                .map(|(child, (layout, tree))| {
                    child
                        .as_widget_mut()
                        .overlay(tree, layout, renderer, viewport, translation)
                });

            if let Some(base_overlay) = overlays.next()? {
                return Some(base_overlay);
            }

            let top_overlay = overlays.next()?;
            self.is_top_overlay_active = top_overlay.is_some();

            top_overlay
        }
    }

    Element::new(Hover {
        base: base.into(),
        top: top.into(),
        is_top_focused: false,
        is_top_overlay_active: false,
        is_hovered: false,
    })
}

pub fn sensor<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Sensor<'a, (), Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    Sensor::new(content)
}

pub fn scrollable<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Scrollable<'a, Message, Theme, Renderer>
where
    Theme: scrollable::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    Scrollable::new(content)
}

pub fn button<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Button<'a, Message, Theme, Renderer>
where
    Theme: button::Catalog + 'a,
    Renderer: core::Renderer,
{
    Button::new(content)
}

pub fn tooltip<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
    tooltip: impl Into<Element<'a, Message, Theme, Renderer>>,
    position: tooltip::Position,
) -> crate::Tooltip<'a, Message, Theme, Renderer>
where
    Theme: container::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    Tooltip::new(content, tooltip, position)
}

pub fn text<'a, Theme, Renderer>(text: impl text::IntoFragment<'a>) -> Text<'a, Theme, Renderer>
where
    Theme: text::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    Text::new(text)
}

pub fn value<'a, Theme, Renderer>(value: impl ToString) -> Text<'a, Theme, Renderer>
where
    Theme: text::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    Text::new(value.to_string())
}

pub fn rich_text<'a, Link, Message, Theme, Renderer>(
    spans: impl AsRef<[text::Span<'a, Link, Renderer::Font>]> + 'a,
) -> text::Rich<'a, Link, Message, Theme, Renderer>
where
    Link: Clone + 'static,
    Theme: text::Catalog + 'a,
    Renderer: core::text::Renderer,
    Renderer::Font: 'a,
{
    text::Rich::with_spans(spans)
}

pub fn span<'a, Link, Font>(text: impl text::IntoFragment<'a>) -> text::Span<'a, Link, Font> {
    text::Span::new(text)
}

#[cfg(feature = "markdown")]
#[doc(inline)]
pub use crate::markdown::view as markdown;

pub fn checkbox<'a, Message, Theme, Renderer>(
    is_checked: bool,
) -> Checkbox<'a, Message, Theme, Renderer>
where
    Theme: checkbox::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    Checkbox::new(is_checked)
}

pub fn radio<'a, Message, Theme, Renderer, V>(
    label: impl Into<String>,
    value: V,
    selected: Option<V>,
    on_click: impl FnOnce(V) -> Message,
) -> Radio<'a, Message, Theme, Renderer>
where
    Message: Clone,
    Theme: radio::Catalog + 'a,
    Renderer: core::text::Renderer,
    V: Copy + Eq,
{
    Radio::new(label, value, selected, on_click)
}

pub fn toggler<'a, Message, Theme, Renderer>(
    is_checked: bool,
) -> Toggler<'a, Message, Theme, Renderer>
where
    Theme: toggler::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    Toggler::new(is_checked)
}

pub fn text_input<'a, Message, Theme, Renderer>(
    placeholder: &str,
    value: &str,
) -> TextInput<'a, Message, Theme, Renderer>
where
    Message: Clone,
    Theme: text_input::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    TextInput::new(placeholder, value)
}

pub fn text_editor<'a, Message, Theme, Renderer>(
    content: &'a text_editor::Content<Renderer>,
) -> TextEditor<'a, core::text::highlighter::PlainText, Message, Theme, Renderer>
where
    Message: Clone,
    Theme: text_editor::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    TextEditor::new(content)
}

pub fn slider<'a, T, Message, Theme>(
    range: std::ops::RangeInclusive<T>,
    value: T,
    on_change: impl Fn(T) -> Message + 'a,
) -> Slider<'a, T, Message, Theme>
where
    T: Copy + From<u8> + std::cmp::PartialOrd,
    Message: Clone,
    Theme: slider::Catalog + 'a,
{
    Slider::new(range, value, on_change)
}

pub fn vertical_slider<'a, T, Message, Theme>(
    range: std::ops::RangeInclusive<T>,
    value: T,
    on_change: impl Fn(T) -> Message + 'a,
) -> VerticalSlider<'a, T, Message, Theme>
where
    T: Copy + From<u8> + std::cmp::PartialOrd,
    Message: Clone,
    Theme: vertical_slider::Catalog + 'a,
{
    VerticalSlider::new(range, value, on_change)
}

pub fn pick_list<'a, T, L, V, Message, Theme, Renderer>(
    selected: Option<V>,
    options: L,
    to_string: impl Fn(&T) -> String + 'a,
) -> PickList<'a, T, L, V, Message, Theme, Renderer>
where
    T: PartialEq + Clone + 'a,
    L: Borrow<[T]> + 'a,
    V: Borrow<T> + 'a,
    Message: Clone,
    Theme: pick_list::Catalog + overlay::menu::Catalog,
    Renderer: core::text::Renderer,
{
    PickList::new(selected, options, to_string)
}

pub fn combo_box<'a, T, Message, Theme, Renderer>(
    state: &'a combo_box::State<T>,
    placeholder: &str,
    selection: Option<&T>,
    on_selected: impl Fn(T) -> Message + 'a,
) -> ComboBox<'a, T, Message, Theme, Renderer>
where
    T: std::fmt::Display + Clone,
    Theme: combo_box::Catalog + 'a,
    Renderer: core::text::Renderer,
{
    ComboBox::new(state, placeholder, selection, on_selected)
}

pub fn space() -> Space {
    Space::new()
}

pub fn progress_bar<'a, Theme>(range: RangeInclusive<f32>, value: f32) -> ProgressBar<'a, Theme>
where
    Theme: progress_bar::Catalog + 'a,
{
    ProgressBar::new(range, value)
}

#[cfg(feature = "image")]
pub fn image<Handle>(handle: impl Into<Handle>) -> crate::Image<Handle> {
    crate::Image::new(handle.into())
}

#[cfg(feature = "svg")]
pub fn svg<'a, Theme>(handle: impl Into<core::svg::Handle>) -> crate::Svg<'a, Theme>
where
    Theme: crate::svg::Catalog,
{
    crate::Svg::new(handle)
}

pub fn iced<'a, Message, Theme, Renderer>(
    text_size: impl Into<core::Pixels>,
) -> Element<'a, Message, Theme, Renderer>
where
    Message: 'a,
    Renderer: core::Renderer + core::text::Renderer<Font = core::Font> + 'a,
    Theme: text::Catalog + container::Catalog + 'a,
    <Theme as container::Catalog>::Class<'a>: From<container::StyleFn<'a, Theme>>,
    <Theme as text::Catalog>::Class<'a>: From<text::StyleFn<'a, Theme>>,
{
    use crate::core::border;
    use crate::core::color;
    use crate::core::gradient;
    use crate::core::{Alignment, Color, Font, Radians};

    let text_size = text_size.into();

    row![
        container(
            text(Renderer::ICED_LOGO)
                .line_height(1.0)
                .size(text_size)
                .font(Renderer::ICON_FONT)
                .color(Color::WHITE)
        )
        .padding(text_size * 0.15)
        .style(move |_| container::Style {
            background: Some(
                gradient::Linear::new(Radians::PI / 4.0)
                    .add_stop(0.0, color!(0x0033ff))
                    .add_stop(1.0, color!(0x1177ff))
                    .into()
            ),
            border: border::rounded(border::radius(text_size * 0.4)),
            ..container::Style::default()
        }),
        text("iced").size(text_size).font(Font::MONOSPACE)
    ]
    .spacing(text_size.0 / 3.0)
    .align_y(Alignment::Center)
    .into()
}

#[cfg(feature = "canvas")]
pub fn canvas<P, Message, Theme, Renderer>(program: P) -> crate::Canvas<P, Message, Theme, Renderer>
where
    Renderer: crate::graphics::geometry::Renderer,
    P: crate::canvas::Program<Message, Theme, Renderer>,
{
    crate::Canvas::new(program)
}

#[cfg(feature = "qr_code")]
pub fn qr_code<'a, Theme>(data: &'a crate::qr_code::Data) -> crate::QRCode<'a, Theme>
where
    Theme: crate::qr_code::Catalog + 'a,
{
    crate::QRCode::new(data)
}

#[cfg(feature = "wgpu")]
pub fn shader<Message, P>(program: P) -> crate::Shader<Message, P>
where
    P: crate::shader::Program<Message>,
{
    crate::Shader::new(program)
}

pub fn mouse_area<'a, Message, Theme, Renderer>(
    widget: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> MouseArea<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
{
    MouseArea::new(widget)
}

pub fn themer<'a, Message, Theme, Renderer>(
    theme: Option<Theme>,
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Themer<'a, Message, Theme, Renderer>
where
    Theme: theme::Base,
    Renderer: core::Renderer,
{
    Themer::new(theme, content)
}

pub fn pane_grid<'a, T, Message, Theme, Renderer>(
    state: &'a pane_grid::State<T>,
    view: impl Fn(pane_grid::Pane, &'a T, bool) -> pane_grid::Content<'a, Message, Theme, Renderer>,
) -> PaneGrid<'a, Message, Theme, Renderer>
where
    Theme: pane_grid::Catalog,
    Renderer: core::Renderer,
{
    PaneGrid::new(state, view)
}

pub fn float<'a, Message, Theme, Renderer>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
) -> Float<'a, Message, Theme, Renderer>
where
    Theme: float::Catalog,
    Renderer: core::Renderer,
{
    Float::new(content)
}

pub fn responsive<'a, Message, Theme, Renderer, E>(
    f: impl Fn(Size) -> E + 'a,
) -> Responsive<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
    E: Into<Element<'a, Message, Theme, Renderer>>,
{
    Responsive::new(f)
}

pub fn transition<'a, Message, Theme, Renderer, P, E>(
    value: P::Value,
    init: impl Fn() -> P + 'a,
    view: impl Fn(&P, Instant) -> E + 'a,
) -> Transition<'a, Message, Theme, Renderer, P>
where
    Renderer: core::Renderer,
    P: transition::Program,
    E: Into<Element<'a, Message, Theme, Renderer>>,
{
    Transition::new(init, value, view)
}

pub fn void() -> core::widget::Void {
    core::widget::Void
}
