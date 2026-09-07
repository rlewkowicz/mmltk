use crate::{Pixels, Size};

#[derive(Debug, Copy, Clone, PartialEq, Default)]
pub struct Padding {
        pub top: f32,
        pub right: f32,
        pub bottom: f32,
        pub left: f32,
}

pub fn all(padding: impl Into<Pixels>) -> Padding {
    Padding::new(padding.into().0)
}

pub fn top(padding: impl Into<Pixels>) -> Padding {
    Padding::default().top(padding)
}

pub fn bottom(padding: impl Into<Pixels>) -> Padding {
    Padding::default().bottom(padding)
}

pub fn left(padding: impl Into<Pixels>) -> Padding {
    Padding::default().left(padding)
}

pub fn right(padding: impl Into<Pixels>) -> Padding {
    Padding::default().right(padding)
}

pub fn horizontal(padding: impl Into<Pixels>) -> Padding {
    Padding::default().horizontal(padding)
}

pub fn vertical(padding: impl Into<Pixels>) -> Padding {
    Padding::default().vertical(padding)
}

impl Padding {
        pub const ZERO: Padding = Padding {
        top: 0.0,
        right: 0.0,
        bottom: 0.0,
        left: 0.0,
    };

        pub const fn new(padding: f32) -> Padding {
        Padding {
            top: padding,
            right: padding,
            bottom: padding,
            left: padding,
        }
    }

                pub fn top(self, top: impl Into<Pixels>) -> Self {
        Self {
            top: top.into().0,
            ..self
        }
    }

                pub fn bottom(self, bottom: impl Into<Pixels>) -> Self {
        Self {
            bottom: bottom.into().0,
            ..self
        }
    }

                pub fn left(self, left: impl Into<Pixels>) -> Self {
        Self {
            left: left.into().0,
            ..self
        }
    }

                pub fn right(self, right: impl Into<Pixels>) -> Self {
        Self {
            right: right.into().0,
            ..self
        }
    }

                    pub fn horizontal(self, horizontal: impl Into<Pixels>) -> Self {
        let horizontal = horizontal.into();

        Self {
            left: horizontal.0,
            right: horizontal.0,
            ..self
        }
    }

                    pub fn vertical(self, vertical: impl Into<Pixels>) -> Self {
        let vertical = vertical.into();

        Self {
            top: vertical.0,
            bottom: vertical.0,
            ..self
        }
    }

        pub fn x(self) -> f32 {
        self.left + self.right
    }

        pub fn y(self) -> f32 {
        self.top + self.bottom
    }

        pub fn fit(self, inner: Size, outer: Size) -> Self {
        let available = (outer - inner).max(Size::ZERO);
        let new_top = self.top.min(available.height);
        let new_left = self.left.min(available.width);

        Padding {
            top: new_top,
            bottom: self.bottom.min(available.height - new_top),
            left: new_left,
            right: self.right.min(available.width - new_left),
        }
    }
}

impl From<u16> for Padding {
    fn from(p: u16) -> Self {
        Padding {
            top: f32::from(p),
            right: f32::from(p),
            bottom: f32::from(p),
            left: f32::from(p),
        }
    }
}

impl From<[u16; 2]> for Padding {
    fn from(p: [u16; 2]) -> Self {
        Padding {
            top: f32::from(p[0]),
            right: f32::from(p[1]),
            bottom: f32::from(p[0]),
            left: f32::from(p[1]),
        }
    }
}

impl From<f32> for Padding {
    fn from(p: f32) -> Self {
        Padding {
            top: p,
            right: p,
            bottom: p,
            left: p,
        }
    }
}

impl From<[f32; 2]> for Padding {
    fn from(p: [f32; 2]) -> Self {
        Padding {
            top: p[0],
            right: p[1],
            bottom: p[0],
            left: p[1],
        }
    }
}

impl From<Padding> for Size {
    fn from(padding: Padding) -> Self {
        Self::new(padding.x(), padding.y())
    }
}

impl From<Pixels> for Padding {
    fn from(pixels: Pixels) -> Self {
        Self::from(pixels.0)
    }
}
