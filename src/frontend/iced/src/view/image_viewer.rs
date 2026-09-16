//! Shared image geometry and controls; source selection and product policy stay local.
use crate::fluent_theme::Element;
use crate::presentation_surface::{self, Surface};
use iced::widget::{button, container, row, space, text};
pub fn image<'a, Message: 'a>(surface: Surface, labels: presentation_surface::labels::Source,
    input: Option<crate::workspace_input::Binding>, show_fps: bool, id: &'static str) -> Element<'a, Message> {
    presentation_surface::labels::view(presentation_surface::Program {
        surface, show_fps, input, local: None, publish: None,
        placement: presentation_surface::Placement::Contain, control_id: id,
    }, labels)
}
pub fn controls<'a, Message: Clone + 'a>(fit: Message, close: Message, fit_id: &'static str, close_id: &'static str) -> Element<'a, Message> {
    row![container(button("Fit").on_press(fit)).id(fit_id), text("Wheel to zoom · right-drag to pan").size(12),
        space::horizontal(), container(button("×").on_press(close)).id(close_id)].spacing(7).into()
}
