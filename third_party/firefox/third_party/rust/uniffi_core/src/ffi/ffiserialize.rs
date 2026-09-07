/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::{Handle, RustBuffer, RustCallStatus, RustCallStatusCode};
use std::{mem::ManuallyDrop, ptr::NonNull};

/// FFIBuffer element
///
/// This is the union of all possible primitive FFI types.
/// Composite FFI types like `RustBuffer` and `RustCallStatus` are stored using multiple elements.
#[repr(C)]
#[derive(Clone, Copy)]
pub union FfiBufferElement {
    pub u8: u8,
    pub i8: i8,
    pub u16: u16,
    pub i16: i16,
    pub u32: u32,
    pub i32: i32,
    pub u64: u64,
    pub i64: i64,
    pub float: std::ffi::c_float,
    pub double: std::ffi::c_double,
    pub ptr: *const std::ffi::c_void,
}

impl Default for FfiBufferElement {
    fn default() -> Self {
        Self { u64: 0 }
    }
}

/// Serialize a FFI value to a buffer
///
/// This trait allows FFI types to be read from/written to FFIBufferElement slices.
/// It's similar, to the [crate::Lift::read] and [crate::Lower::write] methods, but implemented on the FFI types rather than Rust types.
/// It's useful to compare the two:
///
/// - [crate::Lift] and [crate::Lower] are implemented on Rust types like String and user-defined records.
/// - [FfiSerialize] is implemented on the FFI types like RustBuffer, RustCallStatus, and vtable structs.
/// - All 3 traits are implemented for simple cases where the FFI type and Rust type are the same, for example numeric types.
/// - [FfiSerialize] uses FFIBuffer elements rather than u8 elements.  Using a union eliminates the need to cast values and creates better alignment.
/// - [FfiSerialize] uses a constant size to store each type.
///
/// [FfiSerialize] is used to generate alternate forms of the scaffolding functions that simplify work needed to implement the bindings on the other side.
/// This is currently only used in the gecko-js bindings for Firefox, but could maybe be useful for other external bindings or even some of the builtin bindings like Python/Kotlin.
///
/// The FFI-buffer version of the scaffolding functions:
///   - Input two pointers to ffi buffers, one to read arguments from and one to write the return value to.
///   - Rather than inputting an out pointer for `RustCallStatus` it's written to the return buffer after the normal return value.
///
pub trait FfiSerialize: Sized {
    /// Number of elements required to store this FFI type
    const SIZE: usize;

    /// Get a value from a ffi buffer
    ///
    /// Note: `buf` should be thought of as `&[FFIBufferElement; Self::SIZE]`, but it can't be spelled out that way
    /// since Rust doesn't support that usage of const generics yet.
    fn get(buf: &[FfiBufferElement]) -> Self;

    /// Put a value to a ffi buffer
    ///
    /// Note: `buf` should be thought of as `&[FFIBufferElement; Self::SIZE]`, but it can't be spelled out that way
    /// since Rust doesn't support that usage of const generics yet.
    fn put(buf: &mut [FfiBufferElement], value: Self);

    /// Read a value from a ffi buffer ref and advance it
    ///
    /// buf must have a length of at least `Self::Size`
    fn read(buf: &mut &[FfiBufferElement]) -> Self {
        let value = Self::get(buf);
        *buf = &buf[Self::SIZE..];
        value
    }

    /// Write a value to a ffi buffer ref and advance it
    ///
    /// buf must have a length of at least `Self::Size`
    fn write(buf: &mut &mut [FfiBufferElement], value: Self) {
        Self::put(buf, value);
        let (_, new_buf) = ::core::mem::take(buf).split_at_mut(Self::SIZE);
        *buf = new_buf;
    }
}

/// Get the FFI buffer size for list of types
#[macro_export]
macro_rules! ffi_buffer_size {
    ($($T:ty),* $(,)?) => {
        (
            0
            $(
                + <$T as $crate::FfiSerialize>::SIZE
            )*
        )
    }
}

macro_rules! define_ffi_serialize_simple_cases {
    ($(($name: ident, $T:ty)),* $(,)?) => {
        $(
            impl FfiSerialize for $T {
                const SIZE: usize = 1;

                fn get(buf: &[FfiBufferElement]) -> Self {
                    unsafe { buf[0].$name }
                }

                fn put(buf: &mut[FfiBufferElement], value: Self) {
                    buf[0].$name = value
                }
            }
        )*
    };
}

define_ffi_serialize_simple_cases! {
    (i8, i8),
    (u8, u8),
    (i16, i16),
    (u16, u16),
    (i32, i32),
    (u32, u32),
    (i64, i64),
    (u64, u64),
    (ptr, *const std::ffi::c_void),
}

impl FfiSerialize for f32 {
    const SIZE: usize = 1;

    fn get(buf: &[FfiBufferElement]) -> Self {
        unsafe { buf[0].float as Self }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].float = value as std::ffi::c_float;
    }
}

impl FfiSerialize for f64 {
    const SIZE: usize = 1;

    fn get(buf: &[FfiBufferElement]) -> Self {
        unsafe { buf[0].double as Self }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].double = value as std::ffi::c_double;
    }
}

impl FfiSerialize for bool {
    const SIZE: usize = 1;

    fn get(buf: &[FfiBufferElement]) -> Self {
        unsafe { buf[0].i8 == 1 }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].i8 = if value { 1 } else { 0 }
    }
}

impl FfiSerialize for () {
    const SIZE: usize = 0;

    fn get(_buf: &[FfiBufferElement]) -> Self {}

    fn put(_buf: &mut [FfiBufferElement], _value: Self) {}
}

impl<T> FfiSerialize for NonNull<T> {
    const SIZE: usize = 1;

    fn get(buf: &[FfiBufferElement]) -> Self {
        unsafe { Self::new_unchecked(buf[0].ptr as *mut T) }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].ptr = value.as_ptr() as *const std::ffi::c_void
    }
}

impl FfiSerialize for Handle {
    const SIZE: usize = 1;

    fn get(buf: &[FfiBufferElement]) -> Self {
        unsafe { Handle::from_raw_unchecked(buf[0].u64) }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].u64 = value.as_raw()
    }
}

impl FfiSerialize for RustBuffer {
    const SIZE: usize = 3;

    fn get(buf: &[FfiBufferElement]) -> Self {
        let (capacity, len, data) = unsafe { (buf[0].u64, buf[1].u64, buf[2].ptr as *mut u8) };
        unsafe { RustBuffer::from_raw_parts(data, len, capacity) }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].u64 = value.capacity;
        buf[1].u64 = value.len;
        buf[2].ptr = value.data as *const std::ffi::c_void;
    }
}

impl FfiSerialize for RustCallStatus {
    const SIZE: usize = 4;

    fn get(buf: &[FfiBufferElement]) -> Self {
        let code = unsafe { buf[0].i8 };
        Self {
            code: RustCallStatusCode::try_from(code).unwrap_or(RustCallStatusCode::UnexpectedError),
            error_buf: ManuallyDrop::new(RustBuffer::get(&buf[1..])),
        }
    }

    fn put(buf: &mut [FfiBufferElement], value: Self) {
        buf[0].i8 = value.code as i8;
        RustBuffer::put(&mut buf[1..], ManuallyDrop::into_inner(value.error_buf))
    }
}
