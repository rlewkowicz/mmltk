use super::copysignf;
use super::truncf;
use core::f32;

pub fn roundf(x: f32) -> f32 {
    truncf(x + copysignf(0.5 - 0.25 * f32::EPSILON, x))
}

