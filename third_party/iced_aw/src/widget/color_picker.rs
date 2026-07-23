use self::style::{Status, StyleFn};

use super::overlay::color_picker::{
    self, ColorBarDragged, ColorPickerOverlay, ColorPickerOverlayButtons,
};

use iced_core::{
    Color, Element, Event, Layout, Length, Point, Rectangle, Shell, Vector, Widget,
    layout::{Limits, Node},
    mouse::{self, Cursor},
    overlay, renderer,
    widget::{
        Operation,
        tree::{self, Tag, Tree},
    },
};
use iced_widget::Renderer;

pub use crate::style::{self, color_picker::Style};

#[allow(missing_debug_implementations)]
pub struct ColorPicker<'a, Message, Theme = iced_widget::Theme>
where
    Message: Clone,
    Theme: style::color_picker::Catalog + iced_widget::button::Catalog,
{
        show_picker: bool,
        color: Color,
        underlay: Element<'a, Message, Theme, Renderer>,
        on_cancel: Message,
        on_submit: Box<dyn Fn(Color) -> Message>,
        on_color_change: Option<Box<dyn Fn(Color) -> Message>>,
        class: <Theme as style::color_picker::Catalog>::Class<'a>,
        overlay_state: Element<'a, Message, Theme, Renderer>,
}

