#![doc(
    html_logo_url = "https://raw.githubusercontent.com/iced-rs/iced/bdf0430880f5c29443f5f0a0ae4895866dfef4c6/docs/logo.svg"
)]
#![cfg_attr(docsrs, feature(doc_cfg))]
use iced_widget::renderer;
use iced_winit as shell;
use iced_winit::core;
use iced_winit::program;
use iced_winit::runtime;

pub use iced_futures::futures;
pub use iced_futures::stream;

#[cfg(not(any(
    target_arch = "wasm32",
    feature = "thread-pool",
    feature = "tokio",
    feature = "smol"
)))]
compile_error!(
    "No futures executor has been enabled! You must enable an \
    executor feature.\n\
    Available options: thread-pool, tokio, or smol."
);

#[cfg(all(
    target_family = "unix",
    not(target_os = "macos"),
    not(feature = "wayland"),
    not(feature = "x11"),
))]
compile_error!(
    "No Unix display server backend has been enabled. You must enable a \
    display server feature.\n\
    Available options: x11, wayland."
);

#[cfg(feature = "highlighter")]
pub use iced_highlighter as highlighter;

#[cfg(feature = "wgpu-bare")]
pub use iced_renderer::wgpu::wgpu;

mod error;

pub mod application;
pub mod daemon;
pub mod time;
pub mod window;

#[cfg(feature = "advanced")]
pub mod advanced;

pub use crate::core::alignment;
pub use crate::core::animation;
pub use crate::core::border;
pub use crate::core::color;
pub use crate::core::gradient;
pub use crate::core::padding;
pub use crate::core::theme;
pub use crate::core::{
    Alignment, Animation, Background, Border, Color, ContentFit, Degrees, Function, Gradient,
    Length, Never, Padding, Pixels, Point, Radians, Rectangle, Rotation, Settings, Shadow, Size,
    Theme, Transformation, Vector, never,
};
pub use crate::program::Preset;
pub use crate::program::message;
pub use crate::runtime::exit;
pub use iced_futures::Subscription;

pub use Alignment::Center;
pub use Length::{Fill, FillPortion, Fit, Shrink};
pub use alignment::Horizontal::{Left, Right};
pub use alignment::Vertical::{Bottom, Top};

pub mod debug {
        pub use iced_debug::{Span, time, time_with};
}

pub mod task {
        pub use crate::runtime::task::{Handle, Task};

    #[cfg(feature = "sipper")]
    pub use crate::runtime::task::{Never, Sipper, Straw, sipper, stream};
}

pub mod clipboard {
        pub use crate::core::clipboard::{Content, Error, Kind};
    pub use crate::runtime::clipboard::{read, read_files, read_html, read_text, write};

    #[cfg(feature = "image")]
    pub use crate::core::clipboard::Image;

    #[cfg(feature = "image")]
    pub use crate::runtime::clipboard::read_image;
}

pub mod executor {
        pub use iced_futures::Executor;
    pub use iced_futures::backend::default::Executor as Default;
}

pub mod font {
        pub use crate::core::font::*;
    pub use crate::runtime::font::*;
}

pub mod event {
        pub use crate::core::event::{Event, Status};
    pub use iced_futures::event::{listen, listen_raw, listen_url, listen_with};
}

pub mod keyboard {
        pub use crate::core::keyboard::key;
    pub use crate::core::keyboard::{Event, Key, Location, Modifiers};
    pub use iced_futures::keyboard::listen;
}

pub mod mouse {
        pub use crate::core::mouse::{Button, Cursor, Event, Interaction, ScrollDelta};
}

pub mod system {
        pub use crate::runtime::system::{theme, theme_changes};

    #[cfg(feature = "sysinfo")]
    pub use crate::runtime::system::{Information, information};
}

pub mod overlay {
                        pub type Element<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer> =
        crate::core::overlay::Element<'a, Message, Theme, Renderer>;

    pub use iced_widget::overlay::*;
}

pub mod touch {
        pub use crate::core::touch::{Event, Finger};
}

#[allow(hidden_glob_reexports)]
pub mod widget {
        pub use iced_runtime::widget::*;
    pub use iced_widget::*;

    #[cfg(feature = "image-without-codecs")]
    pub mod image {
                pub use iced_runtime::image::{Allocation, Error, allocate};
        pub use iced_widget::image::*;
    }

    mod core {}
    mod graphics {}
    mod renderer {}
}

pub mod backend {
        pub use iced_core::backend::*;
    pub use iced_runtime::backend::*;
}

pub use application::Application;
pub use backend::Backend;
pub use backend::PowerPreference;
pub use daemon::Daemon;
pub use error::Error;
pub use event::Event;
pub use executor::Executor;
pub use font::Font;
pub use program::Program;
pub use renderer::Renderer;
pub use task::Task;
pub use window::Window;

#[doc(inline)]
pub use application::application;
#[doc(inline)]
pub use daemon::daemon;

pub type Element<'a, Message, Theme = crate::Theme, Renderer = crate::Renderer> =
    crate::core::Element<'a, Message, Theme, Renderer>;

pub type Result = std::result::Result<(), Error>;

pub fn run<State, Message, Theme, Renderer>(
    update: impl application::UpdateFn<State, Message> + 'static,
    view: impl for<'a> application::ViewFn<'a, State, Message, Theme, Renderer> + 'static,
) -> Result
where
    State: Default + 'static,
    Message: Send + message::MaybeDebug + message::MaybeClone + 'static,
    Theme: theme::Base + 'static,
    Renderer: program::Renderer + 'static,
{
    application(State::default, update, view).run()
}
