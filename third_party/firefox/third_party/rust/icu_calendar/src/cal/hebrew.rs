// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::calendar_arithmetic::{ArithmeticDate, DateFieldsResolver, ToExtendedYear};
use crate::error::{
    DateError, DateFromFieldsError, EcmaReferenceYearError, MonthCodeError, UnknownEraError,
};
use crate::options::{DateAddOptions, DateDifferenceOptions};
use crate::options::{DateFromFieldsOptions, Overflow};
use crate::types::{DateFields, MonthInfo, ValidMonthCode};
use crate::RangeError;
use crate::{types, Calendar, Date};
use ::tinystr::tinystr;
use calendrical_calculations::hebrew_keviyah::{Keviyah, YearInfo};
use calendrical_calculations::rata_die::RataDie;

/// The [Hebrew Calendar](https://en.wikipedia.org/wiki/Hebrew_calendar)
///
/// The Hebrew calendar is a lunisolar calendar used as the Jewish liturgical calendar
/// as well as an official calendar in Israel.
///
/// This implementation uses civil month numbering, where Tishrei is the first month of the year.
///
/// The precise algorithm used to calculate the Hebrew Calendar has [changed over time], with
/// the modern one being in place since about 4536 AM (776 CE). This implementation extends
/// proleptically for dates before that.
///
/// [changed over time]: https://hakirah.org/vol20AjdlerAppendices.pdf
///
/// This corresponds to the `"hebrew"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// # Era codes
///
/// This calendar uses a single era code `am`, Anno Mundi. Dates before this era use negative years.
///
/// # Months and days
///
/// The 12 months are called Tishrei (`M01`, 30 days), Ḥešvan (`M02`, 29/30 days),
/// Kīslev (`M03`, 30/29 days), Ṭevet (`M04`, 29 days), Šəvaṭ (`M05`, 30 days), ʾĂdār (`M06`, 29 days),
/// Nīsān (`M07`, 30 days), ʾĪyyar (`M08`, 29 days), Sivan (`M09`, 30 days), Tammūz (`M10`, 29 days),
/// ʾAv (`M11`, 30 days), ʾElūl (`M12`, 29 days).
///
/// Due to Rosh Hashanah postponement rules, Ḥešvan and Kislev vary in length.
///  
/// In leap years (years 3, 6, 8, 11, 17, 19 in a 19-year cycle), the leap month Adar I (`M05L`, 30 days)
/// is inserted before Adar, and Adar is called Adar II (the `formatting_code` returned by [`MonthInfo`]
/// will be `M06L` to mark this, while the `standard_code` remains `M06`).
///
/// Standard years thus have 353-355 days, and leap years 383-385.
#[derive(Copy, Clone, Debug, Hash, Eq, PartialEq, PartialOrd, Ord, Default)]
#[allow(clippy::exhaustive_structs)] 
pub struct Hebrew;

/// The inner date type used for representing [`Date`]s of [`Hebrew`]. See [`Date`] and [`Hebrew`] for more details.
#[derive(Copy, Clone, Debug, Hash, Eq, PartialEq, PartialOrd, Ord)]
pub struct HebrewDateInner(ArithmeticDate<Hebrew>);

impl Hebrew {
    /// Construct a new [`Hebrew`]
    pub fn new() -> Self {
        Hebrew
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, PartialOrd, Ord)]
pub(crate) struct HebrewYearInfo {
    keviyah: Keviyah,
    value: i32,
}

impl ToExtendedYear for HebrewYearInfo {
    fn to_extended_year(&self) -> i32 {
        self.value
    }
}

impl HebrewYearInfo {
    /// Convenience method to compute for a given year. Don't use this if you actually need
    /// a YearInfo that you want to call .new_year() on.
    #[inline]
    fn compute(value: i32) -> Self {
        Self {
            keviyah: YearInfo::compute_for(value).keviyah,
            value,
        }
    }
}

impl DateFieldsResolver for Hebrew {
    type YearInfo = HebrewYearInfo;
    fn days_in_provided_month(info: HebrewYearInfo, ordinal_month: u8) -> u8 {
        info.keviyah.month_len(ordinal_month)
    }

    fn months_in_provided_year(info: HebrewYearInfo) -> u8 {
        if info.keviyah.is_leap() {
            13
        } else {
            12
        }
    }

    #[inline]
    fn year_info_from_era(
        &self,
        era: &[u8],
        era_year: i32,
    ) -> Result<Self::YearInfo, UnknownEraError> {
        match era {
            b"am" => Ok(HebrewYearInfo::compute(era_year)),
            _ => Err(UnknownEraError),
        }
    }

    #[inline]
    fn year_info_from_extended(&self, extended_year: i32) -> Self::YearInfo {
        HebrewYearInfo::compute(extended_year)
    }

    fn reference_year_from_month_day(
        &self,
        month_code: types::ValidMonthCode,
        day: u8,
    ) -> Result<Self::YearInfo, EcmaReferenceYearError> {
        let hebrew_year = match month_code.to_tuple() {
            (1, false) => 5733,
            (2, false) => match day {
                ..=29 => 5733,
                _ => 5732,
            },
            (3, false) => match day {
                ..=29 => 5733,
                _ => 5732,
            },
            (4, false) => match day {
                ..=26 => 5733,
                _ => 5732,
            },
            (5..=12, false) => 5732,
            (5, true) => 5730,
            _ => {
                return Err(EcmaReferenceYearError::MonthCodeNotInCalendar);
            }
        };
        Ok(HebrewYearInfo::compute(hebrew_year))
    }

