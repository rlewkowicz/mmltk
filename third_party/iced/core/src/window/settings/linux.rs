//! Platform specific settings for Linux.

/// The platform specific window settings of an application.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct PlatformSpecific {
    /// Sets the application id of the window.
    pub application_id: String,
    /// Whether to bypass window-manager mapping for X11 windows.
    pub override_redirect: bool,
}
