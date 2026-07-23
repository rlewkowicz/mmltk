use crate::core::event;
use crate::core::time::Instant;
use crate::core::window;

#[derive(Debug, Clone)]
pub struct Action<Message> {
    message_to_publish: Option<Message>,
    redraw_request: window::RedrawRequest,
    event_status: event::Status,
}

impl<Message> Action<Message> {
    fn new() -> Self {
        Self {
            message_to_publish: None,
            redraw_request: window::RedrawRequest::Wait,
            event_status: event::Status::Ignored,
        }
    }

                        pub fn capture() -> Self {
        Self {
            event_status: event::Status::Captured,
            ..Self::new()
        }
    }

                    pub fn publish(message: Message) -> Self {
        Self {
            message_to_publish: Some(message),
            ..Self::new()
        }
    }

            pub fn request_redraw() -> Self {
        Self {
            redraw_request: window::RedrawRequest::NextFrame,
            ..Self::new()
        }
    }

                        pub fn request_redraw_at(at: Instant) -> Self {
        Self {
            redraw_request: window::RedrawRequest::At(at),
            ..Self::new()
        }
    }

        pub fn and_capture(mut self) -> Self {
        self.event_status = event::Status::Captured;
        self
    }

                    pub fn into_inner(self) -> (Option<Message>, window::RedrawRequest, event::Status) {
        (
            self.message_to_publish,
            self.redraw_request,
            self.event_status,
        )
    }
}
