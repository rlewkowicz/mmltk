// under the Apache-2.0 license. Accordingly, this file is released under
// the Apache License, Version 2.0 which can be found at the calendrical_calculations
// package root or at http://www.apache.org/licenses/LICENSE-2.0.

use crate::helpers::{final_func, i64_to_i32, next_u8};
use crate::rata_die::{Moment, RataDie};
#[allow(unused_imports)]
use core_maths::*;

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2206>
pub(crate) const FIXED_HEBREW_EPOCH: RataDie =
    crate::julian::fixed_from_julian_book_version(-3761, 10, 7);

/// Biblical Hebrew dates. The months are reckoned a bit strangely, with the new year occurring on
/// Tishri (as in the civil calendar) but the months being numbered in a different order
#[derive(Copy, Clone, Debug, Default, Hash, Eq, PartialEq, PartialOrd, Ord)]
#[allow(clippy::exhaustive_structs)]
pub struct BookHebrew {
    /// The year
    pub year: i32,
    /// The month
    pub month: u8,
    /// The day
    pub day: u8,
}

/// The biblical month number used for the month of Nisan
pub const NISAN: u8 = 1;
/// The biblical month number used for the month of Iyyar
pub const IYYAR: u8 = 2;
/// The biblical month number used for the month of Sivan
pub const SIVAN: u8 = 3;
/// The biblical month number used for the month of Tammuz
pub const TAMMUZ: u8 = 4;
/// The biblical month number used for the month of Av
pub const AV: u8 = 5;
/// The biblical month number used for the month of Elul
pub const ELUL: u8 = 6;
/// The biblical month number used for the month of Tishri
pub const TISHRI: u8 = 7;
/// The biblical month number used for the month of Marheshvan
pub const MARHESHVAN: u8 = 8;
/// The biblical month number used for the month of Kislev
pub const KISLEV: u8 = 9;
/// The biblical month number used for the month of Tevet
pub const TEVET: u8 = 10;
/// The biblical month number used for the month of Shevat
pub const SHEVAT: u8 = 11;
/// The biblical month number used for the month of Adar (and Adar I)
pub const ADAR: u8 = 12;
/// The biblical month number used for the month of Adar II
pub const ADARII: u8 = 13;


impl BookHebrew {
    /// The civil calendar has the same year and day numbering as the book one, but the months are numbered
    /// differently
    pub fn to_civil_date(self) -> (i32, u8, u8) {
        let biblical_month = self.month;
        let biblical_year = self.year;
        let mut civil_month;
        civil_month = (biblical_month + 6) % 12;

        if civil_month == 0 {
            civil_month = 12;
        }

        if Self::is_hebrew_leap_year(biblical_year) && biblical_month < TISHRI {
            civil_month += 1;
        }
        (biblical_year, civil_month, self.day)
    }

