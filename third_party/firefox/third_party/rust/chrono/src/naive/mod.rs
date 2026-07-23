//! Date and time types unconcerned with timezones.
//!
//! They are primarily building blocks for other types
//! (e.g. [`TimeZone`](../offset/trait.TimeZone.html)),
//! but can be also used for the simpler date and time handling.

use core::hash::{Hash, Hasher};
use core::ops::RangeInclusive;

use crate::Weekday;
use crate::expect;

pub(crate) mod date;
pub(crate) mod datetime;
mod internals;
pub(crate) mod isoweek;
pub(crate) mod time;

#[allow(deprecated)]
pub use self::date::{MAX_DATE, MIN_DATE};
pub use self::date::{NaiveDate, NaiveDateDaysIterator, NaiveDateWeeksIterator};
#[allow(deprecated)]
pub use self::datetime::{MAX_DATETIME, MIN_DATETIME, NaiveDateTime};
pub use self::isoweek::IsoWeek;
pub use self::time::NaiveTime;

#[cfg(feature = "__internal_bench")]
#[doc(hidden)]
pub use self::internals::YearFlags as __BenchYearFlags;

/// A week represented by a [`NaiveDate`] and a [`Weekday`] which is the first
/// day of the week.
#[derive(Clone, Copy, Debug, Eq)]
pub struct NaiveWeek {
    date: NaiveDate,
    start: Weekday,
}

impl NaiveWeek {
    /// Create a new `NaiveWeek`
    pub(crate) const fn new(date: NaiveDate, start: Weekday) -> Self {
        Self { date, start }
    }

    /// Returns a date representing the first day of the week.
    ///
    /// # Panics
    ///
    /// Panics if the first day of the week happens to fall just out of range of `NaiveDate`
    /// (more than ca. 262,000 years away from common era).
    ///
    /// # Examples
    ///
    /// ```
    /// use chrono::{NaiveDate, Weekday};
    ///
    /// let date = NaiveDate::from_ymd_opt(2022, 4, 18).unwrap();
    /// let week = date.week(Weekday::Mon);
    /// assert!(week.first_day() <= date);
    /// ```
    #[inline]
    #[must_use]
    pub const fn first_day(&self) -> NaiveDate {
        expect(self.checked_first_day(), "first weekday out of range for `NaiveDate`")
    }

    /// Returns a date representing the first day of the week or
    /// `None` if the date is out of `NaiveDate`'s range
    /// (more than ca. 262,000 years away from common era).
    ///
    /// # Examples
    ///
    /// ```
    /// use chrono::{NaiveDate, Weekday};
    ///
    /// let date = NaiveDate::MIN;
    /// let week = date.week(Weekday::Mon);
    /// if let Some(first_day) = week.checked_first_day() {
    ///     assert!(first_day == date);
    /// } else {
    ///     // error handling code
    ///     return;
    /// };
    /// ```
    #[inline]
    #[must_use]
    pub const fn checked_first_day(&self) -> Option<NaiveDate> {
        let start = self.start.num_days_from_monday() as i32;
        let ref_day = self.date.weekday().num_days_from_monday() as i32;
        let days = start - ref_day - if start > ref_day { 7 } else { 0 };
        self.date.add_days(days)
    }

    /// Returns a date representing the last day of the week.
    ///
    /// # Panics
    ///
    /// Panics if the last day of the week happens to fall just out of range of `NaiveDate`
    /// (more than ca. 262,000 years away from common era).
    ///
    /// # Examples
    ///
    /// ```
    /// use chrono::{NaiveDate, Weekday};
    ///
    /// let date = NaiveDate::from_ymd_opt(2022, 4, 18).unwrap();
    /// let week = date.week(Weekday::Mon);
    /// assert!(week.last_day() >= date);
    /// ```
    #[inline]
    #[must_use]
    pub const fn last_day(&self) -> NaiveDate {
        expect(self.checked_last_day(), "last weekday out of range for `NaiveDate`")
    }

