use crate::core;
use crate::core::layout;
use crate::core::mouse;
use crate::core::renderer;
use crate::core::widget::Tree;
use crate::core::{Element, Layout, Length, Rectangle, Size, Widget};

pub fn horizontal() -> Space {
    Space::new().width(Length::Fill)
}

pub fn vertical() -> Space {
    Space::new().height(Length::Fill)
}

#[derive(Debug)]
pub struct Space {
    width: Length,
    height: Length,
}

impl Space {
        pub fn new() -> Self {
        Space {
            width: Length::Shrink,
            height: Length::Shrink,
        }
    }

        pub fn width(mut self, width: impl Into<Length>) -> Self {
        self.width = width.into();
        self
    }

        pub fn height(mut self, height: impl Into<Length>) -> Self {
        self.height = height.into();
        self
    }
}

impl Default for Space {
    fn default() -> Self {
        Space::new()
    }
}

impl<Message, Theme, Renderer> Widget<Message, Theme, Renderer> for Space
where
    Renderer: core::Renderer,
{
    fn size(&self) -> Size<Length> {
        Size {
            width: self.width,
            height: self.height,
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

    fn draw(
        &self,
        _tree: &Tree,
        _renderer: &mut Renderer,
        _theme: &Theme,
        _style: &renderer::Style,
        _layout: Layout<'_>,
        _cursor: mouse::Cursor,
        _viewport: &Rectangle,
    ) {
    }
}

impl<'a, Message, Theme, Renderer> From<Space> for Element<'a, Message, Theme, Renderer>
where
    Renderer: core::Renderer,
    Message: 'a,
{
    fn from(space: Space) -> Element<'a, Message, Theme, Renderer> {
        Element::new(space)
    }
}
