//! Retained, one-line operational copy. Text owns selection and its context menu.
use crate::fluent_theme::{Element, Theme};
use iced::advanced::{Layout, Shell, Widget, layout, mouse, overlay, renderer, text, widget};
use iced::advanced::text::{Paragraph as _, Renderer as _};
use iced::{Event, Fill, Font, Length, Pixels, Rectangle, Size, Vector};
use iced::widget::{Text, tooltip};

pub fn status_text<'a>(content: impl text::IntoFragment<'a>) -> StatusText<'a> {
    let content = content.into_fragment();
    StatusText {
        text: Text::new(content.clone()).single_line(),
        content,
        normal: None,
        alignment: text::Alignment::Default,
        compact: false,
    }
}

pub struct StatusText<'a> {
    text: Text<'a, Theme, iced::Renderer>,
    content: text::Fragment<'a>,
    normal: Option<Pixels>,
    alignment: text::Alignment,
    compact: bool,
}

impl<'a> StatusText<'a> {
    pub fn size(mut self, size: impl Into<Pixels>) -> Self {
        self.normal = Some(size.into());
        self
    }
    /// Occupy only the fitted paragraph width in a compact control row.
    pub fn compact(mut self) -> Self {
        self.compact = true;
        self
    }
    pub fn align_x(mut self, alignment: impl Into<text::Alignment>) -> Self {
        self.alignment = alignment.into();
        self
    }
    pub fn style(mut self, style: impl Fn(&Theme) -> widget::text::Style + 'a) -> Self {
        self.text = self.text.style(style);
        self
    }

}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Measurement {
    pub size: f32,
    pub normal: f32,
    pub line: f32,
    pub paragraph_height: f32,
}

type Paragraph = <iced::Renderer as text::Renderer>::Paragraph;
#[derive(Default)]
struct State {
    content: String,
    flat: String,
    key: Option<(f32, f32, Font, Option<f32>)>,
    layouts: [Option<((f32, f32, Font, Option<f32>), f32)>; 2],
    resolved: f32,
    measure: text::paragraph::Plain<Paragraph>,
}

// O(log(normal-size range)) paragraph measurements, independent of message length.
fn fit(normal: f32, mut fits: impl FnMut(f32) -> bool) -> f32 {
    if fits(normal) { return normal; }
    let mut low = 0_u32;
    let mut high = ((normal - 8.0) * 2.0).ceil() as u32;
    while low < high {
        let middle = low + (high - low + 1) / 2;
        let size = (normal - middle as f32 * 0.5).max(8.0);
        if fits(size) { high = middle - 1; } else { low = middle; }
    }
    (normal - (low + 1) as f32 * 0.5).max(8.0)
}

