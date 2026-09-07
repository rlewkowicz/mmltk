// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use crate::duration::{DateDuration, DateDurationUnit};
use crate::error::{
    range_check, range_check_with_overflow, DateFromFieldsError, EcmaReferenceYearError,
    MonthCodeError, MonthCodeParseError, UnknownEraError,
};
use crate::options::{DateAddOptions, DateDifferenceOptions};
use crate::options::{DateFromFieldsOptions, MissingFieldsStrategy, Overflow};
use crate::types::{DateFields, ValidMonthCode};
use crate::{types, Calendar, DateError, RangeError};
use core::cmp::Ordering;
use core::fmt::Debug;
use core::hash::{Hash, Hasher};
use core::ops::RangeInclusive;

/// The range ±2²⁷. We use i32::MIN since it is -2³¹
///
/// This range is currently global, and applied to both era years and
/// extended years, but may be replaced with a per-calendar check in the future.
///
/// <https://github.com/unicode-org/icu4x/issues/7076>
const VALID_YEAR_RANGE: RangeInclusive<i32> = (i32::MIN / 16)..=-(i32::MIN / 16);

#[derive(Debug)]
pub(crate) struct ArithmeticDate<C: DateFieldsResolver> {
    pub year: C::YearInfo,
    /// 1-based month of year
    pub month: u8,
    /// 1-based day of month
    pub day: u8,
}

impl<C: DateFieldsResolver> Copy for ArithmeticDate<C> {}
impl<C: DateFieldsResolver> Clone for ArithmeticDate<C> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<C: DateFieldsResolver> PartialEq for ArithmeticDate<C> {
    fn eq(&self, other: &Self) -> bool {
        self.year.to_extended_year() == other.year.to_extended_year()
            && self.month == other.month
            && self.day == other.day
    }
}

impl<C: DateFieldsResolver> Eq for ArithmeticDate<C> {}

impl<C: DateFieldsResolver> Ord for ArithmeticDate<C> {
    fn cmp(&self, other: &Self) -> Ordering {
        self.year
            .to_extended_year()
            .cmp(&other.year.to_extended_year())
            .then(self.month.cmp(&other.month))
            .then(self.day.cmp(&other.day))
    }
}

impl<C: DateFieldsResolver> PartialOrd for ArithmeticDate<C> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl<C: DateFieldsResolver> Hash for ArithmeticDate<C> {
    fn hash<H>(&self, state: &mut H)
    where
        H: Hasher,
    {
        self.year.to_extended_year().hash(state);
        self.month.hash(state);
        self.day.hash(state);
    }
}

/// Maximum number of iterations when iterating through the days of a month; can be increased if necessary
#[allow(dead_code)] 
pub(crate) const MAX_ITERS_FOR_DAYS_OF_MONTH: u8 = 33;

pub(crate) trait ToExtendedYear {
    fn to_extended_year(&self) -> i32;
}

impl ToExtendedYear for i32 {
    fn to_extended_year(&self) -> i32 {
        *self
    }
}

/// Trait for converting from era codes, month codes, and other fields to year/month/day ordinals.
pub(crate) trait DateFieldsResolver: Calendar {
    /// This stores the year as either an i32, or a type containing more
    /// useful computational information.
    type YearInfo: Copy + Debug + PartialEq + ToExtendedYear;

    fn days_in_provided_month(year: Self::YearInfo, month: u8) -> u8;

    fn months_in_provided_year(year: Self::YearInfo) -> u8;

    /// Converts the era and era year to a YearInfo. If the calendar does not have eras,
    /// this should always return an Err result.
    fn year_info_from_era(
        &self,
        era: &[u8],
        era_year: i32,
    ) -> Result<Self::YearInfo, UnknownEraError>;

    /// Converts an extended year to a YearInfo.
    fn year_info_from_extended(&self, extended_year: i32) -> Self::YearInfo;

    /// Calculates the ECMA reference year for the month code and day, or an error
    /// if the month code and day are invalid.
    ///
    /// Note that this is called before any potential Overflow::Constrain application,
    /// so this should accept out-of-range day values as if they are the highest possible
    /// day for the given month.
    fn reference_year_from_month_day(
        &self,
        month_code: ValidMonthCode,
        day: u8,
    ) -> Result<Self::YearInfo, EcmaReferenceYearError>;

    /// Calculates the ordinal month for the given year and month code.
    ///
    /// The default impl is for non-lunisolar calendars with 12 months!
    #[inline]
    fn ordinal_month_from_code(
        &self,
        _year: &Self::YearInfo,
        month_code: ValidMonthCode,
        _options: DateFromFieldsOptions,
    ) -> Result<u8, MonthCodeError> {
        match month_code.to_tuple() {
            (month_number @ 1..=12, false) => Ok(month_number),
            _ => Err(MonthCodeError::NotInCalendar),
        }
    }

