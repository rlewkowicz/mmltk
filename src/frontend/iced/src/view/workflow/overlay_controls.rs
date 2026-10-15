use crate::fluent_theme::Element;
use iced::widget::{checkbox, container, row};
#[derive(Debug, Clone, Copy)]
pub enum Message {
    Labels(bool),
    Masks(bool),
    Boxes(bool),
    Keypoints(bool),
}
#[derive(Debug, Clone, Copy)]
pub enum KeypointSettingsMessage {
    Keypoints(bool),
    Uncertainty(bool),
}
pub fn update_keypoint_settings(
    settings: &mut crate::view::settings::SettingsModel,
    message: KeypointSettingsMessage,
) -> Result<crate::view::settings::EditSchedule, String> {
    settings.edit(
        crate::view::settings::EditCadence::Immediate,
        |draft| match message {
            KeypointSettingsMessage::Keypoints(value) => {
                crate::generated::edit_workflowspredictshowkeypoints(draft, value)
            }
            KeypointSettingsMessage::Uncertainty(value) => {
                crate::generated::edit_workflowspredictshowuncertainty(draft, value)
            }
        },
    )
}
pub fn view(
    labels: bool,
    masks: bool,
    boxes: bool,
    keypoints: bool,
    labels_enabled: bool,
    native_enabled: bool,
    ids: [&'static str; 4],
) -> Element<'static, Message> {
    row![
        container(
            checkbox(labels)
                .label("Labels")
                .on_toggle_maybe(labels_enabled.then_some(Message::Labels))
        )
        .id(ids[0]),
        container(
            checkbox(masks)
                .label("Masks")
                .on_toggle_maybe(native_enabled.then_some(Message::Masks))
        )
        .id(ids[1]),
        container(
            checkbox(boxes)
                .label("Boxes")
                .on_toggle_maybe(native_enabled.then_some(Message::Boxes))
        )
        .id(ids[2]),
        container(
            checkbox(keypoints)
                .label("Skeletons")
                .on_toggle_maybe(native_enabled.then_some(Message::Keypoints))
        )
        .id(ids[3]),
    ]
    .spacing(7)
    .align_y(iced::Center)
    .into()
}
