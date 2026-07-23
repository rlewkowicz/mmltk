// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::cal::iso::Iso;
use crate::calendar_arithmetic::DateFieldsResolver;
use crate::calendar_arithmetic::{ArithmeticDate, ToExtendedYear};
use crate::error::{
    DateError, DateFromFieldsError, EcmaReferenceYearError, MonthCodeError, UnknownEraError,
};
use crate::options::{DateAddOptions, DateDifferenceOptions};
use crate::options::{DateFromFieldsOptions, Overflow};
use crate::types::ValidMonthCode;
use crate::AsCalendar;
use crate::{types, Calendar, Date};
use calendrical_calculations::chinese_based::{
    self, ChineseBased, YearBounds, WELL_BEHAVED_ASTRONOMICAL_RANGE,
};
use calendrical_calculations::rata_die::RataDie;
use icu_locale_core::preferences::extensions::unicode::keywords::CalendarAlgorithm;
use icu_provider::prelude::*;

#[path = "east_asian_traditional/china_data.rs"]
mod china_data;
#[path = "east_asian_traditional/korea_data.rs"]
mod korea_data;
#[path = "east_asian_traditional/qing_data.rs"]
mod qing_data;
#[path = "east_asian_traditional/simple.rs"]
mod simple;

/// The traditional East-Asian lunisolar calendar.
///
/// This calendar used traditionally in China as well as in other countries in East Asia is
/// often used today to track important cultural events and holidays like the Lunar New Year.
///
/// The type parameter specifies a particular set of calculation rules and local
/// time information, which differs by country and over time.
/// It must implement the currently-unstable `Rules` trait, at the moment this crate exports two stable
/// implementors of `Rules`: [`China`] and [`Korea`]. Please comment on [this issue](https://github.com/unicode-org/icu4x/issues/6962)
/// if you would like to see this trait stabilized.
///
/// This corresponds to the `"chinese"` and `"dangi"` [CLDR calendars](https://unicode.org/reports/tr35/#UnicodeCalendarIdentifier)
/// respectively, when used with the [`China`] and [`Korea`] [`Rules`] types.
///
/// # Year and Era codes
///
/// Unlike most calendars, the traditional East-Asian calendar does not traditionally count years in an infinitely
/// increasing sequence. Instead, 10 "celestial stems" and 12 "terrestrial branches" are combined to form a
/// cycle of year names which repeats every 60 years. However, for the purposes of calendar calculations and
/// conversions, this calendar also counts years based on the [`Gregorian`](crate::cal::Gregorian) (ISO) calendar.
/// This "related ISO year" marks the Gregorian year in which a traditional East-Asian year begins.
///
/// Because the traditional East-Asian calendar does not traditionally count years, era codes are not used in this calendar.
///
/// For more information, suggested reading materials include:
/// * _Calendrical Calculations_ by Reingold & Dershowitz
/// * _The Mathematics of the Chinese Calendar_ by Helmer Aslaksen <https://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.139.9311&rep=rep1&type=pdf>
/// * Wikipedia: <https://en.wikipedia.org/wiki/Chinese_calendar>
///
/// # Months and days
///
/// The 12 months (`M01`-`M12`) don't use names in modern usage, instead they are referred to as
/// e.g. 三月 (third month) using Chinese characters.
///
/// As a lunar calendar, the lengths of the months depend on the lunar cycle (a month starts on the day of
/// local new moon), and will be either 29 or 30 days. As 12 such months fall short of a solar year, a leap
/// month is inserted roughly every 3 years; this can be after any month (e.g. `M02L`).
///
/// Both the lengths of the months and the occurence of leap months are determined by the
/// concrete [`Rules`] implementation.
///
/// The length of the year is 353-355 days, and the length of the leap year 383-385 days.
///
/// # Calendar drift
///
/// As leap months are determined with respect to the solar year, this calendar stays anchored
/// to the seasons.
#[derive(Clone, Debug, Default, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[allow(clippy::exhaustive_structs)] 
pub struct EastAsianTraditional<R>(pub R);

/// The rules for the [`EastAsianTraditional`] calendar.
///
/// The calendar depends on both astronomical calculations and local time.
/// The rules for how to perform these calculations, as well as how local
/// time is determined differ between countries and have changed over time.
///
/// This crate currently provides [`Rules`] for [`China`] and [`Korea`].
///
/// <div class="stab unstable">
/// 🚫 This trait is sealed; it should not be implemented by user code. If an API requests an item that implements this
/// trait, please consider using a type from the implementors listed below.
///
/// It is still possible to implement this trait in userland (since `UnstableSealed` is public),
/// do not do so unless you are prepared for things to occasionally break.
/// </div>
pub trait Rules: Clone + core::fmt::Debug + crate::cal::scaffold::UnstableSealed {
    /// Returns data about the given year.
    fn year_data(&self, related_iso: i32) -> EastAsianTraditionalYearData;

