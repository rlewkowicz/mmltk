mod limits;
mod node;

pub mod flex;

pub use limits::Limits;
pub use node::Node;

use crate::{Length, Padding, Point, Rectangle, Size, Vector};

#[derive(Debug, Clone, Copy)]
pub struct Layout<'a> {
    position: Point,
    node: &'a Node,
}

impl<'a> Layout<'a> {
        pub fn new(node: &'a Node) -> Self {
        Self::with_offset(Vector::new(0.0, 0.0), node)
    }

            pub fn with_offset(offset: Vector, node: &'a Node) -> Self {
        let bounds = node.bounds();

        Self {
            position: Point::new(bounds.x, bounds.y) + offset,
            node,
        }
    }

        pub fn position(&self) -> Point {
        self.position
    }

                    pub fn bounds(&self) -> Rectangle {
        let bounds = self.node.bounds();

        Rectangle {
            x: self.position.x,
            y: self.position.y,
            width: bounds.width,
            height: bounds.height,
        }
    }

        pub fn children(self) -> impl DoubleEndedIterator<Item = Layout<'a>> + ExactSizeIterator {
        self.node.children().iter().map(move |node| {
            Layout::with_offset(Vector::new(self.position.x, self.position.y), node)
        })
    }

                                pub fn child(self, index: usize) -> Layout<'a> {
        let node = &self.node.children()[index];

        Layout::with_offset(Vector::new(self.position.x, self.position.y), node)
    }
}

pub fn next_to_each_other(
    limits: &Limits,
    spacing: f32,
    left: impl FnOnce(&Limits) -> Node,
    right: impl FnOnce(&Limits) -> Node,
) -> Node {
    let left_node = left(limits);
    let left_size = left_node.size();

    let right_limits = limits.shrink(Size::new(left_size.width + spacing, 0.0));

    let right_node = right(&right_limits);
    let right_size = right_node.size();

    let (left_y, right_y) = if left_size.height > right_size.height {
        (0.0, (left_size.height - right_size.height) / 2.0)
    } else {
        ((right_size.height - left_size.height) / 2.0, 0.0)
    };

    Node::with_children(
        Size::new(
            left_size.width + spacing + right_size.width,
            left_size.height.max(right_size.height),
        ),
        vec![
            left_node.move_to(Point::new(0.0, left_y)),
            right_node.move_to(Point::new(left_size.width + spacing, right_y)),
        ],
    )
}

pub fn atomic(limits: &Limits, width: impl Into<Length>, height: impl Into<Length>) -> Node {
    let width = width.into();
    let height = height.into();

    Node::new(
        limits
            .width(width)
            .height(height)
            .resolve(width, height, Size::ZERO),
    )
}

pub fn sized(
    limits: &Limits,
    width: impl Into<Length>,
    height: impl Into<Length>,
    f: impl FnOnce(&Limits) -> Size,
) -> Node {
    let width = width.into();
    let height = height.into();

    let limits = limits.width(width).height(height);
    let intrinsic_size = f(&limits);

    Node::new(limits.resolve(width, height, intrinsic_size))
}

pub fn contained(
    limits: &Limits,
    width: impl Into<Length>,
    height: impl Into<Length>,
    f: impl FnOnce(&Limits) -> Node,
) -> Node {
    let width = width.into();
    let height = height.into();

    let limits = limits.width(width).height(height);
    let content = f(&limits);

    Node::with_children(limits.resolve(width, height, content.size()), vec![content])
}

pub fn padded(
    limits: &Limits,
    width: impl Into<Length>,
    height: impl Into<Length>,
    padding: impl Into<Padding>,
    layout: impl FnOnce(&Limits) -> Node,
) -> Node {
    positioned(limits, width, height, padding, layout, |content, _| content)
}

pub fn positioned(
    limits: &Limits,
    width: impl Into<Length>,
    height: impl Into<Length>,
    padding: impl Into<Padding>,
    layout: impl FnOnce(&Limits) -> Node,
    position: impl FnOnce(Node, Size) -> Node,
) -> Node {
    let width = width.into();
    let height = height.into();
    let padding = padding.into();

    let limits = limits.width(width).height(height);
    let content = layout(&limits.shrink(padding));
    let padding = padding.fit(content.size(), limits.max());

    let size = limits
        .shrink(padding)
        .resolve(width, height, content.size());

    Node::with_children(
        size.expand(padding),
        vec![position(content.move_to((padding.left, padding.top)), size)],
    )
}
