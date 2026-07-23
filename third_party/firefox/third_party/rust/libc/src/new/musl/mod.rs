//! Musl libc.
//!
//! * Headers: <https://git.musl-libc.org/cgit/musl> (official)
//! * Headers: <https://github.com/kraj/musl> (mirror)

mod arch;

pub(crate) mod bits {
    cfg_if! {
        if #[cfg(target_arch = "mips")] {
            pub(crate) use super::arch::mips::bits::socket;
        } else if #[cfg(target_arch = "mips64")] {
            pub(crate) use super::arch::mips64::bits::socket;
        } else {
        }
    }
}

pub(crate) mod pthread;

/// Directory: `sys/`
///
/// <https://github.com/kraj/musl/tree/kraj/master/include/sys>
pub(crate) mod sys {
    pub(crate) mod socket;
}

pub(crate) mod sched;
pub(crate) mod unistd;
