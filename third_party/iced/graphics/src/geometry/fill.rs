pub use crate::geometry::Style;

use crate::core::Color;
use crate::gradient::{self, Gradient};

#[derive(Debug, Clone, Copy)]
pub struct Fill {
                pub style: Style,

                                    pub rule: Rule,
}

impl Default for Fill {
    fn default() -> Self {
        Self {
            style: Style::Solid(Color::BLACK),
            rule: Rule::NonZero,
        }
    }
}

impl From<Color> for Fill {
    fn from(color: Color) -> Fill {
        Fill {
            style: Style::Solid(color),
            ..Fill::default()
        }
    }
}

impl From<Gradient> for Fill {
    fn from(gradient: Gradient) -> Self {
        Fill {
            style: Style::Gradient(gradient),
            ..Default::default()
        }
    }
}

impl From<gradient::Linear> for Fill {
    fn from(gradient: gradient::Linear) -> Self {
        Fill {
            style: Style::Gradient(Gradient::Linear(gradient)),
            ..Default::default()
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[allow(missing_docs)]
pub enum Rule {
    NonZero,
    EvenOdd,
}