    /// Returns an ECMA reference year that contains the given month-day combination.
    ///
    /// If the day is out of range, it will return a year that contains the given month
    /// and the maximum day possible for that month. See [the spec][spec] for the
    /// precise algorithm used.
    ///
    /// This API only matters when using [`MissingFieldsStrategy::Ecma`] to compute
    /// a date without providing a year in [`Date::try_from_fields()`]. The default impl
    /// will just error, and custom calendars who do not care about ECMA/Temporal
    /// reference years do not need to override this.
    ///
    /// [spec]: https://tc39.es/proposal-temporal/#sec-temporal-nonisomonthdaytoisoreferencedate
    /// [`MissingFieldsStrategy::Ecma`]: crate::options::MissingFieldsStrategy::Ecma
    fn ecma_reference_year(
        &self,
        _month_code: (u8, bool),
        _day: u8,
    ) -> Result<i32, EcmaReferenceYearError> {
        Err(EcmaReferenceYearError::Unimplemented)
    }

    /// The debug name for the calendar defined by these [`Rules`].
    fn debug_name(&self) -> &'static str {
        "Chinese (custom)"
    }

    /// The BCP-47 [`CalendarAlgorithm`] for the calendar defined by these [`Rules`], if defined.
    fn calendar_algorithm(&self) -> Option<CalendarAlgorithm> {
        None
    }
}

/// The [Chinese](https://en.wikipedia.org/wiki/Chinese_calendar) variant of the [`EastAsianTraditional`] calendar.
///
/// This type agrees with the official data published by the
/// [Purple Mountain Observatory for the years 1900-2025], as well as with
/// the data published by the [Hong Kong Observatory for the years 1901-2100].
///
/// For years since 1912, this uses the [GB/T 33661-2017] rules.
/// As accurate computation is computationally expensive, years until
/// 2100 are precomputed, and after that this type regresses to a simplified
/// calculation. If accuracy beyond 2100 is required, clients
/// can implement their own [`Rules`] type containing more precomputed data.
/// We note that the calendar is inherently uncertain for some future dates.
///
/// Before 1912 [different rules](https://ytliu.epizy.com/Shixian/Shixian_summary.html)
/// were used. This type produces correct data for the years 1900-1912, and
/// falls back to a simplified calculation before 1900. If accuracy is
/// required before 1900, clients can implement their own [`Rules`] type
/// using data such as from the excellent compilation by [Yuk Tung Liu].
///
/// The precise behavior of this calendar may change in the future if:
/// - New ground truth is established by published government sources
/// - We decide to tweak the simplified calculation
/// - We decide to expand or reduce the range where we are correctly handling past dates.
///
/// [Purple Mountain Observatory for the years 1900-2025]: http://www.pmo.cas.cn/xwdt2019/kpdt2019/202203/P020250414456381274062.pdf
/// [Hong Kong Observatory for the years 1901-2100]: https://www.hko.gov.hk/en/gts/time/conversion.htm
/// [GB/T 33661-2017]: China::gb_t_33661_2017
/// [Yuk Tung Liu]: https://ytliu0.github.io/ChineseCalendar/table.html
pub type ChineseTraditional = EastAsianTraditional<China>;

/// The [`Rules`] used in China.
///
/// See [`ChineseTraditional`] for more information.
#[derive(Copy, Clone, Debug, Default)]
#[non_exhaustive]
pub struct China;

impl China {
    /// Computes [`EastAsianTraditionalYearData`] according to [GB/T 33661-2017],
    /// as implemented by [`calendrical_calculations::chinese_based::Chinese`].
    ///
    /// The rules specified in [GB/T 33661-2017] have only been used
    /// since 1912, applying them proleptically to years before 1912 will not
    /// necessarily match historical calendars.
    ///
    /// Note that for future years there is a small degree of uncertainty, as
    /// [GB/T 33661-2017] depends on the uncertain future [difference between UT1
    /// and UTC](https://en.wikipedia.org/wiki/Leap_second#Future).
    /// As noted by
    /// [Yuk Tung Liu](https://ytliu0.github.io/ChineseCalendar/computation.html#modern),
    /// years as early as 2057, 2089, and 2097 have lunar events very close to
    /// local midnight, which might affect the start of a (single) month if additional
    /// leap seconds are introduced.
    ///
    /// [GB/T 33661-2017]: https://openstd.samr.gov.cn/bzgk/gb/newGbInfo?hcno=E107EA4DE9725EDF819F33C60A44B296
    pub fn gb_t_33661_2017(related_iso: i32) -> EastAsianTraditionalYearData {
        EastAsianTraditionalYearData::calendrical_calculations::<chinese_based::Chinese>(
            related_iso,
        )
    }
}

