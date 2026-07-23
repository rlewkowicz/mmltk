//! Implementation of the Eisel-Lemire algorithm.
//!
//! This is adapted from [fast-float-rust](https://github.com/aldanor/fast-float-rust),
//! a port of [fast_float](https://github.com/fastfloat/fast_float) to Rust.

#![cfg(not(feature = "compact"))]
#![doc(hidden)]

use crate::extended_float::ExtendedFloat;
use crate::num::Float;
use crate::number::Number;
use crate::table::{LARGEST_POWER_OF_FIVE, POWER_OF_FIVE_128, SMALLEST_POWER_OF_FIVE};

/// Ensure truncation of digits doesn't affect our computation, by doing 2 passes.
#[inline]
pub fn lemire<F: Float>(num: &Number) -> ExtendedFloat {
    let mut fp = compute_float::<F>(num.exponent, num.mantissa);
    if num.many_digits && fp.exp >= 0 && fp != compute_float::<F>(num.exponent, num.mantissa + 1) {
        fp = compute_error::<F>(num.exponent, num.mantissa);
    }
    fp
}

/// Compute a float using an extended-precision representation.
///
/// Fast conversion of a the significant digits and decimal exponent
/// a float to a extended representation with a binary float. This
/// algorithm will accurately parse the vast majority of cases,
/// and uses a 128-bit representation (with a fallback 192-bit
/// representation).
///
/// This algorithm scales the exponent by the decimal exponent
/// using pre-computed powers-of-5, and calculates if the
/// representation can be unambiguously rounded to the nearest
/// machine float. Near-halfway cases are not handled here,
/// and are represented by a negative, biased binary exponent.
///
/// The algorithm is described in detail in "Daniel Lemire, Number Parsing
/// at a Gigabyte per Second" in section 5, "Fast Algorithm", and
/// section 6, "Exact Numbers And Ties", available online:
/// <https://arxiv.org/abs/2101.11408.pdf>.
pub fn compute_float<F: Float>(q: i32, mut w: u64) -> ExtendedFloat {
    let fp_zero = ExtendedFloat {
        mant: 0,
        exp: 0,
    };
    let fp_inf = ExtendedFloat {
        mant: 0,
        exp: F::INFINITE_POWER,
    };

    if w == 0 || q < F::SMALLEST_POWER_OF_TEN {
        return fp_zero;
    } else if q > F::LARGEST_POWER_OF_TEN {
        return fp_inf;
    }
    let lz = w.leading_zeros() as i32;
    w <<= lz;
    let (lo, hi) = compute_product_approx(q, w, F::MANTISSA_SIZE as usize + 3);
    if lo == 0xFFFF_FFFF_FFFF_FFFF {
        let inside_safe_exponent = (q >= -27) && (q <= 55);
        if !inside_safe_exponent {
            return compute_error_scaled::<F>(q, hi, lz);
        }
    }
    let upperbit = (hi >> 63) as i32;
    let mut mantissa = hi >> (upperbit + 64 - F::MANTISSA_SIZE - 3);
    let mut power2 = power(q) + upperbit - lz - F::MINIMUM_EXPONENT;
    if power2 <= 0 {
        if -power2 + 1 >= 64 {
            return fp_zero;
        }
        mantissa >>= -power2 + 1;
        mantissa += mantissa & 1;
        mantissa >>= 1;
        power2 = (mantissa >= (1_u64 << F::MANTISSA_SIZE)) as i32;
        return ExtendedFloat {
            mant: mantissa,
            exp: power2,
        };
    }
    if lo <= 1
        && q >= F::MIN_EXPONENT_ROUND_TO_EVEN
        && q <= F::MAX_EXPONENT_ROUND_TO_EVEN
        && mantissa & 3 == 1
        && (mantissa << (upperbit + 64 - F::MANTISSA_SIZE - 3)) == hi
    {
        mantissa &= !1_u64;
    }
    mantissa += mantissa & 1;
    mantissa >>= 1;
    if mantissa >= (2_u64 << F::MANTISSA_SIZE) {
        mantissa = 1_u64 << F::MANTISSA_SIZE;
        power2 += 1;
    }
    mantissa &= !(1_u64 << F::MANTISSA_SIZE);
    if power2 >= F::INFINITE_POWER {
        return fp_inf;
    }
    ExtendedFloat {
        mant: mantissa,
        exp: power2,
    }
}

/// Fallback algorithm to calculate the non-rounded representation.
/// This calculates the extended representation, and then normalizes
/// the resulting representation, so the high bit is set.
#[inline]
pub fn compute_error<F: Float>(q: i32, mut w: u64) -> ExtendedFloat {
    let lz = w.leading_zeros() as i32;
    w <<= lz;
    let hi = compute_product_approx(q, w, F::MANTISSA_SIZE as usize + 3).1;
    compute_error_scaled::<F>(q, hi, lz)
}

/// Compute the error from a mantissa scaled to the exponent.
#[inline]
pub fn compute_error_scaled<F: Float>(q: i32, mut w: u64, lz: i32) -> ExtendedFloat {
    let hilz = (w >> 63) as i32 ^ 1;
    w <<= hilz;
    let power2 = power(q as i32) + F::EXPONENT_BIAS - hilz - lz - 62;

    ExtendedFloat {
        mant: w,
        exp: power2 + F::INVALID_FP,
    }
}

/// Calculate a base 2 exponent from a decimal exponent.
/// This uses a pre-computed integer approximation for
/// log2(10), where 217706 / 2^16 is accurate for the
/// entire range of non-finite decimal exponents.
#[inline]
fn power(q: i32) -> i32 {
    (q.wrapping_mul(152_170 + 65536) >> 16) + 63
}

#[inline]
fn full_multiplication(a: u64, b: u64) -> (u64, u64) {
    let r = (a as u128) * (b as u128);
    (r as u64, (r >> 64) as u64)
}

fn compute_product_approx(q: i32, w: u64, precision: usize) -> (u64, u64) {
    debug_assert!(q >= SMALLEST_POWER_OF_FIVE);
    debug_assert!(q <= LARGEST_POWER_OF_FIVE);
    debug_assert!(precision <= 64);

    let mask = if precision < 64 {
        0xFFFF_FFFF_FFFF_FFFF_u64 >> precision
    } else {
        0xFFFF_FFFF_FFFF_FFFF_u64
    };

    let index = (q - SMALLEST_POWER_OF_FIVE) as usize;
    let (lo5, hi5) = POWER_OF_FIVE_128[index];
    let (mut first_lo, mut first_hi) = full_multiplication(w, lo5);
    if first_hi & mask == mask {
        let (_, second_hi) = full_multiplication(w, hi5);
        first_lo = first_lo.wrapping_add(second_hi);
        if second_hi > first_lo {
            first_hi += 1;
        }
    }
    (first_lo, first_hi)
}
