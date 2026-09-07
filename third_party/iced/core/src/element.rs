use crate::layout;
use crate::mouse;
use crate::overlay;
use crate::renderer;
use crate::widget;
use crate::widget::tree::{self, Tree};
use crate::{Border, Color, Event, Layout, Length, Rectangle, Shell, Size, Vector, Widget};

use std::borrow::{Borrow, BorrowMut};

pub struct Element<'a, Message, Theme, Renderer> {
    widget: Box<dyn Widget<Message, Theme, Renderer> + 'a>,
}

impl<'a, Message, Theme, Renderer> Element<'a, Message, Theme, Renderer> {
        pub fn new(widget: impl Widget<Message, Theme, Renderer> + 'a) -> Self
    where
        Renderer: crate::Renderer,
    {
        Self {
            widget: Box::new(widget),
        }
    }

        pub fn as_widget(&self) -> &dyn Widget<Message, Theme, Renderer> {
        self.widget.as_ref()
    }

        pub fn as_widget_mut(&mut self) -> &mut dyn Widget<Message, Theme, Renderer> {
        self.widget.as_mut()
    }

                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                pub fn map<B>(self, f: impl Fn(Message) -> B + 'a) -> Element<'a, B, Theme, Renderer>
    where
        Message: 'a,
        Theme: 'a,
        Renderer: crate::Renderer + 'a,
        B: 'a,
    {
        Element::new(Map::new(self.widget, f))
    }

                            pub fn explain<C: Into<Color>>(self, color: C) -> Element<'a, Message, Theme, Renderer>
    where
        Message: 'a,
        Theme: 'a,
        Renderer: crate::Renderer + 'a,
    {
        Element {
            widget: Box::new(Explain::new(self, color.into())),
        }
    }
}

impl<'a, Message, Theme, Renderer> Borrow<dyn Widget<Message, Theme, Renderer> + 'a>
    for Element<'a, Message, Theme, Renderer>
{
    fn borrow(&self) -> &(dyn Widget<Message, Theme, Renderer> + 'a) {
        self.widget.borrow()
    }
}

impl<'a, Message, Theme, Renderer> Borrow<dyn Widget<Message, Theme, Renderer> + 'a>
    for &Element<'a, Message, Theme, Renderer>
{
    fn borrow(&self) -> &(dyn Widget<Message, Theme, Renderer> + 'a) {
        self.widget.borrow()
    }
}

impl<'a, Message, Theme, Renderer> Borrow<dyn Widget<Message, Theme, Renderer> + 'a>
    for &mut Element<'a, Message, Theme, Renderer>
{
    fn borrow(&self) -> &(dyn Widget<Message, Theme, Renderer> + 'a) {
        self.widget.borrow()
    }
}

impl<'a, Message, Theme, Renderer> BorrowMut<dyn Widget<Message, Theme, Renderer> + 'a>
    for Element<'a, Message, Theme, Renderer>
{
    fn borrow_mut(&mut self) -> &mut (dyn Widget<Message, Theme, Renderer> + 'a) {
        self.widget.borrow_mut()
    }
}

impl<'a, Message, Theme, Renderer> BorrowMut<dyn Widget<Message, Theme, Renderer> + 'a>
    for &mut Element<'a, Message, Theme, Renderer>
{
    fn borrow_mut(&mut self) -> &mut (dyn Widget<Message, Theme, Renderer> + 'a) {
        self.widget.borrow_mut()
    }
}

struct Map<'a, A, B, Theme, Renderer> {
    widget: Box<dyn Widget<A, Theme, Renderer> + 'a>,
    mapper: Box<dyn Fn(A) -> B + 'a>,
}

impl<'a, A, B, Theme, Renderer> Map<'a, A, B, Theme, Renderer> {
    pub fn new<F>(
        widget: Box<dyn Widget<A, Theme, Renderer> + 'a>,
        mapper: F,
    ) -> Map<'a, A, B, Theme, Renderer>
    where
        F: 'a + Fn(A) -> B,
    {
        Map {
            widget,
            mapper: Box::new(mapper),
        }
    }
}

impl<'a, A, B, Theme, Renderer> Widget<B, Theme, Renderer> for Map<'a, A, B, Theme, Renderer>
where
    Renderer: crate::Renderer + 'a,
    A: 'a,
    B: 'a,
{
    fn tag(&self) -> tree::Tag {
        self.widget.tag()
    }

    fn state(&self) -> tree::State {
        self.widget.state()
    }

    fn diff(&mut self, tree: &mut Tree) {
        self.widget.diff(tree);
    }

    fn size(&self) -> Size<Length> {
        self.widget.size()
    }

    fn layout(
        &mut self,
        tree: &mut Tree,
        renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        self.widget.layout(tree, renderer, limits)
    }

    fn operate(
        &mut self,
        tree: &mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        self.widget.operate(tree, layout, renderer, operation);
    }

    fn update(
        &mut self,
        tree: &mut Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &Renderer,
        shell: &mut Shell<'_, B>,
        viewport: &Rectangle,
    ) {
        let mut local_messages = Vec::new();
        let mut local_shell = shell.local(&mut local_messages);

        self.widget.update(
            tree,
            event,
            layout,
            cursor,
            renderer,
            &mut local_shell,
            viewport,
        );

        shell.merge(local_shell, &self.mapper);
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
        self.widget
            .draw(tree, renderer, theme, style, layout, cursor, viewport);
    }

    fn mouse_interaction(
        &self,
        tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        self.widget
            .mouse_interaction(tree, layout, cursor, viewport, renderer)
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        layout: Layout<'b>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, B, Theme, Renderer>> {
        let mapper = &self.mapper;

        self.widget
            .overlay(tree, layout, renderer, viewport, translation)
            .map(move |overlay| overlay.map(mapper))
    }
}

struct Explain<'a, Message, Theme, Renderer: crate::Renderer> {
    element: Element<'a, Message, Theme, Renderer>,
    color: Color,
}

