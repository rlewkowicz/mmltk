#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(feature = "alloc")]
extern crate alloc;

#[cfg(feature = "std")]
extern crate std;

mod borrowed;
mod wayland;

pub use borrowed::{DisplayHandle, HasDisplayHandle, HasWindowHandle, WindowHandle};
pub use wayland::{WaylandDisplayHandle, WaylandWindowHandle};

use core::fmt;

#[deprecated = "Use HasWindowHandle instead"]
pub unsafe trait HasRawWindowHandle {
    fn raw_window_handle(&self) -> Result<RawWindowHandle, HandleError>;
}

#[allow(deprecated)]
unsafe impl<T: HasWindowHandle + ?Sized> HasRawWindowHandle for T {
    fn raw_window_handle(&self) -> Result<RawWindowHandle, HandleError> {
        self.window_handle().map(Into::into)
    }
}

#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RawWindowHandle {
    Wayland(WaylandWindowHandle),
}

#[deprecated = "Use HasDisplayHandle instead"]
pub unsafe trait HasRawDisplayHandle {
    fn raw_display_handle(&self) -> Result<RawDisplayHandle, HandleError>;
}

#[allow(deprecated)]
unsafe impl<T: HasDisplayHandle + ?Sized> HasRawDisplayHandle for T {
    fn raw_display_handle(&self) -> Result<RawDisplayHandle, HandleError> {
        self.display_handle().map(Into::into)
    }
}

#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum RawDisplayHandle {
    Wayland(WaylandDisplayHandle),
}

#[derive(Debug, Clone)]
#[non_exhaustive]
pub enum HandleError {
    NotSupported,
    Unavailable,
}

impl fmt::Display for HandleError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotSupported => write!(
                f,
                "the underlying handle cannot be represented using the types in this crate"
            ),
            Self::Unavailable => write!(f, "the underlying handle is not available"),
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for HandleError {}

impl From<WaylandDisplayHandle> for RawDisplayHandle {
    fn from(value: WaylandDisplayHandle) -> Self {
        Self::Wayland(value)
    }
}

impl From<WaylandWindowHandle> for RawWindowHandle {
    fn from(value: WaylandWindowHandle) -> Self {
        Self::Wayland(value)
    }
}
