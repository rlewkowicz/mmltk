use crate::core::{Size, Transformation};

#[derive(Debug, Clone)]
pub struct Viewport {
    physical_size: Size<u32>,
    logical_size: Size<f32>,
    scale_factor: f32,
    projection: Transformation,
}

impl Viewport {
            pub fn with_physical_size(size: Size<u32>, scale_factor: f32) -> Viewport {
        Viewport {
            physical_size: size,
            logical_size: Size::new(
                size.width as f32 / scale_factor,
                size.height as f32 / scale_factor,
            ),
            scale_factor,
            projection: Transformation::orthographic(size.width, size.height),
        }
    }

        pub fn physical_size(&self) -> Size<u32> {
        self.physical_size
    }

        pub fn physical_width(&self) -> u32 {
        self.physical_size.width
    }

        pub fn physical_height(&self) -> u32 {
        self.physical_size.height
    }

        pub fn logical_size(&self) -> Size<f32> {
        self.logical_size
    }

        pub fn scale_factor(&self) -> f32 {
        self.scale_factor
    }

        pub fn projection(&self) -> Transformation {
        self.projection
    }
}