    /// Calculates the month code from the given ordinal month and year.
    ///
    /// The caller must ensure that the ordinal is in range.
    ///
    /// The default impl is for non-lunisolar calendars!
    #[inline]
    fn month_code_from_ordinal(&self, _year: &Self::YearInfo, ordinal_month: u8) -> ValidMonthCode {
        ValidMonthCode::new_unchecked(ordinal_month, false)
    }
}

impl<C: DateFieldsResolver> ArithmeticDate<C> {
    #[inline]
    pub(crate) const fn new_unchecked(year: C::YearInfo, month: u8, day: u8) -> Self {
        ArithmeticDate { year, month, day }
    }

    pub(crate) const fn cast<C2: DateFieldsResolver<YearInfo = C::YearInfo>>(
        self,
    ) -> ArithmeticDate<C2> {
        ArithmeticDate {
            year: self.year,
            month: self.month,
            day: self.day,
        }
    }

    pub(crate) fn from_codes(
        era: Option<&str>,
        year: i32,
        month_code: types::MonthCode,
        day: u8,
        calendar: &C,
    ) -> Result<Self, DateError> {
        let year = range_check(year, "year", VALID_YEAR_RANGE)?;
        let year = if let Some(era) = era {
            calendar.year_info_from_era(era.as_bytes(), year)?
        } else {
            calendar.year_info_from_extended(year)
        };
        let validated =
            ValidMonthCode::try_from_utf8(month_code.0.as_bytes()).map_err(|e| match e {
                MonthCodeParseError::InvalidSyntax => DateError::UnknownMonthCode(month_code),
            })?;
        let month = calendar
            .ordinal_month_from_code(&year, validated, Default::default())
            .map_err(|e| match e {
                MonthCodeError::NotInCalendar | MonthCodeError::NotInYear => {
                    DateError::UnknownMonthCode(month_code)
                }
            })?;

        let day = range_check(day, "day", 1..=C::days_in_provided_month(year, month))?;

        Ok(ArithmeticDate::new_unchecked(year, month, day))
    }

    pub(crate) fn from_fields(
        fields: DateFields,
        options: DateFromFieldsOptions,
        calendar: &C,
    ) -> Result<Self, DateFromFieldsError> {
        let missing_fields_strategy = options.missing_fields_strategy.unwrap_or_default();

        let day = match fields.day {
            Some(day) => day,
            None => match missing_fields_strategy {
                MissingFieldsStrategy::Reject => return Err(DateFromFieldsError::NotEnoughFields),
                MissingFieldsStrategy::Ecma => {
                    if fields.extended_year.is_some() || fields.era_year.is_some() {
                        1
                    } else {
                        return Err(DateFromFieldsError::NotEnoughFields);
                    }
                }
            },
        };

        if fields.month_code.is_none() && fields.ordinal_month.is_none() {
            return Err(DateFromFieldsError::NotEnoughFields);
        }

        let mut valid_month_code = None;

        let year = match (fields.era, fields.era_year) {
            (None, None) => match fields.extended_year {
                Some(extended_year) => calendar.year_info_from_extended(range_check(
                    extended_year,
                    "year",
                    VALID_YEAR_RANGE,
                )?),
                None => match missing_fields_strategy {
                    MissingFieldsStrategy::Reject => {
                        return Err(DateFromFieldsError::NotEnoughFields)
                    }
                    MissingFieldsStrategy::Ecma => {
                        match (fields.month_code, fields.ordinal_month) {
                            (Some(month_code), None) => {
                                let validated = ValidMonthCode::try_from_utf8(month_code)?;
                                valid_month_code = Some(validated);
                                calendar.reference_year_from_month_day(validated, day)?
                            }
                            _ => return Err(DateFromFieldsError::NotEnoughFields),
                        }
                    }
                },
            },
            (Some(era), Some(era_year)) => {
                let era_year_as_year_info = calendar
                    .year_info_from_era(era, range_check(era_year, "year", VALID_YEAR_RANGE)?)?;
                if let Some(extended_year) = fields.extended_year {
                    if era_year_as_year_info
                        != calendar.year_info_from_extended(range_check(
                            extended_year,
                            "year",
                            VALID_YEAR_RANGE,
                        )?)
                    {
                        return Err(DateFromFieldsError::InconsistentYear);
                    }
                }
                era_year_as_year_info
            }
            (Some(_), None) | (None, Some(_)) => return Err(DateFromFieldsError::NotEnoughFields),
        };

        let month = match fields.month_code {
            Some(month_code) => {
                let validated = match valid_month_code {
                    Some(validated) => validated,
                    None => ValidMonthCode::try_from_utf8(month_code)?,
                };
                let computed_month = calendar.ordinal_month_from_code(&year, validated, options)?;
                if let Some(ordinal_month) = fields.ordinal_month {
                    if computed_month != ordinal_month {
                        return Err(DateFromFieldsError::InconsistentMonth);
                    }
                }
                computed_month
            }
            None => match fields.ordinal_month {
                Some(month) => month,
                None => {
                    debug_assert!(false, "Already checked above");
                    return Err(DateFromFieldsError::NotEnoughFields);
                }
            },
        };

        let constrained_month = range_check_with_overflow(
            month,
            "month",
            1..=C::months_in_provided_year(year),
            options.overflow.unwrap_or_default(),
        )?;
        Ok(Self::new_unchecked(
            year,
            constrained_month,
            range_check_with_overflow(
                day,
                "day",
                1..=C::days_in_provided_month(year, constrained_month),
                options.overflow.unwrap_or_default(),
            )?,
        ))
    }