    /// Returns a date representing the last day of the week or
    /// `None` if the date is out of `NaiveDate`'s range
    /// (more than ca. 262,000 years away from common era).
    ///
    /// # Examples
    ///
    /// ```
    /// use chrono::{NaiveDate, Weekday};
    ///
    /// let date = NaiveDate::MAX;
    /// let week = date.week(Weekday::Mon);
    /// if let Some(last_day) = week.checked_last_day() {
    ///     assert!(last_day == date);
    /// } else {
    ///     // error handling code
    ///     return;
    /// };
    /// ```
    #[inline]
    #[must_use]
    pub const fn checked_last_day(&self) -> Option<NaiveDate> {
        let end = self.start.pred().num_days_from_monday() as i32;
        let ref_day = self.date.weekday().num_days_from_monday() as i32;
        let days = end - ref_day + if end < ref_day { 7 } else { 0 };
        self.date.add_days(days)
    }

    /// Returns a [`RangeInclusive<T>`] representing the whole week bounded by
    /// [first_day](NaiveWeek::first_day) and [last_day](NaiveWeek::last_day) functions.
    ///
    /// # Panics
    ///
    /// Panics if the either the first or last day of the week happens to fall just out of range of
    /// `NaiveDate` (more than ca. 262,000 years away from common era).
    ///
    /// # Examples
    ///
    /// ```
    /// use chrono::{NaiveDate, Weekday};
    ///
    /// let date = NaiveDate::from_ymd_opt(2022, 4, 18).unwrap();
    /// let week = date.week(Weekday::Mon);
    /// let days = week.days();
    /// assert!(days.contains(&date));
    /// ```
    #[inline]
    #[must_use]
    pub const fn days(&self) -> RangeInclusive<NaiveDate> {
        match self.checked_days() {
            Some(val) => val,
            None => panic!("{}", "first or last weekday is out of range for `NaiveDate`"),
        }
    }

    /// Returns an [`Option<RangeInclusive<T>>`] representing the whole week bounded by
    /// [checked_first_day](NaiveWeek::checked_first_day) and
    /// [checked_last_day](NaiveWeek::checked_last_day) functions.
    ///
    /// Returns `None` if either of the boundaries are out of `NaiveDate`'s range
    /// (more than ca. 262,000 years away from common era).
    ///
    ///
    /// # Examples
    ///
    /// ```
    /// use chrono::{NaiveDate, Weekday};
    ///
    /// let date = NaiveDate::MAX;
    /// let week = date.week(Weekday::Mon);
    /// let _days = match week.checked_days() {
    ///     Some(d) => d,
    ///     None => {
    ///         // error handling code
    ///         return;
    ///     }
    /// };
    /// ```
    #[inline]
    #[must_use]
    pub const fn checked_days(&self) -> Option<RangeInclusive<NaiveDate>> {
        match (self.checked_first_day(), self.checked_last_day()) {
            (Some(first), Some(last)) => Some(first..=last),
            (_, _) => None,
        }
    }
}

impl PartialEq for NaiveWeek {
    fn eq(&self, other: &Self) -> bool {
        self.first_day() == other.first_day()
    }
}

impl Hash for NaiveWeek {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.first_day().hash(state);
    }
}

/// A duration in calendar days.
///
/// This is useful because when using `TimeDelta` it is possible that adding `TimeDelta::days(1)`
/// doesn't increment the day value as expected due to it being a fixed number of seconds. This
/// difference applies only when dealing with `DateTime<TimeZone>` data types and in other cases
/// `TimeDelta::days(n)` and `Days::new(n)` are equivalent.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq, PartialOrd, Ord)]
pub struct Days(pub(crate) u64);

impl Days {
    /// Construct a new `Days` from a number of days
    pub const fn new(num: u64) -> Self {
        Self(num)
    }
}

/// Serialization/Deserialization of `NaiveDateTime` in alternate formats
///
/// The various modules in here are intended to be used with serde's [`with` annotation] to
/// serialize as something other than the default ISO 8601 format.
///
/// [`with` annotation]: https://serde.rs/field-attrs.html#with
#[cfg(feature = "serde")]
pub mod serde {
    pub use super::datetime::serde::*;
}