impl crate::cal::scaffold::UnstableSealed for China {}
impl Rules for China {
    fn year_data(&self, related_iso: i32) -> EastAsianTraditionalYearData {
        if let Some(year) = EastAsianTraditionalYearData::lookup(
            related_iso,
            china_data::STARTING_YEAR,
            china_data::DATA,
        ) {
            year
        } else if related_iso > china_data::STARTING_YEAR {
            EastAsianTraditionalYearData::simple(simple::UTC_PLUS_8, related_iso)
        } else if let Some(year) = EastAsianTraditionalYearData::lookup(
            related_iso,
            qing_data::STARTING_YEAR,
            qing_data::DATA,
        ) {
            year
        } else {
            EastAsianTraditionalYearData::simple(simple::BEIJING_UTC_OFFSET, related_iso)
        }
    }

    fn ecma_reference_year(
        &self,
        month_code: (u8, bool),
        day: u8,
    ) -> Result<i32, EcmaReferenceYearError> {
        let (number, is_leap) = month_code;
        let extended_year = match (number, is_leap, day > 29) {
            (1, false, false) => 1972,
            (1, false, true) => 1970,
            (1, true, false) => 1898,
            (1, true, true) => 1898,
            (2, false, false) => 1972,
            (2, false, true) => 1972,
            (2, true, false) => 1947,
            (2, true, true) => 1830,
            (3, false, false) => 1972,
            (3, false, true) => 1966,
            (3, true, false) => 1966,
            (3, true, true) => 1955,
            (4, false, false) => 1972,
            (4, false, true) => 1970,
            (4, true, false) => 1963,
            (4, true, true) => 1944,
            (5, false, false) => 1972,
            (5, false, true) => 1972,
            (5, true, false) => 1971,
            (5, true, true) => 1952,
            (6, false, false) => 1972,
            (6, false, true) => 1971,
            (6, true, false) => 1960,
            (6, true, true) => 1941,
            (7, false, false) => 1972,
            (7, false, true) => 1972,
            (7, true, false) => 1968,
            (7, true, true) => 1938,
            (8, false, false) => 1972,
            (8, false, true) => 1971,
            (8, true, false) => 1957,
            (8, true, true) => 1691,
            (9, false, false) => 1972,
            (9, false, true) => 1972,
            (9, true, false) => 2014,
            (9, true, true) => 1843,
            (10, false, false) => 1972,
            (10, false, true) => 1972,
            (10, true, false) => 1984,
            (10, true, true) => 1737,
            (11, false, false) if day > 26 => 1971,
            (11, false, false) => 1972,
            (11, false, true) => 1969,
            (11, true, false) => 2033,
            (11, true, true) => 1889,
            (12, false, false) => 1971,
            (12, false, true) => 1971,
            (12, true, false) => 1878,
            (12, true, true) => 1783,
            _ => return Err(EcmaReferenceYearError::MonthCodeNotInCalendar),
        };
        Ok(extended_year)
    }

    fn calendar_algorithm(&self) -> Option<CalendarAlgorithm> {
        Some(CalendarAlgorithm::Chinese)
    }

    fn debug_name(&self) -> &'static str {
        "Chinese"
    }
}

