// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

#![allow(clippy::excessive_precision)]

use super::{eval_rational_poly, eval_rational_poly_simd};
use jxl_simd::{F32SimdVec, I32SimdVec, SimdDescriptor, shl, shr};
use std::f32::consts::{PI, SQRT_2};

const POW2F_NUMER_COEFFS: [f32; 3] = [1.01749063e1, 4.88687798e1, 9.85506591e1];
const POW2F_DENOM_COEFFS: [f32; 4] = [2.10242958e-1, -2.22328856e-2, -1.94414990e1, 9.85506633e1];

#[inline(always)]
pub fn fast_cos(x: f32) -> f32 {
    let pi2 = PI * 2.0;
    let pi2_inv = 0.5 / PI;
    let npi2 = (x * pi2_inv).floor() * pi2;
    let xmodpi2 = x - npi2;
    let x_pi = xmodpi2.min(pi2 - xmodpi2);
    let above_pihalf = x_pi >= PI / 2.0;
    let x_pihalf = if above_pihalf { PI - x_pi } else { x_pi };
    let xs = x_pihalf * 0.25;
    let x2 = xs * xs;
    let x4 = x2 * x2;
    let cosx_prescaling = x4 * 0.06960438 + (x2 * -0.84087373 + 1.68179268);
    let cosx_scale1 = cosx_prescaling * cosx_prescaling - SQRT_2;
    let cosx_scale2 = cosx_scale1 * cosx_scale1 - 1.0;
    if above_pihalf {
        -cosx_scale2
    } else {
        cosx_scale2
    }
}

#[inline(always)]
pub fn fast_erff(x: f32) -> f32 {
    let absx = x.abs();
    let denom1 = absx * 7.77394369e-02 + 2.05260015e-04;
    let denom2 = denom1 * absx + 2.32120216e-01;
    let denom3 = denom2 * absx + 2.77820801e-01;
    let denom4 = denom3 * absx + 1.0;
    let denom5 = denom4 * denom4;
    let inv_denom5 = 1.0 / denom5;
    let result = -inv_denom5 * inv_denom5 + 1.0;
    result.copysign(x)
}

#[inline(always)]
pub fn fast_erff_simd<D: SimdDescriptor>(d: D, x: D::F32Vec) -> D::F32Vec {
    let absx = x.abs();
    let denom1 = absx.mul_add(
        D::F32Vec::splat(d, 7.77394369e-02),
        D::F32Vec::splat(d, 2.05260015e-04),
    );
    let denom2 = denom1.mul_add(absx, D::F32Vec::splat(d, 2.32120216e-01));
    let denom3 = denom2.mul_add(absx, D::F32Vec::splat(d, 2.77820801e-01));
    let denom4 = denom3.mul_add(absx, D::F32Vec::splat(d, 1.0));
    let denom5 = denom4 * denom4;
    let inv_denom5 = D::F32Vec::splat(d, 1.0) / denom5;
    let result = D::F32Vec::splat(d, 1.0) - inv_denom5 * inv_denom5;
    result.copysign(x)
}

#[inline(always)]
pub fn fast_pow2f(x: f32) -> f32 {
    let x_floor = x.floor();
    let exp = f32::from_bits(((x_floor as i32 + 127) as u32) << 23);
    let frac = x - x_floor;

    let num = frac + POW2F_NUMER_COEFFS[0];
    let num = num * frac + POW2F_NUMER_COEFFS[1];
    let num = num * frac + POW2F_NUMER_COEFFS[2];
    let num = num * exp;

    let den = POW2F_DENOM_COEFFS[0] * frac + POW2F_DENOM_COEFFS[1];
    let den = den * frac + POW2F_DENOM_COEFFS[2];
    let den = den * frac + POW2F_DENOM_COEFFS[3];

    num / den
}

#[inline(always)]
pub fn fast_pow2f_simd<D: SimdDescriptor>(d: D, x: D::F32Vec) -> D::F32Vec {
    let x_floor = x.floor();
    let exp = shl!(x_floor.as_i32() + D::I32Vec::splat(d, 127), 23).bitcast_to_f32();
    let frac = x - x_floor;

    let num = frac + D::F32Vec::splat(d, POW2F_NUMER_COEFFS[0]);
    let num = num.mul_add(frac, D::F32Vec::splat(d, POW2F_NUMER_COEFFS[1]));
    let num = num.mul_add(frac, D::F32Vec::splat(d, POW2F_NUMER_COEFFS[2]));
    let num = num * exp;

    let den = D::F32Vec::splat(d, POW2F_DENOM_COEFFS[0])
        .mul_add(frac, D::F32Vec::splat(d, POW2F_DENOM_COEFFS[1]));
    let den = den.mul_add(frac, D::F32Vec::splat(d, POW2F_DENOM_COEFFS[2]));
    let den = den.mul_add(frac, D::F32Vec::splat(d, POW2F_DENOM_COEFFS[3]));

    num / den
}

const LOG2F_P: [f32; 3] = [
    -1.8503833400518310e-6,
    1.4287160470083755,
    7.4245873327820566e-1,
];
const LOG2F_Q: [f32; 3] = [
    9.9032814277590719e-1,
    1.0096718572241148,
    1.7409343003366853e-1,
];

#[inline(always)]
pub fn fast_log2f(x: f32) -> f32 {
    let x_bits = x.to_bits() as i32;
    let exp_bits = x_bits.wrapping_sub(0x3f2aaaab);
    let exp_shifted = exp_bits >> 23;
    let mantissa = f32::from_bits((x_bits.wrapping_sub(exp_shifted << 23)) as u32);
    let exp_val = exp_shifted as f32;

    let x = mantissa - 1.0;
    eval_rational_poly(x, LOG2F_P, LOG2F_Q) + exp_val
}

#[inline(always)]
pub fn fast_log2f_simd<D: SimdDescriptor>(d: D, x: D::F32Vec) -> D::F32Vec {
    let x_bits = x.bitcast_to_i32();
    let exp_bits = x_bits - D::I32Vec::splat(d, 0x3f2aaaab);
    let exp_shifted = shr!(exp_bits, 23);
    let mantissa = (x_bits - shl!(exp_shifted, 23)).bitcast_to_f32();
    let exp_val = exp_shifted.as_f32();

    let x = mantissa - D::F32Vec::splat(d, 1.0);
    eval_rational_poly_simd(d, x, LOG2F_P, LOG2F_Q) + exp_val
}

#[inline(always)]
pub fn fast_powf(base: f32, exp: f32) -> f32 {
    fast_pow2f(fast_log2f(base) * exp)
}

#[inline(always)]
pub fn fast_powf_simd<D: SimdDescriptor>(d: D, base: D::F32Vec, exp: D::F32Vec) -> D::F32Vec {
    fast_pow2f_simd(d, fast_log2f_simd(d, base) * exp)
}

#[inline(always)]
pub fn floor_log2_nonzero(x: u64) -> u32 {
    (u64::BITS as usize - 1) as u32 ^ x.leading_zeros()
}
