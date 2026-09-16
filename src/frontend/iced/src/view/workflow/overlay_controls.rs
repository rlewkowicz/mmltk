use crate::fluent_theme::Element;
use iced::widget::{checkbox, container, row};
#[derive(Debug, Clone, Copy)]
pub enum Message { Labels(bool), Masks(bool), Boxes(bool) }
pub fn view(labels: bool, masks: bool, boxes: bool, labels_enabled: bool, native_enabled: bool,
    ids: [&'static str; 3]) -> Element<'static, Message> {
    row![
        container(checkbox(labels).label("Labels").on_toggle_maybe(labels_enabled.then_some(Message::Labels))).id(ids[0]),
        container(checkbox(masks).label("Masks").on_toggle_maybe(native_enabled.then_some(Message::Masks))).id(ids[1]),
        container(checkbox(boxes).label("Boxes").on_toggle_maybe(native_enabled.then_some(Message::Boxes))).id(ids[2]),
    ].spacing(7).align_y(iced::Center).into()
}
