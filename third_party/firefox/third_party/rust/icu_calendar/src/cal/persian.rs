// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::calendar_arithmetic::ArithmeticDate;
use crate::calendar_arithmetic::DateFieldsResolver;
use crate::error::{DateError, DateFromFieldsError, EcmaReferenceYearError, UnknownEraError};
use crate::options::DateFromFieldsOptions;
use crate::options::{DateAddOptions, DateDifferenceOptions};
use crate::types::DateFields;
use crate::{types, Calendar, Date, RangeError};
use ::tinystr::tinystr;
use calendrical_calculations::helpers::I32CastError;
use calendrical_calculations::rata_die::RataDie;

/// The [Persian Calendar](https://en.wikipedia.org/wiki/Solar_Hijri_calendar)
///
/// The Persian Calendar is a solar calendar used officially by the countries of Iran and
/// Afghanistan and many Persian-speaking regions.
///
/// This implementation extends proleptically for dates before the calendar's creation
/// in 458 AP (1079 CE).
///
/// This corresponds to the `"persian"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// # Era codes
///
/// This calendar uses a single era code `ap`, with Anno Persico/Anno Persarum starting the year of the Hijra. Dates before this era use negative years.
///
/// # Months and days
///
/// The 12 months are called Farvardin (`M01`, 31 days), Ordibehesht (`M02`, 31 days),
/// Khordad (`M03`, 31 days), Tir (`M04`, 31 days), Mordad (`M05`, 31 days), Shahrivar (`M06`, 31 days),
/// Mehr (`M07`, 30 days), Aban (`M08`, 30 days), Azar (`M09`, 30 days),
/// Dey (`M10`, 30 days), Bahman (`M11`, 30 days), Esfand (`M12`, 29 days).
///
/// In leap years (determined astronomically with respect to the vernal equinox), Esfand gains a 30th day.
///
/// Standard years thus have 365 days, and leap years 366.
///
/// # Calendar drift
///
/// As leap years are determined with respect to the solar year, this calendar stays anchored
/// to the seasons.
#[derive(Copy, Clone, Debug, Default, Hash, Eq, PartialEq, PartialOrd, Ord)]
#[allow(clippy::exhaustive_structs)]
pub struct Persian;

#[derive(Copy, Clone, Debug, Hash, Eq, PartialEq, PartialOrd, Ord)]

/// The inner date type used for representing [`Date`]s of [`Persian`]. See [`Date`] and [`Persian`] for more details.
pub struct PersianDateInner(ArithmeticDate<Persian>);

impl DateFieldsResolver for Persian {
    type YearInfo = i32;

    fn days_in_provided_month(year: i32, month: u8) -> u8 {
        match month {
            1..=6 => 31,
            7..=11 => 30,
            12 if calendrical_calculations::persian::is_leap_year(year) => 30,
            12 => 29,
            _ => 0,
        }
    }

    fn months_in_provided_year(_: i32) -> u8 {
        12
    }

    #[inline]
    fn year_info_from_era(
        &self,
        era: &[u8],
        era_year: i32,
    ) -> Result<Self::YearInfo, UnknownEraError> {
        match era {
            b"ap" => Ok(era_year),
            _ => Err(UnknownEraError),
        }
    }

    #[inline]
    fn year_info_from_extended(&self, extended_year: i32) -> Self::YearInfo {
        extended_year
    }

    #[inline]
    fn reference_year_from_month_day(
        &self,
        month_code: types::ValidMonthCode,
        day: u8,
    ) -> Result<Self::YearInfo, EcmaReferenceYearError> {
        let (ordinal_month, false) = month_code.to_tuple() else {
            return Err(EcmaReferenceYearError::MonthCodeNotInCalendar);
        };
        let persian_year = if ordinal_month < 10 || (ordinal_month == 10 && day <= 10) {
            1351
        } else {
            1350
        };
        Ok(persian_year)
    }
}

impl crate::cal::scaffold::UnstableSealed for Persian {}
impl Calendar for Persian {
    type DateInner = PersianDateInner;
    type Year = types::EraYear;
    type DifferenceError = core::convert::Infallible;

    fn from_codes(
        &self,
        era: Option<&str>,
        year: i32,
        month_code: types::MonthCode,
        day: u8,
    ) -> Result<Self::DateInner, DateError> {
        ArithmeticDate::from_codes(era, year, month_code, day, self).map(PersianDateInner)
    }

