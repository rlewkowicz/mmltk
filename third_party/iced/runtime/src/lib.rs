#![doc(
    html_logo_url = "https://raw.githubusercontent.com/iced-rs/iced/9ab6923e943f784985e9ef9ca28b10278297225d/docs/logo.svg"
)]
#![cfg_attr(docsrs, feature(doc_cfg))]
pub mod backend;
pub mod clipboard;
pub mod font;
pub mod image;
pub mod keyboard;
pub mod system;
pub mod task;
pub mod user_interface;
pub mod widget;
pub mod window;

pub use iced_core as core;
pub use iced_futures as futures;

pub use task::Task;
pub use user_interface::UserInterface;

use crate::core::Event;

use std::fmt;

pub enum Action<T> {
        Output(T),

        Widget(Box<dyn core::widget::Operation>),

        Clipboard(clipboard::Action),

        Window(window::Action),

        System(system::Action),

        Font(font::Action),

        Image(image::Action),

        Backend(backend::Action),

        Event {
                window: core::window::Id,
                event: Event,
    },

        Tick,

        Reload,

                    Exit,
}

impl<T> Action<T> {
        pub fn widget(operation: impl core::widget::Operation + 'static) -> Self {
        Self::Widget(Box::new(operation))
    }

    fn output<O>(self) -> Result<T, Action<O>> {
        match self {
            Action::Output(output) => Ok(output),
            Action::Widget(operation) => Err(Action::Widget(operation)),
            Action::Clipboard(action) => Err(Action::Clipboard(action)),
            Action::Window(action) => Err(Action::Window(action)),
            Action::System(action) => Err(Action::System(action)),
            Action::Font(action) => Err(Action::Font(action)),
            Action::Image(action) => Err(Action::Image(action)),
            Action::Backend(action) => Err(Action::Backend(action)),
            Action::Event { window, event } => Err(Action::Event { window, event }),
            Action::Tick => Err(Action::Tick),
            Action::Reload => Err(Action::Reload),
            Action::Exit => Err(Action::Exit),
        }
    }
}

impl<T> fmt::Debug for Action<T>
where
    T: fmt::Debug,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Action::Output(output) => write!(f, "Action::Output({output:?})"),
            Action::Widget { .. } => {
                write!(f, "Action::Widget")
            }
            Action::Clipboard(action) => {
                write!(f, "Action::Clipboard({action:?})")
            }
            Action::Window(_) => write!(f, "Action::Window"),
            Action::System(action) => write!(f, "Action::System({action:?})"),
            Action::Font(action) => {
                write!(f, "Action::Font({action:?})")
            }
            Action::Image(action) => write!(f, "Action::Image({action:?})"),
            Action::Backend(action) => write!(f, "Action::Backend({action:?})"),
            Action::Event { window, event } => write!(
                f,
                "Action::Event {{ window: {window:?}, event: {event:?} }}"
            ),
            Action::Tick => write!(f, "Action::Tick"),
            Action::Reload => write!(f, "Action::Reload"),
            Action::Exit => write!(f, "Action::Exit"),
        }
    }
}

pub fn exit<T>() -> Task<T> {
    task::effect(Action::Exit)
}
