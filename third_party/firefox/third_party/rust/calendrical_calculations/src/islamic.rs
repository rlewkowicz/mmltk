use crate::astronomy::*;
use crate::helpers::{i64_to_saturated_i32, next};
use crate::rata_die::{Moment, RataDie};
#[allow(unused_imports)]
use core_maths::*;

pub use crate::astronomy::Location;

/// The average length of an Islamic year, equal to 12 moon cycles
pub const MEAN_YEAR_LENGTH: f64 = MEAN_SYNODIC_MONTH * 12.;

/// Different islamic calendars use different epochs (Thursday vs Friday) due to disagreement on the exact date of Mohammed's migration to Mecca.
/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2066>
pub const ISLAMIC_EPOCH_FRIDAY: RataDie = crate::julian::fixed_from_julian(622, 7, 16);

/// Different islamic calendars use different epochs (Thursday vs Friday) due to disagreement on the exact date of Mohammed's migration to Mecca.
/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2066>
pub const ISLAMIC_EPOCH_THURSDAY: RataDie = crate::julian::fixed_from_julian(622, 7, 15);

#[doc(inline)]
pub use crate::chinese_based::WELL_BEHAVED_ASTRONOMICAL_RANGE;

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L6898>
pub const CAIRO: Location = Location {
    latitude: 30.1,
    longitude: 31.3,
    elevation: 200.0,
    utc_offset: (1_f64 / 12_f64),
};

