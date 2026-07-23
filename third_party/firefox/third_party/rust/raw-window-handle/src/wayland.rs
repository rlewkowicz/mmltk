use core::ffi::c_void;
use core::ptr::NonNull;

/// Raw display handle for Wayland.
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WaylandDisplayHandle {
    /// A pointer to a `wl_display`.
    pub display: NonNull<c_void>,
}

impl WaylandDisplayHandle {
    pub fn new(display: NonNull<c_void>) -> Self {
        Self { display }
    }
}

/// Raw window handle for Wayland.
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WaylandWindowHandle {
    /// A pointer to a `wl_surface`.
    pub surface: NonNull<c_void>,
}

impl WaylandWindowHandle {
    pub fn new(surface: NonNull<c_void>) -> Self {
        Self { surface }
    }
}