    pub(crate) fn try_from_ymd(year: C::YearInfo, month: u8, day: u8) -> Result<Self, RangeError> {
        range_check(month, "month", 1..=C::months_in_provided_year(year))?;
        range_check(day, "day", 1..=C::days_in_provided_month(year, month))?;
        Ok(ArithmeticDate::new_unchecked(year, month, day))
    }

    /// Implements the Temporal abstract operation BalanceNonISODate.
    ///
    /// This takes a year, month, and day, where the month and day might be out of range, then
    /// balances excess months into the year field and excess days into the month field.
    pub(crate) fn new_balanced(year: C::YearInfo, ordinal_month: i64, day: i64, cal: &C) -> Self {
        let mut resolved_year = year;
        let mut resolved_month = ordinal_month;
        let mut months_in_year = C::months_in_provided_year(resolved_year);
        while resolved_month <= 0 {
            resolved_year = cal.year_info_from_extended(resolved_year.to_extended_year() - 1);
            months_in_year = C::months_in_provided_year(resolved_year);
            resolved_month += i64::from(months_in_year);
        }
        while resolved_month > i64::from(months_in_year) {
            resolved_month -= i64::from(months_in_year);
            resolved_year = cal.year_info_from_extended(resolved_year.to_extended_year() + 1);
            months_in_year = C::months_in_provided_year(resolved_year);
        }
        debug_assert!(u8::try_from(resolved_month).is_ok());
        let mut resolved_month = resolved_month as u8;
        let mut resolved_day = day;
        let mut days_in_month = C::days_in_provided_month(resolved_year, resolved_month);
        while resolved_day <= 0 {
            resolved_month -= 1;
            if resolved_month == 0 {
                resolved_year = cal.year_info_from_extended(resolved_year.to_extended_year() - 1);
                months_in_year = C::months_in_provided_year(resolved_year);
                resolved_month = months_in_year;
            }
            days_in_month = C::days_in_provided_month(resolved_year, resolved_month);
            resolved_day += i64::from(days_in_month);
        }
        while resolved_day > i64::from(days_in_month) {
            resolved_day -= i64::from(days_in_month);
            resolved_month += 1;
            if resolved_month > months_in_year {
                resolved_year = cal.year_info_from_extended(resolved_year.to_extended_year() + 1);
                months_in_year = C::months_in_provided_year(resolved_year);
                resolved_month = 1;
            }
            days_in_month = C::days_in_provided_month(resolved_year, resolved_month);
        }
        debug_assert!(u8::try_from(resolved_day).is_ok());
        let resolved_day = resolved_day as u8;
        Self::new_unchecked(resolved_year, resolved_month, resolved_day)
    }

    /// Implements the Temporal abstract operation NonISODateSurpasses.
    ///
    /// This takes two dates (`self` and `other`), `duration`, and `sign` (either -1 or 1), then
    /// returns whether adding the duration to `self` results in a year/month/day that exceeds
    /// `other` in the direction indicated by `sign`, constraining the month but not the day.
    pub(crate) fn surpasses(
        &self,
        other: &Self,
        duration: DateDuration,
        sign: i64,
        cal: &C,
    ) -> bool {
        let y0 = cal.year_info_from_extended(duration.add_years_to(self.year.to_extended_year()));
        let base_month_code = cal.month_code_from_ordinal(&self.year, self.month);
        let constrain = DateFromFieldsOptions {
            overflow: Some(Overflow::Constrain),
            ..Default::default()
        };
        let m0_result = cal.ordinal_month_from_code(&y0, base_month_code, constrain);
        let m0 = match m0_result {
            Ok(m0) => m0,
            Err(_) => {
                debug_assert!(
                    false,
                    "valid month code for calendar, and constrained to the year"
                );
                1
            }
        };
        let end_of_month = Self::new_balanced(y0, duration.add_months_to(m0) + 1, 0, cal);
        let base_day = self.day;
        let y1;
        let m1;
        let d1;
        if duration.weeks != 0 || duration.days != 0 {
            let regulated_day = if base_day < end_of_month.day {
                base_day
            } else {
                end_of_month.day
            };
            let balanced_date = Self::new_balanced(
                end_of_month.year,
                i64::from(end_of_month.month),
                duration.add_weeks_and_days_to(regulated_day),
                cal,
            );
            y1 = balanced_date.year;
            m1 = balanced_date.month;
            d1 = balanced_date.day;
        } else {
            y1 = end_of_month.year;
            m1 = end_of_month.month;
            d1 = base_day;
        }
        #[allow(clippy::collapsible_if)] 
        if y1 != other.year {
            if sign * (i64::from(y1.to_extended_year()) - i64::from(other.year.to_extended_year()))
                > 0
            {
                return true;
            }
        } else if m1 != other.month {
            if sign * (i64::from(m1) - i64::from(other.month)) > 0 {
                return true;
            }
        } else if d1 != other.day {
            if sign * (i64::from(d1) - i64::from(other.day)) > 0 {
                return true;
            }
        }
        false
    }

