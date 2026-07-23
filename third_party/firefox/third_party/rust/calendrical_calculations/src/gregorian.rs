// under the Apache-2.0 license. Accordingly, this file is released under
// the Apache License, Version 2.0 which can be found at the calendrical_calculations
// package root or at http://www.apache.org/licenses/LICENSE-2.0.

use crate::helpers::{i64_to_i32, k_day_after, I32CastError};
use crate::rata_die::RataDie;

const EPOCH: RataDie = RataDie::new(1);

const DAYS_IN_YEAR: i64 = 365;

const DAYS_IN_4_YEAR_CYCLE: i64 = DAYS_IN_YEAR * 4 + 1;

const DAYS_IN_100_YEAR_CYCLE: i64 = 25 * DAYS_IN_4_YEAR_CYCLE - 1;

/// The number of days in the 400 year cycle.
pub const DAYS_IN_400_YEAR_CYCLE: i64 = 4 * DAYS_IN_100_YEAR_CYCLE + 1;

/// Whether or not `year` is a leap year
///
/// Inspired by Neri-Schneider <https://www.youtube.com/watch?v=J9KijLyP-yg&t=1239s>
pub const fn is_leap_year(year: i32) -> bool {
    if year % 25 != 0 {
        year % 4 == 0
    } else {
        year % 16 == 0
    }
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/1ee51ecfaae6f856b0d7de3e36e9042100b4f424/calendar.l#L1167-L1189>
pub const fn fixed_from_gregorian(year: i32, month: u8, day: u8) -> RataDie {
    day_before_year(year)
        .add(days_before_month(year, month) as i64)
        .add(day as i64)
}

/// The number of days in this year before this month starts
///
/// Inspired by Neri-Schneider <https://onlinelibrary.wiley.com/doi/10.1002/spe.3172>
pub const fn days_before_month(year: i32, month: u8) -> u16 {
    if month < 3 {
        if month == 1 {
            0
        } else {
            31
        }
    } else {
        31 + 28 + is_leap_year(year) as u16 + ((979 * (month as u32) - 2919) >> 5) as u16
    }
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/1ee51ecfaae6f856b0d7de3e36e9042100b4f424/calendar.l#L1191-L1217>
pub const fn year_from_fixed(date: RataDie) -> Result<i32, I32CastError> {
    let date = date.since(EPOCH);

    let (n_400, date) = (
        date.div_euclid(DAYS_IN_400_YEAR_CYCLE),
        date.rem_euclid(DAYS_IN_400_YEAR_CYCLE),
    );

    let (n_100, date) = (date / DAYS_IN_100_YEAR_CYCLE, date % DAYS_IN_100_YEAR_CYCLE);

    let (n_4, date) = (date / DAYS_IN_4_YEAR_CYCLE, date % DAYS_IN_4_YEAR_CYCLE);

    let n_1 = date / DAYS_IN_YEAR;

    let year = 400 * n_400 + 100 * n_100 + 4 * n_4 + n_1 + (n_100 != 4 && n_1 != 4) as i64;

    i64_to_i32(year)
}

/// Calculates the day before Jan 1 of `year`.
pub const fn day_before_year(year: i32) -> RataDie {
    let prev_year = (year as i64) - 1;
    let mut fixed: i64 = DAYS_IN_YEAR * prev_year;
    const YEAR_SHIFT: i64 = (-(i32::MIN as i64 - 1) / 400 + 1) * 400;
    fixed += (prev_year + YEAR_SHIFT) / 4 - (prev_year + YEAR_SHIFT) / 100
        + (prev_year + YEAR_SHIFT) / 400
        - const { YEAR_SHIFT / 4 - YEAR_SHIFT / 100 + YEAR_SHIFT / 400 };
    RataDie::new(fixed)
}

/// Calculates the month/day from the 1-based day of the year
pub fn year_day(year: i32, day_of_year: u16) -> (u8, u8) {
    let correction = if day_of_year < 31 + 28 + is_leap_year(year) as u16 {
        -1
    } else {
        (!is_leap_year(year)) as i32
    };
    let month = ((12 * (day_of_year as i32 + correction) + 373) / 367) as u8; 
    let day = (day_of_year - days_before_month(year, month)) as u8; 
    (month, day)
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/1ee51ecfaae6f856b0d7de3e36e9042100b4f424/calendar.l#L1525-L1540>
pub fn gregorian_from_fixed(date: RataDie) -> Result<(i32, u8, u8), I32CastError> {
    let year = year_from_fixed(date)?;
    let day_of_year = date - day_before_year(year);
    let (month, day) = year_day(year, day_of_year as u16);
    Ok((year, month, day))
}

/// Calculates the date of Easter in the given year
pub fn easter(year: i32) -> RataDie {
    let century = (year / 100) + 1;
    let shifted_epact =
        (14 + 11 * year.rem_euclid(19) - century * 3 / 4 + (5 + 8 * century) / 25).rem_euclid(30);
    let adjusted_epact = shifted_epact
        + (shifted_epact == 0 || (shifted_epact == 1 && 10 < year.rem_euclid(19))) as i32;
    let paschal_moon = fixed_from_gregorian(year, 4, 19) - adjusted_epact as i64;

    k_day_after(0, paschal_moon)
}
