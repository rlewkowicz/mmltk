//! Time-related operations.

mod clock;
#[cfg(any(linux_kernel, target_os = "fuchsia", target_os = "illumos"))]
mod timerfd;

pub use clock::*;
#[cfg(any(linux_kernel, target_os = "fuchsia", target_os = "illumos"))]
pub use timerfd::*;
