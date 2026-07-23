//! An implementation of Clinger's Bellerophon algorithm.
//!
//! This is a moderate path algorithm that uses an extended-precision
//! float, represented in 80 bits, by calculating the bits of slop
//! and determining if those bits could prevent unambiguous rounding.
//!
//! This algorithm requires less static storage than the Lemire algorithm,
//! and has decent performance, and is therefore used when non-decimal,
//! non-power-of-two strings need to be parsed. Clinger's algorithm
//! is described in depth in "How to Read Floating Point Numbers Accurately.",
//! available online [here](http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.45.4152&rep=rep1&type=pdf).
//!
//! This implementation is loosely based off the Golang implementation,
//! found [here](https://github.com/golang/go/blob/b10849fbb97a2244c086991b4623ae9f32c212d0/src/strconv/extfloat.go).
//! This code is therefore subject to a 3-clause BSD license.

#![cfg(feature = "compact")]
#![doc(hidden)]

use crate::extended_float::ExtendedFloat;
use crate::mask::{lower_n_halfway, lower_n_mask};
use crate::num::Float;
use crate::number::Number;
use crate::rounding::{round, round_nearest_tie_even};
use crate::table::BASE10_POWERS;


/// Core implementation of the Bellerophon algorithm.
///
/// Create an extended-precision float, scale it to the proper radix power,
/// calculate the bits of slop, and return the representation. The value
/// will always be guaranteed to be within 1 bit, rounded-down, of the real
/// value. If a negative exponent is returned, this represents we were
/// unable to unambiguously round the significant digits.
///
/// This has been modified to return a biased, rather than unbiased exponent.
pub fn bellerophon<F: Float>(num: &Number) -> ExtendedFloat {
    let fp_zero = ExtendedFloat {
        mant: 0,
        exp: 0,
    };
    let fp_inf = ExtendedFloat {
        mant: 0,
        exp: F::INFINITE_POWER,
    };

    if num.mantissa == 0 || num.exponent <= -0x1000 {
        return fp_zero;
    } else if num.exponent >= 0x1000 {
        return fp_inf;
    }

    let exponent = num.exponent as i32 + BASE10_POWERS.bias;
    let small_index = exponent % BASE10_POWERS.step;
    let large_index = exponent / BASE10_POWERS.step;

    if exponent < 0 {
        return fp_zero;
    }
    if large_index as usize >= BASE10_POWERS.large.len() {
        return fp_inf;
    }


    let mut errors: u32 = 0;
    if num.many_digits {
        errors += error_halfscale();
    }

    let mut fp = ExtendedFloat {
        mant: num.mantissa,
        exp: 0,
    };
    match fp.mant.overflowing_mul(BASE10_POWERS.get_small_int(small_index as usize)) {
        (_, true) => {
            normalize(&mut fp);
            fp = mul(&fp, &BASE10_POWERS.get_small(small_index as usize));
            errors += error_halfscale();
        },
        (mant, false) => {
            fp.mant = mant;
            normalize(&mut fp);
        },
    }

    fp = mul(&fp, &BASE10_POWERS.get_large(large_index as usize));
    if errors > 0 {
        errors += 1;
    }
    errors += error_halfscale();

    let shift = normalize(&mut fp);
    errors <<= shift;
    fp.exp += F::EXPONENT_BIAS;

    if -fp.exp + 1 > 65 {
        return fp_zero;
    }

    if !error_is_accurate::<F>(errors, &fp) {
        fp.exp += F::INVALID_FP;
        return fp;
    }

    if -fp.exp + 1 == 65 {
        return fp_zero;
    }

    round::<F, _>(&mut fp, |f, s| {
        round_nearest_tie_even(f, s, |is_odd, is_halfway, is_above| {
            is_above || (is_odd && is_halfway)
        });
    });
    fp
}



/// Get the full error scale.
#[inline(always)]
const fn error_scale() -> u32 {
    8
}

/// Get the half error scale.
#[inline(always)]
const fn error_halfscale() -> u32 {
    error_scale() / 2
}

