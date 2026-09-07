pub mod arc;

mod builder;

#[doc(no_inline)]
pub use arc::Arc;
pub use builder::Builder;

pub use lyon_path;

use crate::core::border;
use crate::core::{Point, Size};

#[derive(Debug, Clone)]
pub struct Path {
    raw: lyon_path::Path,
}

impl Path {
                pub fn new(f: impl FnOnce(&mut Builder)) -> Self {
        let mut builder = Builder::new();

        f(&mut builder);

        builder.build()
    }

            pub fn line(from: Point, to: Point) -> Self {
        Self::new(|p| {
            p.move_to(from);
            p.line_to(to);
        })
    }

            pub fn rectangle(top_left: Point, size: Size) -> Self {
        Self::new(|p| p.rectangle(top_left, size))
    }

            pub fn rounded_rectangle(top_left: Point, size: Size, radius: border::Radius) -> Self {
        Self::new(|p| p.rounded_rectangle(top_left, size, radius))
    }

            pub fn circle(center: Point, radius: f32) -> Self {
        Self::new(|p| {
            p.circle(center, radius);
            p.close();
        })
    }

        #[inline]
    pub fn raw(&self) -> &lyon_path::Path {
        &self.raw
    }

        #[inline]
    pub fn transform(&self, transform: &lyon_path::math::Transform) -> Path {
        Path {
            raw: self.raw.clone().transformed(transform),
        }
    }
}
