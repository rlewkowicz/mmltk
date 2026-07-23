use crate::{Alignment, Padding, Point, Rectangle, Size, Vector};

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Node {
    bounds: Rectangle,
    children: Vec<Node>,
}

impl Node {
        pub const fn new(size: Size) -> Self {
        Self::with_children(size, Vec::new())
    }

        pub const fn with_children(size: Size, children: Vec<Node>) -> Self {
        Node {
            bounds: Rectangle {
                x: 0.0,
                y: 0.0,
                width: size.width,
                height: size.height,
            },
            children,
        }
    }

        pub fn container(child: Self, padding: Padding) -> Self {
        Self::with_children(
            child.bounds.size().expand(padding),
            vec![child.move_to(Point::new(padding.left, padding.top))],
        )
    }

        pub fn size(&self) -> Size {
        Size::new(self.bounds.width, self.bounds.height)
    }

        pub fn bounds(&self) -> Rectangle {
        self.bounds
    }

        pub fn children(&self) -> &[Node] {
        &self.children
    }

        pub fn align(mut self, align_x: Alignment, align_y: Alignment, space: Size) -> Self {
        self.align_mut(align_x, align_y, space);
        self
    }

        pub fn align_mut(&mut self, align_x: Alignment, align_y: Alignment, space: Size) {
        match align_x {
            Alignment::Start => {}
            Alignment::Center => {
                self.bounds.x += (space.width - self.bounds.width) / 2.0;
            }
            Alignment::End => {
                self.bounds.x += space.width - self.bounds.width;
            }
        }

        match align_y {
            Alignment::Start => {}
            Alignment::Center => {
                self.bounds.y += (space.height - self.bounds.height) / 2.0;
            }
            Alignment::End => {
                self.bounds.y += space.height - self.bounds.height;
            }
        }
    }

        pub fn move_to(mut self, position: impl Into<Point>) -> Self {
        self.move_to_mut(position);
        self
    }

        pub fn move_to_mut(&mut self, position: impl Into<Point>) {
        let position = position.into();

        self.bounds.x = position.x;
        self.bounds.y = position.y;
    }

        pub fn translate(mut self, translation: impl Into<Vector>) -> Self {
        self.translate_mut(translation);
        self
    }

        pub fn translate_mut(&mut self, translation: impl Into<Vector>) {
        self.bounds += translation.into();
    }
}