    #[cfg(feature = "unstable")]
    fn from_fields(
        &self,
        fields: DateFields,
        options: DateFromFieldsOptions,
    ) -> Result<Self::DateInner, DateFromFieldsError> {
        ArithmeticDate::from_fields(fields, options, self).map(PersianDateInner)
    }

    fn from_rata_die(&self, rd: RataDie) -> Self::DateInner {
        PersianDateInner(
            match calendrical_calculations::persian::fast_persian_from_fixed(rd) {
                Err(I32CastError::BelowMin) => ArithmeticDate::new_unchecked(i32::MIN, 1, 1),
                Err(I32CastError::AboveMax) => ArithmeticDate::new_unchecked(i32::MAX, 12, 29),
                Ok((year, month, day)) => ArithmeticDate::new_unchecked(year, month, day),
            },
        )
    }

    fn to_rata_die(&self, date: &Self::DateInner) -> RataDie {
        calendrical_calculations::persian::fixed_from_fast_persian(
            date.0.year,
            date.0.month,
            date.0.day,
        )
    }

    fn has_cheap_iso_conversion(&self) -> bool {
        false
    }

    fn months_in_year(&self, date: &Self::DateInner) -> u8 {
        Self::months_in_provided_year(date.0.year)
    }

    fn days_in_year(&self, date: &Self::DateInner) -> u16 {
        if self.is_in_leap_year(date) {
            366
        } else {
            365
        }
    }

    fn days_in_month(&self, date: &Self::DateInner) -> u8 {
        Self::days_in_provided_month(date.0.year, date.0.month)
    }

    #[cfg(feature = "unstable")]
    fn add(
        &self,
        date: &Self::DateInner,
        duration: types::DateDuration,
        options: DateAddOptions,
    ) -> Result<Self::DateInner, DateError> {
        date.0.added(duration, self, options).map(PersianDateInner)
    }

    #[cfg(feature = "unstable")]
    fn until(
        &self,
        date1: &Self::DateInner,
        date2: &Self::DateInner,
        options: DateDifferenceOptions,
    ) -> Result<types::DateDuration, Self::DifferenceError> {
        Ok(date1.0.until(&date2.0, self, options))
    }

    fn year_info(&self, date: &Self::DateInner) -> Self::Year {
        let extended_year = date.0.year;
        types::EraYear {
            era: tinystr!(16, "ap"),
            era_index: Some(0),
            year: extended_year,
            extended_year,
            ambiguity: types::YearAmbiguity::CenturyRequired,
        }
    }

    fn is_in_leap_year(&self, date: &Self::DateInner) -> bool {
        calendrical_calculations::persian::is_leap_year(date.0.year)
    }

    fn month(&self, date: &Self::DateInner) -> types::MonthInfo {
        types::MonthInfo::non_lunisolar(date.0.month)
    }

    fn day_of_month(&self, date: &Self::DateInner) -> types::DayOfMonth {
        types::DayOfMonth(date.0.day)
    }

    fn day_of_year(&self, date: &Self::DateInner) -> types::DayOfYear {
        types::DayOfYear(
            (date.0.month as u16 - 1) * 31 - (date.0.month as u16 - 1).saturating_sub(6)
                + date.0.day as u16,
        )
    }

    fn debug_name(&self) -> &'static str {
        "Persian"
    }

    fn calendar_algorithm(&self) -> Option<crate::preferences::CalendarAlgorithm> {
        Some(crate::preferences::CalendarAlgorithm::Persian)
    }
}

impl Persian {
    /// Constructs a new Persian Calendar
    pub fn new() -> Self {
        Self
    }
}

impl Date<Persian> {
    /// Construct new Persian Date.
    ///
    /// Has no negative years, only era is the AH/AP.
    ///
    /// ```rust
    /// use icu::calendar::Date;
    ///
    /// let date_persian = Date::try_new_persian(1392, 4, 25)
    ///     .expect("Failed to initialize Persian Date instance.");
    ///
    /// assert_eq!(date_persian.era_year().year, 1392);
    /// assert_eq!(date_persian.month().ordinal, 4);
    /// assert_eq!(date_persian.day_of_month().0, 25);
    /// ```
    pub fn try_new_persian(year: i32, month: u8, day: u8) -> Result<Date<Persian>, RangeError> {
        ArithmeticDate::try_from_ymd(year, month, day)
            .map(PersianDateInner)
            .map(|inner| Date::from_raw(inner, Persian))
    }
}
