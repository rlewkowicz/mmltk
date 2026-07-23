// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::cal::abstract_gregorian::{impl_with_abstract_gregorian, GregorianYears};
use crate::calendar_arithmetic::ArithmeticDate;
use crate::error::UnknownEraError;
use crate::preferences::CalendarAlgorithm;
use crate::{types, Date, DateError, RangeError};
use tinystr::tinystr;

/// The [Republic of China Calendar](https://en.wikipedia.org/wiki/Republic_of_China_calendar)
///
/// The ROC Calendar is a variant of the [`Gregorian`](crate::cal::Gregorian) calendar
/// created by the government of the Republic of China. It is identical to the Gregorian
/// calendar except that is uses the ROC/Minguo/民国/民國 Era (1912 CE) instead of the Common Era.
///
/// This implementation extends proleptically for dates before the calendar's creation
/// in 1 Minguo (1912 CE).
///
/// The ROC calendar should not be confused with the [`ChineseTraditional`](crate::cal::ChineseTraditional)
/// lunisolar calendar.
///
/// This corresponds to the `"roc"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// # Era codes
///
/// This calendar uses two era codes: `roc`, corresponding to years in the 民國 era (CE year 1912 and
/// after), and `broc`, corresponding to years before the 民國 era (CE year 1911 and before).
#[derive(Copy, Clone, Debug, Default)]
#[allow(clippy::exhaustive_structs)] 
pub struct Roc;

impl_with_abstract_gregorian!(crate::cal::Roc, RocDateInner, RocEra, _x, RocEra);

#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct RocEra;

impl GregorianYears for RocEra {
    const EXTENDED_YEAR_OFFSET: i32 = 1911;

    fn extended_from_era_year(
        &self,
        era: Option<&[u8]>,
        year: i32,
    ) -> Result<i32, UnknownEraError> {
        match era {
            None => Ok(year),
            Some(b"roc") => Ok(year),
            Some(b"broc") => Ok(1 - year),
            Some(_) => Err(UnknownEraError),
        }
    }

    fn era_year_from_extended(&self, extended_year: i32, _month: u8, _day: u8) -> types::EraYear {
        if extended_year > 0 {
            types::EraYear {
                era: tinystr!(16, "roc"),
                era_index: Some(1),
                year: extended_year,
                extended_year,
                ambiguity: types::YearAmbiguity::CenturyRequired,
            }
        } else {
            types::EraYear {
                era: tinystr!(16, "broc"),
                era_index: Some(0),
                year: 1 - extended_year,
                extended_year,
                ambiguity: types::YearAmbiguity::EraAndCenturyRequired,
            }
        }
    }

    fn debug_name(&self) -> &'static str {
        "ROC"
    }

    fn calendar_algorithm(&self) -> Option<CalendarAlgorithm> {
        Some(CalendarAlgorithm::Roc)
    }
}

impl Date<Roc> {
    /// Construct a new Republic of China calendar Date.
    ///
    /// Years are specified in the "roc" era. This function accepts an extended year in that era, so dates
    /// before Minguo are negative and year 0 is 1 Before Minguo. To specify dates using explicit era
    /// codes, use [`Date::try_new_from_codes()`].
    ///
    /// ```rust
    /// use icu::calendar::Date;
    /// use icu::calendar::cal::Gregorian;
    /// use tinystr::tinystr;
    ///
    /// // Create a new ROC Date
    /// let date_roc = Date::try_new_roc(1, 2, 3)
    ///     .expect("Failed to initialize ROC Date instance.");
    ///
    /// assert_eq!(date_roc.era_year().era, "roc");
    /// assert_eq!(date_roc.era_year().year, 1, "ROC year check failed!");
    /// assert_eq!(date_roc.month().ordinal, 2, "ROC month check failed!");
    /// assert_eq!(date_roc.day_of_month().0, 3, "ROC day of month check failed!");
    ///
    /// // Convert to an equivalent Gregorian date
    /// let date_gregorian = date_roc.to_calendar(Gregorian);
    ///
    /// assert_eq!(date_gregorian.era_year().year, 1912, "Gregorian from ROC year check failed!");
    /// assert_eq!(date_gregorian.month().ordinal, 2, "Gregorian from ROC month check failed!");
    /// assert_eq!(date_gregorian.day_of_month().0, 3, "Gregorian from ROC day of month check failed!");
    pub fn try_new_roc(year: i32, month: u8, day: u8) -> Result<Date<Roc>, RangeError> {
        ArithmeticDate::new_gregorian::<RocEra>(year, month, day)
            .map(RocDateInner)
            .map(|i| Date::from_raw(i, Roc))
    }
}
