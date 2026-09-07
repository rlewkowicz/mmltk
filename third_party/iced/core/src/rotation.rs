use crate::{Degrees, Radians, Size};

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Rotation {
                                Floating(Radians),
                        Solid(Radians),
}

impl Rotation {
        pub fn radians(self) -> Radians {
        match self {
            Rotation::Floating(radians) | Rotation::Solid(radians) => radians,
        }
    }

        pub fn radians_mut(&mut self) -> &mut Radians {
        match self {
            Rotation::Floating(radians) | Rotation::Solid(radians) => radians,
        }
    }

        pub fn degrees(self) -> Degrees {
        Degrees(self.radians().0.to_degrees())
    }

            pub fn apply(self, size: Size) -> Size {
        match self {
            Self::Floating(_) => size,
            Self::Solid(rotation) => size.rotate(rotation),
        }
    }
}

impl Default for Rotation {
    fn default() -> Self {
        Self::Floating(Radians(0.0))
    }
}

impl From<Radians> for Rotation {
    fn from(radians: Radians) -> Self {
        Self::Floating(radians)
    }
}

impl From<f32> for Rotation {
    fn from(radians: f32) -> Self {
        Self::Floating(Radians(radians))
    }
}
