pub mod fill;
pub mod frame;
pub mod path;
pub mod stroke;

mod cache;
mod style;
mod text;

pub use cache::Cache;
pub use fill::Fill;
pub use frame::Frame;
pub use path::Path;
pub use stroke::{LineCap, LineDash, LineJoin, Stroke};
pub use style::Style;
pub use text::Text;

pub use crate::core::{Image, Svg};
pub use crate::gradient::{self, Gradient};

use crate::cache::Cached;
use crate::core::{self, Rectangle};

pub trait Renderer: core::Renderer {
        type Geometry: Cached;

        type Frame: frame::Backend<Geometry = Self::Geometry>;

        fn new_frame(&self, bounds: Rectangle) -> Self::Frame;

        fn draw_geometry(&mut self, geometry: Self::Geometry);
}

#[cfg(debug_assertions)]
impl Renderer for () {
    type Geometry = ();
    type Frame = ();

    fn new_frame(&self, _bounds: Rectangle) -> Self::Frame {}

    fn draw_geometry(&mut self, _geometry: Self::Geometry) {}
}