impl<Message> Widget<Message, Theme, iced::Renderer> for StatusText<'_> {
    fn size(&self) -> Size<Length> { Size::new(if self.compact { Length::Shrink } else { Fill }, Length::Shrink) }
    fn tag(&self) -> widget::tree::Tag { widget::tree::Tag::of::<State>() }
    fn state(&self) -> widget::tree::State { widget::tree::State::new(State::default()) }
    fn diff(&mut self, tree: &mut widget::Tree) {
        tree.diff_children(&mut [&mut self.text as &mut dyn Widget<Message, Theme, iced::Renderer>]);
    }
    fn layout(&mut self, tree: &mut widget::Tree, renderer: &iced::Renderer, limits: &layout::Limits) -> layout::Node {
        let state = tree.state.downcast_mut::<State>();
        let requested = self.normal.unwrap_or_else(|| renderer.default_size()).0;
        let normal = if requested.is_finite() { requested.max(8.0) } else { 12.0 };
        let width = limits.max().width.max(0.0);
        let font = renderer.default_font();
        let scale = renderer.scale_factor();
        let key = (width, normal, font, scale);
        let cache_slot = usize::from(!width.is_finite());
        let changed = state.content != self.content.as_ref();
        if changed {
            self.content.as_ref().clone_into(&mut state.content);
            widget::text::flatten_line_breaks(&self.content, &mut state.flat);
            state.layouts = [None, None];
        }
        if let Some((cached, resolved)) = state.layouts[cache_slot].filter(|(cached, _)| *cached == key) {
            state.key = Some(cached);
            state.resolved = resolved;
        } else {
            state.resolved = fit(normal, |size| {
                state.measure.update(text::Text {
                    content: &state.flat,
                    bounds: Size::INFINITE,
                    size: Pixels(size),
                    line_height: text::LineHeight::default(),
                    font,
                    align_x: text::Alignment::Default,
                    align_y: iced::alignment::Vertical::Top,
                    shaping: text::Shaping::default(),
                    wrapping: text::Wrapping::None,
                    ellipsis: text::Ellipsis::None,
                    hint_factor: scale,
                });
                state.measure.min_width() <= width
            });
            state.key = Some(key);
            state.layouts[cache_slot] = Some((key, state.resolved));
        }
        let height = text::LineHeight::default().to_absolute(Pixels(normal));
        // Configure the retained Text in place; its tree and selection survive fitting.
        self.text.set_format(widget::text::Format {
            size: Some(Pixels(state.resolved)),
            align_x: self.alignment,
            width: if width.is_finite() && !self.compact { Fill } else { Length::Shrink },
            height: Length::Fixed(height.0),
            line_height: text::LineHeight::Absolute(height),
            wrapping: text::Wrapping::None,
            ellipsis: text::Ellipsis::End,
            ..Default::default()
        });
        <Text<'_, Theme, iced::Renderer> as Widget<Message, Theme, iced::Renderer>>::layout(&mut self.text, &mut tree.children[0], renderer, limits)
    }
    fn update(&mut self, tree: &mut widget::Tree, event: &Event, layout: Layout<'_>, cursor: mouse::Cursor, renderer: &iced::Renderer, shell: &mut Shell<'_, Message>, viewport: &Rectangle) {
        self.text.update(&mut tree.children[0], event, layout, cursor, renderer, shell, viewport);
    }
    fn draw(&self, tree: &widget::Tree, renderer: &mut iced::Renderer, theme: &Theme, style: &renderer::Style, layout: Layout<'_>, cursor: mouse::Cursor, viewport: &Rectangle) {
        <Text<'_, Theme, iced::Renderer> as Widget<Message, Theme, iced::Renderer>>::draw(&self.text, &tree.children[0], renderer, theme, style, layout, cursor, viewport);
    }
    fn operate(&mut self, tree: &mut widget::Tree, layout: Layout<'_>, renderer: &iced::Renderer, operation: &mut dyn widget::Operation) {
        if crate::integration_control::reporting_enabled() {
            let state = tree.state.downcast_ref::<State>();
            let paragraph = tree.children[0].state.downcast_ref::<widget::text::State<Paragraph>>().raw();
            let mut measurement = Measurement {
                size: paragraph.size().0,
                normal: state.key.map_or(12.0, |key| key.1),
                line: layout.bounds().height,
                paragraph_height: paragraph.min_height(),
            };
            operation.custom(None, layout.bounds(), &mut measurement);
        }
        <Text<'_, Theme, iced::Renderer> as Widget<Message, Theme, iced::Renderer>>::operate(&mut self.text, &mut tree.children[0], layout, renderer, operation);
    }
    fn mouse_interaction(&self, tree: &widget::Tree, layout: Layout<'_>, cursor: mouse::Cursor, viewport: &Rectangle, renderer: &iced::Renderer) -> mouse::Interaction {
        <Text<'_, Theme, iced::Renderer> as Widget<Message, Theme, iced::Renderer>>::mouse_interaction(&self.text, &tree.children[0], layout, cursor, viewport, renderer)
    }
    fn overlay<'b>(&'b mut self, tree: &'b mut widget::Tree, layout: Layout<'b>, renderer: &iced::Renderer, viewport: &Rectangle, translation: Vector) -> Option<overlay::Element<'b, Message, Theme, iced::Renderer>> {
        self.text.overlay(&mut tree.children[0], layout, renderer, viewport, translation)
    }
}

impl<'a, Message: 'a> From<StatusText<'a>> for Element<'a, Message> {
    fn from(value: StatusText<'a>) -> Self {
        let original = Text::new(value.content.clone()).wrapping(text::Wrapping::WordOrGlyph);
        let original = match value.normal {
            Some(size) => original.size(size),
            None => original,
        };
        tooltip(Element::new(value), original, tooltip::Position::Top).into()
    }
}

#[cfg(test)]
mod tests {
    use super::fit;
    #[test]
    fn largest_half_pixel_fit_preserves_normal_and_floor() {
        for (width, expected) in [(100.0, 12.0), (47.0, 11.5), (32.0, 8.0), (0.0, 8.0), (f32::INFINITY, 12.0)] {
            assert_eq!(fit(12.0, |size| size * 4.0 <= width), expected);
        }
        assert_eq!(fit(12.0, |_| true), 12.0);
    }
    #[test]
    fn flattened_unicode_line_breaks_preserve_every_source_byte_boundary() {
        let source = "café\r\n東京\u{0085}one\u{2028}two\u{2029}three";
        let mut display = String::new();
        iced::advanced::widget::text::flatten_line_breaks(source, &mut display);
        assert_eq!(source.len(), display.len());
        assert_eq!(source.char_indices().map(|(index, _)| index).collect::<Vec<_>>(), display.char_indices().map(|(index, _)| index).collect::<Vec<_>>());
        assert_eq!(display, "café  東京\u{00a0}one\u{202f}two\u{202f}three");
    }

