use crate::{Color, Vector};

#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Shadow {
        pub color: Color,

        pub offset: Vector,

        pub blur_radius: f32,
}
