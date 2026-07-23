// See README.md and LICENSE.txt for details.

//! Functionality for rounding or truncating a `DateTime` by a `TimeDelta`.

use crate::{DateTime, NaiveDateTime, TimeDelta, TimeZone, Timelike};
use core::cmp::Ordering;
use core::fmt;
use core::ops::{Add, Sub};

/// Extension trait for subsecond rounding or truncation to a maximum number
/// of digits. Rounding can be used to decrease the error variance when
/// serializing/persisting to lower precision. Truncation is the default
/// behavior in Chrono display formatting.  Either can be used to guarantee
/// equality (e.g. for testing) when round-tripping through a lower precision
/// format.
pub trait SubsecRound {
    /// Return a copy rounded to the specified number of subsecond digits. With
    /// 9 or more digits, self is returned unmodified. Halfway values are
    /// rounded up (away from zero).
    ///
    /// # Example
    /// ``` rust
    /// # use chrono::{SubsecRound, Timelike, NaiveDate};
    /// let dt = NaiveDate::from_ymd_opt(2018, 1, 11)
    ///     .unwrap()
    ///     .and_hms_milli_opt(12, 0, 0, 154)
    ///     .unwrap()
    ///     .and_utc();
    /// assert_eq!(dt.round_subsecs(2).nanosecond(), 150_000_000);
    /// assert_eq!(dt.round_subsecs(1).nanosecond(), 200_000_000);
    /// ```
    fn round_subsecs(self, digits: u16) -> Self;

    /// Return a copy truncated to the specified number of subsecond
    /// digits. With 9 or more digits, self is returned unmodified.
    ///
    /// # Example
    /// ``` rust
    /// # use chrono::{SubsecRound, Timelike, NaiveDate};
    /// let dt = NaiveDate::from_ymd_opt(2018, 1, 11)
    ///     .unwrap()
    ///     .and_hms_milli_opt(12, 0, 0, 154)
    ///     .unwrap()
    ///     .and_utc();
    /// assert_eq!(dt.trunc_subsecs(2).nanosecond(), 150_000_000);
    /// assert_eq!(dt.trunc_subsecs(1).nanosecond(), 100_000_000);
    /// ```
    fn trunc_subsecs(self, digits: u16) -> Self;
}

impl<T> SubsecRound for T
where
    T: Timelike + Add<TimeDelta, Output = T> + Sub<TimeDelta, Output = T>,
{
    fn round_subsecs(self, digits: u16) -> T {
        let span = span_for_digits(digits);
        let delta_down = self.nanosecond() % span;
        if delta_down > 0 {
            let delta_up = span - delta_down;
            if delta_up <= delta_down {
                self + TimeDelta::nanoseconds(delta_up.into())
            } else {
                self - TimeDelta::nanoseconds(delta_down.into())
            }
        } else {
            self 
        }
    }

    fn trunc_subsecs(self, digits: u16) -> T {
        let span = span_for_digits(digits);
        let delta_down = self.nanosecond() % span;
        if delta_down > 0 {
            self - TimeDelta::nanoseconds(delta_down.into())
        } else {
            self 
        }
    }
}

const fn span_for_digits(digits: u16) -> u32 {
    match digits {
        0 => 1_000_000_000,
        1 => 100_000_000,
        2 => 10_000_000,
        3 => 1_000_000,
        4 => 100_000,
        5 => 10_000,
        6 => 1_000,
        7 => 100,
        8 => 10,
        _ => 1,
    }
}

/// Extension trait for rounding or truncating a DateTime by a TimeDelta.
///
/// # Limitations
/// Both rounding and truncating are done via [`TimeDelta::num_nanoseconds`] and
/// [`DateTime::timestamp_nanos_opt`]. This means that they will fail if either the
/// `TimeDelta` or the `DateTime` are too big to represented as nanoseconds. They
/// will also fail if the `TimeDelta` is bigger than the timestamp, negative or zero.
pub trait DurationRound: Sized {
    /// Error that can occur in rounding or truncating
    #[cfg(feature = "std")]
    type Err: std::error::Error;

    /// Error that can occur in rounding or truncating
    #[cfg(all(not(feature = "std"), feature = "core-error"))]
    type Err: core::error::Error;

    /// Error that can occur in rounding or truncating
    #[cfg(all(not(feature = "std"), not(feature = "core-error")))]
    type Err: fmt::Debug + fmt::Display;