    #[test]
    fn renderer_layout_and_copy_retain_full_unicode_source_and_normal_height() {
        use super::*;
        use iced::advanced::renderer::{Headless, Renderer as _};
        let mut renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(), Some("wgpu"),
        )).expect("status text requires the container renderer");
        let original = "Compiling image pixels · café 東京\ncomplete source / very long output filename.png";
        let mut element: Element<'_, ()> = status_text(original).size(12).into();
        let mut tree = widget::Tree::new(&element);
        tree.diff(&mut element);
        for scale in [1.0, 1.5, 2.0] {
            renderer.hint(scale);
            for width in [0.0, 45.0, 150.0, 900.0] {
                let limits = layout::Limits::new(Size::ZERO, Size::new(width, 100.0));
                let node = element.as_widget_mut().layout(&mut tree, &renderer, &limits);
                assert!((node.size().height - 15.6).abs() < 0.01);
                let state = tree.children[0].state.downcast_ref::<State>();
                assert_eq!(state.key.unwrap().3, Some(scale));
                assert!((8.0..=12.0).contains(&state.resolved));
                assert_eq!(state.flat, original.replace('\n', " "));
                let paragraph = tree.children[0].children[0].state.downcast_ref::<widget::text::State<Paragraph>>().raw();
                assert_eq!(paragraph.wrapping(), text::Wrapping::None);
                assert_eq!(paragraph.ellipsis(), text::Ellipsis::End);
                assert!(paragraph.min_height() <= 15.7);
            }
        }
        let node = element.as_widget_mut().layout(&mut tree, &renderer, &layout::Limits::new(Size::ZERO, Size::new(45.0, 100.0)));
        {
            let paragraph = tree.children[0].children[0].state.downcast_ref::<widget::text::State<Paragraph>>().raw();
            assert_eq!(paragraph.size(), Pixels(8.0));
            let reference = Paragraph::with_text(text::Text {
                content: "…", bounds: Size::INFINITE, size: paragraph.size(),
                line_height: paragraph.line_height(), font: paragraph.font(),
                align_x: text::Alignment::Default, align_y: iced::alignment::Vertical::Top,
                shaping: paragraph.shaping(), wrapping: text::Wrapping::None,
                ellipsis: text::Ellipsis::None, hint_factor: paragraph.hint_factor(),
            });
            let glyphs: Vec<_> = paragraph.buffer().layout_runs().flat_map(|run| run.glyphs.iter()).collect();
            let expected = reference.buffer().layout_runs().next().unwrap().glyphs[0].clone();
            let last = glyphs.last().expect("narrow status paints an ellipsis glyph");
            assert_eq!((last.font_id, last.glyph_id), (expected.font_id, expected.glyph_id));
            assert!(glyphs.len() < original.chars().count());
            assert_eq!(paragraph.buffer().layout_runs().count(), 1);
            assert!(paragraph.min_width() <= 45.1);
        }
        let mut messages = Vec::new();
        let mut shell = Shell::new(&iced::window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
        let position = iced::Point::new(2.0, 7.0);
        let viewport = Rectangle::with_size(Size::new(1000.0, 1000.0));
        element.as_widget_mut().update(&mut tree, &Event::Mouse(mouse::Event::CursorMoved { position }), Layout::new(&node), mouse::Cursor::Available(position), &renderer, &mut shell, &viewport);
        {
            let mut tooltip = element.as_widget_mut().overlay(&mut tree, Layout::new(&node), &renderer, &viewport, Vector::ZERO).expect("hover exposes the original message");
            let hover_layout = tooltip.as_overlay_mut().layout(&renderer, viewport.size());
            assert!(hover_layout.children()[0].size().width > node.size().width);
            assert!(hover_layout.children()[0].bounds().is_within(&viewport));
        }
        let hover = tree.children[1].state.downcast_ref::<widget::text::State<Paragraph>>().raw();
        assert_eq!(hover.buffer().lines.iter().map(|line| line.text()).collect::<Vec<_>>().join("\n"), original);
        assert!(hover.min_height() > node.size().height);
        element.as_widget_mut().update(&mut tree, &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)), Layout::new(&node), mouse::Cursor::Available(position), &renderer, &mut shell, &viewport);
        for (character, code) in [("a", iced::keyboard::key::Code::KeyA), ("c", iced::keyboard::key::Code::KeyC)] {
            let key = iced::keyboard::Key::Character(character.into());
            let event = Event::Keyboard(iced::keyboard::Event::KeyPressed {
                key: key.clone(), modified_key: key,
                physical_key: iced::keyboard::key::Physical::Code(code),
                location: iced::keyboard::Location::Standard,
                modifiers: iced::keyboard::Modifiers::CTRL,
                text: None, repeat: false,
            });
            element.as_widget_mut().update(&mut tree, &event, Layout::new(&node), mouse::Cursor::Available(position), &renderer, &mut shell, &viewport);
        }
        assert!(matches!(shell.clipboard_mut().write.as_ref(), Some(request) if request.content == iced::clipboard::Content::Text(original.into())));
        // Exercise the actual context-menu Select All and Copy actions, not a
        // keyboard substitute or a separate implementation of selection.
        shell.clipboard_mut().write = None;
        element.as_widget_mut().update(&mut tree, &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)), Layout::new(&node), mouse::Cursor::Available(position), &renderer, &mut shell, &viewport);
        for row in [1.0, 0.0] {
            element.as_widget_mut().update(&mut tree, &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)), Layout::new(&node), mouse::Cursor::Available(position), &renderer, &mut shell, &viewport);
            let mut overlay = element.as_widget_mut().overlay(&mut tree, Layout::new(&node), &renderer, &viewport, Vector::ZERO).expect("Text context menu is open");
            let menu_layout = overlay.as_overlay_mut().layout(&renderer, viewport.size());
            let menu = menu_layout.children()[0].bounds();
            assert_eq!(menu.size(), Size::new(124.0, 56.0));
            let target = iced::Point::new(menu.x + 10.0, menu.y + 14.0 + row * 28.0);
            overlay.as_overlay_mut().update(&Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)), Layout::new(&menu_layout), mouse::Cursor::Available(target), &renderer, &mut shell);
        }
        assert!(matches!(shell.clipboard_mut().write.as_ref(), Some(request) if request.content == iced::clipboard::Content::Text(original.into())));
        let replacement: Element<'_, ()> = status_text("Done").size(12).into();
        element = replacement;
        tree.diff(&mut element);
        let node = element.as_widget_mut().layout(&mut tree, &renderer, &layout::Limits::new(Size::ZERO, Size::new(45.0, 100.0)));
        assert!((node.size().height - 15.6).abs() < 0.01);
        assert_eq!(tree.children[0].state.downcast_ref::<State>().resolved, 12.0);
    }

    #[test]
    fn long_unbroken_multibyte_path_is_fully_laid_out_inside_hover() {
        use super::*;
        use iced::advanced::renderer::Headless;
        let renderer = iced::futures::executor::block_on(<iced::Renderer as Headless>::new(
            Default::default(), Some("wgpu"),
        )).expect("status hover requires the container renderer");
        // A valid Linux filename can contain this many UTF-8 bytes while its
        // unbroken label is substantially wider than the modest viewport.
        let original = format!("output/validate/run-0001/samples/{}.png", "éö".repeat(60));
        let mut element: Element<'_, ()> = status_text(original.clone()).size(12).into();
        let mut tree = widget::Tree::new(&element);
        tree.diff(&mut element);
        let viewport = Rectangle::with_size(Size::new(240.0, 180.0));
        let node = element.as_widget_mut().layout(&mut tree, &renderer,
            &layout::Limits::new(Size::ZERO, Size::new(90.0, viewport.height)));
        let mut messages = Vec::new();
        let mut shell = Shell::new(&iced::window::Headless, iced_runtime::core::shell::Waker::new(|| {}), &mut messages);
        let position = iced::Point::new(2.0, 7.0);
        element.as_widget_mut().update(&mut tree, &Event::Mouse(mouse::Event::CursorMoved { position }),
            Layout::new(&node), mouse::Cursor::Available(position), &renderer, &mut shell, &viewport);
        let hover_bounds = {
            let mut tooltip = element.as_widget_mut().overlay(&mut tree, Layout::new(&node), &renderer,
                &viewport, Vector::ZERO).expect("long output path is exposed on hover");
            let layout = tooltip.as_overlay_mut().layout(&renderer, viewport.size());
            let bounds = layout.children()[0].bounds();
            assert!(bounds.is_within(&viewport));
            bounds
        };
        let hover = tree.children[1].state.downcast_ref::<widget::text::State<Paragraph>>().raw();
        assert_eq!(hover.wrapping(), text::Wrapping::WordOrGlyph);
        assert_eq!(hover.buffer().lines.iter().map(|line| line.text()).collect::<Vec<_>>().join("\n"), original);
        assert!(hover.buffer().layout_runs().count() > 1);
        assert!(hover.min_width() <= hover.bounds().width + 0.1);
        assert!(hover.min_height() <= hover.bounds().height + 0.1);
        assert!(hover.min_width() + 10.0 <= hover_bounds.width + 0.1);
        assert!(hover.min_height() + 10.0 <= hover_bounds.height + 0.1);
        assert!((node.size().height - 15.6).abs() < 0.01);
    }

}
