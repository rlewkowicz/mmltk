// See README.md and LICENSE.txt for details.

//! A collection of parsed date and time items.
//! They can be constructed incrementally while being checked for consistency.

use super::{IMPOSSIBLE, NOT_ENOUGH, OUT_OF_RANGE, ParseResult};
use crate::naive::{NaiveDate, NaiveDateTime, NaiveTime};
use crate::offset::{FixedOffset, MappedLocalTime, Offset, TimeZone};
use crate::{DateTime, Datelike, TimeDelta, Timelike, Weekday};

/// A type to hold parsed fields of date and time that can check all fields are consistent.
///
/// There are three classes of methods:
///
/// - `set_*` methods to set fields you have available. They do a basic range check, and if the
///   same field is set more than once it is checked for consistency.
///
/// - `to_*` methods try to make a concrete date and time value out of set fields.
///   They fully check that all fields are consistent and whether the date/datetime exists.
///
/// - Methods to inspect the parsed fields.
///
/// `Parsed` is used internally by all parsing functions in chrono. It is a public type so that it
/// can be used to write custom parsers that reuse the resolving algorithm, or to inspect the
/// results of a string parsed with chrono without converting it to concrete types.
///
/// # Resolving algorithm
///
/// Resolving date/time parts is littered with lots of corner cases, which is why common date/time
/// parsers do not implement it correctly.
///
/// Chrono provides a complete resolution algorithm that checks all fields for consistency via the
/// `Parsed` type.
///
/// As an easy example, consider RFC 2822. The [RFC 2822 date and time format] has a day of the week
/// part, which should be consistent with the other date parts. But a `strptime`-based parse would
/// happily accept inconsistent input:
///
/// ```python
/// >>> import time
/// >>> time.strptime('Wed, 31 Dec 2014 04:26:40 +0000',
///                   '%a, %d %b %Y %H:%M:%S +0000')
/// time.struct_time(tm_year=2014, tm_mon=12, tm_mday=31,
///                  tm_hour=4, tm_min=26, tm_sec=40,
///                  tm_wday=2, tm_yday=365, tm_isdst=-1)
/// >>> time.strptime('Thu, 31 Dec 2014 04:26:40 +0000',
///                   '%a, %d %b %Y %H:%M:%S +0000')
/// time.struct_time(tm_year=2014, tm_mon=12, tm_mday=31,
///                  tm_hour=4, tm_min=26, tm_sec=40,
///                  tm_wday=3, tm_yday=365, tm_isdst=-1)
/// ```
///
/// [RFC 2822 date and time format]: https://tools.ietf.org/html/rfc2822#section-3.3
///
/// # Example
///
/// Let's see how `Parsed` correctly detects the second RFC 2822 string from before is inconsistent.
///
/// ```
/// # #[cfg(feature = "alloc")] {
/// use chrono::format::{ParseErrorKind, Parsed};
/// use chrono::Weekday;
///
/// let mut parsed = Parsed::new();
/// parsed.set_weekday(Weekday::Wed)?;
/// parsed.set_day(31)?;
/// parsed.set_month(12)?;
/// parsed.set_year(2014)?;
/// parsed.set_hour(4)?;
/// parsed.set_minute(26)?;
/// parsed.set_second(40)?;
/// parsed.set_offset(0)?;
/// let dt = parsed.to_datetime()?;
/// assert_eq!(dt.to_rfc2822(), "Wed, 31 Dec 2014 04:26:40 +0000");
///
/// let mut parsed = Parsed::new();
/// parsed.set_weekday(Weekday::Thu)?; // changed to the wrong day
/// parsed.set_day(31)?;
/// parsed.set_month(12)?;
/// parsed.set_year(2014)?;
/// parsed.set_hour(4)?;
/// parsed.set_minute(26)?;
/// parsed.set_second(40)?;
/// parsed.set_offset(0)?;
/// let result = parsed.to_datetime();
///
/// assert!(result.is_err());
/// if let Err(error) = result {
///     assert_eq!(error.kind(), ParseErrorKind::Impossible);
/// }
/// # }
/// # Ok::<(), chrono::ParseError>(())
/// ```
///
/// The same using chrono's built-in parser for RFC 2822 (the [RFC2822 formatting item]) and
/// [`format::parse()`] showing how to inspect a field on failure.
///
/// [RFC2822 formatting item]: crate::format::Fixed::RFC2822
/// [`format::parse()`]: crate::format::parse()
///
/// ```
/// # #[cfg(feature = "alloc")] {
/// use chrono::format::{parse, Fixed, Item, Parsed};
/// use chrono::Weekday;
///
/// let rfc_2822 = [Item::Fixed(Fixed::RFC2822)];
///
/// let mut parsed = Parsed::new();
/// parse(&mut parsed, "Wed, 31 Dec 2014 04:26:40 +0000", rfc_2822.iter())?;
/// let dt = parsed.to_datetime()?;
///
/// assert_eq!(dt.to_rfc2822(), "Wed, 31 Dec 2014 04:26:40 +0000");
///
/// let mut parsed = Parsed::new();
/// parse(&mut parsed, "Thu, 31 Dec 2014 04:26:40 +0000", rfc_2822.iter())?;
/// let result = parsed.to_datetime();
///
/// assert!(result.is_err());
/// if result.is_err() {
///     // What is the weekday?
///     assert_eq!(parsed.weekday(), Some(Weekday::Thu));
/// }
/// # }
/// # Ok::<(), chrono::ParseError>(())
/// ```
#[allow(clippy::manual_non_exhaustive)]
#[derive(Clone, PartialEq, Eq, Debug, Default, Hash)]
pub struct Parsed {
    #[doc(hidden)]
    pub year: Option<i32>,
    #[doc(hidden)]
    pub year_div_100: Option<i32>,
    #[doc(hidden)]
    pub year_mod_100: Option<i32>,
    #[doc(hidden)]
    pub isoyear: Option<i32>,
    #[doc(hidden)]
    pub isoyear_div_100: Option<i32>,
    #[doc(hidden)]
    pub isoyear_mod_100: Option<i32>,
    #[doc(hidden)]
    pub quarter: Option<u32>,
    #[doc(hidden)]
    pub month: Option<u32>,
    #[doc(hidden)]
    pub week_from_sun: Option<u32>,
    #[doc(hidden)]
    pub week_from_mon: Option<u32>,
    #[doc(hidden)]
    pub isoweek: Option<u32>,
    #[doc(hidden)]
    pub weekday: Option<Weekday>,
    #[doc(hidden)]
    pub ordinal: Option<u32>,
    #[doc(hidden)]
    pub day: Option<u32>,
    #[doc(hidden)]
    pub hour_div_12: Option<u32>,
    #[doc(hidden)]
    pub hour_mod_12: Option<u32>,
    #[doc(hidden)]
    pub minute: Option<u32>,
    #[doc(hidden)]
    pub second: Option<u32>,
    #[doc(hidden)]
    pub nanosecond: Option<u32>,
    #[doc(hidden)]
    pub timestamp: Option<i64>,
    #[doc(hidden)]
    pub offset: Option<i32>,
    #[doc(hidden)]
    _dummy: (),
}

