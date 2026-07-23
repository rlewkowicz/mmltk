
//! Estimate the error in an 80-bit approximation of a float.
//!
//! This estimates the error in a floating-point representation.
//!
//! This implementation is loosely based off the Golang implementation,
//! found here: <https://golang.org/src/strconv/atof.go>

use super::float::*;
use super::num::*;
use super::rounding::*;

pub(crate) trait FloatErrors {
    /// Get the full error scale.
    fn error_scale() -> u32;
    /// Get the half error scale.
    fn error_halfscale() -> u32;
    /// Determine if the number of errors is tolerable for float precision.
    fn error_is_accurate<F: Float>(count: u32, fp: &ExtendedFloat) -> bool;
}

/// Check if the error is accurate with a round-nearest rounding scheme.
#[inline]
fn nearest_error_is_accurate(errors: u64, fp: &ExtendedFloat, extrabits: u64) -> bool {
    if extrabits == 65 {
        !fp.mant.overflowing_add(errors).1
    } else {
        let mask: u64 = lower_n_mask(extrabits);
        let extra: u64 = fp.mant & mask;

        let halfway: u64 = lower_n_halfway(extrabits);
        let cmp1 = halfway.wrapping_sub(errors) < extra;
        let cmp2 = extra < halfway.wrapping_add(errors);

        !(cmp1 && cmp2)
    }
}

impl FloatErrors for u64 {
    #[inline]
    fn error_scale() -> u32 {
        8
    }

    #[inline]
    fn error_halfscale() -> u32 {
        u64::error_scale() / 2
    }

    #[inline]
    fn error_is_accurate<F: Float>(count: u32, fp: &ExtendedFloat) -> bool {
        let bias = -(F::EXPONENT_BIAS - F::MANTISSA_SIZE);
        let denormal_exp = bias - 63;
        let extrabits = if fp.exp <= denormal_exp {
            64 - F::MANTISSA_SIZE + denormal_exp - fp.exp
        } else {
            63 - F::MANTISSA_SIZE
        };


        let extrabits = extrabits as u64;
        let errors = count as u64;
        if extrabits > 65 {
            return true;
        }

        nearest_error_is_accurate(errors, fp, extrabits)
    }
}