impl<'a, Message, Theme, Renderer> Explain<'a, Message, Theme, Renderer>
where
    Renderer: crate::Renderer,
{
    fn new(element: Element<'a, Message, Theme, Renderer>, color: Color) -> Self {
        Explain { element, color }
    }
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer>
    for Explain<'_, Message, Theme, Renderer>
where
    Renderer: crate::Renderer,
{
    fn size(&self) -> Size<Length> {
        self.element.widget.size()
    }

    fn tag(&self) -> tree::Tag {
        self.element.widget.tag()
    }

    fn state(&self) -> tree::State {
        self.element.widget.state()
    }

    fn diff(&mut self, tree: &mut Tree) {
        self.element.widget.diff(tree);
    }

    fn layout(
        &mut self,
        tree: &mut Tree,
        renderer: &Renderer,
        limits: &layout::Limits,
    ) -> layout::Node {
        self.element.widget.layout(tree, renderer, limits)
    }

    fn operate(
        &mut self,
        tree: &mut Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn widget::Operation,
    ) {
        self.element
            .widget
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
        self.element
            .widget
            .update(tree, event, layout, cursor, renderer, shell, viewport);
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
        fn explain_layout<Renderer: crate::Renderer>(
            renderer: &mut Renderer,
            color: Color,
            layout: Layout<'_>,
        ) {
            renderer.fill_quad(
                renderer::Quad {
                    bounds: layout.bounds(),
                    border: Border {
                        color,
                        width: 1.0,
                        ..Border::default()
                    },
                    ..renderer::Quad::default()
                },
                Color::TRANSPARENT,
            );

            for child in layout.children() {
                explain_layout(renderer, color, child);
            }
        }

        self.element
            .widget
            .draw(tree, renderer, theme, style, layout, cursor, viewport);

        renderer.with_layer(Rectangle::INFINITE, |renderer| {
            explain_layout(renderer, self.color, layout);
        });
    }

    fn mouse_interaction(
        &self,
        tree: &Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        self.element
            .widget
            .mouse_interaction(tree, layout, cursor, viewport, renderer)
    }

    fn overlay<'b>(
        &'b mut self,
        tree: &'b mut Tree,
        layout: Layout<'b>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<overlay::Element<'b, Message, Theme, Renderer>> {
        self.element
            .widget
            .overlay(tree, layout, renderer, viewport, translation)
    }
}

impl<'a, T, Message, Theme, Renderer> From<Option<T>> for Element<'a, Message, Theme, Renderer>
where
    T: Into<Self>,
    Renderer: crate::Renderer,
{
    fn from(value: Option<T>) -> Self {
        value
            .map(T::into)
            .unwrap_or_else(|| Element::new(widget::Void))
    }
}

impl<'a, Message, Theme, Renderer> From<widget::Void> for Element<'a, Message, Theme, Renderer>
where
    Renderer: crate::Renderer,
{
    fn from(void: widget::Void) -> Self {
        Element::new(void)
    }
}
