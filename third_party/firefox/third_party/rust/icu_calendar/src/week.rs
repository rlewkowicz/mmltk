// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

//! Functions for region-specific weekday information.

use crate::{error::RangeError, provider::*, types::Weekday};
use icu_locale_core::preferences::{define_preferences, extensions::unicode::keywords::FirstDay};
use icu_provider::prelude::*;

/// Minimum number of days in a month unit required for using this module
const MIN_UNIT_DAYS: u16 = 14;

define_preferences!(
    /// The preferences for the week information.
    [Copy]
    WeekPreferences,
    {
        /// The first day of the week
        first_weekday: FirstDay
    }
);

/// Information about the first day of the week and the weekend.
#[derive(Clone, Copy, Debug)]
#[non_exhaustive]
pub struct WeekInformation {
    /// The first day of a week.
    pub first_weekday: Weekday,
    /// The set of weekend days
    pub weekend: WeekdaySet,
}

impl WeekInformation {
    icu_provider::gen_buffer_data_constructors!(
        (prefs: WeekPreferences) -> error: DataError,
        /// Creates a new [`WeekCalculator`] from compiled data.
    );

    #[doc = icu_provider::gen_buffer_unstable_docs!(UNSTABLE, Self::try_new)]
    pub fn try_new_unstable<P>(provider: &P, prefs: WeekPreferences) -> Result<Self, DataError>
    where
        P: DataProvider<crate::provider::CalendarWeekV1> + ?Sized,
    {
        let locale = CalendarWeekV1::make_locale(prefs.locale_preferences);
        provider
            .load(DataRequest {
                id: DataIdentifierBorrowed::for_locale(&locale),
                ..Default::default()
            })
            .map(|response| WeekInformation {
                first_weekday: match prefs.first_weekday {
                    Some(FirstDay::Mon) => Weekday::Monday,
                    Some(FirstDay::Tue) => Weekday::Tuesday,
                    Some(FirstDay::Wed) => Weekday::Wednesday,
                    Some(FirstDay::Thu) => Weekday::Thursday,
                    Some(FirstDay::Fri) => Weekday::Friday,
                    Some(FirstDay::Sat) => Weekday::Saturday,
                    Some(FirstDay::Sun) => Weekday::Sunday,
                    _ => response.payload.get().first_weekday,
                },
                weekend: response.payload.get().weekend,
            })
    }