/// The [Korean](https://en.wikipedia.org/wiki/Korean_calendar) variant of the [`EastAsianTraditional`] calendar.
///
/// This type agrees with the official data published by the
/// [Korea Astronomy and Space Science Institute for the years 1900-2050].
///
/// For years since 1912, this uses [adapted GB/T 33661-2017] rules,
/// using Korea time instead of Beijing Time.
/// As accurate computation is computationally expensive, years until
/// 2100 are precomputed, and after that this type regresses to a simplified
/// calculation. If accuracy beyond 2100 is required, clients
/// can implement their own [`Rules`] type containing more precomputed data.
/// We note that the calendar is inherently uncertain for some future dates.
///
/// Before 1912 [different rules](https://ytliu.epizy.com/Shixian/Shixian_summary.html)
/// were used (those of Qing-dynasty China). This type produces correct data
/// for the years 1900-1912, and falls back to a simplified calculation
/// before 1900. If accuracy is required before 1900, clients can implement
/// their own [`Rules`] type using data such as from the excellent compilation
/// by [Yuk Tung Liu].
///
/// The precise behavior of this calendar may change in the future if:
/// - New ground truth is established by published government sources
/// - We decide to tweak the simplified calculation
/// - We decide to expand or reduce the range where we are correctly handling past dates.
///
/// [Korea Astronomy and Space Science Institute for the years 1900-2050]: https://astro.kasi.re.kr/life/pageView/5
/// [adapted GB/T 33661-2017]: Korea::adapted_gb_t_33661_2017
/// [GB/T 33661-2017]: China::gb_t_33661_2017
/// [Yuk Tung Liu]: https://ytliu0.github.io/ChineseCalendar/table.html
///
/// ```rust
/// use icu::calendar::cal::{ChineseTraditional, KoreanTraditional};
/// use icu::calendar::Date;
///
/// let iso_a = Date::try_new_iso(2012, 4, 23).unwrap();
/// let korean_a = iso_a.to_calendar(KoreanTraditional::new());
/// let chinese_a = iso_a.to_calendar(ChineseTraditional::new());
///
/// assert_eq!(korean_a.month().standard_code.0, "M03L");
/// assert_eq!(chinese_a.month().standard_code.0, "M04");
///
/// let iso_b = Date::try_new_iso(2012, 5, 23).unwrap();
/// let korean_b = iso_b.to_calendar(KoreanTraditional::new());
/// let chinese_b = iso_b.to_calendar(ChineseTraditional::new());
///
/// assert_eq!(korean_b.month().standard_code.0, "M04");
/// assert_eq!(chinese_b.month().standard_code.0, "M04L");
/// ```
pub type KoreanTraditional = EastAsianTraditional<Korea>;

/// The [`Rules`] used in Korea.
///
/// See [`KoreanTraditional`] for more information.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
#[non_exhaustive]
pub struct Korea;

impl Korea {
    /// A version of [`China::gb_t_33661_2017`] adapted for Korean time.
    ///
    /// See [`China::gb_t_33661_2017`] for caveats.
    pub fn adapted_gb_t_33661_2017(related_iso: i32) -> EastAsianTraditionalYearData {
        EastAsianTraditionalYearData::calendrical_calculations::<chinese_based::Dangi>(related_iso)
    }
}

impl KoreanTraditional {
    /// Creates a new [`KoreanTraditional`] calendar.
    pub const fn new() -> Self {
        Self(Korea)
    }

    /// Use [`Self::new`].
    #[cfg(feature = "serde")]
    #[doc = icu_provider::gen_buffer_unstable_docs!(BUFFER,Self::new)]
    #[deprecated(since = "2.1.0", note = "use `Self::new()")]
    pub fn try_new_with_buffer_provider(
        _provider: &(impl icu_provider::buf::BufferProvider + ?Sized),
    ) -> Result<Self, DataError> {
        Ok(Self::new())
    }

    /// Use [`Self::new`].
    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new)]
    #[deprecated(since = "2.1.0", note = "use `Self::new()")]
    pub fn try_new_unstable<D: ?Sized>(_provider: &D) -> Result<Self, DataError> {
        Ok(Self::new())
    }

    /// Use [`Self::new`].
    #[deprecated(since = "2.1.0", note = "use `Self::new()")]
    pub fn new_always_calculating() -> Self {
        Self::new()
    }
}

impl crate::cal::scaffold::UnstableSealed for Korea {}
impl Rules for Korea {
    fn year_data(&self, related_iso: i32) -> EastAsianTraditionalYearData {
        if let Some(year) = EastAsianTraditionalYearData::lookup(
            related_iso,
            korea_data::STARTING_YEAR,
            korea_data::DATA,
        ) {
            year
        } else if related_iso > korea_data::STARTING_YEAR {
            EastAsianTraditionalYearData::simple(simple::UTC_PLUS_9, related_iso)
        } else if let Some(year) = EastAsianTraditionalYearData::lookup(
            related_iso,
            qing_data::STARTING_YEAR,
            qing_data::DATA,
        ) {
            year
        } else {
            EastAsianTraditionalYearData::simple(simple::BEIJING_UTC_OFFSET, related_iso)
        }
    }

