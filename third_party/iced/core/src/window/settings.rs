#[cfg(target_os = "windows")]
#[path = "settings/windows.rs"]
pub mod platform;

#[cfg(target_os = "macos")]
#[path = "settings/macos.rs"]
mod platform;

#[cfg(target_os = "linux")]
#[path = "settings/linux.rs"]
mod platform;

#[cfg(target_arch = "wasm32")]
#[path = "settings/wasm.rs"]
mod platform;

#[cfg(not(any(
    target_os = "windows",
    target_os = "macos",
    target_os = "linux",
    target_arch = "wasm32"
)))]
#[path = "settings/other.rs"]
mod platform;

use crate::Size;
use crate::window::{Icon, Level, Position};

pub use platform::PlatformSpecific;

#[derive(Debug, Clone)]
pub struct Settings {
        pub size: Size,

        pub maximized: bool,

        pub fullscreen: bool,

        pub position: Position,

        pub min_size: Option<Size>,

        pub max_size: Option<Size>,

        pub visible: bool,

        pub resizable: bool,

        pub closeable: bool,

        pub minimizable: bool,

        pub decorations: bool,

        pub transparent: bool,

                                            pub blur: bool,

        pub level: Level,

        pub icon: Option<Icon>,

        pub platform_specific: PlatformSpecific,

                                    pub exit_on_close_request: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            size: Size::new(1024.0, 768.0),
            maximized: false,
            fullscreen: false,
            position: Position::default(),
            min_size: None,
            max_size: None,
            visible: true,
            resizable: true,
            minimizable: true,
            closeable: true,
            decorations: true,
            transparent: false,
            blur: false,
            level: Level::default(),
            icon: None,
            exit_on_close_request: true,
            platform_specific: PlatformSpecific::default(),
        }
    }
}