    /// Return a copy rounded by TimeDelta.
    ///
    /// # Example
    /// ``` rust
    /// # use chrono::{DurationRound, TimeDelta, NaiveDate};
    /// let dt = NaiveDate::from_ymd_opt(2018, 1, 11)
    ///     .unwrap()
    ///     .and_hms_milli_opt(12, 0, 0, 154)
    ///     .unwrap()
    ///     .and_utc();
    /// assert_eq!(
    ///     dt.duration_round(TimeDelta::try_milliseconds(10).unwrap()).unwrap().to_string(),
    ///     "2018-01-11 12:00:00.150 UTC"
    /// );
    /// assert_eq!(
    ///     dt.duration_round(TimeDelta::try_days(1).unwrap()).unwrap().to_string(),
    ///     "2018-01-12 00:00:00 UTC"
    /// );
    /// ```
    fn duration_round(self, duration: TimeDelta) -> Result<Self, Self::Err>;

    /// Return a copy truncated by TimeDelta.
    ///
    /// # Example
    /// ``` rust
    /// # use chrono::{DurationRound, TimeDelta, NaiveDate};
    /// let dt = NaiveDate::from_ymd_opt(2018, 1, 11)
    ///     .unwrap()
    ///     .and_hms_milli_opt(12, 0, 0, 154)
    ///     .unwrap()
    ///     .and_utc();
    /// assert_eq!(
    ///     dt.duration_trunc(TimeDelta::try_milliseconds(10).unwrap()).unwrap().to_string(),
    ///     "2018-01-11 12:00:00.150 UTC"
    /// );
    /// assert_eq!(
    ///     dt.duration_trunc(TimeDelta::try_days(1).unwrap()).unwrap().to_string(),
    ///     "2018-01-11 00:00:00 UTC"
    /// );
    /// ```
    fn duration_trunc(self, duration: TimeDelta) -> Result<Self, Self::Err>;

    /// Return a copy rounded **up** by TimeDelta.
    ///
    /// # Example
    /// ``` rust
    /// # use chrono::{DurationRound, TimeDelta, NaiveDate};
    /// let dt = NaiveDate::from_ymd_opt(2018, 1, 11)
    ///     .unwrap()
    ///     .and_hms_milli_opt(12, 0, 0, 154)
    ///     .unwrap()
    ///     .and_utc();
    /// assert_eq!(
    ///     dt.duration_round_up(TimeDelta::milliseconds(10)).unwrap().to_string(),
    ///     "2018-01-11 12:00:00.160 UTC"
    /// );
    /// assert_eq!(
    ///     dt.duration_round_up(TimeDelta::hours(1)).unwrap().to_string(),
    ///     "2018-01-11 13:00:00 UTC"
    /// );
    ///
    /// assert_eq!(
    ///     dt.duration_round_up(TimeDelta::days(1)).unwrap().to_string(),
    ///     "2018-01-12 00:00:00 UTC"
    /// );
    /// ```
    fn duration_round_up(self, duration: TimeDelta) -> Result<Self, Self::Err>;
}

impl<Tz: TimeZone> DurationRound for DateTime<Tz> {
    type Err = RoundingError;

    fn duration_round(self, duration: TimeDelta) -> Result<Self, Self::Err> {
        duration_round(self.naive_local(), self, duration)
    }

    fn duration_trunc(self, duration: TimeDelta) -> Result<Self, Self::Err> {
        duration_trunc(self.naive_local(), self, duration)
    }

    fn duration_round_up(self, duration: TimeDelta) -> Result<Self, Self::Err> {
        duration_round_up(self.naive_local(), self, duration)
    }
}

impl DurationRound for NaiveDateTime {
    type Err = RoundingError;

    fn duration_round(self, duration: TimeDelta) -> Result<Self, Self::Err> {
        duration_round(self, self, duration)
    }

    fn duration_trunc(self, duration: TimeDelta) -> Result<Self, Self::Err> {
        duration_trunc(self, self, duration)
    }

    fn duration_round_up(self, duration: TimeDelta) -> Result<Self, Self::Err> {
        duration_round_up(self, self, duration)
    }
}

fn duration_round<T>(
    naive: NaiveDateTime,
    original: T,
    duration: TimeDelta,
) -> Result<T, RoundingError>
where
    T: Timelike + Add<TimeDelta, Output = T> + Sub<TimeDelta, Output = T>,
{
    if let Some(span) = duration.num_nanoseconds() {
        if span <= 0 {
            return Err(RoundingError::DurationExceedsLimit);
        }
        let stamp =
            naive.and_utc().timestamp_nanos_opt().ok_or(RoundingError::TimestampExceedsLimit)?;
        let delta_down = stamp % span;
        if delta_down == 0 {
            Ok(original)
        } else {
            let (delta_up, delta_down) = if delta_down < 0 {
                (delta_down.abs(), span - delta_down.abs())
            } else {
                (span - delta_down, delta_down)
            };
            if delta_up <= delta_down {
                Ok(original + TimeDelta::nanoseconds(delta_up))
            } else {
                Ok(original - TimeDelta::nanoseconds(delta_down))
            }
        }
    } else {
        Err(RoundingError::DurationExceedsLimit)
    }
}