    fn ecma_reference_year(
        &self,
        month_code: (u8, bool),
        day: u8,
    ) -> Result<i32, EcmaReferenceYearError> {
        let (number, is_leap) = month_code;
        let extended_year = match (number, is_leap, day > 29) {
            (1, false, false) => 1972,
            (1, false, true) => 1970,
            (1, true, false) => 1898,
            (1, true, true) => 1898,
            (2, false, false) => 1972,
            (2, false, true) => 1972,
            (2, true, false) => 1947,
            (2, true, true) => 1830,
            (3, false, false) => 1972,
            (3, false, true) => 1968,
            (3, true, false) => 1966,
            (3, true, true) => 1955,
            (4, false, false) => 1972,
            (4, false, true) => 1970,
            (4, true, false) => 1963,
            (4, true, true) => 1944,
            (5, false, false) => 1972,
            (5, false, true) => 1972,
            (5, true, false) => 1971,
            (5, true, true) => 1952,
            (6, false, false) => 1972,
            (6, false, true) => 1971,
            (6, true, false) => 1960,
            (6, true, true) => 1941,
            (7, false, false) => 1972,
            (7, false, true) => 1972,
            (7, true, false) => 1968,
            (7, true, true) => 1938,
            (8, false, false) => 1972,
            (8, false, true) => 1971,
            (8, true, false) => 1957,
            (8, true, true) => 1691,
            (9, false, false) => 1972,
            (9, false, true) => 1972,
            (9, true, false) => 2014,
            (9, true, true) => 1843,
            (10, false, false) => 1972,
            (10, false, true) => 1972,
            (10, true, false) => 1984,
            (10, true, true) => 1737,
            (11, false, false) if day > 26 => 1971,
            (11, false, false) => 1972,
            (11, false, true) => 1969,
            (11, true, false) => 2033,
            (11, true, true) => 1889,
            (12, false, false) => 1971,
            (12, false, true) => 1971,
            (12, true, false) => 1878,
            (12, true, true) => 1783,
            _ => return Err(EcmaReferenceYearError::MonthCodeNotInCalendar),
        };
        Ok(extended_year)
    }

    fn calendar_algorithm(&self) -> Option<CalendarAlgorithm> {
        Some(CalendarAlgorithm::Dangi)
    }
    fn debug_name(&self) -> &'static str {
        "Korean"
    }
}

impl<A: AsCalendar<Calendar = KoreanTraditional>> Date<A> {
    /// This method uses an ordinal month, which is probably not what you want.
    ///
    /// Use [`Date::try_new_from_codes`]
    #[deprecated(since = "2.1.0", note = "use `Date::try_new_from_codes`")]
    pub fn try_new_dangi_with_calendar(
        related_iso_year: i32,
        ordinal_month: u8,
        day: u8,
        calendar: A,
    ) -> Result<Date<A>, DateError> {
        ArithmeticDate::try_from_ymd(
            calendar.as_calendar().0.year_data(related_iso_year),
            ordinal_month,
            day,
        )
        .map(ChineseDateInner)
        .map(|inner| Date::from_raw(inner, calendar))
        .map_err(Into::into)
    }
}

/// The inner date type used for representing [`Date`]s of [`EastAsianTraditional`].
#[derive(Debug, Clone)]
pub struct ChineseDateInner<R: Rules>(ArithmeticDate<EastAsianTraditional<R>>);

impl<R: Rules> Copy for ChineseDateInner<R> {}
impl<R: Rules> PartialEq for ChineseDateInner<R> {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}
impl<R: Rules> Eq for ChineseDateInner<R> {}
impl<R: Rules> PartialOrd for ChineseDateInner<R> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl<R: Rules> Ord for ChineseDateInner<R> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.0.cmp(&other.0)
    }
}

impl ChineseTraditional {
    /// Creates a new [`ChineseTraditional`] calendar.
    pub const fn new() -> Self {
        EastAsianTraditional(China)
    }

    #[cfg(feature = "serde")]
    #[doc = icu_provider::gen_buffer_unstable_docs!(BUFFER,Self::new)]
    #[deprecated(since = "2.1.0", note = "use `Self::new()")]
    pub fn try_new_with_buffer_provider(
        _provider: &(impl icu_provider::buf::BufferProvider + ?Sized),
    ) -> Result<Self, DataError> {
        Ok(Self::new())
    }

    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::new)]
    #[deprecated(since = "2.1.0", note = "use `Self::new()")]
    pub fn try_new_unstable<D: ?Sized>(_provider: &D) -> Result<Self, DataError> {
        Ok(Self::new())
    }

    /// Use [`Self::new()`].
    #[deprecated(since = "2.1.0", note = "use `Self::new()")]
    pub fn new_always_calculating() -> Self {
        Self::new()
    }
}

impl<R: Rules> DateFieldsResolver for EastAsianTraditional<R> {
    type YearInfo = EastAsianTraditionalYearData;

    fn days_in_provided_month(year: EastAsianTraditionalYearData, month: u8) -> u8 {
        29 + year.packed.month_has_30_days(month) as u8
    }

    /// Returns the number of months in a given year, which is 13 in a leap year, and 12 in a common year.
    fn months_in_provided_year(year: EastAsianTraditionalYearData) -> u8 {
        12 + year.packed.leap_month().is_some() as u8
    }

    #[inline]
    fn year_info_from_era(
        &self,
        _era: &[u8],
        _era_year: i32,
    ) -> Result<Self::YearInfo, UnknownEraError> {
        Err(UnknownEraError)
    }

