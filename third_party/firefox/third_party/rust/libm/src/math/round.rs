use super::copysign;
use super::trunc;
use core::f64;

pub fn round(x: f64) -> f64 {
    trunc(x + copysign(0.5 - 0.25 * f64::EPSILON, x))
}
