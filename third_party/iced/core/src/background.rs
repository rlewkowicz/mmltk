use crate::Color;
use crate::gradient::{self, Gradient};

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Background {
        Color(Color),
        Gradient(Gradient),
}

impl Background {
            pub fn scale_alpha(self, factor: f32) -> Self {
        match self {
            Self::Color(color) => Self::Color(color.scale_alpha(factor)),
            Self::Gradient(gradient) => Self::Gradient(gradient.scale_alpha(factor)),
        }
    }
}

impl From<Color> for Background {
    fn from(color: Color) -> Self {
        Background::Color(color)
    }
}

impl From<Gradient> for Background {
    fn from(gradient: Gradient) -> Self {
        Background::Gradient(gradient)
    }
}

impl From<gradient::Linear> for Background {
    fn from(gradient: gradient::Linear) -> Self {
        Background::Gradient(Gradient::Linear(gradient))
    }
}