    #[inline]
    fn year_info_from_extended(&self, extended_year: i32) -> Self::YearInfo {
        self.0.year_data(extended_year)
    }

    #[inline]
    fn reference_year_from_month_day(
        &self,
        month_code: types::ValidMonthCode,
        day: u8,
    ) -> Result<Self::YearInfo, EcmaReferenceYearError> {
        self.0
            .ecma_reference_year(month_code.to_tuple(), day)
            .map(|y| self.0.year_data(y))
    }

    fn ordinal_month_from_code(
        &self,
        year: &Self::YearInfo,
        month_code: types::ValidMonthCode,
        options: DateFromFieldsOptions,
    ) -> Result<u8, MonthCodeError> {
        let leap_month = year.packed.leap_month().unwrap_or(14);

        if month_code == ValidMonthCode::new_unchecked(leap_month - 1, true) {
            return Ok(leap_month);
        }

        let (number @ 1..13, leap) = month_code.to_tuple() else {
            return Err(MonthCodeError::NotInCalendar);
        };

        if leap && options.overflow != Some(Overflow::Constrain) {
            return Err(MonthCodeError::NotInYear);
        }

        Ok(number + (number >= leap_month) as u8)
    }

    fn month_code_from_ordinal(&self, year: &Self::YearInfo, ordinal_month: u8) -> ValidMonthCode {
        let leap_month = year.packed.leap_month().unwrap_or(14);
        ValidMonthCode::new_unchecked(
            ordinal_month - (ordinal_month >= leap_month) as u8,
            ordinal_month == leap_month,
        )
    }
}

impl<R: Rules> crate::cal::scaffold::UnstableSealed for EastAsianTraditional<R> {}
impl<R: Rules> Calendar for EastAsianTraditional<R> {
    type DateInner = ChineseDateInner<R>;
    type Year = types::CyclicYear;
    type DifferenceError = core::convert::Infallible;

    fn from_codes(
        &self,
        era: Option<&str>,
        year: i32,
        month_code: types::MonthCode,
        day: u8,
    ) -> Result<Self::DateInner, DateError> {
        ArithmeticDate::from_codes(era, year, month_code, day, self).map(ChineseDateInner)
    }

    #[cfg(feature = "unstable")]
    fn from_fields(
        &self,
        fields: types::DateFields,
        options: DateFromFieldsOptions,
    ) -> Result<Self::DateInner, DateFromFieldsError> {
        ArithmeticDate::from_fields(fields, options, self).map(ChineseDateInner)
    }

    fn from_rata_die(&self, rd: RataDie) -> Self::DateInner {
        let iso = Iso.from_rata_die(rd);
        let year = {
            let candidate = self.0.year_data(iso.0.year);

            if rd >= candidate.new_year() {
                candidate
            } else {
                self.0.year_data(iso.0.year - 1)
            }
        };

        let rd = rd.clamp(
            year.new_year(),
            year.new_year() + year.packed.days_in_year() as i64,
        );

        let day_of_year = (rd - year.new_year()) as u16;

        let mut month = (day_of_year / 30) as u8 + 1;
        let mut last_day_of_month = year.packed.last_day_of_month(month);
        let mut last_day_of_prev_month = year.packed.last_day_of_month(month - 1);

        while day_of_year >= last_day_of_month {
            month += 1;
            last_day_of_prev_month = last_day_of_month;
            last_day_of_month = year.packed.last_day_of_month(month);
        }

        let day = (day_of_year + 1 - last_day_of_prev_month) as u8;

        ChineseDateInner(ArithmeticDate::new_unchecked(year, month, day))
    }

    fn to_rata_die(&self, date: &Self::DateInner) -> RataDie {
        date.0.year.new_year()
            + date.0.year.packed.last_day_of_month(date.0.month - 1) as i64
            + (date.0.day - 1) as i64
    }

    fn has_cheap_iso_conversion(&self) -> bool {
        false
    }

    fn days_in_year(&self, date: &Self::DateInner) -> u16 {
        date.0.year.packed.days_in_year()
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
        date.0.added(duration, self, options).map(ChineseDateInner)
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

    /// Obtain a name for the calendar for debug printing
    fn debug_name(&self) -> &'static str {
        self.0.debug_name()
    }

    fn year_info(&self, date: &Self::DateInner) -> Self::Year {
        let year = date.0.year;
        types::CyclicYear {
            year: (year.related_iso - 4).rem_euclid(60) as u8 + 1,
            related_iso: year.related_iso,
        }
    }

    fn is_in_leap_year(&self, date: &Self::DateInner) -> bool {
        date.0.year.packed.leap_month().is_some()
    }

