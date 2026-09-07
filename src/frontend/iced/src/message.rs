use crate::transport::TransportEvent;
use crate::view::{diagnostics, error_modal, file_dialog, router, settings};

#[derive(Debug, Clone)]
pub enum Message {
    Transport(TransportEvent),
    Window(iced::window::Event),
    Presentation(crate::app::presentation::Message),
    ExploreWritable {
        peer_generation: u64,
        result: Result<(), crate::transport_connection::OutboundSendError>,
    },
    Workspace(router::Message),
    FileDialog(file_dialog::Message),
    Settings(settings::Message),
    Error(error_modal::Message),
    Diagnostics(diagnostics::Message),
    Integration(crate::integration_control::Message),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn root_message_surface_wraps_component_messages() {
        let messages = [
            Message::Workspace(router::Message::Navigation(
                crate::view::navigation::Message::SettingsRequested,
            )),
            Message::FileDialog(file_dialog::Message::StopRequested),
            Message::Settings(settings::Message::Close),
            Message::Error(error_modal::Message::Dismiss),
            Message::Diagnostics(diagnostics::Message::Toggled),
            Message::Integration(crate::integration_control::Message::Located {
                control: "fixture".into(),
                bounds: iced::Rectangle::default(),
            }),
        ];
        assert_eq!(messages.len(), 6);
    }
}
