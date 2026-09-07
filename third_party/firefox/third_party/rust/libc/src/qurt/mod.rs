//! Interface to QuRT (Qualcomm Real-Time OS) C library
//!
//! This module re-exports items from the new module structure.
//! QuRT was introduced after the `src/new/` module structure was established,
//! so all definitions live in `src/new/qurt/` and are re-exported here
//! for compatibility with the existing libc structure.

pub use crate::new::qurt::*;

cfg_if! {
    if #[cfg(target_arch = "hexagon")] {
    } else {
    }
}
