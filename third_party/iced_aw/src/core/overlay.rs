use iced_core::{Point, Size, layout};

pub trait Position {
            fn center_and_bounce(&mut self, position: Point, bounds: Size);
}

impl Position for layout::Node {
    fn center_and_bounce(&mut self, position: Point, bounds: Size) {
        let size = self.size();

        self.move_to_mut(Point::new(
            (position.x - (size.width / 2.0)).max(0.0),
            (position.y - (size.height / 2.0)).max(0.0),
        ));

        let new_self_bounds = self.bounds();

        self.move_to_mut(Point::new(
            if new_self_bounds.x + new_self_bounds.width > bounds.width {
                (new_self_bounds.x - (new_self_bounds.width - (bounds.width - new_self_bounds.x)))
                    .max(0.0)
            } else {
                new_self_bounds.x
            },
            if new_self_bounds.y + new_self_bounds.height > bounds.height {
                (new_self_bounds.y - (new_self_bounds.height - (bounds.height - new_self_bounds.y)))
                    .max(0.0)
            } else {
                new_self_bounds.y
            },
        ));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iced_core::layout::Node;

    #[test]
    fn center_within_bounds() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(200.0, 150.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 150.0);
        assert_eq!(node_bounds.y, 125.0);
    }

    #[test]
    fn bounce_right_edge() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(380.0, 150.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 300.0);
        assert_eq!(node_bounds.y, 125.0);
    }

    #[test]
    fn bounce_bottom_edge() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(200.0, 285.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 150.0);
        assert_eq!(node_bounds.y, 250.0);
    }

    #[test]
    fn bounce_right_and_bottom() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(380.0, 285.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 300.0);
        assert_eq!(node_bounds.y, 250.0);
    }

    #[test]
    fn position_at_origin() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(0.0, 0.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 0.0);
        assert_eq!(node_bounds.y, 0.0);
    }

    #[test]
    fn position_near_top_left() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(10.0, 10.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 0.0);
        assert_eq!(node_bounds.y, 0.0);
    }

    #[test]
    fn exact_fit_at_corner() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(350.0, 275.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 300.0);
        assert_eq!(node_bounds.y, 250.0);
    }

    #[test]
    fn small_node_centered() {
        let mut node = Node::new(Size::new(20.0, 10.0));
        let position = Point::new(200.0, 150.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 190.0);
        assert_eq!(node_bounds.y, 145.0);
    }

    #[test]
    fn node_larger_than_bounds() {
        let mut node = Node::new(Size::new(500.0, 400.0));
        let position = Point::new(200.0, 150.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 0.0);
        assert_eq!(node_bounds.y, 0.0);
    }

    #[test]
    fn node_width_equals_bounds_width() {
        let mut node = Node::new(Size::new(400.0, 50.0));
        let position = Point::new(200.0, 150.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 0.0);
        assert_eq!(node_bounds.y, 125.0);
    }

    #[test]
    fn node_height_equals_bounds_height() {
        let mut node = Node::new(Size::new(100.0, 300.0));
        let position = Point::new(200.0, 150.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 150.0);
        assert_eq!(node_bounds.y, 0.0);
    }

    #[test]
    fn position_at_exact_bounds() {
        let mut node = Node::new(Size::new(100.0, 50.0));
        let position = Point::new(400.0, 300.0);
        let bounds = Size::new(400.0, 300.0);

        node.center_and_bounce(position, bounds);

        let node_bounds = node.bounds();
        assert_eq!(node_bounds.x, 300.0);
        assert_eq!(node_bounds.y, 250.0);
    }
}
