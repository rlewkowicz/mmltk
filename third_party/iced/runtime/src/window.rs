use crate::core::time::Instant;
use crate::core::window::{
    Direction, Event, Icon, Id, Level, Mode, Screenshot, Settings, UserAttention,
};
use crate::core::{Point, Size, Window};
use crate::futures::Subscription;
use crate::futures::event;
use crate::futures::futures::channel::oneshot;
use crate::task::{self, Task};

pub enum Action {
        Open(Id, Settings, oneshot::Sender<Id>),

        Close(Id),

        GetOldest(oneshot::Sender<Option<Id>>),

        GetLatest(oneshot::Sender<Option<Id>>),

                        Drag(Id),

                        DragResize(Id, Direction),

        Resize(Id, Size),

        GetSize(Id, oneshot::Sender<Size>),

        GetMaximized(Id, oneshot::Sender<bool>),

        Maximize(Id, bool),

                    GetMinimized(Id, oneshot::Sender<Option<bool>>),

        Minimize(Id, bool),

        GetPosition(Id, oneshot::Sender<Option<Point>>),

        GetScaleFactor(Id, oneshot::Sender<f32>),

                Move(Id, Point),

        SetMode(Id, Mode),

        GetMode(Id, oneshot::Sender<Mode>),

        ToggleMaximize(Id),

                        ToggleDecorations(Id),

                                                        RequestUserAttention(Id, Option<UserAttention>),

                                            GainFocus(Id),

        SetLevel(Id, Level),

                    ShowSystemMenu(Id),

        GetRawId(Id, oneshot::Sender<u64>),

                                                            SetIcon(Id, Icon),

        Run(Id, Box<dyn FnOnce(&dyn Window) + Send>),

        Screenshot(Id, oneshot::Sender<Screenshot>),

                    EnableMousePassthrough(Id),

                    DisableMousePassthrough(Id),

        SetMinSize(Id, Option<Size>),

        SetMaxSize(Id, Option<Size>),

        SetResizable(Id, bool),

        SetResizeIncrements(Id, Option<Size>),

        GetMonitorSize(Id, oneshot::Sender<Option<Size>>),

                SetAllowAutomaticTabbing(bool),

        RedrawAll,

        RelayoutAll,
}

pub fn frames() -> Subscription<Instant> {
    event::listen_raw(|event, _status, _window| match event {
        crate::core::Event::Window(Event::RedrawRequested(at)) => Some(at),
        _ => None,
    })
}

pub fn events() -> Subscription<(Id, Event)> {
    event::listen_with(|event, _status, id| {
        if let crate::core::Event::Window(event) = event {
            Some((id, event))
        } else {
            None
        }
    })
}

pub fn open_events() -> Subscription<Id> {
    event::listen_with(|event, _status, id| {
        if let crate::core::Event::Window(Event::Opened { .. }) = event {
            Some(id)
        } else {
            None
        }
    })
}

pub fn close_events() -> Subscription<Id> {
    event::listen_with(|event, _status, id| {
        if let crate::core::Event::Window(Event::Closed) = event {
            Some(id)
        } else {
            None
        }
    })
}

pub fn resize_events() -> Subscription<(Id, Size)> {
    event::listen_with(|event, _status, id| {
        if let crate::core::Event::Window(Event::Resized(size)) = event {
            Some((id, size))
        } else {
            None
        }
    })
}

pub fn close_requests() -> Subscription<Id> {
    event::listen_with(|event, _status, id| {
        if let crate::core::Event::Window(Event::CloseRequested) = event {
            Some(id)
        } else {
            None
        }
    })
}

pub fn open(settings: Settings) -> (Id, Task<Id>) {
    let id = Id::unique();

    (
        id,
        task::oneshot(|channel| crate::Action::Window(Action::Open(id, settings, channel))),
    )
}

pub fn close<T>(id: Id) -> Task<T> {
    task::effect(crate::Action::Window(Action::Close(id)))
}

pub fn oldest() -> Task<Option<Id>> {
    task::oneshot(|channel| crate::Action::Window(Action::GetOldest(channel)))
}

pub fn latest() -> Task<Option<Id>> {
    task::oneshot(|channel| crate::Action::Window(Action::GetLatest(channel)))
}

pub fn drag<T>(id: Id) -> Task<T> {
    task::effect(crate::Action::Window(Action::Drag(id)))
}

