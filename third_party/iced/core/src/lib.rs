#![doc(
    html_logo_url = "https://raw.githubusercontent.com/iced-rs/iced/9ab6923e943f784985e9ef9ca28b10278297225d/docs/logo.svg"
)]
pub mod alignment;
pub mod animation;
pub mod backend;
pub mod border;
pub mod clipboard;
pub mod color;
pub mod event;
pub mod font;
pub mod gradient;
pub mod image;
pub mod input_method;
pub mod keyboard;
pub mod layout;
pub mod length;
pub mod mouse;
pub mod overlay;
pub mod padding;
pub mod renderer;
pub mod shell;
pub mod svg;
pub mod text;
pub mod theme;
pub mod time;
pub mod touch;
pub mod widget;
pub mod window;

mod angle;
mod background;
mod content_fit;
mod element;
mod pixels;
mod point;
mod rectangle;
mod rotation;
mod settings;
mod shadow;
mod size;
mod transformation;
mod vector;

pub use alignment::Alignment;
pub use angle::{Degrees, Radians};
pub use animation::Animation;
pub use backend::Backend;
pub use background::Background;
pub use border::Border;
pub use clipboard::Clipboard;
pub use color::Color;
pub use content_fit::ContentFit;
pub use element::Element;
pub use event::Event;
pub use font::Font;
pub use gradient::Gradient;
pub use image::Image;
pub use input_method::InputMethod;
pub use layout::Layout;
pub use length::Length;
pub use overlay::Overlay;
pub use padding::Padding;
pub use pixels::Pixels;
pub use point::Point;
pub use rectangle::Rectangle;
pub use renderer::Renderer;
pub use rotation::Rotation;
pub use settings::Settings;
pub use shadow::Shadow;
pub use shell::Shell;
pub use size::Size;
pub use svg::Svg;
pub use text::Text;
pub use theme::Theme;
pub use transformation::Transformation;
pub use vector::Vector;
pub use widget::Widget;
pub use window::Window;

pub use bytes::Bytes;
pub use smol_str::SmolStr;
pub use std::convert::Infallible as Never;

pub fn never<T>(never: Never) -> T {
    match never {}
}

pub trait Function<A, B, O> {
                                                                                                                                                            fn with(self, prefix: A) -> impl Fn(B) -> O;
}

impl<F, A, B, O> Function<A, B, O> for F
where
    F: Fn(A, B) -> O,
    Self: Sized,
    A: Clone,
{
    fn with(self, prefix: A) -> impl Fn(B) -> O {
        move |result| self(prefix.clone(), result)
    }
}