fn duration_trunc<T>(
    naive: NaiveDateTime,
    original: T,
    duration: TimeDelta,
) -> Result<T, RoundingError>
where
    T: Timelike + Add<TimeDelta, Output = T> + Sub<TimeDelta, Output = T>,
{
    if let Some(span) = duration.num_nanoseconds() {
        if span <= 0 {
            return Err(RoundingError::DurationExceedsLimit);
        }
        let stamp =
            naive.and_utc().timestamp_nanos_opt().ok_or(RoundingError::TimestampExceedsLimit)?;
        let delta_down = stamp % span;
        match delta_down.cmp(&0) {
            Ordering::Equal => Ok(original),
            Ordering::Greater => Ok(original - TimeDelta::nanoseconds(delta_down)),
            Ordering::Less => Ok(original - TimeDelta::nanoseconds(span - delta_down.abs())),
        }
    } else {
        Err(RoundingError::DurationExceedsLimit)
    }
}

fn duration_round_up<T>(
    naive: NaiveDateTime,
    original: T,
    duration: TimeDelta,
) -> Result<T, RoundingError>
where
    T: Timelike + Add<TimeDelta, Output = T> + Sub<TimeDelta, Output = T>,
{
    if let Some(span) = duration.num_nanoseconds() {
        if span <= 0 {
            return Err(RoundingError::DurationExceedsLimit);
        }
        let stamp =
            naive.and_utc().timestamp_nanos_opt().ok_or(RoundingError::TimestampExceedsLimit)?;
        let delta_down = stamp % span;
        match delta_down.cmp(&0) {
            Ordering::Equal => Ok(original),
            Ordering::Greater => Ok(original + TimeDelta::nanoseconds(span - delta_down)),
            Ordering::Less => Ok(original + TimeDelta::nanoseconds(delta_down.abs())),
        }
    } else {
        Err(RoundingError::DurationExceedsLimit)
    }
}

/// An error from rounding by `TimeDelta`
///
/// See: [`DurationRound`]
#[derive(Debug, Clone, PartialEq, Eq, Copy)]
pub enum RoundingError {
    /// Error when the TimeDelta exceeds the TimeDelta from or until the Unix epoch.
    ///
    /// Note: this error is not produced anymore.
    DurationExceedsTimestamp,

    /// Error when `TimeDelta.num_nanoseconds` exceeds the limit.
    ///
    /// ``` rust
    /// # use chrono::{DurationRound, TimeDelta, RoundingError, NaiveDate};
    /// let dt = NaiveDate::from_ymd_opt(2260, 12, 31)
    ///     .unwrap()
    ///     .and_hms_nano_opt(23, 59, 59, 1_75_500_000)
    ///     .unwrap()
    ///     .and_utc();
    ///
    /// assert_eq!(
    ///     dt.duration_round(TimeDelta::try_days(300 * 365).unwrap()),
    ///     Err(RoundingError::DurationExceedsLimit)
    /// );
    /// ```
    DurationExceedsLimit,

    /// Error when `DateTime.timestamp_nanos` exceeds the limit.
    ///
    /// ``` rust
    /// # use chrono::{DurationRound, TimeDelta, RoundingError, TimeZone, Utc};
    /// let dt = Utc.with_ymd_and_hms(2300, 12, 12, 0, 0, 0).unwrap();
    ///
    /// assert_eq!(
    ///     dt.duration_round(TimeDelta::try_days(1).unwrap()),
    ///     Err(RoundingError::TimestampExceedsLimit)
    /// );
    /// ```
    TimestampExceedsLimit,
}

impl fmt::Display for RoundingError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            RoundingError::DurationExceedsTimestamp => {
                write!(f, "duration in nanoseconds exceeds timestamp")
            }
            RoundingError::DurationExceedsLimit => {
                write!(f, "duration exceeds num_nanoseconds limit")
            }
            RoundingError::TimestampExceedsLimit => {
                write!(f, "timestamp exceeds num_nanoseconds limit")
            }
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for RoundingError {
    #[allow(deprecated)]
    fn description(&self) -> &str {
        "error from rounding or truncating with DurationRound"
    }
}

#[cfg(all(not(feature = "std"), feature = "core-error"))]
impl core::error::Error for RoundingError {
    #[allow(deprecated)]
    fn description(&self) -> &str {
        "error from rounding or truncating with DurationRound"
    }
}
