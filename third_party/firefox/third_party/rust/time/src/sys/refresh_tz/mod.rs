#[path = "unix.rs"]
mod imp;

/// Update time zone information from the system.
///
/// For safety documentation, see [`time::util::refresh_tz`].
#[inline]
pub(crate) unsafe fn refresh_tz_unchecked() {
    unsafe { imp::refresh_tz_unchecked() }
}

/// Attempt to update time zone information from the system.
///
/// Returns `None` if the call is not known to be sound.
#[inline]
pub(crate) fn refresh_tz() -> Option<()> {
    imp::refresh_tz()
}
