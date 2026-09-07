// Copyright 2012-2014 The Rust Project Developers. See the COPYRIGHT
// file at the top-level directory of this distribution and at
// http://rust-lang.org/COPYRIGHT.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Temporal quantification

#[cfg(all(not(feature = "std"), feature = "core-error"))]
use core::error::Error;
use core::fmt;
use core::ops::{Add, AddAssign, Div, Mul, Neg, Sub, SubAssign};
use core::time::Duration;
#[cfg(feature = "std")]
use std::error::Error;

use crate::{expect, try_opt};

#[cfg(any(feature = "rkyv", feature = "rkyv-16", feature = "rkyv-32", feature = "rkyv-64"))]
use rkyv::{Archive, Deserialize, Serialize};

/// The number of nanoseconds in a microsecond.
const NANOS_PER_MICRO: i32 = 1000;
/// The number of nanoseconds in a millisecond.
const NANOS_PER_MILLI: i32 = 1_000_000;
/// The number of nanoseconds in seconds.
pub(crate) const NANOS_PER_SEC: i32 = 1_000_000_000;
/// The number of microseconds per second.
const MICROS_PER_SEC: i64 = 1_000_000;
/// The number of milliseconds per second.
const MILLIS_PER_SEC: i64 = 1000;
/// The number of seconds in a minute.
const SECS_PER_MINUTE: i64 = 60;
/// The number of seconds in an hour.
const SECS_PER_HOUR: i64 = 3600;
/// The number of (non-leap) seconds in days.
const SECS_PER_DAY: i64 = 86_400;
/// The number of (non-leap) seconds in a week.
const SECS_PER_WEEK: i64 = 604_800;

/// Time duration with nanosecond precision.
///
/// This also allows for negative durations; see individual methods for details.
///
/// A `TimeDelta` is represented internally as a complement of seconds and
/// nanoseconds. The range is restricted to that of `i64` milliseconds, with the
/// minimum value notably being set to `-i64::MAX` rather than allowing the full
/// range of `i64::MIN`. This is to allow easy flipping of sign, so that for
/// instance `abs()` can be called without any checks.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
#[cfg_attr(any(feature = "rkyv", feature = "rkyv-16", feature = "rkyv-32", feature = "rkyv-64"), derive(Archive, Deserialize, Serialize),
    archive(compare(PartialEq, PartialOrd)),
    archive_attr(derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)))]
#[cfg_attr(feature = "rkyv-validation", archive(check_bytes))]
pub struct TimeDelta {
    secs: i64,
    nanos: i32, 
}

/// The minimum possible `TimeDelta`: `-i64::MAX` milliseconds.
pub(crate) const MIN: TimeDelta = TimeDelta {
    secs: -i64::MAX / MILLIS_PER_SEC - 1,
    nanos: NANOS_PER_SEC + (-i64::MAX % MILLIS_PER_SEC) as i32 * NANOS_PER_MILLI,
};

/// The maximum possible `TimeDelta`: `i64::MAX` milliseconds.
pub(crate) const MAX: TimeDelta = TimeDelta {
    secs: i64::MAX / MILLIS_PER_SEC,
    nanos: (i64::MAX % MILLIS_PER_SEC) as i32 * NANOS_PER_MILLI,
};

impl TimeDelta {
    /// Makes a new `TimeDelta` with given number of seconds and nanoseconds.
    ///
    /// # Errors
    ///
    /// Returns `None` when the duration is out of bounds, or if `nanos` ≥ 1,000,000,000.
    pub const fn new(secs: i64, nanos: u32) -> Option<TimeDelta> {
        if secs < MIN.secs
            || secs > MAX.secs
            || nanos >= 1_000_000_000
            || (secs == MAX.secs && nanos > MAX.nanos as u32)
            || (secs == MIN.secs && nanos < MIN.nanos as u32)
        {
            return None;
        }
        Some(TimeDelta { secs, nanos: nanos as i32 })
    }

    /// Makes a new `TimeDelta` with the given number of weeks.
    ///
    /// Equivalent to `TimeDelta::seconds(weeks * 7 * 24 * 60 * 60)` with
    /// overflow checks.
    ///
    /// # Panics
    ///
    /// Panics when the duration is out of bounds.
    #[inline]
    #[must_use]
    pub const fn weeks(weeks: i64) -> TimeDelta {
        expect(TimeDelta::try_weeks(weeks), "TimeDelta::weeks out of bounds")
    }