    /// The calendar-specific month code represented by `date`;
    /// since the Chinese calendar has leap months, an "L" is appended to the month code for
    /// leap months. For example, in a year where an intercalary month is added after the second
    /// month, the month codes for ordinal months 1, 2, 3, 4, 5 would be "M01", "M02", "M02L", "M03", "M04".
    fn month(&self, date: &Self::DateInner) -> types::MonthInfo {
        types::MonthInfo::for_code_and_ordinal(
            self.month_code_from_ordinal(&date.0.year, date.0.month),
            date.0.month,
        )
    }

    /// The calendar-specific day-of-month represented by `date`
    fn day_of_month(&self, date: &Self::DateInner) -> types::DayOfMonth {
        types::DayOfMonth(date.0.day)
    }

    /// Information of the day of the year
    fn day_of_year(&self, date: &Self::DateInner) -> types::DayOfYear {
        types::DayOfYear(date.0.year.packed.last_day_of_month(date.0.month - 1) + date.0.day as u16)
    }

    fn calendar_algorithm(&self) -> Option<CalendarAlgorithm> {
        self.0.calendar_algorithm()
    }

    fn months_in_year(&self, date: &Self::DateInner) -> u8 {
        Self::months_in_provided_year(date.0.year)
    }
}

impl<A: AsCalendar<Calendar = ChineseTraditional>> Date<A> {
    /// This method uses an ordinal month, which is probably not what you want.
    ///
    /// Use [`Date::try_new_from_codes`]
    #[deprecated(since = "2.1.0", note = "use `Date::try_new_from_codes`")]
    pub fn try_new_chinese_with_calendar(
        related_iso_year: i32,
        ordinal_month: u8,
        day: u8,
        calendar: A,
    ) -> Result<Date<A>, DateError> {
        ArithmeticDate::try_from_ymd(
            calendar.as_calendar().0.year_data(related_iso_year),
            ordinal_month,
            day,
        )
        .map(ChineseDateInner)
        .map(|inner| Date::from_raw(inner, calendar))
        .map_err(Into::into)
    }
}

/// Information about a [`EastAsianTraditional`] year.
#[derive(Copy, Clone, Debug, Eq, PartialEq, PartialOrd, Ord)]
pub struct EastAsianTraditionalYearData {
    /// Contains:
    /// - length of each month in the year
    /// - whether or not there is a leap month, and which month it is
    /// - the date of Chinese New Year in the related ISO year
    packed: PackedEastAsianTraditionalYearData,
    related_iso: i32,
}

impl ToExtendedYear for EastAsianTraditionalYearData {
    fn to_extended_year(&self) -> i32 {
        self.related_iso
    }
}

impl EastAsianTraditionalYearData {
    /// Creates [`EastAsianTraditionalYearData`] from the given parts.
    ///
    /// `start_day` is the date for the first day of the year, see [`Date::to_rata_die`]
    /// to obtain a [`RataDie`] from a [`Date`] in an arbitrary calendar.
    ///
    /// `leap_month` is the ordinal number of the leap month, for example if a year
    /// has months 1, 2, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, the `leap_month`
    /// would be `Some(4)`.
    ///
    /// `month_lengths[n - 1]` is true if the nth month has 30 days, and false otherwise.
    /// The leap month does not necessarily have the same number of days as the previous
    /// month, which is why this has length 13. In non-leap years, the last value is ignored.
    pub fn new(
        related_iso: i32,
        start_day: RataDie,
        month_lengths: [bool; 13],
        leap_month: Option<u8>,
    ) -> Self {
        Self {
            packed: PackedEastAsianTraditionalYearData::new(
                related_iso,
                month_lengths,
                leap_month,
                start_day,
            ),
            related_iso,
        }
    }

    fn lookup(
        related_iso: i32,
        starting_year: i32,
        data: &[PackedEastAsianTraditionalYearData],
    ) -> Option<Self> {
        Some(related_iso)
            .and_then(|e| usize::try_from(e.checked_sub(starting_year)?).ok())
            .and_then(|i| data.get(i))
            .map(|&packed| Self {
                related_iso,
                packed,
            })
    }

    fn calendrical_calculations<CB: ChineseBased>(
        related_iso: i32,
    ) -> EastAsianTraditionalYearData {
        let mid_year = calendrical_calculations::gregorian::fixed_from_gregorian(related_iso, 7, 1);
        let year_bounds = YearBounds::compute::<CB>(mid_year);

        let YearBounds {
            new_year,
            next_new_year,
            ..
        } = year_bounds;
        let (month_lengths, leap_month) =
            chinese_based::month_structure_for_year::<CB>(new_year, next_new_year);

        EastAsianTraditionalYearData {
            packed: PackedEastAsianTraditionalYearData::new(
                related_iso,
                month_lengths,
                leap_month,
                new_year,
            ),
            related_iso,
        }
    }

