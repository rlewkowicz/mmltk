// under the Apache-2.0 license. Accordingly, this file is released under
// the Apache License, Version 2.0 which can be found at the calendrical_calculations
// package root or at http://www.apache.org/licenses/LICENSE-2.0.

use crate::astronomy::Location;
use crate::rata_die::{Moment, RataDie};
#[allow(unused_imports)]
use core_maths::*;

pub(crate) trait IntegerRoundings {
    fn div_ceil(self, rhs: Self) -> Self;
}

impl IntegerRoundings for i64 {
    fn div_ceil(self, rhs: Self) -> Self {
        let d = self / rhs;
        let r = self % rhs;
        if (r > 0 && rhs > 0) || (r < 0 && rhs < 0) {
            d + 1
        } else {
            d
        }
    }
}


pub(crate) fn poly(x: f64, coeffs: &[f64]) -> f64 {
    coeffs.iter().rev().fold(0.0, |a, c| a * x + c)
}

pub(crate) fn binary_search(
    mut l: f64,
    mut h: f64,
    test: impl Fn(f64) -> bool,
    epsilon: f64,
) -> f64 {
    debug_assert!(l < h);

    loop {
        let mid = l + (h - l) / 2.0;

        (l, h) = if test(mid) { (l, mid) } else { (mid, h) };

        if (h - l) < epsilon {
            return mid;
        }
    }
}

pub(crate) fn next_moment<F>(mut index: Moment, location: Location, condition: F) -> RataDie
where
    F: Fn(Moment, Location) -> bool,
{
    loop {
        if condition(index, location) {
            return index.as_rata_die();
        }
        index += 1.0;
    }
}

pub(crate) fn next<F>(mut index: RataDie, condition: F) -> RataDie
where
    F: Fn(RataDie) -> bool,
{
    loop {
        if condition(index) {
            return index;
        }
        index += 1;
    }
}

pub(crate) fn next_u8<F>(mut index: u8, condition: F) -> u8
where
    F: Fn(u8) -> bool,
{
    loop {
        if condition(index) {
            return index;
        }
        index += 1;
    }
}

pub(crate) fn final_func<F>(mut index: i32, condition: F) -> i32
where
    F: Fn(i32) -> bool,
{
    while condition(index) {
        index += 1;
    }
    index - 1
}


pub(crate) fn invert_angular<F: Fn(f64) -> f64>(f: F, y: f64, r: (f64, f64)) -> f64 {
    binary_search(r.0, r.1, |x| (f(x) - y).rem_euclid(360.0) < 180.0, 1e-5)
}


/// Error returned when casting from an i32
#[derive(Copy, Clone, Debug, displaydoc::Display)]
#[allow(clippy::exhaustive_enums)] 
pub enum I32CastError {
    /// Less than i32::MIN
    BelowMin,
    /// Greater than i32::MAX
    AboveMax,
}

impl core::error::Error for I32CastError {}

impl I32CastError {
    /// Recovers the value saturated to `i32:::MIN..=i32::MAX`.
    pub const fn saturate(self) -> i32 {
        match self {
            I32CastError::BelowMin => i32::MIN,
            I32CastError::AboveMax => i32::MAX,
        }
    }
}

/// Convert an i64 to i32 and with information on which way it was out of bounds if so
#[inline]
pub const fn i64_to_i32(input: i64) -> Result<i32, I32CastError> {
    if input < i32::MIN as i64 {
        Err(I32CastError::BelowMin)
    } else if input > i32::MAX as i64 {
        Err(I32CastError::AboveMax)
    } else {
        Ok(input as i32)
    }
}

/// Convert an i64 to i32 but saturate at th ebounds
#[inline]
pub(crate) fn i64_to_saturated_i32(input: i64) -> i32 {
    i64_to_i32(input).unwrap_or_else(|i| i.saturate())
}


/// returns the weekday (0-6) after (strictly) the fixed date
pub(crate) const fn k_day_after(weekday: i64, fixed: RataDie) -> RataDie {
    let day_of_week = fixed.to_i64_date().rem_euclid(7);
    let beginning_of_week = fixed.to_i64_date() - day_of_week;
    let day = beginning_of_week + weekday;
    RataDie::new(day + if weekday <= day_of_week { 7 } else { 0 })
}
