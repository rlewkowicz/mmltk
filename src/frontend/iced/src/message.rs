use crate::transport::TransportEvent;
use crate::view::{diagnostics, file_dialog, router, settings, status};

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
    Status(status::Message),
    Diagnostics(diagnostics::Message),
    Integration(crate::integration_control::Message),
}