    /// Get the new year R.D.    
    fn new_year(self) -> RataDie {
        self.packed.new_year(self.related_iso)
    }
}

/// The struct containing compiled ChineseData
///
/// Bit structure (little endian: note that shifts go in the opposite direction!)
///
/// ```text
/// Bit:             0   1   2   3   4   5   6   7
/// Byte 0:          [  month lengths .............
/// Byte 1:         .. month lengths ] | [ leap month index ..
/// Byte 2:          ] | [   NY offset       ] | unused
/// ```
///
/// Where the New Year Offset is the offset from ISO Jan 19 of that year for Chinese New Year,
/// the month lengths are stored as 1 = 30, 0 = 29 for each month including the leap month.
/// The largest possible offset is 33, which requires 6 bits of storage.
///
/// <div class="stab unstable">
/// 🚧 This code is considered unstable; it may change at any time, in breaking or non-breaking ways,
/// including in SemVer minor releases. While the serde representation of data structs is guaranteed
/// to be stable, their Rust representation might not be. Use with caution.
/// </div>
#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct PackedEastAsianTraditionalYearData(u8, u8, u8);

impl PackedEastAsianTraditionalYearData {
    /// The first day on which Chinese New Year may occur
    ///
    /// According to Reingold & Dershowitz, ch 19.6, Chinese New Year occurs on Jan 21 - Feb 21 inclusive.
    ///
    /// Our simple approximation sometimes returns Feb 22.
    ///
    /// We allow it to occur as early as January 19 which is the earliest the second new moon
    /// could occur after the Winter Solstice if the solstice is pinned to December 20.
    const fn earliest_ny(related_iso: i32) -> RataDie {
        calendrical_calculations::gregorian::fixed_from_gregorian(related_iso, 1, 19)
    }

    /// It clamps some values to avoid debug assertions on calendrical invariants.
    const fn new(
        related_iso: i32,
        month_lengths: [bool; 13],
        leap_month: Option<u8>,
        new_year: RataDie,
    ) -> Self {
        if let Some(l) = leap_month {
            debug_assert!(2 <= l && l <= 13, "Leap month indices must be 2 <= i <= 13");
        } else {
            debug_assert!(
                !month_lengths[12],
                "Last month length should not be set for non-leap years"
            )
        }

        let ny_offset = new_year.since(Self::earliest_ny(related_iso));

        #[cfg(debug_assertions)]
        let out_of_valid_astronomical_range = WELL_BEHAVED_ASTRONOMICAL_RANGE.start.to_i64_date()
            > new_year.to_i64_date()
            || new_year.to_i64_date() > WELL_BEHAVED_ASTRONOMICAL_RANGE.end.to_i64_date();

        #[cfg(debug_assertions)]
        debug_assert!(
            ny_offset >= 0 || out_of_valid_astronomical_range,
            "Year offset too small to store"
        );
        #[cfg(debug_assertions)]
        debug_assert!(
            ny_offset < 35 || out_of_valid_astronomical_range,
            "Year offset too big to store"
        );

        let ny_offset = ny_offset & (0x40 - 1);

        let mut all = 0u32; 

        let mut month = 0;
        while month < month_lengths.len() {
            #[allow(clippy::indexing_slicing)] 
            if month_lengths[month] {
                all |= 1 << month as u32;
            }
            month += 1;
        }
        let leap_month_idx = if let Some(leap_month_idx) = leap_month {
            leap_month_idx
        } else {
            0
        };
        all |= (leap_month_idx as u32) << (8 + 5);
        all |= (ny_offset as u32) << (16 + 1);
        let le = all.to_le_bytes();
        Self(le[0], le[1], le[2])
    }

    fn new_year(self, related_iso: i32) -> RataDie {
        Self::earliest_ny(related_iso) + (self.2 as i64 >> 1)
    }

    fn leap_month(self) -> Option<u8> {
        let bits = (self.1 >> 5) + ((self.2 & 0b1) << 3);

        (bits != 0).then_some(bits)
    }

    fn month_has_30_days(self, month: u8) -> bool {
        let months = u16::from_le_bytes([self.0, self.1]);
        months & (1 << (month - 1) as u16) != 0
    }

    fn last_day_of_month(self, month: u8) -> u16 {
        let months = u16::from_le_bytes([self.0, self.1]);
        let mut prev_month_lengths = 29 * month as u16;
        let long_month_bits = months & ((1 << month as u16) - 1);
        prev_month_lengths += long_month_bits.count_ones().try_into().unwrap_or(0);
        prev_month_lengths
    }

    fn days_in_year(self) -> u16 {
        self.last_day_of_month(12 + self.leap_month().is_some() as u8)
    }
}