    /// Makes a new `TimeDelta` with the given number of weeks.
    ///
    /// Equivalent to `TimeDelta::try_seconds(weeks * 7 * 24 * 60 * 60)` with
    /// overflow checks.
    ///
    /// # Errors
    ///
    /// Returns `None` when the `TimeDelta` would be out of bounds.
    #[inline]
    pub const fn try_weeks(weeks: i64) -> Option<TimeDelta> {
        TimeDelta::try_seconds(try_opt!(weeks.checked_mul(SECS_PER_WEEK)))
    }

    /// Makes a new `TimeDelta` with the given number of days.
    ///
    /// Equivalent to `TimeDelta::seconds(days * 24 * 60 * 60)` with overflow
    /// checks.
    ///
    /// # Panics
    ///
    /// Panics when the `TimeDelta` would be out of bounds.
    #[inline]
    #[must_use]
    pub const fn days(days: i64) -> TimeDelta {
        expect(TimeDelta::try_days(days), "TimeDelta::days out of bounds")
    }

    /// Makes a new `TimeDelta` with the given number of days.
    ///
    /// Equivalent to `TimeDelta::try_seconds(days * 24 * 60 * 60)` with overflow
    /// checks.
    ///
    /// # Errors
    ///
    /// Returns `None` when the `TimeDelta` would be out of bounds.
    #[inline]
    pub const fn try_days(days: i64) -> Option<TimeDelta> {
        TimeDelta::try_seconds(try_opt!(days.checked_mul(SECS_PER_DAY)))
    }

    /// Makes a new `TimeDelta` with the given number of hours.
    ///
    /// Equivalent to `TimeDelta::seconds(hours * 60 * 60)` with overflow checks.
    ///
    /// # Panics
    ///
    /// Panics when the `TimeDelta` would be out of bounds.
    #[inline]
    #[must_use]
    pub const fn hours(hours: i64) -> TimeDelta {
        expect(TimeDelta::try_hours(hours), "TimeDelta::hours out of bounds")
    }

    /// Makes a new `TimeDelta` with the given number of hours.
    ///
    /// Equivalent to `TimeDelta::try_seconds(hours * 60 * 60)` with overflow checks.
    ///
    /// # Errors
    ///
    /// Returns `None` when the `TimeDelta` would be out of bounds.
    #[inline]
    pub const fn try_hours(hours: i64) -> Option<TimeDelta> {
        TimeDelta::try_seconds(try_opt!(hours.checked_mul(SECS_PER_HOUR)))
    }

    /// Makes a new `TimeDelta` with the given number of minutes.
    ///
    /// Equivalent to `TimeDelta::seconds(minutes * 60)` with overflow checks.
    ///
    /// # Panics
    ///
    /// Panics when the `TimeDelta` would be out of bounds.
    #[inline]
    #[must_use]
    pub const fn minutes(minutes: i64) -> TimeDelta {
        expect(TimeDelta::try_minutes(minutes), "TimeDelta::minutes out of bounds")
    }

    /// Makes a new `TimeDelta` with the given number of minutes.
    ///
    /// Equivalent to `TimeDelta::try_seconds(minutes * 60)` with overflow checks.
    ///
    /// # Errors
    ///
    /// Returns `None` when the `TimeDelta` would be out of bounds.
    #[inline]
    pub const fn try_minutes(minutes: i64) -> Option<TimeDelta> {
        TimeDelta::try_seconds(try_opt!(minutes.checked_mul(SECS_PER_MINUTE)))
    }

    /// Makes a new `TimeDelta` with the given number of seconds.
    ///
    /// # Panics
    ///
    /// Panics when `seconds` is more than `i64::MAX / 1_000` or less than `-i64::MAX / 1_000`
    /// (in this context, this is the same as `i64::MIN / 1_000` due to rounding).
    #[inline]
    #[must_use]
    pub const fn seconds(seconds: i64) -> TimeDelta {
        expect(TimeDelta::try_seconds(seconds), "TimeDelta::seconds out of bounds")
    }

    /// Makes a new `TimeDelta` with the given number of seconds.
    ///
    /// # Errors
    ///
    /// Returns `None` when `seconds` is more than `i64::MAX / 1_000` or less than
    /// `-i64::MAX / 1_000` (in this context, this is the same as `i64::MIN / 1_000` due to
    /// rounding).
    #[inline]
    pub const fn try_seconds(seconds: i64) -> Option<TimeDelta> {
        TimeDelta::new(seconds, 0)
    }

