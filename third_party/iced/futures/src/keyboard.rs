use crate::core;
use crate::core::keyboard::Event;
use crate::subscription::{self, Subscription};

pub fn listen() -> Subscription<Event> {
    #[derive(Hash)]
    struct Listen;

    subscription::filter_map(Listen, move |event| match event {
        subscription::Event::Interaction {
            event: core::Event::Keyboard(event),
            status: core::event::Status::Ignored,
            ..
        } => Some(event),
        _ => None,
    })
}
