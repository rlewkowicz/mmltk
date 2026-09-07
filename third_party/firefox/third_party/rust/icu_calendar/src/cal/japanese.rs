// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::cal::abstract_gregorian::{impl_with_abstract_gregorian, GregorianYears};
use crate::cal::gregorian::CeBce;
use crate::calendar_arithmetic::ArithmeticDate;
use crate::error::{DateError, UnknownEraError};
use crate::provider::{CalendarJapaneseExtendedV1, CalendarJapaneseModernV1, EraStartDate};
use crate::{types, AsCalendar, Date};
use icu_provider::prelude::*;
use tinystr::tinystr;

/// The [Japanese Calendar] (with modern eras only)
///
/// The [Japanese Calendar] is a variant of the [`Gregorian`](crate::cal::Gregorian) calendar
/// created by the Japanese government. It is identical to the Gregorian calendar except that
/// is uses Japanese eras instead of the Common Era.
///
/// This implementation extends proleptically for dates before the calendar's creation
/// in 6 Meiji (1873 CE).
/// The Meiji era is used proleptically back to and including 1868-10-23, Gregorian eras are used before that.
///
/// For a variant that uses approximations of historical Japanese eras proleptically, check out [`JapaneseExtended`].
///
/// This corresponds to the `"japanese"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// [Japanese calendar]: https://en.wikipedia.org/wiki/Japanese_calendar
///
/// # Era codes
///
/// This calendar currently supports seven era codes. It supports the five eras since its
/// introduction (`meiji`, `taisho`, `showa`, `heisei`, `reiwa`), as well as the Gregorian
/// `bce` (alias `bc`), and `ce` (alias `ad`) for earlier dates.
///
/// Future eras will also be added to this type when they are decided.
///
/// These eras are loaded from data, requiring a data provider capable of providing [`CalendarJapaneseModernV1`]
/// data.
#[derive(Clone, Debug, Default)]
pub struct Japanese {
    eras: DataPayload<CalendarJapaneseModernV1>,
}

/// The [Japanese Calendar] (with historical eras)
///
/// The [Japanese Calendar] is a variant of the [`Gregorian`](crate::cal::Gregorian) calendar
/// created by the Japanese government. It is identical to the Gregorian calendar except that
/// is uses Japanese eras instead of the Common Era.
///
/// This implementation extends proleptically for dates before the calendar's creation
/// in 6 Meiji (1873 CE).
/// This implementation uses approximations of earlier Japanese eras proleptically and uses the Gregorian eras for
/// even earlier dates that don't have an approximate Japanese era.
///
/// For a variant whose Japanese eras start with Meiji, check out [`Japanese`].
///
/// This corresponds to the `"japanext"` [CLDR calendar](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier).
///
/// [Japanese calendar]: https://en.wikipedia.org/wiki/Japanese_calendar
///
/// # Era codes
///
/// This calendar supports a large number of era codes. It supports the five eras since its introduction
/// (`meiji`, `taisho`, `showa`, `heisei`, `reiwa`). Proleptic eras are represented
/// with their names converted to lowercase ASCII and followed by their start year. E.g. the *Ten'ō*
/// era (781 - 782 CE) has the code `teno-781`. The  Gregorian `bce` (alias `bc`), and `ce` (alias `ad`)
/// are used for dates before the first known era era.
///
///
/// These eras are loaded from data, requiring a data provider capable of providing [`CalendarJapaneseExtendedV1`]
/// data.
#[derive(Clone, Debug, Default)]
pub struct JapaneseExtended(Japanese);

impl Japanese {
    /// Creates a new [`Japanese`] using only modern eras (post-meiji) from compiled data.
    ///
    /// ✨ *Enabled with the `compiled_data` Cargo feature.*
    ///
    /// [📚 Help choosing a constructor](icu_provider::constructors)
    #[cfg(feature = "compiled_data")]
    pub const fn new() -> Self {
        Self {
            eras: DataPayload::from_static_ref(
                crate::provider::Baked::SINGLETON_CALENDAR_JAPANESE_MODERN_V1,
            ),
        }
    }

    icu_provider::gen_buffer_data_constructors!(() -> error: DataError,
        functions: [
            new: skip,
            try_new_with_buffer_provider,
            try_new_unstable,
            Self,
    ]);

    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new)]
    pub fn try_new_unstable<D: DataProvider<CalendarJapaneseModernV1> + ?Sized>(
        provider: &D,
    ) -> Result<Self, DataError> {
        Ok(Self {
            eras: provider.load(Default::default())?.payload,
        })
    }
}

