//! A method to obtain the local offset from UTC.

#![allow(
    clippy::missing_const_for_fn,
    reason = "system APIs are inherently not const, so this will only trigger on the fallback"
)]

#[cfg_attr(target_family = "windows", path = "windows.rs")]
#[path = "unix.rs"]
mod imp;

use crate::{OffsetDateTime, UtcOffset};

/// Attempt to obtain the system's UTC offset. If the offset cannot be determined, `None` is
/// returned.
#[inline]
pub(crate) fn local_offset_at(datetime: OffsetDateTime) -> Option<UtcOffset> {
    imp::local_offset_at(datetime)
}
