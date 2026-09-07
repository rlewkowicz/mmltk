use crate::core::Color;

use bytemuck::{Pod, Zeroable};

#[derive(Debug, Clone, Copy, PartialEq, Zeroable, Pod)]
#[repr(C)]
pub struct Packed([f32; 4]);

impl Packed {
        pub fn components(self) -> [f32; 4] {
        self.0
    }
}

pub const GAMMA_CORRECTION: bool = internal::GAMMA_CORRECTION;

pub fn pack(color: impl Into<Color>) -> Packed {
    Packed(internal::pack(color.into()))
}

#[cfg(not(feature = "web-colors"))]
mod internal {
    use crate::core::Color;

    pub const GAMMA_CORRECTION: bool = true;

    pub fn pack(color: Color) -> [f32; 4] {
        color.into_linear()
    }
}

#[cfg(feature = "web-colors")]
mod internal {
    use crate::core::Color;

    pub const GAMMA_CORRECTION: bool = false;

    pub fn pack(color: Color) -> [f32; 4] {
        [color.r, color.g, color.b, color.a]
    }
}