impl JapaneseExtended {
    /// Creates a new [`Japanese`] from using all eras (including pre-meiji) from compiled data.
    ///
    /// ✨ *Enabled with the `compiled_data` Cargo feature.*
    ///
    /// [📚 Help choosing a constructor](icu_provider::constructors)
    #[cfg(feature = "compiled_data")]
    pub const fn new() -> Self {
        Self(Japanese {
            eras: DataPayload::from_static_ref(
                crate::provider::Baked::SINGLETON_CALENDAR_JAPANESE_EXTENDED_V1,
            ),
        })
    }

    icu_provider::gen_buffer_data_constructors!(() -> error: DataError,
        functions: [
            new: skip,
            try_new_with_buffer_provider,
            try_new_unstable,
            Self,
    ]);

    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new)]
    pub fn try_new_unstable<D: DataProvider<CalendarJapaneseExtendedV1> + ?Sized>(
        provider: &D,
    ) -> Result<Self, DataError> {
        Ok(Self(Japanese {
            eras: provider.load(Default::default())?.payload.cast(),
        }))
    }
}

const MEIJI_START: EraStartDate = EraStartDate {
    year: 1868,
    month: 10,
    day: 23,
};
const TAISHO_START: EraStartDate = EraStartDate {
    year: 1912,
    month: 7,
    day: 30,
};
const SHOWA_START: EraStartDate = EraStartDate {
    year: 1926,
    month: 12,
    day: 25,
};
const HEISEI_START: EraStartDate = EraStartDate {
    year: 1989,
    month: 1,
    day: 8,
};
const REIWA_START: EraStartDate = EraStartDate {
    year: 2019,
    month: 5,
    day: 1,
};

impl GregorianYears for &'_ Japanese {
    fn extended_from_era_year(
        &self,
        era: Option<&[u8]>,
        year: i32,
    ) -> Result<i32, UnknownEraError> {
        if let Ok(g) = CeBce.extended_from_era_year(era, year) {
            return Ok(g);
        }
        let Some(era) = era else {
            return Err(UnknownEraError);
        };

        if era == b"reiwa" {
            return Ok(year - 1 + REIWA_START.year);
        } else if era == b"heisei" {
            return Ok(year - 1 + HEISEI_START.year);
        } else if era == b"showa" {
            return Ok(year - 1 + SHOWA_START.year);
        } else if era == b"taisho" {
            return Ok(year - 1 + TAISHO_START.year);
        } else if era == b"meiji" {
            return Ok(year - 1 + MEIJI_START.year);
        }

        let data = &self.eras.get().dates_to_eras;

        if let Some(start_year) = era
            .split(|x| *x == b'-')
            .nth(1)
            .and_then(|y| core::str::from_utf8(y).ok()?.parse::<i32>().ok())
        {
            if let Ok(index) = data.binary_search_by(|(d, _)| d.year.cmp(&start_year)) {
                #[expect(clippy::unwrap_used)] 
                if data.get(index).unwrap().1.as_bytes() == era {
                    return Ok(start_year + year - 1);
                }
            }
        }

        let era_start = data
            .iter()
            .rev()
            .find_map(|(s, e)| (e.as_bytes() == era).then_some(s))
            .ok_or(UnknownEraError)?;
        Ok(era_start.year + year - 1)
    }

    fn era_year_from_extended(&self, year: i32, month: u8, day: u8) -> types::EraYear {
        let date: EraStartDate = EraStartDate { year, month, day };

        let (start, era) = if date >= MEIJI_START
            && self
                .eras
                .get()
                .dates_to_eras
                .last()
                .is_some_and(|(_, e)| e == tinystr!(16, "reiwa"))
        {
            if date >= REIWA_START {
                (REIWA_START, tinystr!(16, "reiwa"))
            } else if date >= HEISEI_START {
                (HEISEI_START, tinystr!(16, "heisei"))
            } else if date >= SHOWA_START {
                (SHOWA_START, tinystr!(16, "showa"))
            } else if date >= TAISHO_START {
                (TAISHO_START, tinystr!(16, "taisho"))
            } else {
                (MEIJI_START, tinystr!(16, "meiji"))
            }
        } else {
            let data = &self.eras.get().dates_to_eras;
            #[allow(clippy::unwrap_used)] 
            match data.binary_search_by(|(d, _)| d.cmp(&date)) {
                Err(0) => {
                    return types::EraYear {
                        era_index: None,
                        ..CeBce.era_year_from_extended(year, month, day)
                    };
                }
                Ok(index) => data.get(index).unwrap(),
                Err(index) => data.get(index - 1).unwrap(),
            }
        };

        types::EraYear {
            era,
            era_index: None,
            year: year - start.year + 1,
            extended_year: year,
            ambiguity: types::YearAmbiguity::CenturyRequired,
        }
    }

    fn debug_name(&self) -> &'static str {
        if self.eras.get().dates_to_eras.len() > 10 {
            "Japanese (historical era data)"
        } else {
            "Japanese"
        }
    }

    fn calendar_algorithm(&self) -> Option<crate::preferences::CalendarAlgorithm> {
        if self.eras.get().dates_to_eras.len() > 10 {
            None
        } else {
            Some(crate::preferences::CalendarAlgorithm::Japanese)
        }
    }
}

