use iced_core::Rectangle;
use iced_widget::graphics::text::{Editor, Paragraph};

// Exercise real text shaping and widget trees without a graphics device.
pub(crate) struct Renderer;
impl iced_core::Renderer for Renderer {
    fn start_layer(&mut self, _: Rectangle) {}
    fn end_layer(&mut self) {}
    fn start_transformation(&mut self, _: iced_core::Transformation) {}
    fn end_transformation(&mut self) {}
    fn fill_quad(&mut self, _: iced_core::renderer::Quad, _: impl Into<iced_core::Background>) {}
    fn allocate_image(&mut self, _: &iced_core::image::Handle,
        callback: impl FnOnce(Result<iced_core::image::Allocation, iced_core::image::Error>) + Send + 'static) {
        callback(Err(iced_core::image::Error::Unsupported));
    }
    fn hint(&mut self, _: f32) {}
    fn scale_factor(&self) -> Option<f32> { None }
    fn reset(&mut self, _: Rectangle) {}
}
impl iced_core::text::Renderer for Renderer {
    type Font = iced_core::Font;
    type Paragraph = Paragraph;
    type Editor = Editor;
    const ICON_FONT: iced_core::Font = iced_core::Font::DEFAULT;
    const CHECKMARK_ICON: char = ' ';
    const ARROW_DOWN_ICON: char = ' ';
    const SCROLL_UP_ICON: char = ' ';
    const SCROLL_DOWN_ICON: char = ' ';
    const SCROLL_LEFT_ICON: char = ' ';
    const SCROLL_RIGHT_ICON: char = ' ';
    const ICED_LOGO: char = ' ';
    fn default_font(&self) -> iced_core::Font { iced_core::Font::DEFAULT }
    fn default_size(&self) -> iced_core::Pixels { iced_core::Pixels(16.0) }
    fn fill_paragraph(&mut self, _: &Paragraph, _: iced_core::Point, _: iced_core::Color, _: Rectangle) {}
    fn fill_editor(&mut self, _: &Editor, _: iced_core::Point, _: iced_core::Color, _: Rectangle) {}
    fn fill_text(&mut self, _: iced_core::Text<String>, _: iced_core::Point, _: iced_core::Color, _: Rectangle) {}
}
