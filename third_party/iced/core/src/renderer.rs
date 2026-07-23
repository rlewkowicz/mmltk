#[cfg(debug_assertions)]
mod null;

use crate::image;
use crate::{
    Background, Border, Color, Font, Pixels, Rectangle, Shadow, Size, Transformation, Vector,
};

pub const CRISP: bool = cfg!(feature = "crisp");

pub trait Renderer {
        fn start_layer(&mut self, bounds: Rectangle);

                fn end_layer(&mut self);

                fn with_layer(&mut self, bounds: Rectangle, f: impl FnOnce(&mut Self)) {
        self.start_layer(bounds);
        f(self);
        self.end_layer();
    }

        fn start_transformation(&mut self, transformation: Transformation);

                fn end_transformation(&mut self);

        fn with_transformation(&mut self, transformation: Transformation, f: impl FnOnce(&mut Self)) {
        self.start_transformation(transformation);
        f(self);
        self.end_transformation();
    }

        fn with_translation(&mut self, translation: Vector, f: impl FnOnce(&mut Self)) {
        self.with_transformation(Transformation::translate(translation.x, translation.y), f);
    }

        fn fill_quad(&mut self, quad: Quad, background: impl Into<Background>);

        fn allocate_image(
        &mut self,
        handle: &image::Handle,
        callback: impl FnOnce(Result<image::Allocation, image::Error>) + Send + 'static,
    );

                                    fn hint(&mut self, scale_factor: f32);

        fn scale_factor(&self) -> Option<f32>;

        fn reset(&mut self, new_bounds: Rectangle);

                fn tick(&mut self) {}
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Quad {
        pub bounds: Rectangle,

        pub border: Border,

        pub shadow: Shadow,

        pub snap: bool,
}

impl Default for Quad {
    fn default() -> Self {
        Self {
            bounds: Rectangle::with_size(Size::ZERO),
            border: Border::default(),
            shadow: Shadow::default(),
            snap: CRISP,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
        pub text_color: Color,
}

impl Default for Style {
    fn default() -> Self {
        Style {
            text_color: Color::BLACK,
        }
    }
}

pub trait Headless {
        fn new(settings: Settings, backend: Option<&str>) -> impl Future<Output = Option<Self>>
    where
        Self: Sized;

                    fn name(&self) -> String;

            fn screenshot(
        &mut self,
        size: Size<u32>,
        scale_factor: f32,
        background_color: Color,
    ) -> Vec<u8>;
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Settings {
        pub default_font: Font,

                pub default_text_size: Pixels,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            default_font: Font::DEFAULT,
            default_text_size: Pixels(16.0),
        }
    }
}