pub fn drag_resize<T>(id: Id, direction: Direction) -> Task<T> {
    task::effect(crate::Action::Window(Action::DragResize(id, direction)))
}

pub fn resize<T>(id: Id, new_size: Size) -> Task<T> {
    task::effect(crate::Action::Window(Action::Resize(id, new_size)))
}

pub fn set_resizable<T>(id: Id, resizable: bool) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetResizable(id, resizable)))
}

pub fn set_max_size<T>(id: Id, size: Option<Size>) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetMaxSize(id, size)))
}

pub fn set_min_size<T>(id: Id, size: Option<Size>) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetMinSize(id, size)))
}

pub fn set_resize_increments<T>(id: Id, increments: Option<Size>) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetResizeIncrements(
        id, increments,
    )))
}

pub fn size(id: Id) -> Task<Size> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetSize(id, channel)))
}

pub fn is_maximized(id: Id) -> Task<bool> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetMaximized(id, channel)))
}

pub fn maximize<T>(id: Id, maximized: bool) -> Task<T> {
    task::effect(crate::Action::Window(Action::Maximize(id, maximized)))
}

pub fn is_minimized(id: Id) -> Task<Option<bool>> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetMinimized(id, channel)))
}

pub fn minimize<T>(id: Id, minimized: bool) -> Task<T> {
    task::effect(crate::Action::Window(Action::Minimize(id, minimized)))
}

pub fn position(id: Id) -> Task<Option<Point>> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetPosition(id, channel)))
}

pub fn scale_factor(id: Id) -> Task<f32> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetScaleFactor(id, channel)))
}

pub fn move_to<T>(id: Id, position: Point) -> Task<T> {
    task::effect(crate::Action::Window(Action::Move(id, position)))
}

pub fn mode(id: Id) -> Task<Mode> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetMode(id, channel)))
}

pub fn set_mode<T>(id: Id, mode: Mode) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetMode(id, mode)))
}

pub fn toggle_maximize<T>(id: Id) -> Task<T> {
    task::effect(crate::Action::Window(Action::ToggleMaximize(id)))
}

pub fn toggle_decorations<T>(id: Id) -> Task<T> {
    task::effect(crate::Action::Window(Action::ToggleDecorations(id)))
}

pub fn request_user_attention<T>(id: Id, user_attention: Option<UserAttention>) -> Task<T> {
    task::effect(crate::Action::Window(Action::RequestUserAttention(
        id,
        user_attention,
    )))
}

pub fn gain_focus<T>(id: Id) -> Task<T> {
    task::effect(crate::Action::Window(Action::GainFocus(id)))
}

pub fn set_level<T>(id: Id, level: Level) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetLevel(id, level)))
}

pub fn show_system_menu<T>(id: Id) -> Task<T> {
    task::effect(crate::Action::Window(Action::ShowSystemMenu(id)))
}

pub fn raw_id<Message>(id: Id) -> Task<u64> {
    task::oneshot(|channel| crate::Action::Window(Action::GetRawId(id, channel)))
}

pub fn set_icon<T>(id: Id, icon: Icon) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetIcon(id, icon)))
}

pub fn run<T>(id: Id, f: impl FnOnce(&dyn Window) -> T + Send + 'static) -> Task<T>
where
    T: Send + 'static,
{
    task::oneshot(move |channel| {
        crate::Action::Window(Action::Run(
            id,
            Box::new(move |handle| {
                let _ = channel.send(f(handle));
            }),
        ))
    })
}

pub fn screenshot(id: Id) -> Task<Screenshot> {
    task::oneshot(move |channel| crate::Action::Window(Action::Screenshot(id, channel)))
}

pub fn enable_mouse_passthrough<Message>(id: Id) -> Task<Message> {
    task::effect(crate::Action::Window(Action::EnableMousePassthrough(id)))
}

pub fn disable_mouse_passthrough<Message>(id: Id) -> Task<Message> {
    task::effect(crate::Action::Window(Action::DisableMousePassthrough(id)))
}

pub fn monitor_size(id: Id) -> Task<Option<Size>> {
    task::oneshot(move |channel| crate::Action::Window(Action::GetMonitorSize(id, channel)))
}

pub fn allow_automatic_tabbing<T>(enabled: bool) -> Task<T> {
    task::effect(crate::Action::Window(Action::SetAllowAutomaticTabbing(
        enabled,
    )))
}