    /// Makes a new `TimeDelta` with the given number of milliseconds.
    ///
    /// # Panics
    ///
    /// Panics when the `TimeDelta` would be out of bounds, i.e. when `milliseconds` is more than
    /// `i64::MAX` or less than `-i64::MAX`. Notably, this is not the same as `i64::MIN`.
    #[inline]
    pub const fn milliseconds(milliseconds: i64) -> TimeDelta {
        expect(TimeDelta::try_milliseconds(milliseconds), "TimeDelta::milliseconds out of bounds")
    }

    /// Makes a new `TimeDelta` with the given number of milliseconds.
    ///
    /// # Errors
    ///
    /// Returns `None` the `TimeDelta` would be out of bounds, i.e. when `milliseconds` is more
    /// than `i64::MAX` or less than `-i64::MAX`. Notably, this is not the same as `i64::MIN`.
    #[inline]
    pub const fn try_milliseconds(milliseconds: i64) -> Option<TimeDelta> {
        if milliseconds < -i64::MAX {
            return None;
        }
        let (secs, millis) = div_mod_floor_64(milliseconds, MILLIS_PER_SEC);
        let d = TimeDelta { secs, nanos: millis as i32 * NANOS_PER_MILLI };
        Some(d)
    }

    /// Makes a new `TimeDelta` with the given number of microseconds.
    ///
    /// The number of microseconds acceptable by this constructor is less than
    /// the total number that can actually be stored in a `TimeDelta`, so it is
    /// not possible to specify a value that would be out of bounds. This
    /// function is therefore infallible.
    #[inline]
    pub const fn microseconds(microseconds: i64) -> TimeDelta {
        let (secs, micros) = div_mod_floor_64(microseconds, MICROS_PER_SEC);
        let nanos = micros as i32 * NANOS_PER_MICRO;
        TimeDelta { secs, nanos }
    }

    /// Makes a new `TimeDelta` with the given number of nanoseconds.
    ///
    /// The number of nanoseconds acceptable by this constructor is less than
    /// the total number that can actually be stored in a `TimeDelta`, so it is
    /// not possible to specify a value that would be out of bounds. This
    /// function is therefore infallible.
    #[inline]
    pub const fn nanoseconds(nanos: i64) -> TimeDelta {
        let (secs, nanos) = div_mod_floor_64(nanos, NANOS_PER_SEC as i64);
        TimeDelta { secs, nanos: nanos as i32 }
    }

    /// Returns the total number of whole weeks in the `TimeDelta`.
    #[inline]
    pub const fn num_weeks(&self) -> i64 {
        self.num_days() / 7
    }

    /// Returns the total number of whole days in the `TimeDelta`.
    #[inline]
    pub const fn num_days(&self) -> i64 {
        self.num_seconds() / SECS_PER_DAY
    }

    /// Returns the total number of whole hours in the `TimeDelta`.
    #[inline]
    pub const fn num_hours(&self) -> i64 {
        self.num_seconds() / SECS_PER_HOUR
    }

    /// Returns the total number of whole minutes in the `TimeDelta`.
    #[inline]
    pub const fn num_minutes(&self) -> i64 {
        self.num_seconds() / SECS_PER_MINUTE
    }

    /// Returns the total number of whole seconds in the `TimeDelta`.
    pub const fn num_seconds(&self) -> i64 {
        if self.secs < 0 && self.nanos > 0 { self.secs + 1 } else { self.secs }
    }

    /// Returns the fractional number of seconds in the `TimeDelta`.
    pub fn as_seconds_f64(self) -> f64 {
        self.secs as f64 + self.nanos as f64 / NANOS_PER_SEC as f64
    }

    /// Returns the fractional number of seconds in the `TimeDelta`.
    pub fn as_seconds_f32(self) -> f32 {
        self.secs as f32 + self.nanos as f32 / NANOS_PER_SEC as f32
    }

    /// Returns the total number of whole milliseconds in the `TimeDelta`.
    pub const fn num_milliseconds(&self) -> i64 {
        let secs_part = self.num_seconds() * MILLIS_PER_SEC;
        let nanos_part = self.subsec_nanos() / NANOS_PER_MILLI;
        secs_part + nanos_part as i64
    }

