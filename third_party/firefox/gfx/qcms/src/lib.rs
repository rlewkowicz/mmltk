/*! A pure Rust color management library.
*/

#![allow(dead_code)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![cfg_attr(all(target_arch = "arm", feature = "neon"), feature(stdarch_arm_neon_intrinsics))]
#![cfg_attr(all(target_arch = "arm", feature = "neon"), feature(stdarch_arm_feature_detection))]
#![cfg_attr(
    all(target_arch = "arm", feature = "neon"),
    feature(arm_target_feature)
)]

/// These values match the Rendering Intent values from the ICC spec
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub enum Intent {
    AbsoluteColorimetric = 3,
    Saturation = 2,
    RelativeColorimetric = 1,
    Perceptual = 0,
}

use Intent::*;

impl Default for Intent {
    fn default() -> Self {
        Perceptual
    }
}

pub(crate) type s15Fixed16Number = i32;

#[inline]
fn s15Fixed16Number_to_float(a: s15Fixed16Number) -> f32 {
    a as f32 / 65536.
}

#[inline]
fn double_to_s15Fixed16Number(v: f64) -> s15Fixed16Number {
    (v * 65536.) as i32
}

#[cfg(feature = "c_bindings")]
extern crate libc;
#[cfg(feature = "c_bindings")]
pub mod c_bindings;
mod chain;
mod iccread;
mod matrix;
mod transform;
pub use iccread::qcms_CIE_xyY as CIE_xyY;
pub use iccread::qcms_CIE_xyYTRIPLE as CIE_xyYTRIPLE;
pub use iccread::Profile;
pub use transform::DataType;
pub use transform::Transform;

mod transform_avx;

mod transform_sse2;
mod transform_util;
