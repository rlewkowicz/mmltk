use crate::{Point, Size};

#[derive(Debug, Clone, Copy, Default)]
pub enum Position {
        #[default]
    Default,
        Centered,
                                Specific(Point),
                        SpecificWith(fn(Size, Size) -> Point),
}