/// The location of Mecca; used for Islamic calendar calculations.
pub const MECCA: Location = Location {
    latitude: 6427.0 / 300.0,
    longitude: 11947.0 / 300.0,
    elevation: 298.0,
    utc_offset: (1_f64 / 8_f64),
};

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L6904>
pub fn fixed_from_observational_islamic(
    year: i32,
    month: u8,
    day: u8,
    location: Location,
) -> RataDie {
    let year = i64::from(year);
    let month = i64::from(month);
    let day = i64::from(day);
    let midmonth = ISLAMIC_EPOCH_FRIDAY.to_f64_date()
        + (((year - 1) as f64) * 12.0 + month as f64 - 0.5) * MEAN_SYNODIC_MONTH;
    let lunar_phase = Astronomical::calculate_new_moon_at_or_before(RataDie::new(midmonth as i64));
    Astronomical::phasis_on_or_before(RataDie::new(midmonth as i64), location, Some(lunar_phase))
        + day
        - 1
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/1ee51ecfaae6f856b0d7de3e36e9042100b4f424/calendar.l#L6983-L6995>
pub fn observational_islamic_from_fixed(date: RataDie, location: Location) -> (i32, u8, u8) {
    let lunar_phase = Astronomical::calculate_new_moon_at_or_before(date);
    let crescent = Astronomical::phasis_on_or_before(date, location, Some(lunar_phase));
    let elapsed_months =
        ((crescent - ISLAMIC_EPOCH_FRIDAY) as f64 / MEAN_SYNODIC_MONTH).round() as i32;
    let year = elapsed_months.div_euclid(12) + 1;
    let month = elapsed_months.rem_euclid(12) + 1;
    let day = (date - crescent + 1) as u8;

    (year, month as u8, day)
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L6957>
fn saudi_criterion(date: RataDie) -> Option<bool> {
    let sunset = Astronomical::sunset((date - 1).as_moment(), MECCA)?;
    let tee = Location::universal_from_standard(sunset, MECCA);
    let phase = Astronomical::lunar_phase(tee, Astronomical::julian_centuries(tee));
    let moonlag = Astronomical::moonlag((date - 1).as_moment(), MECCA)?;

    Some(phase > 0.0 && phase < 90.0 && moonlag > 0.0)
}

fn adjusted_saudi_criterion(date: RataDie) -> bool {
    saudi_criterion(date).unwrap_or_default()
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L6966>
pub fn saudi_new_month_on_or_before(date: RataDie) -> RataDie {
    let last_new_moon = (Astronomical::lunar_phase_at_or_before(0.0, date.as_moment()))
        .inner()
        .floor(); 
    let age = date.to_f64_date() - last_new_moon;
    let tau = if age <= 3.0 && !adjusted_saudi_criterion(date) {
        last_new_moon - 30.0 
    } else {
        last_new_moon
    };

    next(RataDie::new(tau as i64), adjusted_saudi_criterion) 
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L6996>
pub fn saudi_islamic_from_fixed(date: RataDie) -> (i32, u8, u8) {
    let crescent = saudi_new_month_on_or_before(date);
    let elapsed_months =
        ((crescent - ISLAMIC_EPOCH_FRIDAY) as f64 / MEAN_SYNODIC_MONTH).round() as i64;
    let year = i64_to_saturated_i32(elapsed_months.div_euclid(12) + 1);
    let month = (elapsed_months.rem_euclid(12) + 1) as u8;
    let day = ((date - crescent) + 1) as u8;

    (year, month, day)
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L6981>
pub fn fixed_from_saudi_islamic(year: i32, month: u8, day: u8) -> RataDie {
    let midmonth = RataDie::new(
        ISLAMIC_EPOCH_FRIDAY.to_i64_date()
            + (((year as f64 - 1.0) * 12.0 + month as f64 - 0.5) * MEAN_SYNODIC_MONTH).floor()
                as i64,
    );
    let first_day_of_month = saudi_new_month_on_or_before(midmonth).to_i64_date();

    RataDie::new(first_day_of_month + day as i64 - 1)
}

/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2076>
pub fn fixed_from_tabular_islamic(year: i32, month: u8, day: u8, epoch: RataDie) -> RataDie {
    let year = i64::from(year);
    let month = i64::from(month);
    let day = i64::from(day);

    RataDie::new(
        (epoch.to_i64_date() - 1)
            + (year - 1) * 354
            + (3 + year * 11).div_euclid(30)
            + 29 * (month - 1)
            + month.div_euclid(2)
            + day,
    )
}
/// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/main/calendar.l#L2090>
pub fn tabular_islamic_from_fixed(date: RataDie, epoch: RataDie) -> (i32, u8, u8) {
    let year = i64_to_saturated_i32(((date - epoch) * 30 + 10646).div_euclid(10631));
    let prior_days =
        date.to_f64_date() - fixed_from_tabular_islamic(year, 1, 1, epoch).to_f64_date();
    debug_assert!(prior_days >= 0.0);
    debug_assert!(prior_days <= 354.);
    let month = (((prior_days * 11.0) + 330.0) / 325.0) as u8; 
    debug_assert!(month <= 12);
    let day = (date.to_f64_date() - fixed_from_tabular_islamic(year, month, 1, epoch).to_f64_date()
        + 1.0) as u8; 

    (year, month, day)
}

/// The number of days in a month for the observational islamic calendar
pub fn observational_islamic_month_days(year: i32, month: u8, location: Location) -> u8 {
    let midmonth = ISLAMIC_EPOCH_FRIDAY.to_f64_date()
        + (((year - 1) as f64) * 12.0 + month as f64 - 0.5) * MEAN_SYNODIC_MONTH;

    let lunar_phase: f64 =
        Astronomical::calculate_new_moon_at_or_before(RataDie::new(midmonth as i64));
    let f_date = Astronomical::phasis_on_or_before(
        RataDie::new(midmonth as i64),
        location,
        Some(lunar_phase),
    );

    Astronomical::month_length(f_date, location)
}

/// The number of days in a month for the Saudi (Umm Al-Qura) calendar
pub fn saudi_islamic_month_days(year: i32, month: u8) -> u8 {
    let midmonth = Moment::new(
        ISLAMIC_EPOCH_FRIDAY.to_f64_date()
            + (((year - 1) as f64) * 12.0 + month as f64 - 0.5) * MEAN_SYNODIC_MONTH,
    );
    let midmonth_next = midmonth + MEAN_SYNODIC_MONTH;

    let month_start = saudi_new_month_on_or_before(midmonth.as_rata_die());
    let next_month_start = saudi_new_month_on_or_before(midmonth_next.as_rata_die());

    let diff = next_month_start - month_start;
    debug_assert!(
        diff <= 30 || !WELL_BEHAVED_ASTRONOMICAL_RANGE.contains(&month_start),
        "umm-al-qura months must not be more than 30 days"
    );
    u8::try_from(diff).unwrap_or(30)
}
