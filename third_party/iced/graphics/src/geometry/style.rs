use crate::core::Color;
use crate::geometry::Gradient;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Style {
        Solid(Color),

        Gradient(Gradient),
}

impl From<Color> for Style {
    fn from(color: Color) -> Self {
        Self::Solid(color)
    }
}

impl From<Gradient> for Style {
    fn from(gradient: Gradient) -> Self {
        Self::Gradient(gradient)
    }
}