/// Checks if `old` is either empty or has the same value as `new` (i.e. "consistent"),
/// and if it is empty, set `old` to `new` as well.
#[inline]
fn set_if_consistent<T: PartialEq>(old: &mut Option<T>, new: T) -> ParseResult<()> {
    match old {
        Some(old) if *old != new => Err(IMPOSSIBLE),
        _ => {
            *old = Some(new);
            Ok(())
        }
    }
}

impl Parsed {
    /// Returns the initial value of parsed parts.
    #[must_use]
    pub fn new() -> Parsed {
        Parsed::default()
    }

    /// Set the [`year`](Parsed::year) field to the given value.
    ///
    /// The value can be negative, unlike the [`year_div_100`](Parsed::year_div_100) and
    /// [`year_mod_100`](Parsed::year_mod_100) fields.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is outside the range of an `i32`.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_year(&mut self, value: i64) -> ParseResult<()> {
        set_if_consistent(&mut self.year, i32::try_from(value).map_err(|_| OUT_OF_RANGE)?)
    }

    /// Set the [`year_div_100`](Parsed::year_div_100) field to the given value.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is negative or if it is greater than `i32::MAX`.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_year_div_100(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=i32::MAX as i64).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.year_div_100, value as i32)
    }

    /// Set the [`year_mod_100`](Parsed::year_mod_100) field to the given value.
    ///
    /// When set it implies that the year is not negative.
    ///
    /// If this field is set while the [`year_div_100`](Parsed::year_div_100) field is missing (and
    /// the full [`year`](Parsed::year) field is also not set), it assumes a default value for the
    /// [`year_div_100`](Parsed::year_div_100) field.
    /// The default is 19 when `year_mod_100 >= 70` and 20 otherwise.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is negative or if it is greater than 99.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_year_mod_100(&mut self, value: i64) -> ParseResult<()> {
        if !(0..100).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.year_mod_100, value as i32)
    }

    /// Set the [`isoyear`](Parsed::isoyear) field, that is part of an [ISO 8601 week date], to the
    /// given value.
    ///
    /// The value can be negative, unlike the [`isoyear_div_100`](Parsed::isoyear_div_100) and
    /// [`isoyear_mod_100`](Parsed::isoyear_mod_100) fields.
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is outside the range of an `i32`.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_isoyear(&mut self, value: i64) -> ParseResult<()> {
        set_if_consistent(&mut self.isoyear, i32::try_from(value).map_err(|_| OUT_OF_RANGE)?)
    }

    /// Set the [`isoyear_div_100`](Parsed::isoyear_div_100) field, that is part of an
    /// [ISO 8601 week date], to the given value.
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is negative or if it is greater than `i32::MAX`.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_isoyear_div_100(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=i32::MAX as i64).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.isoyear_div_100, value as i32)
    }

    /// Set the [`isoyear_mod_100`](Parsed::isoyear_mod_100) field, that is part of an
    /// [ISO 8601 week date], to the given value.
    ///
    /// When set it implies that the year is not negative.
    ///
    /// If this field is set while the [`isoyear_div_100`](Parsed::isoyear_div_100) field is missing
    /// (and the full [`isoyear`](Parsed::isoyear) field is also not set), it assumes a default
    /// value for the [`isoyear_div_100`](Parsed::isoyear_div_100) field.
    /// The default is 19 when `year_mod_100 >= 70` and 20 otherwise.
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is negative or if it is greater than 99.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_isoyear_mod_100(&mut self, value: i64) -> ParseResult<()> {
        if !(0..100).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.isoyear_mod_100, value as i32)
    }

    /// Set the [`quarter`](Parsed::quarter) field to the given value.
    ///
    /// Quarter 1 starts in January.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 1-4.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_quarter(&mut self, value: i64) -> ParseResult<()> {
        if !(1..=4).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.quarter, value as u32)
    }

    /// Set the [`month`](Parsed::month) field to the given value.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 1-12.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_month(&mut self, value: i64) -> ParseResult<()> {
        if !(1..=12).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.month, value as u32)
    }

    /// Set the [`week_from_sun`](Parsed::week_from_sun) week number field to the given value.
    ///
    /// Week 1 starts at the first Sunday of January.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 0-53.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_week_from_sun(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=53).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.week_from_sun, value as u32)
    }

    /// Set the [`week_from_mon`](Parsed::week_from_mon) week number field to the given value.
    /// Set the 'week number starting with Monday' field to the given value.
    ///
    /// Week 1 starts at the first Monday of January.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 0-53.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_week_from_mon(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=53).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.week_from_mon, value as u32)
    }

    /// Set the [`isoweek`](Parsed::isoweek) field for an [ISO 8601 week date] to the given value.
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 1-53.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_isoweek(&mut self, value: i64) -> ParseResult<()> {
        if !(1..=53).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.isoweek, value as u32)
    }

    /// Set the [`weekday`](Parsed::weekday) field to the given value.
    ///
    /// # Errors
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_weekday(&mut self, value: Weekday) -> ParseResult<()> {
        set_if_consistent(&mut self.weekday, value)
    }

    /// Set the [`ordinal`](Parsed::ordinal) (day of the year) field to the given value.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 1-366.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_ordinal(&mut self, value: i64) -> ParseResult<()> {
        if !(1..=366).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.ordinal, value as u32)
    }

    /// Set the [`day`](Parsed::day) of the month field to the given value.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 1-31.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_day(&mut self, value: i64) -> ParseResult<()> {
        if !(1..=31).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.day, value as u32)
    }

    /// Set the [`hour_div_12`](Parsed::hour_div_12) am/pm field to the given value.
    ///
    /// `false` indicates AM and `true` indicates PM.
    ///
    /// # Errors
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_ampm(&mut self, value: bool) -> ParseResult<()> {
        set_if_consistent(&mut self.hour_div_12, value as u32)
    }

    /// Set the [`hour_mod_12`](Parsed::hour_mod_12) field, for the hour number in 12-hour clocks,
    /// to the given value.
    ///
    /// Value must be in the canonical range of 1-12.
    /// It will internally be stored as 0-11 (`value % 12`).
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 1-12.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_hour12(&mut self, mut value: i64) -> ParseResult<()> {
        if !(1..=12).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        if value == 12 {
            value = 0
        }
        set_if_consistent(&mut self.hour_mod_12, value as u32)
    }

    /// Set the [`hour_div_12`](Parsed::hour_div_12) and [`hour_mod_12`](Parsed::hour_mod_12)
    /// fields to the given value for a 24-hour clock.
    ///
    /// # Errors
    ///
    /// May return `OUT_OF_RANGE` if `value` is not in the range 0-23.
    /// Currently only checks the value is not out of range for a `u32`.
    ///
    /// Returns `IMPOSSIBLE` one of the fields was already set to a different value.
    #[inline]
    pub fn set_hour(&mut self, value: i64) -> ParseResult<()> {
        let (hour_div_12, hour_mod_12) = match value {
            hour @ 0..=11 => (0, hour as u32),
            hour @ 12..=23 => (1, hour as u32 - 12),
            _ => return Err(OUT_OF_RANGE),
        };
        set_if_consistent(&mut self.hour_div_12, hour_div_12)?;
        set_if_consistent(&mut self.hour_mod_12, hour_mod_12)
    }

    /// Set the [`minute`](Parsed::minute) field to the given value.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 0-59.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_minute(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=59).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.minute, value as u32)
    }

    /// Set the [`second`](Parsed::second) field to the given value.
    ///
    /// The value can be 60 in the case of a leap second.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 0-60.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_second(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=60).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.second, value as u32)
    }

    /// Set the [`nanosecond`](Parsed::nanosecond) field to the given value.
    ///
    /// This is the number of nanoseconds since the whole second.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is not in the range 0-999,999,999.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_nanosecond(&mut self, value: i64) -> ParseResult<()> {
        if !(0..=999_999_999).contains(&value) {
            return Err(OUT_OF_RANGE);
        }
        set_if_consistent(&mut self.nanosecond, value as u32)
    }

    /// Set the [`timestamp`](Parsed::timestamp) field to the given value.
    ///
    /// A Unix timestamp is defined as the number of non-leap seconds since midnight UTC on
    /// January 1, 1970.
    ///
    /// # Errors
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_timestamp(&mut self, value: i64) -> ParseResult<()> {
        set_if_consistent(&mut self.timestamp, value)
    }

    /// Set the [`offset`](Parsed::offset) field to the given value.
    ///
    /// The offset is in seconds from local time to UTC.
    ///
    /// # Errors
    ///
    /// Returns `OUT_OF_RANGE` if `value` is outside the range of an `i32`.
    ///
    /// Returns `IMPOSSIBLE` if this field was already set to a different value.
    #[inline]
    pub fn set_offset(&mut self, value: i64) -> ParseResult<()> {
        set_if_consistent(&mut self.offset, i32::try_from(value).map_err(|_| OUT_OF_RANGE)?)
    }

    /// Returns a parsed naive date out of given fields.
    ///
    /// This method is able to determine the date from given subset of fields:
    ///
    /// - Year, month, day.
    /// - Year, day of the year (ordinal).
    /// - Year, week number counted from Sunday or Monday, day of the week.
    /// - ISO week date.
    ///
    /// Gregorian year and ISO week date year can have their century number (`*_div_100`) omitted,
    /// the two-digit year is used to guess the century number then.
    ///
    /// It checks all given date fields are consistent with each other.
    ///
    /// # Errors
    ///
    /// This method returns:
    /// - `IMPOSSIBLE` if any of the date fields conflict.
    /// - `NOT_ENOUGH` if there are not enough fields set in `Parsed` for a complete date.
    /// - `OUT_OF_RANGE`
    ///   - if any of the date fields of `Parsed` are set to a value beyond their acceptable range.
    ///   - if the value would be outside the range of a [`NaiveDate`].
    ///   - if the date does not exist.
    pub fn to_naive_date(&self) -> ParseResult<NaiveDate> {
        fn resolve_year(
            y: Option<i32>,
            q: Option<i32>,
            r: Option<i32>,
        ) -> ParseResult<Option<i32>> {
            match (y, q, r) {
                (y, None, None) => Ok(y),

                (Some(y), q, r @ Some(0..=99)) | (Some(y), q, r @ None) => {
                    if y < 0 {
                        return Err(IMPOSSIBLE);
                    }
                    let q_ = y / 100;
                    let r_ = y % 100;
                    if q.unwrap_or(q_) == q_ && r.unwrap_or(r_) == r_ {
                        Ok(Some(y))
                    } else {
                        Err(IMPOSSIBLE)
                    }
                }

                (None, Some(q), Some(r @ 0..=99)) => {
                    if q < 0 {
                        return Err(IMPOSSIBLE);
                    }
                    let y = q.checked_mul(100).and_then(|v| v.checked_add(r));
                    Ok(Some(y.ok_or(OUT_OF_RANGE)?))
                }

                (None, None, Some(r @ 0..=99)) => Ok(Some(r + if r < 70 { 2000 } else { 1900 })),

                (None, Some(_), None) => Err(NOT_ENOUGH),
                (_, _, Some(_)) => Err(OUT_OF_RANGE),
            }
        }

        let given_year = resolve_year(self.year, self.year_div_100, self.year_mod_100)?;
        let given_isoyear = resolve_year(self.isoyear, self.isoyear_div_100, self.isoyear_mod_100)?;

        let verify_ymd = |date: NaiveDate| {
            let year = date.year();
            let (year_div_100, year_mod_100) = if year >= 0 {
                (Some(year / 100), Some(year % 100))
            } else {
                (None, None) 
            };
            let month = date.month();
            let day = date.day();
            self.year.unwrap_or(year) == year
                && self.year_div_100.or(year_div_100) == year_div_100
                && self.year_mod_100.or(year_mod_100) == year_mod_100
                && self.month.unwrap_or(month) == month
                && self.day.unwrap_or(day) == day
        };

        let verify_isoweekdate = |date: NaiveDate| {
            let week = date.iso_week();
            let isoyear = week.year();
            let isoweek = week.week();
            let weekday = date.weekday();
            let (isoyear_div_100, isoyear_mod_100) = if isoyear >= 0 {
                (Some(isoyear / 100), Some(isoyear % 100))
            } else {
                (None, None) 
            };
            self.isoyear.unwrap_or(isoyear) == isoyear
                && self.isoyear_div_100.or(isoyear_div_100) == isoyear_div_100
                && self.isoyear_mod_100.or(isoyear_mod_100) == isoyear_mod_100
                && self.isoweek.unwrap_or(isoweek) == isoweek
                && self.weekday.unwrap_or(weekday) == weekday
        };

        let verify_ordinal = |date: NaiveDate| {
            let ordinal = date.ordinal();
            let week_from_sun = date.weeks_from(Weekday::Sun);
            let week_from_mon = date.weeks_from(Weekday::Mon);
            self.ordinal.unwrap_or(ordinal) == ordinal
                && self.week_from_sun.map_or(week_from_sun, |v| v as i32) == week_from_sun
                && self.week_from_mon.map_or(week_from_mon, |v| v as i32) == week_from_mon
        };

        let (verified, parsed_date) = match (given_year, given_isoyear, self) {
            (Some(year), _, &Parsed { month: Some(month), day: Some(day), .. }) => {
                let date = NaiveDate::from_ymd_opt(year, month, day).ok_or(OUT_OF_RANGE)?;
                (verify_isoweekdate(date) && verify_ordinal(date), date)
            }

            (Some(year), _, &Parsed { ordinal: Some(ordinal), .. }) => {
                let date = NaiveDate::from_yo_opt(year, ordinal).ok_or(OUT_OF_RANGE)?;
                (verify_ymd(date) && verify_isoweekdate(date) && verify_ordinal(date), date)
            }

            (Some(year), _, &Parsed { week_from_sun: Some(week), weekday: Some(weekday), .. }) => {
                let date = resolve_week_date(year, week, weekday, Weekday::Sun)?;
                (verify_ymd(date) && verify_isoweekdate(date) && verify_ordinal(date), date)
            }

            (Some(year), _, &Parsed { week_from_mon: Some(week), weekday: Some(weekday), .. }) => {
                let date = resolve_week_date(year, week, weekday, Weekday::Mon)?;
                (verify_ymd(date) && verify_isoweekdate(date) && verify_ordinal(date), date)
            }

            (_, Some(isoyear), &Parsed { isoweek: Some(isoweek), weekday: Some(weekday), .. }) => {
                let date = NaiveDate::from_isoywd_opt(isoyear, isoweek, weekday);
                let date = date.ok_or(OUT_OF_RANGE)?;
                (verify_ymd(date) && verify_ordinal(date), date)
            }

            (_, _, _) => return Err(NOT_ENOUGH),
        };

        if !verified {
            return Err(IMPOSSIBLE);
        } else if let Some(parsed) = self.quarter {
            if parsed != parsed_date.quarter() {
                return Err(IMPOSSIBLE);
            }
        }

        Ok(parsed_date)
    }

    /// Returns a parsed naive time out of given fields.
    ///
    /// This method is able to determine the time from given subset of fields:
    ///
    /// - Hour, minute. (second and nanosecond assumed to be 0)
    /// - Hour, minute, second. (nanosecond assumed to be 0)
    /// - Hour, minute, second, nanosecond.
    ///
    /// It is able to handle leap seconds when given second is 60.
    ///
    /// # Errors
    ///
    /// This method returns:
    /// - `OUT_OF_RANGE` if any of the time fields of `Parsed` are set to a value beyond
    ///   their acceptable range.
    /// - `NOT_ENOUGH` if an hour field is missing, if AM/PM is missing in a 12-hour clock,
    ///   if minutes are missing, or if seconds are missing while the nanosecond field is present.
    pub fn to_naive_time(&self) -> ParseResult<NaiveTime> {
        let hour_div_12 = match self.hour_div_12 {
            Some(v @ 0..=1) => v,
            Some(_) => return Err(OUT_OF_RANGE),
            None => return Err(NOT_ENOUGH),
        };
        let hour_mod_12 = match self.hour_mod_12 {
            Some(v @ 0..=11) => v,
            Some(_) => return Err(OUT_OF_RANGE),
            None => return Err(NOT_ENOUGH),
        };
        let hour = hour_div_12 * 12 + hour_mod_12;

        let minute = match self.minute {
            Some(v @ 0..=59) => v,
            Some(_) => return Err(OUT_OF_RANGE),
            None => return Err(NOT_ENOUGH),
        };

        let (second, mut nano) = match self.second.unwrap_or(0) {
            v @ 0..=59 => (v, 0),
            60 => (59, 1_000_000_000),
            _ => return Err(OUT_OF_RANGE),
        };
        nano += match self.nanosecond {
            Some(v @ 0..=999_999_999) if self.second.is_some() => v,
            Some(0..=999_999_999) => return Err(NOT_ENOUGH), 
            Some(_) => return Err(OUT_OF_RANGE),
            None => 0,
        };

        NaiveTime::from_hms_nano_opt(hour, minute, second, nano).ok_or(OUT_OF_RANGE)
    }

    /// Returns a parsed naive date and time out of given fields, except for the offset field.
    ///
    /// The offset is assumed to have a given value. It is not compared against the offset field set
    /// in the `Parsed` type, so it is allowed to be inconsistent.
    ///
    /// This method is able to determine the combined date and time from date and time fields or
    /// from a single timestamp field. It checks all fields are consistent with each other.
    ///
    /// # Errors
    ///
    /// This method returns:
    /// - `IMPOSSIBLE`  if any of the date fields conflict, or if a timestamp conflicts with any of
    ///   the other fields.
    /// - `NOT_ENOUGH` if there are not enough fields set in `Parsed` for a complete datetime.
    /// - `OUT_OF_RANGE`
    ///   - if any of the date or time fields of `Parsed` are set to a value beyond their acceptable
    ///     range.
    ///   - if the value would be outside the range of a [`NaiveDateTime`].
    ///   - if the date does not exist.
    pub fn to_naive_datetime_with_offset(&self, offset: i32) -> ParseResult<NaiveDateTime> {
        let date = self.to_naive_date();
        let time = self.to_naive_time();
        if let (Ok(date), Ok(time)) = (date, time) {
            let datetime = date.and_time(time);

            let timestamp = datetime.and_utc().timestamp() - i64::from(offset);
            if let Some(given_timestamp) = self.timestamp {
                if given_timestamp != timestamp
                    && !(datetime.nanosecond() >= 1_000_000_000 && given_timestamp == timestamp + 1)
                {
                    return Err(IMPOSSIBLE);
                }
            }

            Ok(datetime)
        } else if let Some(timestamp) = self.timestamp {
            use super::ParseError as PE;
            use super::ParseErrorKind::{Impossible, OutOfRange};

            match (date, time) {
                (Err(PE(OutOfRange)), _) | (_, Err(PE(OutOfRange))) => return Err(OUT_OF_RANGE),
                (Err(PE(Impossible)), _) | (_, Err(PE(Impossible))) => return Err(IMPOSSIBLE),
                (_, _) => {} 
            }

            let ts = timestamp.checked_add(i64::from(offset)).ok_or(OUT_OF_RANGE)?;
            let mut datetime = DateTime::from_timestamp_secs(ts).ok_or(OUT_OF_RANGE)?.naive_utc();

            let mut parsed = self.clone();
            if parsed.second == Some(60) {
                match datetime.second() {
                    59 => {}
                    0 => {
                        datetime -= TimeDelta::try_seconds(1).unwrap();
                    }
                    _ => return Err(IMPOSSIBLE),
                }
            } else {
                parsed.set_second(i64::from(datetime.second()))?;
            }
            parsed.set_year(i64::from(datetime.year()))?;
            parsed.set_ordinal(i64::from(datetime.ordinal()))?; 
            parsed.set_hour(i64::from(datetime.hour()))?;
            parsed.set_minute(i64::from(datetime.minute()))?;

            let date = parsed.to_naive_date()?;
            let time = parsed.to_naive_time()?;
            Ok(date.and_time(time))
        } else {
            date?;
            time?;
            unreachable!()
        }
    }

    /// Returns a parsed fixed time zone offset out of given fields.
    ///
    /// # Errors
    ///
    /// This method returns:
    /// - `OUT_OF_RANGE` if the offset is out of range for a `FixedOffset`.
    /// - `NOT_ENOUGH` if the offset field is not set.
    pub fn to_fixed_offset(&self) -> ParseResult<FixedOffset> {
        FixedOffset::east_opt(self.offset.ok_or(NOT_ENOUGH)?).ok_or(OUT_OF_RANGE)
    }

    /// Returns a parsed timezone-aware date and time out of given fields.
    ///
    /// This method is able to determine the combined date and time from date, time and offset
    /// fields, and/or from a single timestamp field. It checks all fields are consistent with each
    /// other.
    ///
    /// # Errors
    ///
    /// This method returns:
    /// - `IMPOSSIBLE`  if any of the date fields conflict, or if a timestamp conflicts with any of
    ///   the other fields.
    /// - `NOT_ENOUGH` if there are not enough fields set in `Parsed` for a complete datetime
    ///   including offset from UTC.
    /// - `OUT_OF_RANGE`
    ///   - if any of the fields of `Parsed` are set to a value beyond their acceptable
    ///     range.
    ///   - if the value would be outside the range of a [`NaiveDateTime`] or [`FixedOffset`].
    ///   - if the date does not exist.
    pub fn to_datetime(&self) -> ParseResult<DateTime<FixedOffset>> {
        let offset = match (self.offset, self.timestamp) {
            (Some(off), _) => off,
            (None, Some(_)) => 0, 
            (None, None) => return Err(NOT_ENOUGH),
        };
        let datetime = self.to_naive_datetime_with_offset(offset)?;
        let offset = FixedOffset::east_opt(offset).ok_or(OUT_OF_RANGE)?;

        match offset.from_local_datetime(&datetime) {
            MappedLocalTime::None => Err(IMPOSSIBLE),
            MappedLocalTime::Single(t) => Ok(t),
            MappedLocalTime::Ambiguous(..) => Err(NOT_ENOUGH),
        }
    }

    /// Returns a parsed timezone-aware date and time out of given fields,
    /// with an additional [`TimeZone`] used to interpret and validate the local date.
    ///
    /// This method is able to determine the combined date and time from date and time, and/or from
    /// a single timestamp field. It checks all fields are consistent with each other.
    ///
    /// If the parsed fields include an UTC offset, it also has to be consistent with the offset in
    /// the provided `tz` time zone for that datetime.
    ///
    /// # Errors
    ///
    /// This method returns:
    /// - `IMPOSSIBLE`
    ///   - if any of the date fields conflict, if a timestamp conflicts with any of the other
    ///     fields, or if the offset field is set but differs from the offset at that time in the
    ///     `tz` time zone.
    ///   - if the local datetime does not exists in the provided time zone (because it falls in a
    ///     transition due to for example DST).
    /// - `NOT_ENOUGH` if there are not enough fields set in `Parsed` for a complete datetime, or if
    ///   the local time in the provided time zone is ambiguous (because it falls in a transition
    ///   due to for example DST) while there is no offset field or timestamp field set.
    /// - `OUT_OF_RANGE`
    ///   - if the value would be outside the range of a [`NaiveDateTime`] or [`FixedOffset`].
    ///   - if any of the fields of `Parsed` are set to a value beyond their acceptable range.
    ///   - if the date does not exist.
    pub fn to_datetime_with_timezone<Tz: TimeZone>(&self, tz: &Tz) -> ParseResult<DateTime<Tz>> {
        let mut guessed_offset = 0;
        if let Some(timestamp) = self.timestamp {
            let nanosecond = self.nanosecond.unwrap_or(0);
            let dt =
                DateTime::from_timestamp(timestamp, nanosecond).ok_or(OUT_OF_RANGE)?.naive_utc();
            guessed_offset = tz.offset_from_utc_datetime(&dt).fix().local_minus_utc();
        }

        let check_offset = |dt: &DateTime<Tz>| {
            if let Some(offset) = self.offset {
                dt.offset().fix().local_minus_utc() == offset
            } else {
                true
            }
        };

        let datetime = self.to_naive_datetime_with_offset(guessed_offset)?;
        match tz.from_local_datetime(&datetime) {
            MappedLocalTime::None => Err(IMPOSSIBLE),
            MappedLocalTime::Single(t) => {
                if check_offset(&t) {
                    Ok(t)
                } else {
                    Err(IMPOSSIBLE)
                }
            }
            MappedLocalTime::Ambiguous(min, max) => {
                match (check_offset(&min), check_offset(&max)) {
                    (false, false) => Err(IMPOSSIBLE),
                    (false, true) => Ok(max),
                    (true, false) => Ok(min),
                    (true, true) => Err(NOT_ENOUGH),
                }
            }
        }
    }

    /// Get the `year` field if set.
    ///
    /// See also [`set_year()`](Parsed::set_year).
    #[inline]
    pub fn year(&self) -> Option<i32> {
        self.year
    }

    /// Get the `year_div_100` field if set.
    ///
    /// See also [`set_year_div_100()`](Parsed::set_year_div_100).
    #[inline]
    pub fn year_div_100(&self) -> Option<i32> {
        self.year_div_100
    }

    /// Get the `year_mod_100` field if set.
    ///
    /// See also [`set_year_mod_100()`](Parsed::set_year_mod_100).
    #[inline]
    pub fn year_mod_100(&self) -> Option<i32> {
        self.year_mod_100
    }

    /// Get the `isoyear` field that is part of an [ISO 8601 week date] if set.
    ///
    /// See also [`set_isoyear()`](Parsed::set_isoyear).
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    #[inline]
    pub fn isoyear(&self) -> Option<i32> {
        self.isoyear
    }

    /// Get the `isoyear_div_100` field that is part of an [ISO 8601 week date] if set.
    ///
    /// See also [`set_isoyear_div_100()`](Parsed::set_isoyear_div_100).
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    #[inline]
    pub fn isoyear_div_100(&self) -> Option<i32> {
        self.isoyear_div_100
    }

    /// Get the `isoyear_mod_100` field that is part of an [ISO 8601 week date] if set.
    ///
    /// See also [`set_isoyear_mod_100()`](Parsed::set_isoyear_mod_100).
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    #[inline]
    pub fn isoyear_mod_100(&self) -> Option<i32> {
        self.isoyear_mod_100
    }

    /// Get the `quarter` field if set.
    ///
    /// See also [`set_quarter()`](Parsed::set_quarter).
    #[inline]
    pub fn quarter(&self) -> Option<u32> {
        self.quarter
    }

    /// Get the `month` field if set.
    ///
    /// See also [`set_month()`](Parsed::set_month).
    #[inline]
    pub fn month(&self) -> Option<u32> {
        self.month
    }

    /// Get the `week_from_sun` field if set.
    ///
    /// See also [`set_week_from_sun()`](Parsed::set_week_from_sun).
    #[inline]
    pub fn week_from_sun(&self) -> Option<u32> {
        self.week_from_sun
    }

    /// Get the `week_from_mon` field if set.
    ///
    /// See also [`set_week_from_mon()`](Parsed::set_week_from_mon).
    #[inline]
    pub fn week_from_mon(&self) -> Option<u32> {
        self.week_from_mon
    }

    /// Get the `isoweek` field that is part of an [ISO 8601 week date] if set.
    ///
    /// See also [`set_isoweek()`](Parsed::set_isoweek).
    ///
    /// [ISO 8601 week date]: crate::NaiveDate#week-date
    #[inline]
    pub fn isoweek(&self) -> Option<u32> {
        self.isoweek
    }

    /// Get the `weekday` field if set.
    ///
    /// See also [`set_weekday()`](Parsed::set_weekday).
    #[inline]
    pub fn weekday(&self) -> Option<Weekday> {
        self.weekday
    }

    /// Get the `ordinal` (day of the year) field if set.
    ///
    /// See also [`set_ordinal()`](Parsed::set_ordinal).
    #[inline]
    pub fn ordinal(&self) -> Option<u32> {
        self.ordinal
    }

    /// Get the `day` of the month field if set.
    ///
    /// See also [`set_day()`](Parsed::set_day).
    #[inline]
    pub fn day(&self) -> Option<u32> {
        self.day
    }

    /// Get the `hour_div_12` field (am/pm) if set.
    ///
    /// 0 indicates AM and 1 indicates PM.
    ///
    /// See also [`set_ampm()`](Parsed::set_ampm) and [`set_hour()`](Parsed::set_hour).
    #[inline]
    pub fn hour_div_12(&self) -> Option<u32> {
        self.hour_div_12
    }

    /// Get the `hour_mod_12` field if set.
    ///
    /// See also [`set_hour12()`](Parsed::set_hour12) and [`set_hour()`](Parsed::set_hour).
    pub fn hour_mod_12(&self) -> Option<u32> {
        self.hour_mod_12
    }

    /// Get the `minute` field if set.
    ///
    /// See also [`set_minute()`](Parsed::set_minute).
    #[inline]
    pub fn minute(&self) -> Option<u32> {
        self.minute
    }

    /// Get the `second` field if set.
    ///
    /// See also [`set_second()`](Parsed::set_second).
    #[inline]
    pub fn second(&self) -> Option<u32> {
        self.second
    }

    /// Get the `nanosecond` field if set.
    ///
    /// See also [`set_nanosecond()`](Parsed::set_nanosecond).
    #[inline]
    pub fn nanosecond(&self) -> Option<u32> {
        self.nanosecond
    }

    /// Get the `timestamp` field if set.
    ///
    /// See also [`set_timestamp()`](Parsed::set_timestamp).
    #[inline]
    pub fn timestamp(&self) -> Option<i64> {
        self.timestamp
    }

    /// Get the `offset` field if set.
    ///
    /// See also [`set_offset()`](Parsed::set_offset).
    #[inline]
    pub fn offset(&self) -> Option<i32> {
        self.offset
    }
}

/// Create a `NaiveDate` when given a year, week, weekday, and the definition at which day of the
/// week a week starts.
///
/// Returns `IMPOSSIBLE` if `week` is `0` or `53` and the `weekday` falls outside the year.
fn resolve_week_date(
    year: i32,
    week: u32,
    weekday: Weekday,
    week_start_day: Weekday,
) -> ParseResult<NaiveDate> {
    if week > 53 {
        return Err(OUT_OF_RANGE);
    }

    let first_day_of_year = NaiveDate::from_yo_opt(year, 1).ok_or(OUT_OF_RANGE)?;
    let first_week_start = 1 + week_start_day.days_since(first_day_of_year.weekday()) as i32;
    let weekday = weekday.days_since(week_start_day) as i32;
    let ordinal = first_week_start + (week as i32 - 1) * 7 + weekday;
    if ordinal <= 0 {
        return Err(IMPOSSIBLE);
    }
    first_day_of_year.with_ordinal(ordinal as u32).ok_or(IMPOSSIBLE)
}
