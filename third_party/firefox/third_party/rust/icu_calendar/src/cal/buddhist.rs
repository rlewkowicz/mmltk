// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::error::UnknownEraError;
use crate::preferences::CalendarAlgorithm;
use crate::{
    cal::abstract_gregorian::{impl_with_abstract_gregorian, GregorianYears},
    calendar_arithmetic::ArithmeticDate,
    types, Date, DateError, RangeError,
};
use tinystr::tinystr;

#[derive(Copy, Clone, Debug, Default)]
/// The [Thai Solar Buddhist Calendar](https://en.wikipedia.org/wiki/Thai_solar_calendar)
///
/// The Thai Solar Buddhist Calendar is a variant of the [`Gregorian`](crate::cal::Gregorian) calendar
/// created by the Thai government. It is identical to the Gregorian calendar except that is uses
/// the Buddhist Era (-543 CE) instead of the Common Era.
///
/// This implementation extends proleptically for dates before the calendar's creation
/// in 2484 BE (1941 CE).
///
/// This corresponds to the `"buddhist"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// # Era codes
///
/// This calendar uses a single era code `be`, with 1 Buddhist Era being 543 BCE. Dates before this era use negative years.
#[allow(clippy::exhaustive_structs)] 
pub struct Buddhist;

impl_with_abstract_gregorian!(
    crate::cal::Buddhist,
    BuddhistDateInner,
    BuddhistEra,
    _x,
    BuddhistEra
);

#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct BuddhistEra;

impl GregorianYears for BuddhistEra {
    const EXTENDED_YEAR_OFFSET: i32 = -543;

    fn extended_from_era_year(
        &self,
        era: Option<&[u8]>,
        year: i32,
    ) -> Result<i32, UnknownEraError> {
        match era {
            Some(b"be") | None => Ok(year),
            _ => Err(UnknownEraError),
        }
    }

    fn era_year_from_extended(&self, extended_year: i32, _month: u8, _day: u8) -> types::EraYear {
        types::EraYear {
            era: tinystr!(16, "be"),
            era_index: Some(0),
            year: extended_year,
            extended_year,
            ambiguity: types::YearAmbiguity::CenturyRequired,
        }
    }

    fn debug_name(&self) -> &'static str {
        "Buddhist"
    }

    fn calendar_algorithm(&self) -> Option<CalendarAlgorithm> {
        Some(CalendarAlgorithm::Buddhist)
    }
}

impl Date<Buddhist> {
    /// Construct a new Buddhist Date.
    ///
    /// Years are specified as BE years.
    ///
    /// ```rust
    /// use icu::calendar::Date;
    ///
    /// let date_buddhist = Date::try_new_buddhist(1970, 1, 2)
    ///     .expect("Failed to initialize Buddhist Date instance.");
    ///
    /// assert_eq!(date_buddhist.era_year().year, 1970);
    /// assert_eq!(date_buddhist.month().ordinal, 1);
    /// assert_eq!(date_buddhist.day_of_month().0, 2);
    /// ```
    pub fn try_new_buddhist(year: i32, month: u8, day: u8) -> Result<Date<Buddhist>, RangeError> {
        ArithmeticDate::new_gregorian::<BuddhistEra>(year, month, day)
            .map(BuddhistDateInner)
            .map(|i| Date::from_raw(i, Buddhist))
    }
}