    /// Returns the number of milliseconds in the fractional part of the duration.
    ///
    /// This is the number of milliseconds such that
    /// `subsec_millis() + num_seconds() * 1_000` is the truncated number of
    /// milliseconds in the duration.
    pub const fn subsec_millis(&self) -> i32 {
        self.subsec_nanos() / NANOS_PER_MILLI
    }

    /// Returns the total number of whole microseconds in the `TimeDelta`,
    /// or `None` on overflow (exceeding 2^63 microseconds in either direction).
    pub const fn num_microseconds(&self) -> Option<i64> {
        let secs_part = try_opt!(self.num_seconds().checked_mul(MICROS_PER_SEC));
        let nanos_part = self.subsec_nanos() / NANOS_PER_MICRO;
        secs_part.checked_add(nanos_part as i64)
    }

    /// Returns the number of microseconds in the fractional part of the duration.
    ///
    /// This is the number of microseconds such that
    /// `subsec_micros() + num_seconds() * 1_000_000` is the truncated number of
    /// microseconds in the duration.
    pub const fn subsec_micros(&self) -> i32 {
        self.subsec_nanos() / NANOS_PER_MICRO
    }

    /// Returns the total number of whole nanoseconds in the `TimeDelta`,
    /// or `None` on overflow (exceeding 2^63 nanoseconds in either direction).
    pub const fn num_nanoseconds(&self) -> Option<i64> {
        let secs_part = try_opt!(self.num_seconds().checked_mul(NANOS_PER_SEC as i64));
        let nanos_part = self.subsec_nanos();
        secs_part.checked_add(nanos_part as i64)
    }

    /// Returns the number of nanoseconds in the fractional part of the duration.
    ///
    /// This is the number of nanoseconds such that
    /// `subsec_nanos() + num_seconds() * 1_000_000_000` is the total number of
    /// nanoseconds in the `TimeDelta`.
    pub const fn subsec_nanos(&self) -> i32 {
        if self.secs < 0 && self.nanos > 0 { self.nanos - NANOS_PER_SEC } else { self.nanos }
    }

    /// Add two `TimeDelta`s, returning `None` if overflow occurred.
    #[must_use]
    pub const fn checked_add(&self, rhs: &TimeDelta) -> Option<TimeDelta> {
        let mut secs = self.secs + rhs.secs;
        let mut nanos = self.nanos + rhs.nanos;
        if nanos >= NANOS_PER_SEC {
            nanos -= NANOS_PER_SEC;
            secs += 1;
        }
        TimeDelta::new(secs, nanos as u32)
    }

    /// Subtract two `TimeDelta`s, returning `None` if overflow occurred.
    #[must_use]
    pub const fn checked_sub(&self, rhs: &TimeDelta) -> Option<TimeDelta> {
        let mut secs = self.secs - rhs.secs;
        let mut nanos = self.nanos - rhs.nanos;
        if nanos < 0 {
            nanos += NANOS_PER_SEC;
            secs -= 1;
        }
        TimeDelta::new(secs, nanos as u32)
    }

    /// Multiply a `TimeDelta` with a i32, returning `None` if overflow occurred.
    #[must_use]
    pub const fn checked_mul(&self, rhs: i32) -> Option<TimeDelta> {
        let total_nanos = self.nanos as i64 * rhs as i64;
        let (extra_secs, nanos) = div_mod_floor_64(total_nanos, NANOS_PER_SEC as i64);
        let secs: i128 = self.secs as i128 * rhs as i128 + extra_secs as i128;
        if secs <= i64::MIN as i128 || secs >= i64::MAX as i128 {
            return None;
        };
        Some(TimeDelta { secs: secs as i64, nanos: nanos as i32 })
    }

    /// Divide a `TimeDelta` with a i32, returning `None` if dividing by 0.
    #[must_use]
    pub const fn checked_div(&self, rhs: i32) -> Option<TimeDelta> {
        if rhs == 0 {
            return None;
        }
        let secs = self.secs / rhs as i64;
        let carry = self.secs % rhs as i64;
        let extra_nanos = carry * NANOS_PER_SEC as i64 / rhs as i64;
        let nanos = self.nanos / rhs + extra_nanos as i32;

        let (secs, nanos) = match nanos {
            i32::MIN..=-1 => (secs - 1, nanos + NANOS_PER_SEC),
            NANOS_PER_SEC..=i32::MAX => (secs + 1, nanos - NANOS_PER_SEC),
            _ => (secs, nanos),
        };

        Some(TimeDelta { secs, nanos })
    }