    fn ordinal_month_from_code(
        &self,
        year: &Self::YearInfo,
        month_code: types::ValidMonthCode,
        options: DateFromFieldsOptions,
    ) -> Result<u8, MonthCodeError> {
        let is_leap_year = year.keviyah.is_leap();
        let ordinal_month = match month_code.to_tuple() {
            (n @ 1..=12, false) => n + (n >= 6 && is_leap_year) as u8,
            (5, true) => {
                if is_leap_year {
                    6
                } else if matches!(options.overflow, Some(Overflow::Constrain)) {
                    6
                } else {
                    return Err(MonthCodeError::NotInYear);
                }
            }
            _ => return Err(MonthCodeError::NotInCalendar),
        };
        Ok(ordinal_month)
    }

    fn month_code_from_ordinal(
        &self,
        year: &Self::YearInfo,
        ordinal_month: u8,
    ) -> types::ValidMonthCode {
        let is_leap = year.keviyah.is_leap();
        ValidMonthCode::new_unchecked(
            ordinal_month - (is_leap && ordinal_month >= 6) as u8,
            ordinal_month == 6 && is_leap,
        )
    }
}

impl crate::cal::scaffold::UnstableSealed for Hebrew {}
impl Calendar for Hebrew {
    type DateInner = HebrewDateInner;
    type Year = types::EraYear;
    type DifferenceError = core::convert::Infallible;

    fn from_codes(
        &self,
        era: Option<&str>,
        year: i32,
        month_code: types::MonthCode,
        day: u8,
    ) -> Result<Self::DateInner, DateError> {
        ArithmeticDate::from_codes(era, year, month_code, day, self).map(HebrewDateInner)
    }

    #[cfg(feature = "unstable")]
    fn from_fields(
        &self,
        fields: DateFields,
        options: DateFromFieldsOptions,
    ) -> Result<Self::DateInner, DateFromFieldsError> {
        ArithmeticDate::from_fields(fields, options, self).map(HebrewDateInner)
    }

    fn from_rata_die(&self, rd: RataDie) -> Self::DateInner {
        let (year_info, year) = YearInfo::year_containing_rd(rd);
        let keviyah = year_info.keviyah;

        let day_in_year = u16::try_from(rd - year_info.new_year() + 1).unwrap_or(u16::MAX);
        let (month, day) = keviyah.month_day_for(day_in_year);

        HebrewDateInner(ArithmeticDate::new_unchecked(
            HebrewYearInfo {
                keviyah,
                value: year,
            },
            month,
            day,
        ))
    }

    fn to_rata_die(&self, date: &Self::DateInner) -> RataDie {
        let ny = date.0.year.keviyah.year_info(date.0.year.value).new_year();
        let days_preceding = date.0.year.keviyah.days_preceding(date.0.month);

        ny + i64::from(days_preceding) + i64::from(date.0.day) - 1
    }

    fn has_cheap_iso_conversion(&self) -> bool {
        false
    }

    fn months_in_year(&self, date: &Self::DateInner) -> u8 {
        Self::months_in_provided_year(date.0.year)
    }

    fn days_in_year(&self, date: &Self::DateInner) -> u16 {
        date.0.year.keviyah.year_length()
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
        date.0.added(duration, self, options).map(HebrewDateInner)
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

    fn debug_name(&self) -> &'static str {
        "Hebrew"
    }

    fn year_info(&self, date: &Self::DateInner) -> Self::Year {
        let extended_year = date.0.year.value;
        types::EraYear {
            era_index: Some(0),
            era: tinystr!(16, "am"),
            year: extended_year,
            extended_year,
            ambiguity: types::YearAmbiguity::CenturyRequired,
        }
    }

    fn is_in_leap_year(&self, date: &Self::DateInner) -> bool {
        date.0.year.keviyah.is_leap()
    }

    fn month(&self, date: &Self::DateInner) -> MonthInfo {
        let valid_standard_code = self.month_code_from_ordinal(&date.0.year, date.0.month);

        let valid_formatting_code = if valid_standard_code.number() == 6 && date.0.month == 7 {
            ValidMonthCode::new_unchecked(6, true) 
        } else {
            valid_standard_code
        };

        types::MonthInfo {
            ordinal: date.0.month,
            standard_code: valid_standard_code.to_month_code(),
            valid_standard_code,
            formatting_code: valid_formatting_code.to_month_code(),
            valid_formatting_code,
        }
    }

    fn day_of_month(&self, date: &Self::DateInner) -> types::DayOfMonth {
        types::DayOfMonth(date.0.day)
    }

    fn day_of_year(&self, date: &Self::DateInner) -> types::DayOfYear {
        types::DayOfYear(date.0.year.keviyah.days_preceding(date.0.month) + date.0.day as u16)
    }

    fn calendar_algorithm(&self) -> Option<crate::preferences::CalendarAlgorithm> {
        Some(crate::preferences::CalendarAlgorithm::Hebrew)
    }
}

impl Date<Hebrew> {
    /// This method uses an ordinal month, which is probably not what you want.
    ///
    /// Use [`Date::try_new_from_codes`]
    #[deprecated(since = "2.1.0", note = "use `Date::try_new_from_codes`")]
    pub fn try_new_hebrew(
        year: i32,
        ordinal_month: u8,
        day: u8,
    ) -> Result<Date<Hebrew>, RangeError> {
        let year = HebrewYearInfo::compute(year);

        ArithmeticDate::try_from_ymd(year, ordinal_month, day)
            .map(HebrewDateInner)
            .map(|inner| Date::from_raw(inner, Hebrew))
    }
}