    /// The civil calendar has the same year and day numbering as the book one, but the months are numbered
    /// differently
    pub fn from_civil_date(civil_year: i32, civil_month: u8, civil_day: u8) -> Self {
        let mut biblical_month;

        if civil_month <= 6 {
            biblical_month = civil_month + 6; 
        } else {
            biblical_month = civil_month - 6; 
            if Self::is_hebrew_leap_year(civil_year) {
                biblical_month -= 1
            }
            if biblical_month == 0 {
                biblical_month = 13;
            }
        }

        BookHebrew {
            year: civil_year,
            month: biblical_month,
            day: civil_day,
        }
    }
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2244>
    #[allow(dead_code)]
    pub(crate) fn molad(book_year: i32, book_month: u8) -> Moment {
        let y = if book_month < TISHRI {
            book_year + 1
        } else {
            book_year
        }; 

        let months_elapsed = (book_month as f64 - TISHRI as f64) 
            + ((235.0 * y as f64 - 234.0) / 19.0).floor(); 

        Moment::new(
            FIXED_HEBREW_EPOCH.to_f64_date() - (876.0 / 25920.0)
                + months_elapsed * (29.0 + (1.0 / 2.0) + (793.0 / 25920.0)),
        )
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2217>
    #[allow(dead_code)]
    fn last_month_of_book_hebrew_year(book_year: i32) -> u8 {
        if Self::is_hebrew_leap_year(book_year) {
            ADARII
        } else {
            ADAR
        }
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2261>
    fn book_hebrew_calendar_elapsed_days(book_year: i32) -> i32 {
        let months_elapsed = ((235.0 * book_year as f64 - 234.0) / 19.0).floor() as i64;
        let parts_elapsed = 12084 + 13753 * months_elapsed;
        let days = 29 * months_elapsed + (parts_elapsed as f64 / 25920.0).floor() as i64;

        if (3 * (days + 1)).rem_euclid(7) < 3 {
            days as i32 + 1
        } else {
            days as i32
        }
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2301>
    fn book_hebrew_year_length_correction(book_year: i32) -> u8 {
        let ny0 = Self::book_hebrew_calendar_elapsed_days(book_year - 1);
        let ny1 = Self::book_hebrew_calendar_elapsed_days(book_year);
        let ny2 = Self::book_hebrew_calendar_elapsed_days(book_year + 1);

        if (ny2 - ny1) == 356 {
            2
        } else if (ny1 - ny0) == 382 {
            1
        } else {
            0
        }
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2294>
    pub fn book_hebrew_new_year(book_year: i32) -> RataDie {
        RataDie::new(
            FIXED_HEBREW_EPOCH.to_i64_date()
                + Self::book_hebrew_calendar_elapsed_days(book_year) as i64
                + Self::book_hebrew_year_length_correction(book_year) as i64,
        )
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2315>
    pub fn days_in_book_hebrew_year(book_year: i32) -> u16 {
        (Self::book_hebrew_new_year(1 + book_year) - Self::book_hebrew_new_year(book_year)) as u16
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/1ee51ecfaae6f856b0d7de3e36e9042100b4f424/calendar.l#L2275-L2278>
    pub fn is_hebrew_leap_year(book_year: i32) -> bool {
        (7 * book_year + 1).rem_euclid(19) < 7
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2321>
    #[allow(dead_code)]
    fn is_long_marheshvan(book_year: i32) -> bool {
        let long_marheshavan_year_lengths = [355, 385];
        long_marheshavan_year_lengths.contains(&Self::days_in_book_hebrew_year(book_year))
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2326>
    #[allow(dead_code)]
    fn is_short_kislev(book_year: i32) -> bool {
        let short_kislev_year_lengths = [353, 383];
        short_kislev_year_lengths.contains(&Self::days_in_book_hebrew_year(book_year))
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2230>
    pub fn last_day_of_book_hebrew_month(book_year: i32, book_month: u8) -> u8 {
        match book_month {
            IYYAR | TAMMUZ | ELUL | TEVET | ADARII => 29,
            ADAR => {
                if !Self::is_hebrew_leap_year(book_year) {
                    29
                } else {
                    30
                }
            }
            MARHESHVAN => {
                if !Self::is_long_marheshvan(book_year) {
                    29
                } else {
                    30
                }
            }
            KISLEV => {
                if Self::is_short_kislev(book_year) {
                    29
                } else {
                    30
                }
            }
            _ => 30,
        }
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2331>
    pub fn fixed_from_book_hebrew(date: BookHebrew) -> RataDie {
        let book_year = date.year;
        let book_month = date.month;
        let book_day = date.day;

        let mut total_days = Self::book_hebrew_new_year(book_year) + book_day.into() - 1; 

        if book_month < TISHRI {
            for m in
                (TISHRI..=Self::last_month_of_book_hebrew_year(book_year)).chain(NISAN..book_month)
            {
                total_days += Self::last_day_of_book_hebrew_month(book_year, m).into();
            }
        } else {
            for m in TISHRI..book_month {
                total_days += Self::last_day_of_book_hebrew_month(book_year, m).into();
            }
        }

        total_days
    }

    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2352>
    pub fn book_hebrew_from_fixed(date: RataDie) -> BookHebrew {
        let approx = i64_to_i32(
            1 + ((date - FIXED_HEBREW_EPOCH) as f64).div_euclid(35975351.0 / 98496.0) as i64, 
        )
        .unwrap_or_else(|e| e.saturate());

        let year_condition = |year: i32| Self::book_hebrew_new_year(year) <= date;
        let year = final_func(approx - 1, year_condition);

        let start = if date
            < Self::fixed_from_book_hebrew(BookHebrew {
                year,
                month: NISAN,
                day: 1,
            }) {
            TISHRI
        } else {
            NISAN
        };

        let month_condition = |m: u8| {
            date <= Self::fixed_from_book_hebrew(BookHebrew {
                year,
                month: m,
                day: Self::last_day_of_book_hebrew_month(year, m),
            })
        };
        let month = next_u8(start, month_condition);

        let day = (date
            - Self::fixed_from_book_hebrew(BookHebrew {
                year,
                month,
                day: 1,
            }))
            + 1;

        BookHebrew {
            year,
            month,
            day: day as u8,
        }
    }
}