    /// Returns the `TimeDelta` as an absolute (non-negative) value.
    #[inline]
    pub const fn abs(&self) -> TimeDelta {
        if self.secs < 0 && self.nanos != 0 {
            TimeDelta { secs: (self.secs + 1).abs(), nanos: NANOS_PER_SEC - self.nanos }
        } else {
            TimeDelta { secs: self.secs.abs(), nanos: self.nanos }
        }
    }

    /// The minimum possible `TimeDelta`: `-i64::MAX` milliseconds.
    #[deprecated(since = "0.4.39", note = "Use `TimeDelta::MIN` instead")]
    #[inline]
    pub const fn min_value() -> TimeDelta {
        MIN
    }

    /// The maximum possible `TimeDelta`: `i64::MAX` milliseconds.
    #[deprecated(since = "0.4.39", note = "Use `TimeDelta::MAX` instead")]
    #[inline]
    pub const fn max_value() -> TimeDelta {
        MAX
    }

    /// A `TimeDelta` where the stored seconds and nanoseconds are equal to zero.
    #[inline]
    pub const fn zero() -> TimeDelta {
        TimeDelta { secs: 0, nanos: 0 }
    }

    /// Returns `true` if the `TimeDelta` equals `TimeDelta::zero()`.
    #[inline]
    pub const fn is_zero(&self) -> bool {
        self.secs == 0 && self.nanos == 0
    }

    /// Creates a `TimeDelta` object from `std::time::Duration`
    ///
    /// This function errors when original duration is larger than the maximum
    /// value supported for this type.
    pub const fn from_std(duration: Duration) -> Result<TimeDelta, OutOfRangeError> {
        if duration.as_secs() > MAX.secs as u64 {
            return Err(OutOfRangeError(()));
        }
        match TimeDelta::new(duration.as_secs() as i64, duration.subsec_nanos()) {
            Some(d) => Ok(d),
            None => Err(OutOfRangeError(())),
        }
    }

    /// Creates a `std::time::Duration` object from a `TimeDelta`.
    ///
    /// This function errors when duration is less than zero. As standard
    /// library implementation is limited to non-negative values.
    pub const fn to_std(&self) -> Result<Duration, OutOfRangeError> {
        if self.secs < 0 {
            return Err(OutOfRangeError(()));
        }
        Ok(Duration::new(self.secs as u64, self.nanos as u32))
    }

    /// This duplicates `Neg::neg` because trait methods can't be const yet.
    pub(crate) const fn neg(self) -> TimeDelta {
        let (secs_diff, nanos) = match self.nanos {
            0 => (0, 0),
            nanos => (1, NANOS_PER_SEC - nanos),
        };
        TimeDelta { secs: -self.secs - secs_diff, nanos }
    }

    /// The minimum possible `TimeDelta`: `-i64::MAX` milliseconds.
    pub const MIN: Self = MIN;

    /// The maximum possible `TimeDelta`: `i64::MAX` milliseconds.
    pub const MAX: Self = MAX;
}

impl Neg for TimeDelta {
    type Output = TimeDelta;

    #[inline]
    fn neg(self) -> TimeDelta {
        let (secs_diff, nanos) = match self.nanos {
            0 => (0, 0),
            nanos => (1, NANOS_PER_SEC - nanos),
        };
        TimeDelta { secs: -self.secs - secs_diff, nanos }
    }
}

impl Add for TimeDelta {
    type Output = TimeDelta;

    fn add(self, rhs: TimeDelta) -> TimeDelta {
        self.checked_add(&rhs).expect("`TimeDelta + TimeDelta` overflowed")
    }
}

impl Sub for TimeDelta {
    type Output = TimeDelta;

    fn sub(self, rhs: TimeDelta) -> TimeDelta {
        self.checked_sub(&rhs).expect("`TimeDelta - TimeDelta` overflowed")
    }
}

impl AddAssign for TimeDelta {
    fn add_assign(&mut self, rhs: TimeDelta) {
        let new = self.checked_add(&rhs).expect("`TimeDelta + TimeDelta` overflowed");
        *self = new;
    }
}

