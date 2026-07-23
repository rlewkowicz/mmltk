// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::cal::abstract_gregorian::{impl_with_abstract_gregorian, GregorianYears};
use crate::calendar_arithmetic::ArithmeticDate;
use crate::error::UnknownEraError;
use crate::{types, Date, DateError, RangeError};
use tinystr::tinystr;

/// The [ISO-8601 Calendar](https://en.wikipedia.org/wiki/ISO_8601#Dates)
///
/// This calendar is identical to the [`Gregorian`](super::Gregorian) calendar,
/// except that it uses a single `default` era instead of `bce` and `ce`.
///
/// This corresponds to the `"iso8601"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// # Era codes
///
/// This calendar uses a single era: `default`
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[allow(clippy::exhaustive_structs)] 
pub struct Iso;

impl_with_abstract_gregorian!(crate::cal::Iso, IsoDateInner, IsoEra, _x, IsoEra);

#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct IsoEra;

impl GregorianYears for IsoEra {
    fn extended_from_era_year(
        &self,
        era: Option<&[u8]>,
        year: i32,
    ) -> Result<i32, UnknownEraError> {
        match era {
            Some(b"default") | None => Ok(year),
            Some(_) => Err(UnknownEraError),
        }
    }

    fn era_year_from_extended(&self, extended_year: i32, _month: u8, _day: u8) -> types::EraYear {
        types::EraYear {
            era_index: Some(0),
            era: tinystr!(16, "default"),
            year: extended_year,
            extended_year,
            ambiguity: types::YearAmbiguity::Unambiguous,
        }
    }

    fn debug_name(&self) -> &'static str {
        "ISO"
    }
}

impl Date<Iso> {
    /// Construct a new ISO date from integers.
    ///
    /// ```rust
    /// use icu::calendar::Date;
    ///
    /// let date_iso = Date::try_new_iso(1970, 1, 2)
    ///     .expect("Failed to initialize ISO Date instance.");
    ///
    /// assert_eq!(date_iso.era_year().year, 1970);
    /// assert_eq!(date_iso.month().ordinal, 1);
    /// assert_eq!(date_iso.day_of_month().0, 2);
    /// ```
    pub fn try_new_iso(year: i32, month: u8, day: u8) -> Result<Date<Iso>, RangeError> {
        ArithmeticDate::new_gregorian::<IsoEra>(year, month, day)
            .map(IsoDateInner)
            .map(|i| Date::from_raw(i, Iso))
    }
}

impl Iso {
    /// Construct a new ISO Calendar
    pub fn new() -> Self {
        Self
    }
}
