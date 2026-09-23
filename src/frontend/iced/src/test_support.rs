//! Shared renderer for CPU-only widget layout and interaction tests.
use iced_runtime::core::{self, Point, Rectangle};

pub(crate) struct Renderer;
impl core::Renderer for Renderer {
    fn start_layer(&mut self, _: Rectangle) {}
    fn end_layer(&mut self) {}
    fn start_transformation(&mut self, _: core::Transformation) {}
    fn end_transformation(&mut self) {}
    fn fill_quad(&mut self, _: core::renderer::Quad, _: impl Into<core::Background>) {}
    fn allocate_image(
        &mut self,
        _: &core::image::Handle,
        callback: impl FnOnce(Result<core::image::Allocation, core::image::Error>) + Send + 'static,
    ) {
        callback(Err(core::image::Error::Unsupported));
    }
    fn hint(&mut self, _: f32) {}
    fn scale_factor(&self) -> Option<f32> {
        None
    }
    fn reset(&mut self, _: core::Rectangle) {}
}
impl core::text::Renderer for Renderer {
    type Font = core::Font;
    type Paragraph = iced::advanced::graphics::text::Paragraph;
    type Editor = iced::advanced::graphics::text::Editor;
    const ICON_FONT: core::Font = core::Font::DEFAULT;
    const CHECKMARK_ICON: char = ' ';
    const ARROW_DOWN_ICON: char = ' ';
    const SCROLL_UP_ICON: char = ' ';
    const SCROLL_DOWN_ICON: char = ' ';
    const SCROLL_LEFT_ICON: char = ' ';
    const SCROLL_RIGHT_ICON: char = ' ';
    const ICED_LOGO: char = ' ';
    fn default_font(&self) -> core::Font {
        core::Font::DEFAULT
    }
    fn default_size(&self) -> core::Pixels {
        core::Pixels(16.0)
    }
    fn fill_paragraph(&mut self, _: &Self::Paragraph, _: Point, _: core::Color, _: Rectangle) {}
    fn fill_editor(&mut self, _: &Self::Editor, _: Point, _: core::Color, _: Rectangle) {}
    fn fill_text(&mut self, _: core::Text<String>, _: Point, _: core::Color, _: Rectangle) {}
}