/// Determine if the number of errors is tolerable for float precision.
fn error_is_accurate<F: Float>(errors: u32, fp: &ExtendedFloat) -> bool {
    debug_assert!(fp.exp >= -64);


    let mantissa_shift = 64 - F::MANTISSA_SIZE - 1;

    let extrabits = match fp.exp <= -mantissa_shift {
        true => 1 - fp.exp,
        false => 64 - F::MANTISSA_SIZE - 1,
    };

    let maskbits = extrabits as u64;
    let errors = errors as u64;

    if extrabits > 64 {
        !fp.mant.overflowing_add(errors).1
    } else {
        let mask = lower_n_mask(maskbits);
        let extra = fp.mant & mask;

        let halfway = lower_n_halfway(maskbits);
        let cmp1 = halfway.wrapping_sub(errors) < extra;
        let cmp2 = extra < halfway.wrapping_add(errors);

        !(cmp1 && cmp2)
    }
}


/// Normalize float-point number.
///
/// Shift the mantissa so the number of leading zeros is 0, or the value
/// itself is 0.
///
/// Get the number of bytes shifted.
pub fn normalize(fp: &mut ExtendedFloat) -> i32 {

    if fp.mant != 0 {
        let shift = fp.mant.leading_zeros() as i32;
        fp.mant <<= shift;
        fp.exp -= shift;
        shift
    } else {
        0
    }
}

/// Multiply two normalized extended-precision floats, as if by `a*b`.
///
/// The precision is maximal when the numbers are normalized, however,
/// decent precision will occur as long as both values have high bits
/// set. The result is not normalized.
///
/// Algorithm:
///     1. Non-signed multiplication of mantissas (requires 2x as many bits as input).
///     2. Normalization of the result (not done here).
///     3. Addition of exponents.
pub fn mul(x: &ExtendedFloat, y: &ExtendedFloat) -> ExtendedFloat {
    debug_assert!(x.mant >> 32 != 0);
    debug_assert!(y.mant >> 32 != 0);

    const LOMASK: u64 = 0xffff_ffff;
    let x1 = x.mant >> 32;
    let x0 = x.mant & LOMASK;
    let y1 = y.mant >> 32;
    let y0 = y.mant & LOMASK;

    let x1_y0 = x1 * y0;
    let x0_y1 = x0 * y1;
    let x0_y0 = x0 * y0;
    let x1_y1 = x1 * y1;

    let mut tmp = (x1_y0 & LOMASK) + (x0_y1 & LOMASK) + (x0_y0 >> 32);
    tmp += 1 << (32 - 1);

    ExtendedFloat {
        mant: x1_y1 + (x1_y0 >> 32) + (x0_y1 >> 32) + (tmp >> 32),
        exp: x.exp + y.exp + 64,
    }
}


/// Precalculated powers of base N for the Bellerophon algorithm.
pub struct BellerophonPowers {
    pub small: &'static [u64],
    pub large: &'static [u64],
    /// Pre-calculated small powers as 64-bit integers
    pub small_int: &'static [u64],
    pub step: i32,
    pub bias: i32,
    /// ceil(log2(radix)) scaled as a multiplier.
    pub log2: i64,
    /// Bitshift for the log2 multiplier.
    pub log2_shift: i32,
}

/// Allow indexing of values without bounds checking
impl BellerophonPowers {
    #[inline]
    pub fn get_small(&self, index: usize) -> ExtendedFloat {
        let mant = self.small[index];
        let exp = (1 - 64) + ((self.log2 * index as i64) >> self.log2_shift);
        ExtendedFloat {
            mant,
            exp: exp as i32,
        }
    }

    #[inline]
    pub fn get_large(&self, index: usize) -> ExtendedFloat {
        let mant = self.large[index];
        let biased_e = index as i64 * self.step as i64 - self.bias as i64;
        let exp = (1 - 64) + ((self.log2 * biased_e) >> self.log2_shift);
        ExtendedFloat {
            mant,
            exp: exp as i32,
        }
    }

    #[inline]
    pub fn get_small_int(&self, index: usize) -> u64 {
        self.small_int[index]
    }
}
