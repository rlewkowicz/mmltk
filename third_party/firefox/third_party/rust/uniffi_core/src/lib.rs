/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! # Runtime support code for uniffi
//!
//! This crate provides the small amount of runtime code that is required by the generated uniffi
//! component scaffolding in order to transfer data back and forth across the C-style FFI layer,
//! as well as some utilities for testing the generated bindings.
//!
//! The key concept here is the [`FfiConverter`] trait, which is responsible for converting between
//! a Rust type and a low-level C-style type that can be passed across the FFI:
//!
//!  * How to [represent](FfiConverter::FfiType) values of the Rust type in the low-level C-style type
//!    system of the FFI layer.
//!  * How to ["lower"](FfiConverter::lower) values of the Rust type into an appropriate low-level
//!    FFI value.
//!  * How to ["lift"](FfiConverter::try_lift) low-level FFI values back into values of the Rust
//!    type.
//!  * How to [write](FfiConverter::write) values of the Rust type into a buffer, for cases
//!    where they are part of a compound data structure that is serialized for transfer.
//!  * How to [read](FfiConverter::try_read) values of the Rust type from buffer, for cases
//!    where they are received as part of a compound data structure that was serialized for transfer.
//!  * How to [return](FfiConverter::lower_return) values of the Rust type from scaffolding
//!    functions.
//!
//! This logic encapsulates the Rust-side handling of data transfer. Each foreign-language binding
//! must also implement a matching set of data-handling rules for each data type.
//!
//! In addition to the core `FfiConverter` trait, we provide a handful of struct definitions useful
//! for passing core rust types over the FFI, such as [`RustBuffer`].

#![warn(rust_2018_idioms, unused_qualifications)]

/// Print out tracing information for FFI calls if the `ffi-trace` feature is enabled
#[cfg(feature = "ffi-trace")]
#[macro_export]
macro_rules! trace {
    ($($tt:tt)*) => {
        ::std::println!($($tt)*);
    }
}

#[cfg(not(feature = "ffi-trace"))]
#[macro_export]
macro_rules! trace {
    ($($tt:tt)*) => {};
}

use anyhow::bail;
use bytes::buf::Buf;

pub use anyhow::Result;

pub mod ffi;
mod ffi_converter_impls;
mod ffi_converter_traits;
pub mod metadata;
mod oneshot;

#[cfg(feature = "scaffolding-ffi-buffer-fns")]
pub use ffi::ffiserialize::FfiBufferElement;
pub use ffi::*;
pub use ffi_converter_traits::{
    ConvertError, FfiConverter, FfiConverterArc, HandleAlloc, Lift, LiftRef, LiftReturn, Lower,
    LowerError, LowerReturn, TypeId,
};
pub use metadata::*;

pub mod deps {
    pub use crate::trace;
    pub use anyhow;
    #[cfg(feature = "tokio")]
    pub use async_compat;
    pub use bytes;
    pub use static_assertions;
}

const PACKAGE_VERSION: &str = env!("CARGO_PKG_VERSION");

static_assertions::const_assert!(PACKAGE_VERSION.len() < 10);

/// Check whether the uniffi runtime version is compatible a given uniffi_bindgen version.
///
/// The result of this check may be used to ensure that generated Rust scaffolding is
/// using a compatible version of the uniffi runtime crate. It's a `const fn` so that it
/// can be used to perform such a check at compile time.
#[allow(clippy::len_zero)]
pub const fn check_compatible_version(bindgen_version: &'static str) -> bool {
    let package_version = PACKAGE_VERSION.as_bytes();
    let bindgen_version = bindgen_version.as_bytes();
    package_version.len() == bindgen_version.len()
        && (package_version.len() == 0 || package_version[0] == bindgen_version[0])
        && (package_version.len() <= 1 || package_version[1] == bindgen_version[1])
        && (package_version.len() <= 2 || package_version[2] == bindgen_version[2])
        && (package_version.len() <= 3 || package_version[3] == bindgen_version[3])
        && (package_version.len() <= 4 || package_version[4] == bindgen_version[4])
        && (package_version.len() <= 5 || package_version[5] == bindgen_version[5])
        && (package_version.len() <= 6 || package_version[6] == bindgen_version[6])
        && (package_version.len() <= 7 || package_version[7] == bindgen_version[7])
        && (package_version.len() <= 8 || package_version[8] == bindgen_version[8])
        && (package_version.len() <= 9 || package_version[9] == bindgen_version[9])
        && package_version.len() < 10
}

/// Assert that the uniffi runtime version matches an expected value.
///
/// This is a helper hook for the generated Rust scaffolding, to produce a compile-time
/// error if the version of `uniffi_bindgen` used to generate the scaffolding was
/// incompatible with the version of `uniffi` being used at runtime.
#[macro_export]
macro_rules! assert_compatible_version {
    ($v:expr $(,)?) => {
        uniffi::deps::static_assertions::const_assert!(uniffi::check_compatible_version($v));
    };
}

/// Struct to use when we want to lift/lower/serialize types inside the `uniffi` crate.
struct UniFfiTag;

/// A helper function to ensure we don't read past the end of a buffer.
///
/// Rust won't actually let us read past the end of a buffer, but the `Buf` trait does not support
/// returning an explicit error in this case, and will instead panic. This is a look-before-you-leap
/// helper function to instead return an explicit error, to help with debugging.
pub fn check_remaining(buf: &[u8], num_bytes: usize) -> Result<()> {
    if buf.remaining() < num_bytes {
        bail!(
            "not enough bytes remaining in buffer ({} < {num_bytes})",
            buf.remaining(),
        );
    }
    Ok(())
}

/// Macro to implement lowering/lifting using a `RustBuffer`
///
/// For complex types where it's too fiddly or too unsafe to convert them into a special-purpose
/// C-compatible value, you can use this trait to implement `lower()` in terms of `write()` and
/// `lift` in terms of `read()`.
///
/// This macro implements the boilerplate needed to define `lower`, `lift` and `FFIType`.
#[macro_export]
macro_rules! ffi_converter_rust_buffer_lift_and_lower {
    ($uniffi_tag:ty) => {
        type FfiType = $crate::RustBuffer;

        fn lower(v: Self) -> $crate::RustBuffer {
            let mut buf = ::std::vec::Vec::new();
            <Self as $crate::FfiConverter<$uniffi_tag>>::write(v, &mut buf);
            $crate::RustBuffer::from_vec(buf)
        }

        fn try_lift(buf: $crate::RustBuffer) -> $crate::Result<Self> {
            let vec = buf.destroy_into_vec();
            let mut buf = vec.as_slice();
            let value = <Self as $crate::FfiConverter<$uniffi_tag>>::try_read(&mut buf)?;
            match $crate::deps::bytes::Buf::remaining(&buf) {
                0 => ::std::result::Result::Ok(value),
                n => $crate::deps::anyhow::bail!(
                    "junk data left in buffer after lifting (count: {n})",
                ),
            }
        }
    };
}