impl SubAssign for TimeDelta {
    fn sub_assign(&mut self, rhs: TimeDelta) {
        let new = self.checked_sub(&rhs).expect("`TimeDelta - TimeDelta` overflowed");
        *self = new;
    }
}

impl Mul<i32> for TimeDelta {
    type Output = TimeDelta;

    fn mul(self, rhs: i32) -> TimeDelta {
        self.checked_mul(rhs).expect("`TimeDelta * i32` overflowed")
    }
}

impl Div<i32> for TimeDelta {
    type Output = TimeDelta;

    fn div(self, rhs: i32) -> TimeDelta {
        self.checked_div(rhs).expect("`i32` is zero")
    }
}

impl<'a> core::iter::Sum<&'a TimeDelta> for TimeDelta {
    fn sum<I: Iterator<Item = &'a TimeDelta>>(iter: I) -> TimeDelta {
        iter.fold(TimeDelta::zero(), |acc, x| acc + *x)
    }
}

impl core::iter::Sum<TimeDelta> for TimeDelta {
    fn sum<I: Iterator<Item = TimeDelta>>(iter: I) -> TimeDelta {
        iter.fold(TimeDelta::zero(), |acc, x| acc + x)
    }
}

impl fmt::Display for TimeDelta {
    /// Format a `TimeDelta` using the [ISO 8601] format
    ///
    /// [ISO 8601]: https://en.wikipedia.org/wiki/ISO_8601#Durations
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let (abs, sign) = if self.secs < 0 { (-*self, "-") } else { (*self, "") };

        write!(f, "{sign}P")?;
        if abs.secs == 0 && abs.nanos == 0 {
            return f.write_str("0D");
        }

        f.write_fmt(format_args!("T{}", abs.secs))?;

        if abs.nanos > 0 {
            let mut figures = 9usize;
            let mut fraction_digits = abs.nanos;
            loop {
                let div = fraction_digits / 10;
                let last_digit = fraction_digits % 10;
                if last_digit != 0 {
                    break;
                }
                fraction_digits = div;
                figures -= 1;
            }
            f.write_fmt(format_args!(".{fraction_digits:0figures$}"))?;
        }
        f.write_str("S")?;
        Ok(())
    }
}

/// Represents error when converting `TimeDelta` to/from a standard library
/// implementation
///
/// The `std::time::Duration` supports a range from zero to `u64::MAX`
/// *seconds*, while this module supports signed range of up to
/// `i64::MAX` of *milliseconds*.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OutOfRangeError(());

impl fmt::Display for OutOfRangeError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "Source duration value is out of range for the target type")
    }
}

#[cfg(any(feature = "std", feature = "core-error"))]
impl Error for OutOfRangeError {
    #[allow(deprecated)]
    fn description(&self) -> &str {
        "out of range error"
    }
}

#[inline]
const fn div_mod_floor_64(this: i64, other: i64) -> (i64, i64) {
    (this.div_euclid(other), this.rem_euclid(other))
}

#[cfg(all(feature = "arbitrary", feature = "std"))]
impl arbitrary::Arbitrary<'_> for TimeDelta {
    fn arbitrary(u: &mut arbitrary::Unstructured) -> arbitrary::Result<TimeDelta> {
        const MIN_SECS: i64 = -i64::MAX / MILLIS_PER_SEC - 1;
        const MAX_SECS: i64 = i64::MAX / MILLIS_PER_SEC;

        let secs: i64 = u.int_in_range(MIN_SECS..=MAX_SECS)?;
        let nanos: i32 = u.int_in_range(0..=(NANOS_PER_SEC - 1))?;
        let duration = TimeDelta { secs, nanos };

        if duration < MIN || duration > MAX {
            Err(arbitrary::Error::IncorrectFormat)
        } else {
            Ok(duration)
        }
    }
}

#[cfg(feature = "serde")]
mod serde {
    use super::TimeDelta;
    use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Error};

    impl Serialize for TimeDelta {
        fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
            <(i64, i32) as Serialize>::serialize(&(self.secs, self.nanos), serializer)
        }
    }

    impl<'de> Deserialize<'de> for TimeDelta {
        fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
            let (secs, nanos) = <(i64, i32) as Deserialize>::deserialize(deserializer)?;
            TimeDelta::new(secs, nanos as u32).ok_or(Error::custom("TimeDelta out of bounds"))
        }
    }

}