    /// Implements the Temporal abstract operation NonISODateAdd.
    ///
    /// This takes a date (`self`) and `duration`, then returns a new date resulting from
    /// adding `duration` to `self`, constrained according to `options`.
    pub(crate) fn added(
        &self,
        duration: DateDuration,
        cal: &C,
        options: DateAddOptions,
    ) -> Result<Self, DateError> {
        let y0 = cal.year_info_from_extended(duration.add_years_to(self.year.to_extended_year()));
        let base_month = cal.month_code_from_ordinal(&self.year, self.month);
        let m0 = cal
            .ordinal_month_from_code(
                &y0,
                base_month,
                DateFromFieldsOptions::from_add_options(options),
            )
            .map_err(|e| {
                match e {
                    MonthCodeError::NotInCalendar => {
                        DateError::UnknownMonthCode(base_month.to_month_code())
                    }
                    MonthCodeError::NotInYear => {
                        DateError::UnknownMonthCode(base_month.to_month_code())
                    }
                }
            })?;
        let end_of_month = Self::new_balanced(y0, duration.add_months_to(m0) + 1, 0, cal);
        let base_day = self.day;
        let regulated_day = if base_day < end_of_month.day {
            base_day
        } else {
            if matches!(options.overflow, Some(Overflow::Reject)) {
                return Err(DateError::Range {
                    field: "day",
                    value: i32::from(base_day),
                    min: 1,
                    max: i32::from(end_of_month.day),
                });
            }
            end_of_month.day
        };
        Ok(Self::new_balanced(
            end_of_month.year,
            i64::from(end_of_month.month),
            duration.add_weeks_and_days_to(regulated_day),
            cal,
        ))
    }

    /// Implements the Temporal abstract operation NonISODateUntil.
    ///
    /// This takes a duration (`self`) and a date (`other`), then returns a duration that, when
    /// added to `self`, results in `other`, with largest unit according to `options`.
    pub(crate) fn until(
        &self,
        other: &Self,
        cal: &C,
        options: DateDifferenceOptions,
    ) -> DateDuration {
        let sign = match other.cmp(self) {
            Ordering::Greater => 1i64,
            Ordering::Equal => return DateDuration::default(),
            Ordering::Less => -1i64,
        };
        let mut years = 0;
        if matches!(options.largest_unit, Some(DateDurationUnit::Years)) {
            let mut candidate_years = sign;
            while !self.surpasses(
                other,
                DateDuration::from_signed_ymwd(candidate_years, 0, 0, 0),
                sign,
                cal,
            ) {
                years = candidate_years;
                candidate_years += sign;
            }
        }
        let mut months = 0;
        if matches!(
            options.largest_unit,
            Some(DateDurationUnit::Years) | Some(DateDurationUnit::Months)
        ) {
            let mut candidate_months = sign;
            while !self.surpasses(
                other,
                DateDuration::from_signed_ymwd(years, candidate_months, 0, 0),
                sign,
                cal,
            ) {
                months = candidate_months;
                candidate_months += sign;
            }
        }
        let mut weeks = 0;
        if matches!(options.largest_unit, Some(DateDurationUnit::Weeks)) {
            let mut candidate_weeks = sign;
            while !self.surpasses(
                other,
                DateDuration::from_signed_ymwd(years, months, candidate_weeks, 0),
                sign,
                cal,
            ) {
                weeks = candidate_weeks;
                candidate_weeks += sign;
            }
        }
        let mut days = 0;
        let mut candidate_days = sign;
        while !self.surpasses(
            other,
            DateDuration::from_signed_ymwd(years, months, weeks, candidate_days),
            sign,
            cal,
        ) {
            days = candidate_days;
            candidate_days += sign;
        }
        DateDuration::from_signed_ymwd(years, months, weeks, days)
    }
}