impl_with_abstract_gregorian!(Japanese, JapaneseDateInner, Japanese, this, this);

impl_with_abstract_gregorian!(
    JapaneseExtended,
    JapaneseExtendedDateInner,
    Japanese,
    this,
    &this.0
);

impl Date<Japanese> {
    /// Construct a new Japanese Date.
    ///
    /// Years are specified in the era provided, and must be in range for Japanese
    /// eras (e.g. dates past April 30 Heisei 31 must be in Reiwa; "Jun 5 Heisei 31" and "Jan 1 Heisei 32"
    /// will not be adjusted to being in Reiwa 1 and 2 respectively)
    ///
    /// However, dates may always be specified in "bce" or "ce" and they will be adjusted as necessary.
    ///
    /// ```rust
    /// use icu::calendar::cal::Japanese;
    /// use icu::calendar::{Date, Ref};
    /// use tinystr::tinystr;
    ///
    /// let japanese_calendar = Japanese::new();
    /// // for easy sharing
    /// let japanese_calendar = Ref(&japanese_calendar);
    ///
    /// let era = "heisei";
    ///
    /// let date =
    ///     Date::try_new_japanese_with_calendar(era, 14, 1, 2, japanese_calendar)
    ///         .expect("Constructing a date should succeed");
    ///
    /// assert_eq!(date.era_year().era, era);
    /// assert_eq!(date.era_year().year, 14);
    /// assert_eq!(date.month().ordinal, 1);
    /// assert_eq!(date.day_of_month().0, 2);
    ///
    /// // This function will error for unknown eras
    /// let fake_era = "neko"; // 🐱
    /// let fake_date = Date::try_new_japanese_with_calendar(
    ///     fake_era,
    ///     10,
    ///     1,
    ///     2,
    ///     japanese_calendar,
    /// );
    /// assert!(fake_date.is_err());
    /// ```
    pub fn try_new_japanese_with_calendar<A: AsCalendar<Calendar = Japanese>>(
        era: &str,
        year: i32,
        month: u8,
        day: u8,
        japanese_calendar: A,
    ) -> Result<Date<A>, DateError> {
        let extended = japanese_calendar
            .as_calendar()
            .extended_from_era_year(Some(era.as_bytes()), year)?;
        Ok(Date::from_raw(
            JapaneseDateInner(ArithmeticDate::new_gregorian::<&Japanese>(
                extended, month, day,
            )?),
            japanese_calendar,
        ))
    }
}

impl Date<JapaneseExtended> {
    /// Construct a new Japanese Date with all eras.
    ///
    /// Years are specified in the era provided, and must be in range for Japanese
    /// eras (e.g. dates past April 30 Heisei 31 must be in Reiwa; "Jun 5 Heisei 31" and "Jan 1 Heisei 32"
    /// will not be adjusted to being in Reiwa 1 and 2 respectively)
    ///
    /// However, dates may always be specified in "bce" or "ce" and they will be adjusted as necessary.
    ///
    /// ```rust
    /// use icu::calendar::cal::JapaneseExtended;
    /// use icu::calendar::{Date, Ref};
    /// use tinystr::tinystr;
    ///
    /// let japanext_calendar = JapaneseExtended::new();
    /// // for easy sharing
    /// let japanext_calendar = Ref(&japanext_calendar);
    ///
    /// let era = "kansei-1789";
    ///
    /// let date = Date::try_new_japanese_extended_with_calendar(
    ///     era,
    ///     7,
    ///     1,
    ///     2,
    ///     japanext_calendar,
    /// )
    /// .expect("Constructing a date should succeed");
    ///
    /// assert_eq!(date.era_year().era, era);
    /// assert_eq!(date.era_year().year, 7);
    /// assert_eq!(date.month().ordinal, 1);
    /// assert_eq!(date.day_of_month().0, 2);
    /// ```
    pub fn try_new_japanese_extended_with_calendar<A: AsCalendar<Calendar = JapaneseExtended>>(
        era: &str,
        year: i32,
        month: u8,
        day: u8,
        japanext_calendar: A,
    ) -> Result<Date<A>, DateError> {
        let extended = (&japanext_calendar.as_calendar().0)
            .extended_from_era_year(Some(era.as_bytes()), year)?;
        Ok(Date::from_raw(
            JapaneseExtendedDateInner(ArithmeticDate::new_gregorian::<&Japanese>(
                extended, month, day,
            )?),
            japanext_calendar,
        ))
    }
}
