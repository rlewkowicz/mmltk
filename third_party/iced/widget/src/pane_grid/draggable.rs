use crate::core::{Layout, Point};

pub trait Draggable {
            fn can_be_dragged_at(&self, layout: Layout<'_>, cursor: Point) -> bool;
}