    /// Weekdays that are part of the 'weekend', for calendar purposes.
    /// Days may not be contiguous, and order is based off the first weekday.
    pub fn weekend(self) -> WeekdaySetIterator {
        WeekdaySetIterator::new(self.first_weekday, self.weekend)
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct WeekCalculator {
    first_weekday: Weekday,
    min_week_days: u8,
}

impl WeekCalculator {
    pub(crate) const ISO: Self = Self {
        first_weekday: Weekday::Monday,
        min_week_days: 4,
    };

    /// Returns the zero based index of `weekday` vs this calendar's start of week.
    fn weekday_index(self, weekday: Weekday) -> i8 {
        (7 + (weekday as i8) - (self.first_weekday as i8)) % 7
    }

    /// Computes & returns the week of given month/year according to `calendar`.
    ///
    /// # Arguments
    ///  - calendar: Calendar information used to compute the week number.
    ///  - num_days_in_previous_unit: The number of days in the preceding month/year.
    ///  - num_days_in_unit: The number of days in the month/year.
    ///  - day: 1-based day of month/year.
    ///  - week_day: The weekday of `day`..
    ///
    /// # Error
    /// If num_days_in_unit/num_days_in_previous_unit < MIN_UNIT_DAYS
    pub(crate) fn week_of(
        self,
        num_days_in_previous_unit: u16,
        num_days_in_unit: u16,
        day: u16,
        week_day: Weekday,
    ) -> Result<WeekOf, RangeError> {
        let current = UnitInfo::new(
            add_to_weekday(week_day, 1 - i32::from(day)),
            num_days_in_unit,
        )?;

        match current.relative_week(self, day) {
            RelativeWeek::LastWeekOfPreviousUnit => {
                let previous = UnitInfo::new(
                    add_to_weekday(current.first_day, -i32::from(num_days_in_previous_unit)),
                    num_days_in_previous_unit,
                )?;

                Ok(WeekOf {
                    week: previous.num_weeks(self),
                    unit: RelativeUnit::Previous,
                })
            }
            RelativeWeek::WeekOfCurrentUnit(w) => Ok(WeekOf {
                week: w,
                unit: RelativeUnit::Current,
            }),
            RelativeWeek::FirstWeekOfNextUnit => Ok(WeekOf {
                week: 1,
                unit: RelativeUnit::Next,
            }),
        }
    }
}

/// Returns the weekday that's `num_days` after `weekday`.
fn add_to_weekday(weekday: Weekday, num_days: i32) -> Weekday {
    let new_weekday = (7 + (weekday as i32) + (num_days % 7)) % 7;
    Weekday::from_days_since_sunday(new_weekday as isize)
}

/// Which year or month that a calendar assigns a week to relative to the year/month
/// the week is in.
#[derive(Clone, Copy, Debug, PartialEq)]
#[expect(clippy::enum_variant_names)]
enum RelativeWeek {
    /// A week that is assigned to the last week of the previous year/month. e.g. 2021-01-01 is week 54 of 2020 per the ISO calendar.
    LastWeekOfPreviousUnit,
    /// A week that's assigned to the current year/month. The offset is 1-based. e.g. 2021-01-11 is week 2 of 2021 per the ISO calendar so would be WeekOfCurrentUnit(2).
    WeekOfCurrentUnit(u8),
    /// A week that is assigned to the first week of the next year/month. e.g. 2019-12-31 is week 1 of 2020 per the ISO calendar.
    FirstWeekOfNextUnit,
}

/// Information about a year or month.
#[derive(Clone, Copy)]
struct UnitInfo {
    /// The weekday of this year/month's first day.
    first_day: Weekday,
    /// The number of days in this year/month.
    duration_days: u16,
}

impl UnitInfo {
    /// Creates a UnitInfo for a given year or month.
    fn new(first_day: Weekday, duration_days: u16) -> Result<UnitInfo, RangeError> {
        if duration_days < MIN_UNIT_DAYS {
            return Err(RangeError {
                field: "num_days_in_unit",
                value: duration_days as i32,
                min: MIN_UNIT_DAYS as i32,
                max: i32::MAX,
            });
        }
        Ok(UnitInfo {
            first_day,
            duration_days,
        })
    }

    /// Returns the start of this unit's first week.
    ///
    /// The returned value can be negative if this unit's first week started during the previous
    /// unit.
    fn first_week_offset(self, calendar: WeekCalculator) -> i8 {
        let first_day_index = calendar.weekday_index(self.first_day);
        if 7 - first_day_index >= calendar.min_week_days as i8 {
            -first_day_index
        } else {
            7 - first_day_index
        }
    }

    /// Returns the number of weeks in this unit according to `calendar`.
    fn num_weeks(self, calendar: WeekCalculator) -> u8 {
        let first_week_offset = self.first_week_offset(calendar);
        let num_days_including_first_week =
            (self.duration_days as i32) - (first_week_offset as i32);
        debug_assert!(
            num_days_including_first_week >= 0,
            "Unit is shorter than a week."
        );
        ((num_days_including_first_week + 7 - (calendar.min_week_days as i32)) / 7) as u8
    }

    /// Returns the week number for the given day in this unit.
    fn relative_week(self, calendar: WeekCalculator, day: u16) -> RelativeWeek {
        let days_since_first_week =
            i32::from(day) - i32::from(self.first_week_offset(calendar)) - 1;
        if days_since_first_week < 0 {
            return RelativeWeek::LastWeekOfPreviousUnit;
        }

        let week_number = (1 + days_since_first_week / 7) as u8;
        if week_number > self.num_weeks(calendar) {
            return RelativeWeek::FirstWeekOfNextUnit;
        }
        RelativeWeek::WeekOfCurrentUnit(week_number)
    }
}

/// The year or month that a calendar assigns a week to relative to the year/month that it is in.
#[derive(Debug, PartialEq)]
#[allow(clippy::exhaustive_enums)] 
pub(crate) enum RelativeUnit {
    /// A week that is assigned to previous year/month. e.g. 2021-01-01 is week 54 of 2020 per the ISO calendar.
    Previous,
    /// A week that's assigned to the current year/month. e.g. 2021-01-11 is week 2 of 2021 per the ISO calendar.
    Current,
    /// A week that is assigned to the next year/month. e.g. 2019-12-31 is week 1 of 2020 per the ISO calendar.
    Next,
}

/// The week number assigned to a given week according to a calendar.
#[derive(Debug, PartialEq)]
#[allow(clippy::exhaustive_structs)] 
pub(crate) struct WeekOf {
    /// Week of month/year. 1 based.
    pub week: u8,
    /// The month/year that this week is in, relative to the month/year of the input date.
    pub unit: RelativeUnit,
}

/// [Iterator] that yields weekdays that are part of the weekend.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WeekdaySetIterator {
    /// Determines the order in which we should start reading values from `weekend`.
    first_weekday: Weekday,
    /// Day being evaluated.
    current_day: Weekday,
    /// Bitset to read weekdays from.
    weekend: WeekdaySet,
}

impl WeekdaySetIterator {
    /// Creates the Iterator. Sets `current_day` to the day after `first_weekday`.
    pub(crate) fn new(first_weekday: Weekday, weekend: WeekdaySet) -> Self {
        WeekdaySetIterator {
            first_weekday,
            current_day: first_weekday,
            weekend,
        }
    }
}

impl Iterator for WeekdaySetIterator {
    type Item = Weekday;

    fn next(&mut self) -> Option<Self::Item> {
        while self.current_day.next_day() != self.first_weekday {
            if self.weekend.contains(self.current_day) {
                let result = self.current_day;
                self.current_day = self.current_day.next_day();
                return Some(result);
            } else {
                self.current_day = self.current_day.next_day();
            }
        }

        if self.weekend.contains(self.current_day) {
            self.weekend = WeekdaySet::new(&[]);
            return Some(self.current_day);
        }

        Option::None
    }
}
