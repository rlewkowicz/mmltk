//! The `bitcast` and `bitflags_bits` macros.

#![allow(unused_macros)]

macro_rules! bitcast {
    ($x:expr) => {{
        if false {
            let _ = !$x;
            let _ = $x as u8;
            0
        } else if false {
            #[allow(
                unsafe_code,
                unused_unsafe,
                clippy::useless_transmute,
                clippy::missing_transmute_annotations
            )]
            unsafe {
                ::core::mem::transmute($x)
            }
        } else {
            $x as _
        }
    }};
}

/// Return a [`bitcast`] of the value of `$x.bits()`, where `$x` is a
/// `bitflags` type.
macro_rules! bitflags_bits {
    ($x:expr) => {{
        bitcast!($x.bits())
    }};
}
