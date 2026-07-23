use crate::MaybeSend;
use crate::core::event::{self, Event};
use crate::core::window;
use crate::subscription::{self, Subscription};

pub fn listen() -> Subscription<Event> {
    listen_with(|event, status, _window| match status {
        event::Status::Ignored => Some(event),
        event::Status::Captured => None,
    })
}

pub fn listen_with<Message>(
    f: fn(Event, event::Status, window::Id) -> Option<Message>,
) -> Subscription<Message>
where
    Message: 'static + MaybeSend,
{
    #[derive(Hash)]
    struct EventsWith;

    subscription::filter_map((EventsWith, f), move |event| match event {
        subscription::Event::Interaction {
            event: Event::Window(window::Event::RedrawRequested(_)),
            ..
        }
        | subscription::Event::SystemThemeChanged(_)
        | subscription::Event::PlatformSpecific(_) => None,
        subscription::Event::Interaction {
            window,
            event,
            status,
        } => f(event, status, window),
    })
}

pub fn listen_raw<Message>(
    f: fn(Event, event::Status, window::Id) -> Option<Message>,
) -> Subscription<Message>
where
    Message: 'static + MaybeSend,
{
    #[derive(Hash)]
    struct RawEvents;

    subscription::filter_map((RawEvents, f), move |event| match event {
        subscription::Event::Interaction {
            window,
            event,
            status,
        } => f(event, status, window),
        subscription::Event::SystemThemeChanged(_) | subscription::Event::PlatformSpecific(_) => {
            None
        }
    })
}

pub fn listen_url() -> Subscription<String> {
    #[derive(Hash)]
    struct ListenUrl;

    subscription::filter_map(ListenUrl, move |event| match event {
        subscription::Event::PlatformSpecific(subscription::PlatformSpecific::MacOS(
            subscription::MacOS::ReceivedUrl(url),
        )) => Some(url),
        _ => None,
    })
}