impl<'a, Message, Theme> ColorPicker<'a, Message, Theme>
where
    Message: 'a + Clone,
    Theme: 'a
        + style::color_picker::Catalog
        + iced_widget::button::Catalog
        + iced_widget::text::Catalog,
{
                                                pub fn new<U, F>(
        show_picker: bool,
        color: Color,
        underlay: U,
        on_cancel: Message,
        on_submit: F,
    ) -> Self
    where
        U: Into<Element<'a, Message, Theme, Renderer>>,
        F: 'static + Fn(Color) -> Message,
    {
        Self {
            show_picker,
            color,
            underlay: underlay.into(),
            on_cancel,
            on_submit: Box::new(on_submit),
            on_color_change: None,
            class: <Theme as style::color_picker::Catalog>::default(),
            overlay_state: ColorPickerOverlayButtons::default().into(),
        }
    }

        #[must_use]
    pub fn on_color_change<F>(mut self, on_color_change: F) -> Self
    where
        F: 'static + Fn(Color) -> Message,
    {
        self.on_color_change = Some(Box::new(on_color_change));
        self
    }

        #[must_use]
    pub fn style(mut self, style: impl Fn(&Theme, Status) -> Style + 'a) -> Self
    where
        <Theme as style::color_picker::Catalog>::Class<'a>: From<StyleFn<'a, Theme, Style>>,
    {
        self.class = (Box::new(style) as StyleFn<'a, Theme, Style>).into();
        self
    }

        #[must_use]
    pub fn class(
        mut self,
        class: impl Into<<Theme as style::color_picker::Catalog>::Class<'a>>,
    ) -> Self {
        self.class = class.into();
        self
    }
}

#[derive(Debug, Default)]
pub struct State {
        pub(crate) overlay_state: color_picker::State,
        pub(crate) old_show_picker: bool,
}

impl State {
        #[must_use]
    pub fn new(color: Color) -> Self {
        Self {
            overlay_state: color_picker::State::new(color),
            old_show_picker: false,
        }
    }

        pub fn reset(&mut self) {
        self.overlay_state.color = Color::from_rgb(0.5, 0.25, 0.25);
        self.overlay_state.color_bar_dragged = ColorBarDragged::None;
    }

                        fn synchronize(&mut self, show_picker: bool, color: Color) {
        if show_picker && (!self.old_show_picker || self.overlay_state.initial_color != color) {
            self.overlay_state.force_synchronize(color);
        }
        self.old_show_picker = show_picker;
    }
}

impl<'a, Message, Theme> Widget<Message, Theme, Renderer> for ColorPicker<'a, Message, Theme>
where
    Message: 'static + Clone,
    Theme: 'a
        + style::color_picker::Catalog
        + iced_widget::button::Catalog
        + iced_widget::text::Catalog,
{
    fn tag(&self) -> Tag {
        Tag::of::<State>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(State::new(self.color))
    }

    fn diff(&mut self, tree: &mut Tree) {
        let color_picker_state = tree.state.downcast_mut::<State>();

        color_picker_state.synchronize(self.show_picker, self.color);

        tree.diff_children(&mut [&mut self.underlay, &mut self.overlay_state]);
    }

    fn size(&self) -> iced_core::Size<Length> {
        self.underlay.as_widget().size()
    }

    fn layout(&mut self, tree: &mut Tree, renderer: &Renderer, limits: &Limits) -> Node {
        self.underlay
            .as_widget_mut()
            .layout(&mut tree.children[0], renderer, limits)
    }

    fn update(
        &mut self,
        state: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: Cursor,
        renderer: &Renderer,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        self.underlay.as_widget_mut().update(
            &mut state.children[0],
            event,
            layout,
            cursor,
            renderer,
            shell,
            viewport,
        );
    }

    fn mouse_interaction(
        &self,
        state: &Tree,
        layout: Layout<'_>,
        cursor: Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        self.underlay.as_widget().mouse_interaction(
            &state.children[0],
            layout,
            cursor,
            viewport,
            renderer,
        )
    }

    fn draw(
        &self,
        state: &Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: Cursor,
        viewport: &Rectangle,
    ) {
        self.underlay.as_widget().draw(
            &state.children[0],
            renderer,
            theme,
            style,
            layout,
            cursor,
            viewport,
        );
    }

    fn operate<'b>(
        &'b mut self,
        state: &'b mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        self.underlay
            .as_widget_mut()
            .operate(&mut state.children[0], layout, renderer, operation);
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        layout: Layout<'b>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        let picker_state: &mut State = tree.state.downcast_mut();

        if !self.show_picker {
            return self.underlay.as_widget_mut().overlay(
                &mut tree.children[0],
                layout,
                renderer,
                viewport,
                translation,
            );
        }

        let bounds = layout.bounds();
        let position = Point::new(bounds.center_x(), bounds.center_y());

        Some(
            ColorPickerOverlay::new(
                picker_state,
                self.on_cancel.clone(),
                &self.on_submit,
                self.on_color_change.as_deref(),
                position,
                &self.class,
                &mut tree.children[1],
                *viewport,
            )
            .overlay(),
        )
    }
}

impl<'a, Message, Theme> From<ColorPicker<'a, Message, Theme>>
    for Element<'a, Message, Theme, Renderer>
where
    Message: 'static + Clone,
    Theme: 'a
        + style::color_picker::Catalog
        + iced_widget::button::Catalog
        + iced_widget::text::Catalog,
{
    fn from(color_picker: ColorPicker<'a, Message, Theme>) -> Self {
        Element::new(color_picker)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, Debug, PartialEq)]
    enum TestMessage {
        Cancel,
        Submit(Color),
    }

    type TestColorPicker<'a> = ColorPicker<'a, TestMessage, iced_widget::Theme>;

    fn create_test_button() -> iced_widget::Button<'static, TestMessage, iced_widget::Theme> {
        iced_widget::button(iced_widget::text::Text::new("Pick"))
    }

    #[test]
    fn color_picker_new_with_picker_hidden() {
        let color = Color::from_rgb(0.5, 0.5, 0.5);
        let button = create_test_button();

        let picker = TestColorPicker::new(
            false,
            color,
            button,
            TestMessage::Cancel,
            TestMessage::Submit,
        );

        assert!(!picker.show_picker);
        assert_eq!(picker.color, color);
    }

    #[test]
    fn color_picker_new_with_picker_shown() {
        let color = Color::from_rgb(0.3, 0.6, 0.9);
        let button = create_test_button();

        let picker = TestColorPicker::new(
            true,
            color,
            button,
            TestMessage::Cancel,
            TestMessage::Submit,
        );

        assert!(picker.show_picker);
        assert_eq!(picker.color, color);
    }

    #[test]
    fn color_picker_state_new() {
        let color = Color::from_rgb(0.5, 0.5, 0.5);
        let state = State::new(color);

        assert!(!state.old_show_picker);
    }

    #[test]
    fn color_picker_state_default() {
        let state = State::default();

        assert!(!state.old_show_picker);
    }

    #[test]
    fn color_picker_state_reset() {
        let color = Color::from_rgb(0.5, 0.5, 0.5);
        let mut state = State::new(color);

        state.reset();
        assert!(!state.old_show_picker);
    }
}
