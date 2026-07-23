//! Reliably remove a directory and all of its children.
//!
//! This library re-exports `std::fs::remove_dir_all` for Linux.

#![deny(missing_debug_implementations)]
#![deny(missing_docs)]



pub use std::fs::remove_dir_all;
