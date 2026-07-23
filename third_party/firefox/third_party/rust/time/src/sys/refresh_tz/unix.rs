/// Whether the operating system has a thread-safe environment. This allows bypassing the check for
/// if the process is multi-threaded.
const OS_HAS_THREAD_SAFE_ENVIRONMENT: bool = match std::env::consts::OS.as_bytes() {
    b"illumos"
    | b"netbsd"
    | b"macos"
    => true,
    _ => false,
};

/// Update time zone information from the system.
///
/// For safety documentation, see [`time::util::refresh_tz`].
#[inline]
pub(super) unsafe fn refresh_tz_unchecked() {
    unsafe extern "C" {
fn tzset();
    }

    unsafe { tzset() };
}

/// Attempt to update time zone information from the system. Returns `None` if the call is not known
/// to be sound.
#[inline]
pub(super) fn refresh_tz() -> Option<()> {

    if OS_HAS_THREAD_SAFE_ENVIRONMENT || num_threads::is_single_threaded() == Some(true) {
        unsafe { refresh_tz_unchecked() };
        Some(())
    } else {
        None
    }
}
