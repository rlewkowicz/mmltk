// under the Apache-2.0 license. Accordingly, this file is released under
// the Apache License, Version 2.0 which can be found at the calendrical_calculations
// package root or at http://www.apache.org/licenses/LICENSE-2.0.

//! This file contains important structs and functions relating to location,
//! time, and astronomy; these are intended for calender calculations and based off
//! _Calendrical Calculations_ by Reingold & Dershowitz.


use crate::error::LocationOutOfBoundsError;
use crate::helpers::{binary_search, i64_to_i32, invert_angular, next_moment, poly};
use crate::rata_die::{Moment, RataDie};
use core::f64::consts::PI;
#[allow(unused_imports)]
use core_maths::*;

fn div_euclid_f64(n: f64, d: f64) -> f64 {
    debug_assert!(d > 0.0);
    let (a, b) = (n / d, n % d);
    if n >= 0.0 || b == 0.0 {
        a
    } else {
        a - 1.0
    }
}

#[derive(Debug, Copy, Clone, PartialEq)]
/// A Location on the Earth given as a latitude, longitude, elevation, and standard time zone.
/// Latitude is given in degrees from -90 to 90, longitude in degrees from -180 to 180,
/// elevation in meters, and zone as a UTC offset in fractional days (ex. UTC+1 would have zone = 1.0 / 24.0)
#[allow(clippy::exhaustive_structs)] 
pub struct Location {
    /// latitude from -90 to 90
    pub(crate) latitude: f64,
    /// longitude from -180 to 180
    pub(crate) longitude: f64,
    /// elevation in meters
    pub(crate) elevation: f64,
    /// UTC timezone offset in fractional days (1 hr = 1.0 / 24.0 day)
    pub(crate) utc_offset: f64,
}

/// The mean synodic month in days of 86400 atomic seconds
/// (86400 seconds = 24 hours * 60 minutes/hour * 60 seconds/minute)
///
/// This is defined in _Calendrical Calculations_ by Reingold & Dershowitz.
/// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3880-L3882>
pub const MEAN_SYNODIC_MONTH: f64 = 29.530588861;

/// The Moment of noon on January 1, 2000
pub const J2000: Moment = Moment::new(730120.5);

/// The mean tropical year in days
///
/// This is defined in _Calendrical Calculations_ by Reingold & Dershowitz.
/// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3872-L3874>
pub const MEAN_TROPICAL_YEAR: f64 = 365.242189;

/// The minimum allowable UTC offset (-12 hours) in fractional days (-0.5 days)
pub const MIN_UTC_OFFSET: f64 = -0.5;

/// The maximum allowable UTC offset (+14 hours) in fractional days (14.0 / 24.0 days)
pub const MAX_UTC_OFFSET: f64 = 14.0 / 24.0;

/// The angle of winter for the purposes of solar calculations
pub const WINTER: f64 = 270.0;

/// The moment of the first new moon of the CE, which occurred on January 11, 1 CE.
pub const NEW_MOON_ZERO: Moment = Moment::new(11.458922815770109);

impl Location {
    /// Create a location; latitude is from -90 to 90, and longitude is from -180 to 180;
    /// attempting to create a location outside of these bounds will result in a LocationOutOfBoundsError.
    #[allow(dead_code)] 
    pub(crate) fn try_new(
        latitude: f64,
        longitude: f64,
        elevation: f64,
        utc_offset: f64,
    ) -> Result<Location, LocationOutOfBoundsError> {
        if !(-90.0..=90.0).contains(&latitude) {
            return Err(LocationOutOfBoundsError::Latitude(latitude));
        }
        if !(-180.0..=180.0).contains(&longitude) {
            return Err(LocationOutOfBoundsError::Longitude(longitude));
        }
        if !(MIN_UTC_OFFSET..=MAX_UTC_OFFSET).contains(&utc_offset) {
            return Err(LocationOutOfBoundsError::Offset(
                utc_offset,
                MIN_UTC_OFFSET,
                MAX_UTC_OFFSET,
            ));
        }
        Ok(Location {
            latitude,
            longitude,
            elevation,
            utc_offset,
        })
    }

    /// Get the longitude of a Location
    #[allow(dead_code)]
    pub(crate) fn longitude(&self) -> f64 {
        self.longitude
    }

    /// Get the latitude of a Location
    #[allow(dead_code)]
    pub(crate) fn latitude(&self) -> f64 {
        self.latitude
    }

    /// Get the elevation of a Location
    #[allow(dead_code)]
    pub(crate) fn elevation(&self) -> f64 {
        self.elevation
    }

    /// Get the utc-offset of a Location
    #[allow(dead_code)]
    pub(crate) fn zone(&self) -> f64 {
        self.utc_offset
    }

    /// Convert a longitude into a mean time zone;
    /// this yields the difference in Moment given a longitude
    /// e.g. a longitude of 90 degrees is 0.25 (90 / 360) days ahead
    /// of a location with a longitude of 0 degrees.
    pub(crate) fn zone_from_longitude(longitude: f64) -> f64 {
        longitude / (360.0)
    }

    /// Convert standard time to local mean time given a location and a time zone with given offset
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3501-L3506>
    #[allow(dead_code)]
    pub(crate) fn standard_from_local(standard_time: Moment, location: Location) -> Moment {
        Self::standard_from_universal(
            Self::universal_from_local(standard_time, location),
            location,
        )
    }

    /// Convert from local mean time to universal time given a location
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3496-L3499>
    pub(crate) fn universal_from_local(local_time: Moment, location: Location) -> Moment {
        local_time - Self::zone_from_longitude(location.longitude)
    }

    /// Convert from universal time to local time given a location
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3491-L3494>
    #[allow(dead_code)] 
    pub(crate) fn local_from_universal(universal_time: Moment, location: Location) -> Moment {
        universal_time + Self::zone_from_longitude(location.longitude)
    }

    /// Given a UTC-offset in hours and a Moment in standard time,
    /// return the Moment in universal time from the time zone with the given offset.
    /// The field utc_offset should be within the range of possible offsets given by
    /// the constand fields `MIN_UTC_OFFSET` and `MAX_UTC_OFFSET`.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3479-L3483>
    pub(crate) fn universal_from_standard(standard_moment: Moment, location: Location) -> Moment {
        debug_assert!(location.utc_offset > MIN_UTC_OFFSET && location.utc_offset < MAX_UTC_OFFSET, "UTC offset {0} was not within the possible range of offsets (see astronomy::MIN_UTC_OFFSET and astronomy::MAX_UTC_OFFSET)", location.utc_offset);
        standard_moment - location.utc_offset
    }
    /// Given a Moment in standard time and UTC-offset in hours,
    /// return the Moment in standard time from the time zone with the given offset.
    /// The field utc_offset should be within the range of possible offsets given by
    /// the constand fields `MIN_UTC_OFFSET` and `MAX_UTC_OFFSET`.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3473-L3477>
    #[allow(dead_code)]
    pub(crate) fn standard_from_universal(standard_time: Moment, location: Location) -> Moment {
        debug_assert!(location.utc_offset > MIN_UTC_OFFSET && location.utc_offset < MAX_UTC_OFFSET, "UTC offset {0} was not within the possible range of offsets (see astronomy::MIN_UTC_OFFSET and astronomy::MAX_UTC_OFFSET)", location.utc_offset);
        standard_time + location.utc_offset
    }
}

#[derive(Debug)]
/// The Astronomical struct provides functions which support astronomical
/// calculations used by many observational calendars.
#[allow(clippy::exhaustive_structs)] 
pub struct Astronomical;

impl Astronomical {
    /// Function for the ephemeris correction, which corrects the
    /// somewhat-unpredictable discrepancy between dynamical time
    /// and universal time
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus (1991) with data from NASA.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3884-L3952>
    pub fn ephemeris_correction(moment: Moment) -> f64 {
        let year = moment.inner() / 365.2425;
        let year_int = (if year > 0.0 { year + 1.0 } else { year }) as i32;
        let fixed_mid_year = crate::gregorian::fixed_from_gregorian(year_int, 7, 1);
        let c = ((fixed_mid_year.to_i64_date() as f64) - 693596.0) / 36525.0;
        let y2000 = (year_int - 2000) as f64;
        let y1700 = (year_int - 1700) as f64;
        let y1600 = (year_int - 1600) as f64;
        let y1000 = ((year_int - 1000) as f64) / 100.0;
        let y0 = year_int as f64 / 100.0;
        let y1820 = ((year_int - 1820) as f64) / 100.0;

        if (2051..=2150).contains(&year_int) {
            (-20.0
                + 32.0 * (((year_int - 1820) * (year_int - 1820)) as f64 / 10000.0)
                + 0.5628 * (2150 - year_int) as f64)
                / 86400.0
        } else if (2006..=2050).contains(&year_int) {
            (62.92 + 0.32217 * y2000 + 0.005589 * y2000 * y2000) / 86400.0
        } else if (1987..=2005).contains(&year_int) {
            (63.86 + 0.3345 * y2000 - 0.060374 * y2000 * y2000
                + 0.0017275 * y2000 * y2000 * y2000
                + 0.000651814 * y2000 * y2000 * y2000 * y2000
                + 0.00002373599 * y2000 * y2000 * y2000 * y2000 * y2000)
                / 86400.0
        } else if (1900..=1986).contains(&year_int) {
            -0.00002 + 0.000297 * c + 0.025184 * c * c - 0.181133 * c * c * c
                + 0.553040 * c * c * c * c
                - 0.861938 * c * c * c * c * c
                + 0.677066 * c * c * c * c * c * c
                - 0.212591 * c * c * c * c * c * c * c
        } else if (1800..=1899).contains(&year_int) {
            -0.000009
                + 0.003844 * c
                + 0.083563 * c * c
                + 0.865736 * c * c * c
                + 4.867575 * c * c * c * c
                + 15.845535 * c * c * c * c * c
                + 31.332267 * c * c * c * c * c * c
                + 38.291999 * c * c * c * c * c * c * c
                + 28.316289 * c * c * c * c * c * c * c * c
                + 11.636204 * c * c * c * c * c * c * c * c * c
                + 2.043794 * c * c * c * c * c * c * c * c * c * c
        } else if (1700..=1799).contains(&year_int) {
            (8.118780842 - 0.005092142 * y1700 + 0.003336121 * y1700 * y1700
                - 0.0000266484 * y1700 * y1700 * y1700)
                / 86400.0
        } else if (1600..=1699).contains(&year_int) {
            (120.0 - 0.9808 * y1600 - 0.01532 * y1600 * y1600
                + 0.000140272128 * y1600 * y1600 * y1600)
                / 86400.0
        } else if (500..=1599).contains(&year_int) {
            (1574.2 - 556.01 * y1000 + 71.23472 * y1000 * y1000 + 0.319781 * y1000 * y1000 * y1000
                - 0.8503463 * y1000 * y1000 * y1000 * y1000
                - 0.005050998 * y1000 * y1000 * y1000 * y1000 * y1000
                + 0.0083572073 * y1000 * y1000 * y1000 * y1000 * y1000 * y1000)
                / 86400.0
        } else if (-499..=499).contains(&year_int) {
            (10583.6 - 1014.41 * y0 + 33.78311 * y0 * y0
                - 5.952053 * y0 * y0 * y0
                - 0.1798452 * y0 * y0 * y0 * y0
                + 0.022174192 * y0 * y0 * y0 * y0 * y0
                + 0.0090316521 * y0 * y0 * y0 * y0 * y0 * y0)
                / 86400.0
        } else {
            (-20.0 + 32.0 * y1820 * y1820) / 86400.0
        }
    }

    /// Include the ephemeris correction to universal time, yielding dynamical time
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3850-L3853>
    pub fn dynamical_from_universal(universal: Moment) -> Moment {
        universal + Self::ephemeris_correction(universal)
    }

    /// Remove the ephemeris correction from dynamical time, yielding universal time
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3845-L3848>
    pub fn universal_from_dynamical(dynamical: Moment) -> Moment {
        dynamical - Self::ephemeris_correction(dynamical)
    }

    /// The number of uniform length centuries (36525 days measured in dynamical time)
    /// before or after noon on January 1, 2000
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3551-L3555>
    pub fn julian_centuries(moment: Moment) -> f64 {
        let intermediate = Self::dynamical_from_universal(moment);
        (intermediate - J2000) / 36525.0
    }

    /// The equation of time, which approximates the difference between apparent solar time and
    /// mean time; for example, the difference between when the sun is highest in the sky (solar noon)
    /// and noon as measured by a clock adjusted to the local longitude. This varies throughout the
    /// year and the difference is given by the equation of time.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, p. 185.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3954-L3983>
    pub fn equation_of_time(moment: Moment) -> f64 {
        let c = Self::julian_centuries(moment);
        let lambda = poly(c, &[280.46645, 36000.76983, 0.0003032]);
        let anomaly = poly(c, &[357.52910, 35999.05030, -0.0001559, -0.00000048]);
        let eccentricity = poly(c, &[0.016708617, -0.000042037, -0.0000001236]);
        let varepsilon = Self::obliquity(moment);
        let y = (varepsilon / 2.0).to_radians().tan();
        let y = y * y;
        let equation = (y * (2.0 * lambda).to_radians().sin()
            - 2.0 * eccentricity * anomaly.to_radians().sin()
            + 4.0
                * eccentricity
                * y
                * anomaly.to_radians().sin()
                * (2.0 * lambda).to_radians().cos()
            - 0.5 * y * y * (4.0 * lambda).to_radians().sin()
            - 1.25 * eccentricity * eccentricity * (2.0 * anomaly).to_radians().sin())
            / (2.0 * PI);

        equation.signum() * equation.abs().min(12.0 / 24.0)
    }

    /// The standard time of dusk at a given location on a given date, or `None` if there is no
    /// dusk on that date.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3670-L3679>
    pub fn dusk(date: f64, location: Location, alpha: f64) -> Option<Moment> {
        let evening = false;
        let moment_of_depression = Self::moment_of_depression(
            Moment::new(date + (18.0 / 24.0)),
            location,
            alpha,
            evening,
        )?;
        Some(Location::standard_from_local(
            moment_of_depression,
            location,
        ))
    }

    /// Calculates the obliquity of the ecliptic at a given moment, meaning the angle of the Earth's
    /// axial tilt with respect to the plane of its orbit around the sun  (currently ~23.4 deg)
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3557-L3565>
    pub fn obliquity(moment: Moment) -> f64 {
        let c = Self::julian_centuries(moment);
        let angle = 23.0 + 26.0 / 60.0 + 21.448 / 3600.0;
        let coefs = &[0.0, -46.8150 / 3600.0, -0.00059 / 3600.0, 0.001813 / 3600.0];
        angle + poly(c, coefs)
    }

    /// Calculates the declination at a given [`Moment`] of UTC time of an object at ecliptic latitude `beta` and ecliptic longitude `lambda`; all angles are in degrees.
    /// the declination is the angular distance north or south of an object in the sky with respect to the plane
    /// of the Earth's equator; analogous to latitude.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3567-L3576>
    pub fn declination(moment: Moment, beta: f64, lambda: f64) -> f64 {
        let varepsilon = Self::obliquity(moment);
        (beta.to_radians().sin() * varepsilon.to_radians().cos()
            + beta.to_radians().cos() * varepsilon.to_radians().sin() * lambda.to_radians().sin())
        .asin()
        .to_degrees()
        .rem_euclid(360.0)
    }

    /// Calculates the right ascension at a given [`Moment`] of UTC time of an object at ecliptic latitude `beta` and ecliptic longitude `lambda`; all angles are in degrees.
    /// the right ascension is the angular distance east or west of an object in the sky with respect to the plane
    /// of the vernal equinox, which is the celestial coordinate point at which the ecliptic intersects the celestial
    /// equator marking spring in the northern hemisphere; analogous to longitude.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3578-L3588>
    pub fn right_ascension(moment: Moment, beta: f64, lambda: f64) -> f64 {
        let varepsilon = Self::obliquity(moment);

        let y = lambda.to_radians().sin() * varepsilon.to_radians().cos()
            - beta.to_radians().tan() * varepsilon.to_radians().sin();
        let x = lambda.to_radians().cos();

        y.atan2(x).to_degrees().rem_euclid(360.0)
    }

    /// Local time from apparent solar time at a given location
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3521-L3524>
    pub fn local_from_apparent(moment: Moment, location: Location) -> Moment {
        moment - Self::equation_of_time(Location::universal_from_local(moment, location))
    }

    /// Approx moment in local time near `moment` at which the depression angle of the sun is `alpha` (negative if
    /// the sun is above the horizon) at the given location; since the same angle of depression of the sun
    /// can exist twice in a day, early is set to true to specify the morning moment, and false for the
    /// evening. Returns `None` if the specified angle is not reached.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3607-L3631>
    pub fn approx_moment_of_depression(
        moment: Moment,
        location: Location,
        alpha: f64,
        early: bool, 
    ) -> Option<Moment> {
        let date = moment.as_rata_die().to_f64_date().floor();
        let alt = if alpha >= 0.0 {
            if early {
                date
            } else {
                date + 1.0
            }
        } else {
            date + 12.0 / 24.0
        };

        let value = if Self::sine_offset(moment, location, alpha).abs() > 1.0 {
            Self::sine_offset(Moment::new(alt), location, alpha)
        } else {
            Self::sine_offset(moment, location, alpha)
        };

        if value.abs() <= 1.0 {
            let offset =
                (value.asin().to_degrees().rem_euclid(360.0) / 360.0 + 0.5).rem_euclid(1.0) - 0.5;

            let moment = Moment::new(
                date + if early {
                    (6.0 / 24.0) - offset
                } else {
                    (18.0 / 24.0) + offset
                },
            );
            Some(Self::local_from_apparent(moment, location))
        } else {
            None
        }
    }

    /// Moment in local time near `approx` at which the depression angle of the sun is `alpha` (negative if
    /// the sun is above the horizon) at the given location; since the same angle of depression of the sun
    /// can exist twice in a day, early is set to true to specify the morning moment, and false for the
    /// evening. Returns `None` if the specified angle is not reached.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3633-L3647>
    pub fn moment_of_depression(
        approx: Moment,
        location: Location,
        alpha: f64,
        early: bool, 
    ) -> Option<Moment> {
        let moment = Self::approx_moment_of_depression(approx, location, alpha, early)?;
        if (approx - moment).abs() < 30.0 {
            Some(moment)
        } else {
            Self::moment_of_depression(moment, location, alpha, early)
        }
    }

    /// The angle of refraction caused by Earth's atmosphere at a given location.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3681-L3690>
    pub fn refraction(location: Location) -> f64 {
        let h = location.elevation.max(0.0);
        let earth_r = 6.372e6; 
        let dip = (earth_r / (earth_r + h)).acos().to_degrees();

        (34.0 / 60.0) + dip + ((19.0 / 3600.0) * h.sqrt())
    }

    /// The moment (in universal time) of the nth new moon after
    /// (or before if n is negative) the new moon of January 11, 1 CE,
    /// which is the first new moon after R.D. 0.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, corrected 2nd edn., 2005.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4288-L4377>
    pub fn nth_new_moon(n: i32) -> Moment {
        let n0 = 24724.0;
        let k = (n as f64) - n0;
        let c = k / 1236.85;
        let approx = J2000
            + (5.09766 + (MEAN_SYNODIC_MONTH * 1236.85 * c) + (0.00015437 * c * c)
                - (0.00000015 * c * c * c)
                + (0.00000000073 * c * c * c * c));
        let e = 1.0 - (0.002516 * c) - (0.0000074 * c * c);
        let solar_anomaly =
            2.5534 + (1236.85 * 29.10535670 * c) - (0.0000014 * c * c) - (0.00000011 * c * c * c);
        let lunar_anomaly = 201.5643
            + (385.81693528 * 1236.85 * c)
            + (0.0107582 * c * c)
            + (0.00001238 * c * c * c)
            - (0.000000058 * c * c * c * c);
        let moon_argument = 160.7108 + (390.67050284 * 1236.85 * c)
            - (0.0016118 * c * c)
            - (0.00000227 * c * c * c)
            + (0.000000011 * c * c * c * c);
        let omega =
            124.7746 + (-1.56375588 * 1236.85 * c) + (0.0020672 * c * c) + (0.00000215 * c * c * c);

        let mut st = (
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        );
        let [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23] = [
            -0.40720, 0.17241, 0.01608, 0.01039, 0.00739, -0.00514, 0.00208, -0.00111, -0.00057,
            0.00056, -0.00042, 0.00042, 0.00038, -0.00024, -0.00007, 0.00004, 0.00004, 0.00003,
            0.00003, -0.00003, 0.00003, -0.00002, -0.00002, 0.00002,
        ];
        let [x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22, x23] = [
            0.0, 1.0, 0.0, 0.0, -1.0, 1.0, 2.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, -1.0, 2.0, 0.0, 3.0,
            1.0, 0.0, 1.0, -1.0, -1.0, 1.0, 0.0,
        ];
        let [y0, y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12, y13, y14, y15, y16, y17, y18, y19, y20, y21, y22, y23] = [
            1.0, 0.0, 2.0, 0.0, 1.0, 1.0, 0.0, 1.0, 1.0, 2.0, 3.0, 0.0, 0.0, 2.0, 1.0, 2.0, 0.0,
            1.0, 2.0, 1.0, 1.0, 1.0, 3.0, 4.0,
        ];
        let [z0, z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12, z13, z14, z15, z16, z17, z18, z19, z20, z21, z22, z23] = [
            0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, -2.0, 2.0, 0.0, 0.0, 2.0, -2.0, 0.0, 0.0, -2.0, 0.0,
            -2.0, 2.0, 2.0, 2.0, -2.0, 0.0, 0.0,
        ];

        let mut at = (
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        );
        let [i0, i1, i2, i3, i4, i5, i6, i7, i8, i9, i10, i11, i12] = [
            251.88, 251.83, 349.42, 84.66, 141.74, 207.14, 154.84, 34.52, 207.19, 291.34, 161.72,
            239.56, 331.55,
        ];
        let [j0, j1, j2, j3, j4, j5, j6, j7, j8, j9, j10, j11, j12] = [
            0.016321, 26.651886, 36.412478, 18.206239, 53.303771, 2.453732, 7.306860, 27.261239,
            0.121824, 1.844379, 24.198154, 25.513099, 3.592518,
        ];
        let [l0, l1, l2, l3, l4, l5, l6, l7, l8, l9, l10, l11, l12] = [
            0.000165, 0.000164, 0.000126, 0.000110, 0.000062, 0.000060, 0.000056, 0.000047,
            0.000042, 0.000040, 0.000037, 0.000035, 0.000023,
        ];

        let mut correction = -0.00017 * omega.to_radians().sin();

        st.0 = v0
            * (x0 * solar_anomaly + y0 * lunar_anomaly + z0 * moon_argument)
                .to_radians()
                .sin();
        st.1 = v1
            * e
            * (x1 * solar_anomaly + y1 * lunar_anomaly + z1 * moon_argument)
                .to_radians()
                .sin();
        st.2 = v2
            * (x2 * solar_anomaly + y2 * lunar_anomaly + z2 * moon_argument)
                .to_radians()
                .sin();
        st.3 = v3
            * (x3 * solar_anomaly + y3 * lunar_anomaly + z3 * moon_argument)
                .to_radians()
                .sin();
        st.4 = v4
            * e
            * (x4 * solar_anomaly + y4 * lunar_anomaly + z4 * moon_argument)
                .to_radians()
                .sin();
        st.5 = v5
            * e
            * (x5 * solar_anomaly + y5 * lunar_anomaly + z5 * moon_argument)
                .to_radians()
                .sin();
        st.6 = v6
            * e
            * e
            * (x6 * solar_anomaly + y6 * lunar_anomaly + z6 * moon_argument)
                .to_radians()
                .sin();
        st.7 = v7
            * (x7 * solar_anomaly + y7 * lunar_anomaly + z7 * moon_argument)
                .to_radians()
                .sin();
        st.8 = v8
            * (x8 * solar_anomaly + y8 * lunar_anomaly + z8 * moon_argument)
                .to_radians()
                .sin();
        st.9 = v9
            * e
            * (x9 * solar_anomaly + y9 * lunar_anomaly + z9 * moon_argument)
                .to_radians()
                .sin();
        st.10 = v10
            * (x10 * solar_anomaly + y10 * lunar_anomaly + z10 * moon_argument)
                .to_radians()
                .sin();
        st.11 = v11
            * e
            * (x11 * solar_anomaly + y11 * lunar_anomaly + z11 * moon_argument)
                .to_radians()
                .sin();
        st.12 = v12
            * e
            * (x12 * solar_anomaly + y12 * lunar_anomaly + z12 * moon_argument)
                .to_radians()
                .sin();
        st.13 = v13
            * e
            * (x13 * solar_anomaly + y13 * lunar_anomaly + z13 * moon_argument)
                .to_radians()
                .sin();
        st.14 = v14
            * (x14 * solar_anomaly + y14 * lunar_anomaly + z14 * moon_argument)
                .to_radians()
                .sin();
        st.15 = v15
            * (x15 * solar_anomaly + y15 * lunar_anomaly + z15 * moon_argument)
                .to_radians()
                .sin();
        st.16 = v16
            * (x16 * solar_anomaly + y16 * lunar_anomaly + z16 * moon_argument)
                .to_radians()
                .sin();
        st.17 = v17
            * (x17 * solar_anomaly + y17 * lunar_anomaly + z17 * moon_argument)
                .to_radians()
                .sin();
        st.18 = v18
            * (x18 * solar_anomaly + y18 * lunar_anomaly + z18 * moon_argument)
                .to_radians()
                .sin();
        st.19 = v19
            * (x19 * solar_anomaly + y19 * lunar_anomaly + z19 * moon_argument)
                .to_radians()
                .sin();
        st.20 = v20
            * (x20 * solar_anomaly + y20 * lunar_anomaly + z20 * moon_argument)
                .to_radians()
                .sin();
        st.21 = v21
            * (x21 * solar_anomaly + y21 * lunar_anomaly + z21 * moon_argument)
                .to_radians()
                .sin();
        st.22 = v22
            * (x22 * solar_anomaly + y22 * lunar_anomaly + z22 * moon_argument)
                .to_radians()
                .sin();
        st.23 = v23
            * (x23 * solar_anomaly + y23 * lunar_anomaly + z23 * moon_argument)
                .to_radians()
                .sin();

        let sum = st.0
            + st.1
            + st.2
            + st.3
            + st.4
            + st.5
            + st.6
            + st.7
            + st.8
            + st.9
            + st.10
            + st.11
            + st.12
            + st.13
            + st.14
            + st.15
            + st.16
            + st.17
            + st.18
            + st.19
            + st.20
            + st.21
            + st.22
            + st.23;

        correction += sum;
        let extra = 0.000325
            * (299.77 + (132.8475848 * c) - (0.009173 * c * c))
                .to_radians()
                .sin();

        at.0 = l0 * (i0 + j0 * k).to_radians().sin();
        at.1 = l1 * (i1 + j1 * k).to_radians().sin();
        at.2 = l2 * (i2 + j2 * k).to_radians().sin();
        at.3 = l3 * (i3 + j3 * k).to_radians().sin();
        at.4 = l4 * (i4 + j4 * k).to_radians().sin();
        at.5 = l5 * (i5 + j5 * k).to_radians().sin();
        at.6 = l6 * (i6 + j6 * k).to_radians().sin();
        at.7 = l7 * (i7 + j7 * k).to_radians().sin();
        at.8 = l8 * (i8 + j8 * k).to_radians().sin();
        at.9 = l9 * (i9 + j9 * k).to_radians().sin();
        at.10 = l10 * (i10 + j10 * k).to_radians().sin();
        at.11 = l11 * (i11 + j11 * k).to_radians().sin();
        at.12 = l12 * (i12 + j12 * k).to_radians().sin();

        let additional = at.0
            + at.1
            + at.2
            + at.3
            + at.4
            + at.5
            + at.6
            + at.7
            + at.8
            + at.9
            + at.10
            + at.11
            + at.12;
        Self::universal_from_dynamical(approx + correction + extra + additional)
    }

    /// Sidereal time, as the hour angle between the meridian and the vernal equinox,
    /// from a given moment.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Meeus, 2nd edition (1988), p. 88.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3860-L3870>
    #[allow(dead_code)] 
    pub fn sidereal_from_moment(moment: Moment) -> f64 {
        let c = (moment - J2000) / 36525.0;
        let coefficients = &[
            (280.46061837),
            (36525.0 * 360.98564736629),
            (0.000387933),
            (-1.0 / 38710000.0),
        ];

        let angle = poly(c, coefficients);

        angle.rem_euclid(360.0)
    }

    /// Ecliptic (aka celestial) latitude of the moon (in degrees)
    ///
    /// This is not a geocentric or geodetic latitude, it does not take into account the
    /// rotation of the Earth and is instead measured from the ecliptic.
    ///
    /// `julian_centuries` is the result of calling `Self::julian_centuries(moment)`.
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, pp. 338-342.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4466>
    pub fn lunar_latitude(julian_centuries: f64) -> f64 {
        let c = julian_centuries;
        let l = Self::mean_lunar_longitude(c);
        let d = Self::lunar_elongation(c);
        let ms = Self::solar_anomaly(c);
        let ml = Self::lunar_anomaly(c);
        let f = Self::moon_node(c);
        let e = 1.0 - (0.002516 * c) - (0.0000074 * c * c);

        let mut ct = (
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        );

        let [w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, w15, w16, w17, w18, w19, w20, w21, w22, w23, w24, w25, w26, w27, w28, w29, w30, w31, w32, w33, w34, w35, w36, w37, w38, w39, w40, w41, w42, w43, w44, w45, w46, w47, w48, w49, w50, w51, w52, w53, w54, w55, w56, w57, w58, w59] = [
            0.0, 0.0, 0.0, 2.0, 2.0, 2.0, 2.0, 0.0, 2.0, 0.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0,
            0.0, 4.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 4.0, 0.0, 4.0, 2.0, 2.0,
            2.0, 2.0, 0.0, 2.0, 2.0, 2.0, 2.0, 4.0, 2.0, 2.0, 0.0, 2.0, 1.0, 1.0, 0.0, 2.0, 1.0,
            2.0, 0.0, 4.0, 4.0, 1.0, 4.0, 1.0, 4.0, 2.0,
        ];

        let [x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, x31, x32, x33, x34, x35, x36, x37, x38, x39, x40, x41, x42, x43, x44, x45, x46, x47, x48, x49, x50, x51, x52, x53, x54, x55, x56, x57, x58, x59] = [
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0, -1.0, -1.0,
            -1.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, -1.0, -2.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0,
            0.0, -1.0, 1.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0, -2.0,
        ];

        let [y0, y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12, y13, y14, y15, y16, y17, y18, y19, y20, y21, y22, y23, y24, y25, y26, y27, y28, y29, y30, y31, y32, y33, y34, y35, y36, y37, y38, y39, y40, y41, y42, y43, y44, y45, y46, y47, y48, y49, y50, y51, y52, y53, y54, y55, y56, y57, y58, y59] = [
            0.0, 1.0, 1.0, 0.0, -1.0, -1.0, 0.0, 2.0, 1.0, 2.0, 0.0, -2.0, 1.0, 0.0, -1.0, 0.0,
            -1.0, -1.0, -1.0, 0.0, 0.0, -1.0, 0.0, 1.0, 1.0, 0.0, 0.0, 3.0, 0.0, -1.0, 1.0, -2.0,
            0.0, 2.0, 1.0, -2.0, 3.0, 2.0, -3.0, -1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 0.0,
            -2.0, -1.0, 1.0, -2.0, 2.0, -2.0, -1.0, 1.0, 1.0, -1.0, 0.0, 0.0,
        ];

        let [z0, z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12, z13, z14, z15, z16, z17, z18, z19, z20, z21, z22, z23, z24, z25, z26, z27, z28, z29, z30, z31, z32, z33, z34, z35, z36, z37, z38, z39, z40, z41, z42, z43, z44, z45, z46, z47, z48, z49, z50, z51, z52, z53, z54, z55, z56, z57, z58, z59] = [
            1.0, 1.0, -1.0, -1.0, 1.0, -1.0, 1.0, 1.0, -1.0, -1.0, -1.0, -1.0, 1.0, -1.0, 1.0, 1.0,
            -1.0, -1.0, -1.0, 1.0, 3.0, 1.0, 1.0, 1.0, -1.0, -1.0, -1.0, 1.0, -1.0, 1.0, -3.0, 1.0,
            -3.0, -1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, 1.0, 1.0, 1.0, -1.0, 3.0, -1.0, -1.0, 1.0,
            -1.0, -1.0, 1.0, -1.0, 1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, 1.0,
        ];

        let [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58, v59] = [
            5128122.0, 280602.0, 277693.0, 173237.0, 55413.0, 46271.0, 32573.0, 17198.0, 9266.0,
            8822.0, 8216.0, 4324.0, 4200.0, -3359.0, 2463.0, 2211.0, 2065.0, -1870.0, 1828.0,
            -1794.0, -1749.0, -1565.0, -1491.0, -1475.0, -1410.0, -1344.0, -1335.0, 1107.0, 1021.0,
            833.0, 777.0, 671.0, 607.0, 596.0, 491.0, -451.0, 439.0, 422.0, 421.0, -366.0, -351.0,
            331.0, 315.0, 302.0, -283.0, -229.0, 223.0, 223.0, -220.0, -220.0, -185.0, 181.0,
            -177.0, 176.0, 166.0, -164.0, 132.0, -119.0, 115.0, 107.0,
        ];

        ct.0 = v0 * (w0 * d + x0 * ms + y0 * ml + z0 * f).to_radians().sin();
        ct.1 = v1 * (w1 * d + x1 * ms + y1 * ml + z1 * f).to_radians().sin();
        ct.2 = v2 * (w2 * d + x2 * ms + y2 * ml + z2 * f).to_radians().sin();
        ct.3 = v3 * (w3 * d + x3 * ms + y3 * ml + z3 * f).to_radians().sin();
        ct.4 = v4 * (w4 * d + x4 * ms + y4 * ml + z4 * f).to_radians().sin();
        ct.5 = v5 * (w5 * d + x5 * ms + y5 * ml + z5 * f).to_radians().sin();
        ct.6 = v6 * (w6 * d + x6 * ms + y6 * ml + z6 * f).to_radians().sin();
        ct.7 = v7 * (w7 * d + x7 * ms + y7 * ml + z7 * f).to_radians().sin();
        ct.8 = v8 * (w8 * d + x8 * ms + y8 * ml + z8 * f).to_radians().sin();
        ct.9 = v9 * (w9 * d + x9 * ms + y9 * ml + z9 * f).to_radians().sin();
        ct.10 = v10 * e * (w10 * d + x10 * ms + y10 * ml + z10 * f).to_radians().sin();
        ct.11 = v11 * (w11 * d + x11 * ms + y11 * ml + z11 * f).to_radians().sin();
        ct.12 = v12 * (w12 * d + x12 * ms + y12 * ml + z12 * f).to_radians().sin();
        ct.13 = v13 * e * (w13 * d + x13 * ms + y13 * ml + z13 * f).to_radians().sin();
        ct.14 = v14 * e * (w14 * d + x14 * ms + y14 * ml + z14 * f).to_radians().sin();
        ct.15 = v15 * e * (w15 * d + x15 * ms + y15 * ml + z15 * f).to_radians().sin();
        ct.16 = v16 * e * (w16 * d + x16 * ms + y16 * ml + z16 * f).to_radians().sin();
        ct.17 = v17 * e * (w17 * d + x17 * ms + y17 * ml + z17 * f).to_radians().sin();
        ct.18 = v18 * (w18 * d + x18 * ms + y18 * ml + z18 * f).to_radians().sin();
        ct.19 = v19 * e * (w19 * d + x19 * ms + y19 * ml + z19 * f).to_radians().sin();
        ct.20 = v20 * (w20 * d + x20 * ms + y20 * ml + z20 * f).to_radians().sin();
        ct.21 = v21 * e * (w21 * d + x21 * ms + y21 * ml + z21 * f).to_radians().sin();
        ct.22 = v22 * (w22 * d + x22 * ms + y22 * ml + z22 * f).to_radians().sin();
        ct.23 = v23 * e * (w23 * d + x23 * ms + y23 * ml + z23 * f).to_radians().sin();
        ct.24 = v24 * e * (w24 * d + x24 * ms + y24 * ml + z24 * f).to_radians().sin();
        ct.25 = v25 * e * (w25 * d + x25 * ms + y25 * ml + z25 * f).to_radians().sin();
        ct.26 = v26 * (w26 * d + x26 * ms + y26 * ml + z26 * f).to_radians().sin();
        ct.27 = v27 * (w27 * d + x27 * ms + y27 * ml + z27 * f).to_radians().sin();
        ct.28 = v28 * (w28 * d + x28 * ms + y28 * ml + z28 * f).to_radians().sin();
        ct.29 = v29 * (w29 * d + x29 * ms + y29 * ml + z29 * f).to_radians().sin();
        ct.30 = v30 * (w30 * d + x30 * ms + y30 * ml + z30 * f).to_radians().sin();
        ct.31 = v31 * (w31 * d + x31 * ms + y31 * ml + z31 * f).to_radians().sin();
        ct.32 = v32 * (w32 * d + x32 * ms + y32 * ml + z32 * f).to_radians().sin();
        ct.33 = v33 * (w33 * d + x33 * ms + y33 * ml + z33 * f).to_radians().sin();
        ct.34 = v34 * e * (w34 * d + x34 * ms + y34 * ml + z34 * f).to_radians().sin();
        ct.35 = v35 * (w35 * d + x35 * ms + y35 * ml + z35 * f).to_radians().sin();
        ct.36 = v36 * (w36 * d + x36 * ms + y36 * ml + z36 * f).to_radians().sin();
        ct.37 = v37 * (w37 * d + x37 * ms + y37 * ml + z37 * f).to_radians().sin();
        ct.38 = v38 * (w38 * d + x38 * ms + y38 * ml + z38 * f).to_radians().sin();
        ct.39 = v39 * e * (w39 * d + x39 * ms + y39 * ml + z39 * f).to_radians().sin();
        ct.40 = v40 * e * (w40 * d + x40 * ms + y40 * ml + z40 * f).to_radians().sin();
        ct.41 = v41 * (w41 * d + x41 * ms + y41 * ml + z41 * f).to_radians().sin();
        ct.42 = v42 * e * (w42 * d + x42 * ms + y42 * ml + z42 * f).to_radians().sin();
        ct.43 = v43 * e * e * (w43 * d + x43 * ms + y43 * ml + z43 * f).to_radians().sin();
        ct.44 = v44 * (w44 * d + x44 * ms + y44 * ml + z44 * f).to_radians().sin();
        ct.45 = v45 * e * (w45 * d + x45 * ms + y45 * ml + z45 * f).to_radians().sin();
        ct.46 = v46 * e * (w46 * d + x46 * ms + y46 * ml + z46 * f).to_radians().sin();
        ct.47 = v47 * e * (w47 * d + x47 * ms + y47 * ml + z47 * f).to_radians().sin();
        ct.48 = v48 * e * (w48 * d + x48 * ms + y48 * ml + z48 * f).to_radians().sin();
        ct.49 = v49 * e * (w49 * d + x49 * ms + y49 * ml + z49 * f).to_radians().sin();
        ct.50 = v50 * (w50 * d + x50 * ms + y50 * ml + z50 * f).to_radians().sin();
        ct.51 = v51 * e * (w51 * d + x51 * ms + y51 * ml + z51 * f).to_radians().sin();
        ct.52 = v52 * e * (w52 * d + x52 * ms + y52 * ml + z52 * f).to_radians().sin();
        ct.53 = v53 * (w53 * d + x53 * ms + y53 * ml + z53 * f).to_radians().sin();
        ct.54 = v54 * e * (w54 * d + x54 * ms + y54 * ml + z54 * f).to_radians().sin();
        ct.55 = v55 * (w55 * d + x55 * ms + y55 * ml + z55 * f).to_radians().sin();
        ct.56 = v56 * (w56 * d + x56 * ms + y56 * ml + z56 * f).to_radians().sin();
        ct.57 = v57 * (w57 * d + x57 * ms + y57 * ml + z57 * f).to_radians().sin();
        ct.58 = v58 * e * (w58 * d + x58 * ms + y58 * ml + z58 * f).to_radians().sin();
        ct.59 = v59 * e * e * (w59 * d + x59 * ms + y59 * ml + z59 * f).to_radians().sin();

        let mut correction = ct.0
            + ct.1
            + ct.2
            + ct.3
            + ct.4
            + ct.5
            + ct.6
            + ct.7
            + ct.8
            + ct.9
            + ct.10
            + ct.11
            + ct.12
            + ct.13
            + ct.14
            + ct.15
            + ct.16
            + ct.17
            + ct.18
            + ct.19
            + ct.20
            + ct.21
            + ct.22
            + ct.23
            + ct.24
            + ct.25
            + ct.26
            + ct.27
            + ct.28
            + ct.29
            + ct.30
            + ct.31
            + ct.32
            + ct.33
            + ct.34
            + ct.35
            + ct.36
            + ct.37
            + ct.38
            + ct.39
            + ct.40
            + ct.41
            + ct.42
            + ct.43
            + ct.44
            + ct.45
            + ct.46
            + ct.47
            + ct.48
            + ct.49
            + ct.50
            + ct.51
            + ct.52
            + ct.53
            + ct.54
            + ct.55
            + ct.56
            + ct.57
            + ct.58
            + ct.59;

        correction /= 1_000_000.0;

        let venus = (175.0
            * ((119.75 + c * 131.849 + f).to_radians().sin()
                + (119.75 + c * 131.849 - f).to_radians().sin()))
            / 1_000_000.0;

        let flat_earth = (-2235.0 * l.to_radians().sin()
            + 127.0 * (l - ml).to_radians().sin()
            + -115.0 * (l + ml).to_radians().sin())
            / 1_000_000.0;

        let extra = (382.0 * (313.45 + (c * 481266.484)).to_radians().sin()) / 1_000_000.0;

        correction + venus + flat_earth + extra
    }

    /// Ecliptic (aka celestial) longitude of the moon (in degrees)
    ///
    /// This is not a geocentric or geodetic longitude, it does not take into account the
    /// rotation of the Earth and is instead measured from the ecliptic and the vernal equinox.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, pp. 338-342.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4215-L4278>
    pub fn lunar_longitude(julian_centuries: f64) -> f64 {
        let c = julian_centuries;
        let l = Self::mean_lunar_longitude(c);
        let d = Self::lunar_elongation(c);
        let ms = Self::solar_anomaly(c);
        let ml = Self::lunar_anomaly(c);
        let f = Self::moon_node(c);
        let e = 1.0 - (0.002516 * c) - (0.0000074 * c * c);

        let mut ct = (
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        );

        let [v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31, v32, v33, v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54, v55, v56, v57, v58] = [
            6288774.0, 1274027.0, 658314.0, 213618.0, -185116.0, -114332.0, 58793.0, 57066.0,
            53322.0, 45758.0, -40923.0, -34720.0, -30383.0, 15327.0, -12528.0, 10980.0, 10675.0,
            10034.0, 8548.0, -7888.0, -6766.0, -5163.0, 4987.0, 4036.0, 3994.0, 3861.0, 3665.0,
            -2689.0, -2602.0, 2390.0, -2348.0, 2236.0, -2120.0, -2069.0, 2048.0, -1773.0, -1595.0,
            1215.0, -1110.0, -892.0, -810.0, 759.0, -713.0, -700.0, 691.0, 596.0, 549.0, 537.0,
            520.0, -487.0, -399.0, -381.0, 351.0, -340.0, 330.0, 327.0, -323.0, 299.0, 294.0,
        ];
        let [w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, w15, w16, w17, w18, w19, w20, w21, w22, w23, w24, w25, w26, w27, w28, w29, w30, w31, w32, w33, w34, w35, w36, w37, w38, w39, w40, w41, w42, w43, w44, w45, w46, w47, w48, w49, w50, w51, w52, w53, w54, w55, w56, w57, w58] = [
            0.0, 2.0, 2.0, 0.0, 0.0, 0.0, 2.0, 2.0, 2.0, 2.0, 0.0, 1.0, 0.0, 2.0, 0.0, 0.0, 4.0,
            0.0, 4.0, 2.0, 2.0, 1.0, 1.0, 2.0, 2.0, 4.0, 2.0, 0.0, 2.0, 2.0, 1.0, 2.0, 0.0, 0.0,
            2.0, 2.0, 2.0, 4.0, 0.0, 3.0, 2.0, 4.0, 0.0, 2.0, 2.0, 2.0, 4.0, 0.0, 4.0, 1.0, 2.0,
            0.0, 1.0, 3.0, 4.0, 2.0, 0.0, 1.0, 2.0,
        ];
        let [x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, x31, x32, x33, x34, x35, x36, x37, x38, x39, x40, x41, x42, x43, x44, x45, x46, x47, x48, x49, x50, x51, x52, x53, x54, x55, x56, x57, x58] = [
            0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0, 0.0, -1.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 1.0, 0.0, 1.0, -1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0, -2.0, 1.0, 2.0,
            -2.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0, -1.0, 2.0, 2.0, 1.0, -1.0, 0.0, 0.0, -1.0, 0.0,
            1.0, 0.0, 1.0, 0.0, 0.0, -1.0, 2.0, 1.0, 0.0,
        ];
        let [y0, y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12, y13, y14, y15, y16, y17, y18, y19, y20, y21, y22, y23, y24, y25, y26, y27, y28, y29, y30, y31, y32, y33, y34, y35, y36, y37, y38, y39, y40, y41, y42, y43, y44, y45, y46, y47, y48, y49, y50, y51, y52, y53, y54, y55, y56, y57, y58] = [
            1.0, -1.0, 0.0, 2.0, 0.0, 0.0, -2.0, -1.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 1.0,
            -1.0, 3.0, -2.0, -1.0, 0.0, -1.0, 0.0, 1.0, 2.0, 0.0, -3.0, -2.0, -1.0, -2.0, 1.0, 0.0,
            2.0, 0.0, -1.0, 1.0, 0.0, -1.0, 2.0, -1.0, 1.0, -2.0, -1.0, -1.0, -2.0, 0.0, 1.0, 4.0,
            0.0, -2.0, 0.0, 2.0, 1.0, -2.0, -3.0, 2.0, 1.0, -1.0, 3.0,
        ];
        let [z0, z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12, z13, z14, z15, z16, z17, z18, z19, z20, z21, z22, z23, z24, z25, z26, z27, z28, z29, z30, z31, z32, z33, z34, z35, z36, z37, z38, z39, z40, z41, z42, z43, z44, z45, z46, z47, z48, z49, z50, z51, z52, z53, z54, z55, z56, z57, z58] = [
            0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -2.0, 2.0, -2.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, -2.0, 2.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -2.0, 0.0, 0.0, 0.0, 0.0, -2.0,
            -2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        ];

        ct.0 = v0 * (w0 * d + x0 * ms + y0 * ml + z0 * f).to_radians().sin();
        ct.1 = v1 * (w1 * d + x1 * ms + y1 * ml + z1 * f).to_radians().sin();
        ct.2 = v2 * (w2 * d + x2 * ms + y2 * ml + z2 * f).to_radians().sin();
        ct.3 = v3 * (w3 * d + x3 * ms + y3 * ml + z3 * f).to_radians().sin();
        ct.4 = v4 * e * (w4 * d + x4 * ms + y4 * ml + z4 * f).to_radians().sin();
        ct.5 = v5 * (w5 * d + x5 * ms + y5 * ml + z5 * f).to_radians().sin();
        ct.6 = v6 * (w6 * d + x6 * ms + y6 * ml + z6 * f).to_radians().sin();
        ct.7 = v7 * e * (w7 * d + x7 * ms + y7 * ml + z7 * f).to_radians().sin();
        ct.8 = v8 * (w8 * d + x8 * ms + y8 * ml + z8 * f).to_radians().sin();
        ct.9 = v9 * e * (w9 * d + x9 * ms + y9 * ml + z9 * f).to_radians().sin();
        ct.10 = v10 * e * (w10 * d + x10 * ms + y10 * ml + z10 * f).to_radians().sin();
        ct.11 = v11 * (w11 * d + x11 * ms + y11 * ml + z11 * f).to_radians().sin();
        ct.12 = v12 * e * (w12 * d + x12 * ms + y12 * ml + z12 * f).to_radians().sin();
        ct.13 = v13 * (w13 * d + x13 * ms + y13 * ml + z13 * f).to_radians().sin();
        ct.14 = v14 * (w14 * d + x14 * ms + y14 * ml + z14 * f).to_radians().sin();
        ct.15 = v15 * (w15 * d + x15 * ms + y15 * ml + z15 * f).to_radians().sin();
        ct.16 = v16 * (w16 * d + x16 * ms + y16 * ml + z16 * f).to_radians().sin();
        ct.17 = v17 * (w17 * d + x17 * ms + y17 * ml + z17 * f).to_radians().sin();
        ct.18 = v18 * (w18 * d + x18 * ms + y18 * ml + z18 * f).to_radians().sin();
        ct.19 = v19 * e * (w19 * d + x19 * ms + y19 * ml + z19 * f).to_radians().sin();
        ct.20 = v20 * e * (w20 * d + x20 * ms + y20 * ml + z20 * f).to_radians().sin();
        ct.21 = v21 * (w21 * d + x21 * ms + y21 * ml + z21 * f).to_radians().sin();
        ct.22 = v22 * e * (w22 * d + x22 * ms + y22 * ml + z22 * f).to_radians().sin();
        ct.23 = v23 * e * (w23 * d + x23 * ms + y23 * ml + z23 * f).to_radians().sin();
        ct.24 = v24 * (w24 * d + x24 * ms + y24 * ml + z24 * f).to_radians().sin();
        ct.25 = v25 * (w25 * d + x25 * ms + y25 * ml + z25 * f).to_radians().sin();
        ct.26 = v26 * (w26 * d + x26 * ms + y26 * ml + z26 * f).to_radians().sin();
        ct.27 = v27 * e * (w27 * d + x27 * ms + y27 * ml + z27 * f).to_radians().sin();
        ct.28 = v28 * (w28 * d + x28 * ms + y28 * ml + z28 * f).to_radians().sin();
        ct.29 = v29 * e * (w29 * d + x29 * ms + y29 * ml + z29 * f).to_radians().sin();
        ct.30 = v30 * (w30 * d + x30 * ms + y30 * ml + z30 * f).to_radians().sin();
        ct.31 = v31 * e * e * (w31 * d + x31 * ms + y31 * ml + z31 * f).to_radians().sin();
        ct.32 = v32 * e * (w32 * d + x32 * ms + y32 * ml + z32 * f).to_radians().sin();
        ct.33 = v33 * e * e * (w33 * d + x33 * ms + y33 * ml + z33 * f).to_radians().sin();
        ct.34 = v34 * e * e * (w34 * d + x34 * ms + y34 * ml + z34 * f).to_radians().sin();
        ct.35 = v35 * (w35 * d + x35 * ms + y35 * ml + z35 * f).to_radians().sin();
        ct.36 = v36 * (w36 * d + x36 * ms + y36 * ml + z36 * f).to_radians().sin();
        ct.37 = v37 * e * (w37 * d + x37 * ms + y37 * ml + z37 * f).to_radians().sin();
        ct.38 = v38 * (w38 * d + x38 * ms + y38 * ml + z38 * f).to_radians().sin();
        ct.39 = v39 * (w39 * d + x39 * ms + y39 * ml + z39 * f).to_radians().sin();
        ct.40 = v40 * e * (w40 * d + x40 * ms + y40 * ml + z40 * f).to_radians().sin();
        ct.41 = v41 * e * (w41 * d + x41 * ms + y41 * ml + z41 * f).to_radians().sin();
        ct.42 = v42 * e * e * (w42 * d + x42 * ms + y42 * ml + z42 * f).to_radians().sin();
        ct.43 = v43 * e * e * (w43 * d + x43 * ms + y43 * ml + z43 * f).to_radians().sin();
        ct.44 = v44 * e * (w44 * d + x44 * ms + y44 * ml + z44 * f).to_radians().sin();
        ct.45 = v45 * e * (w45 * d + x45 * ms + y45 * ml + z45 * f).to_radians().sin();
        ct.46 = v46 * (w46 * d + x46 * ms + y46 * ml + z46 * f).to_radians().sin();
        ct.47 = v47 * (w47 * d + x47 * ms + y47 * ml + z47 * f).to_radians().sin();
        ct.48 = v48 * e * (w48 * d + x48 * ms + y48 * ml + z48 * f).to_radians().sin();
        ct.49 = v49 * (w49 * d + x49 * ms + y49 * ml + z49 * f).to_radians().sin();
        ct.50 = v50 * e * (w50 * d + x50 * ms + y50 * ml + z50 * f).to_radians().sin();
        ct.51 = v51 * (w51 * d + x51 * ms + y51 * ml + z51 * f).to_radians().sin();
        ct.52 = v52 * e * (w52 * d + x52 * ms + y52 * ml + z52 * f).to_radians().sin();
        ct.53 = v53 * (w53 * d + x53 * ms + y53 * ml + z53 * f).to_radians().sin();
        ct.54 = v54 * (w54 * d + x54 * ms + y54 * ml + z54 * f).to_radians().sin();
        ct.55 = v55 * e * (w55 * d + x55 * ms + y55 * ml + z55 * f).to_radians().sin();
        ct.56 = v56 * e * e * (w56 * d + x56 * ms + y56 * ml + z56 * f).to_radians().sin();
        ct.57 = v57 * e * (w57 * d + x57 * ms + y57 * ml + z57 * f).to_radians().sin();
        ct.58 = v58 * (w58 * d + x58 * ms + y58 * ml + z58 * f).to_radians().sin();

        let mut correction = ct.0
            + ct.1
            + ct.2
            + ct.3
            + ct.4
            + ct.5
            + ct.6
            + ct.7
            + ct.8
            + ct.9
            + ct.10
            + ct.11
            + ct.12
            + ct.13
            + ct.14
            + ct.15
            + ct.16
            + ct.17
            + ct.18
            + ct.19
            + ct.20
            + ct.21
            + ct.22
            + ct.23
            + ct.24
            + ct.25
            + ct.26
            + ct.27
            + ct.28
            + ct.29
            + ct.30
            + ct.31
            + ct.32
            + ct.33
            + ct.34
            + ct.35
            + ct.36
            + ct.37
            + ct.38
            + ct.39
            + ct.40
            + ct.41
            + ct.42
            + ct.43
            + ct.44
            + ct.45
            + ct.46
            + ct.47
            + ct.48
            + ct.49
            + ct.50
            + ct.51
            + ct.52
            + ct.53
            + ct.54
            + ct.55
            + ct.56
            + ct.57
            + ct.58;

        correction /= 1000000.0;
        let venus = 3958.0 / 1000000.0 * (119.75 + c * 131.849).to_radians().sin();
        let jupiter = 318.0 / 1000000.0 * (53.09 + c * 479264.29).to_radians().sin();
        let flat_earth = 1962.0 / 1000000.0 * (l - f).to_radians().sin();
        (l + correction + venus + jupiter + flat_earth + Self::nutation(julian_centuries))
            .rem_euclid(360.0)
    }

    /// Mean longitude of the moon (in degrees) at a given Moment in Julian centuries.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, pp. 336-340.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4148-L4158>
    fn mean_lunar_longitude(c: f64) -> f64 {
        let n = 218.3164477
            + c * (481267.88123421 - 0.0015786 * c + c * c / 538841.0 - c * c * c / 65194000.0);

        n.rem_euclid(360.0)
    }

    /// Closest fixed date on or after `date` on the eve of which crescent moon first became visible at `location`.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L6883-L6896>
    pub fn phasis_on_or_after(
        date: RataDie,
        location: Location,
        lunar_phase: Option<f64>,
    ) -> RataDie {
        let lunar_phase =
            lunar_phase.unwrap_or_else(|| Self::calculate_new_moon_at_or_before(date));
        let age = date.to_f64_date() - lunar_phase;
        let tau = if age <= 4.0 || Self::visible_crescent((date - 1).as_moment(), location) {
            lunar_phase + 29.0 
        } else {
            date.to_f64_date()
        };
        next_moment(Moment::new(tau), location, Self::visible_crescent)
    }

    /// Closest fixed date on or before `date` when crescent moon first became visible at `location`.
    /// Lunar phase is the result of calling `lunar_phase(moment, julian_centuries) in an earlier function.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L6868-L6881>
    pub fn phasis_on_or_before(
        date: RataDie,
        location: Location,
        lunar_phase: Option<f64>,
    ) -> RataDie {
        let lunar_phase =
            lunar_phase.unwrap_or_else(|| Self::calculate_new_moon_at_or_before(date));
        let age = date.to_f64_date() - lunar_phase;
        let tau = if age <= 3.0 && !Self::visible_crescent((date).as_moment(), location) {
            lunar_phase - 30.0 
        } else {
            lunar_phase
        };
        next_moment(Moment::new(tau), location, Self::visible_crescent)
    }

    /// Calculate the day that the new moon occurred on or before the given date.
    pub fn calculate_new_moon_at_or_before(date: RataDie) -> f64 {
        Self::lunar_phase_at_or_before(0.0, date.as_moment())
            .inner()
            .floor()
    }

    /// Length of the lunar month containing `date` in days, based on observability at `location`.
    /// Calculates the month length for the Islamic Observational Calendar
    /// Can return 31 days due to the imprecise nature of trying to approximate an observational calendar. (See page 294 of the Calendrical Calculations book)
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L7068-L7074>
    pub fn month_length(date: RataDie, location: Location) -> u8 {
        let moon = Self::phasis_on_or_after(date + 1, location, None);
        let prev = Self::phasis_on_or_before(date, location, None);

        debug_assert!(moon > prev);
        debug_assert!(moon - prev < u8::MAX.into());
        (moon - prev) as u8
    }

    /// Lunar elongation (the moon's angular distance east of the Sun) at a given Moment in Julian centuries
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, p. 338.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4160-L4170>
    fn lunar_elongation(c: f64) -> f64 {
        (297.85019021 + 445267.1114034 * c - 0.0018819 * c * c + c * c * c / 545868.0
            - c * c * c * c / 113065000.0)
            .rem_euclid(360.0)
    }

    /// Altitude of the moon (in degrees) at a given moment
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4537>
    pub fn lunar_altitude(moment: Moment, location: Location) -> f64 {
        let phi = location.latitude;
        let psi = location.longitude;
        let c = Self::julian_centuries(moment);
        let lambda = Self::lunar_longitude(c);
        let beta = Self::lunar_latitude(c);
        let alpha = Self::right_ascension(moment, beta, lambda);
        let delta = Self::declination(moment, beta, lambda);
        let theta0 = Self::sidereal_from_moment(moment);
        let cap_h = (theta0 + psi - alpha).rem_euclid(360.0);

        let altitude = (phi.to_radians().sin() * delta.to_radians().sin()
            + phi.to_radians().cos() * delta.to_radians().cos() * cap_h.to_radians().cos())
        .asin()
        .to_degrees();

        (altitude + 180.0).rem_euclid(360.0) - 180.0
    }

    /// Distance to the moon in meters at the given moment.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, pp. 338-342.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4568-L4617>
    #[allow(dead_code)]
    pub fn lunar_distance(moment: Moment) -> f64 {
        let c = Self::julian_centuries(moment);
        let cap_d = Self::lunar_elongation(c);
        let cap_m = Self::solar_anomaly(c);
        let cap_m_prime = Self::lunar_anomaly(c);
        let cap_f = Self::moon_node(c);
        let cap_e = 1.0 - (0.002516 * c) - (0.0000074 * c * c);

        let args_lunar_elongation = [
            0.0, 2.0, 2.0, 0.0, 0.0, 0.0, 2.0, 2.0, 2.0, 2.0, 0.0, 1.0, 0.0, 2.0, 0.0, 0.0, 4.0,
            0.0, 4.0, 2.0, 2.0, 1.0, 1.0, 2.0, 2.0, 4.0, 2.0, 0.0, 2.0, 2.0, 1.0, 2.0, 0.0, 0.0,
            2.0, 2.0, 2.0, 4.0, 0.0, 3.0, 2.0, 4.0, 0.0, 2.0, 2.0, 2.0, 4.0, 0.0, 4.0, 1.0, 2.0,
            0.0, 1.0, 3.0, 4.0, 2.0, 0.0, 1.0, 2.0, 2.0,
        ];

        let args_solar_anomaly = [
            0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0, 0.0, -1.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 1.0, 0.0, 1.0, -1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0, -2.0, 1.0, 2.0,
            -2.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0, -1.0, 2.0, 2.0, 1.0, -1.0, 0.0, 0.0, -1.0, 0.0,
            1.0, 0.0, 1.0, 0.0, 0.0, -1.0, 2.0, 1.0, 0.0, 0.0,
        ];

        let args_lunar_anomaly = [
            1.0, -1.0, 0.0, 2.0, 0.0, 0.0, -2.0, -1.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 1.0, 1.0,
            -1.0, 3.0, -2.0, -1.0, 0.0, -1.0, 0.0, 1.0, 2.0, 0.0, -3.0, -2.0, -1.0, -2.0, 1.0, 0.0,
            2.0, 0.0, -1.0, 1.0, 0.0, -1.0, 2.0, -1.0, 1.0, -2.0, -1.0, -1.0, -2.0, 0.0, 1.0, 4.0,
            0.0, -2.0, 0.0, 2.0, 1.0, -2.0, -3.0, 2.0, 1.0, -1.0, 3.0, -1.0,
        ];

        let args_moon_node = [
            0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -2.0, 2.0, -2.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, -2.0, 2.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -2.0, 0.0, 0.0, 0.0, 0.0, -2.0,
            -2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -2.0,
        ];

        let cosine_coeff = [
            -20905355.0,
            -3699111.0,
            -2955968.0,
            -569925.0,
            48888.0,
            -3149.0,
            246158.0,
            -152138.0,
            -170733.0,
            -204586.0,
            -129620.0,
            108743.0,
            104755.0,
            10321.0,
            0.0,
            79661.0,
            -34782.0,
            -23210.0,
            -21636.0,
            24208.0,
            30824.0,
            -8379.0,
            -16675.0,
            -12831.0,
            -10445.0,
            -11650.0,
            14403.0,
            -7003.0,
            0.0,
            10056.0,
            6322.0,
            -9884.0,
            5751.0,
            0.0,
            -4950.0,
            4130.0,
            0.0,
            -3958.0,
            0.0,
            3258.0,
            2616.0,
            -1897.0,
            -2117.0,
            2354.0,
            0.0,
            0.0,
            -1423.0,
            -1117.0,
            -1571.0,
            -1739.0,
            0.0,
            -4421.0,
            0.0,
            0.0,
            0.0,
            0.0,
            1165.0,
            0.0,
            0.0,
            8752.0,
        ];

        let correction: f64 = cosine_coeff
            .iter()
            .zip(args_lunar_elongation.iter())
            .zip(args_solar_anomaly.iter())
            .zip(args_lunar_anomaly.iter())
            .zip(args_moon_node.iter())
            .map(|((((&v, &w), &x), &y), &z)| {
                v * cap_e.powf(x.abs())
                    * (w * cap_d + x * cap_m + y * cap_m_prime + z * cap_f)
                        .to_radians()
                        .cos()
            })
            .sum();

        385000560.0 + correction
    }

    /// The parallax of the moon, meaning the difference in angle of the direction of the moon
    /// as measured from a given location and from the center of the Earth, in degrees.
    /// Note: the location is encoded as the `lunar_altitude_val` which is the result of `lunar_altitude(moment,location)`.
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4619-L4628>
    pub fn lunar_parallax(lunar_altitude_val: f64, moment: Moment) -> f64 {
        let cap_delta = Self::lunar_distance(moment);
        let alt = 6378140.0 / cap_delta;
        let arg = alt * lunar_altitude_val.to_radians().cos();
        arg.asin().to_degrees().rem_euclid(360.0)
    }

    /// Topocentric altitude of the moon.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4630-L4636>
    fn topocentric_lunar_altitude(moment: Moment, location: Location) -> f64 {
        let lunar_altitude = Self::lunar_altitude(moment, location);
        lunar_altitude - Self::lunar_parallax(lunar_altitude, moment)
    }

    /// Observed altitude of upper limb of moon at moment at location.
    /// /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4646-L4653>
    fn observed_lunar_altitude(moment: Moment, location: Location) -> f64 {
        let r = Self::topocentric_lunar_altitude(moment, location);
        let y = Self::refraction(location);
        let z = 16.0 / 60.0;

        r + y + z
    }

    /// Average anomaly of the sun (in degrees) at a given Moment in Julian centuries.
    /// See: https://en.wikipedia.org/wiki/Mean_anomaly
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, p. 338.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4172-L4182>
    fn solar_anomaly(c: f64) -> f64 {
        (357.5291092 + 35999.0502909 * c - 0.0001536 * c * c + c * c * c / 24490000.0)
            .rem_euclid(360.0)
    }

    /// Average anomaly of the moon (in degrees) at a given Moment in Julian centuries
    /// See: https://en.wikipedia.org/wiki/Mean_anomaly
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, p. 338.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4184-L4194>
    fn lunar_anomaly(c: f64) -> f64 {
        (134.9633964 + 477198.8675055 * c + 0.0087414 * c * c + c * c * c / 69699.0
            - c * c * c * c / 14712000.0)
            .rem_euclid(360.0)
    }

    /// The moon's argument of latitude, in degrees, at the moment given by `c` in Julian centuries.
    /// The argument of latitude is used to define the position of a body moving in a Kepler orbit.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from _Astronomical Algorithms_ by Jean Meeus, 2nd edn., 1998, p. 338.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4196-L4206>
    fn moon_node(c: f64) -> f64 {
        (93.2720950 + 483202.0175233 * c - 0.0036539 * c * c - c * c * c / 3526000.0
            + c * c * c * c / 863310000.0)
            .rem_euclid(360.0)
    }

    /// Standard time of moonset on the date of the given moment and at the given location.
    /// Returns `None` if there is no such moonset.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4655-L4681>
    #[allow(dead_code)] 
    fn moonset(date: Moment, location: Location) -> Option<Moment> {
        let moment = Location::universal_from_standard(date, location);
        let waxing = Self::lunar_phase(date, Self::julian_centuries(date)) < 180.0;
        let alt = Self::observed_lunar_altitude(moment, location);
        let lat = location.latitude;
        let offset = alt / (4.0 * (90.0 - lat.abs()));

        let approx = if waxing {
            if offset > 0.0 {
                moment + offset
            } else {
                moment + 1.0 + offset
            }
        } else {
            moment - offset + 0.5
        };

        let set = Moment::new(binary_search(
            approx.inner() - (6.0 / 24.0),
            approx.inner() + (6.0 / 24.0),
            |x| Self::observed_lunar_altitude(Moment::new(x), location) < 0.0,
            1.0 / 24.0 / 60.0,
        ));

        if set < moment + 1.0 {
            let std = Moment::new(
                Location::standard_from_universal(set, location)
                    .inner()
                    .max(date.inner()),
            );
            debug_assert!(std >= date, "std should not be less than date");
            if std < date {
                return None;
            }
            Some(std)
        } else {
            None
        }
    }

    /// Standard time of sunset on the date of the given moment and at the given location.
    /// Returns `None` if there is no such sunset.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3700-L3706>
    #[allow(dead_code)]
    pub fn sunset(date: Moment, location: Location) -> Option<Moment> {
        let alpha = Self::refraction(location) + (16.0 / 60.0);
        Self::dusk(date.inner(), location, alpha)
    }

    /// Time between sunset and moonset on the date of the given moment at the given location.
    /// Returns `None` if there is no such sunset.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L6770-L6778>
    pub fn moonlag(date: Moment, location: Location) -> Option<f64> {
        if let Some(sun) = Self::sunset(date, location) {
            if let Some(moon) = Self::moonset(date, location) {
                Some(moon - sun)
            } else {
                Some(1.0)
            }
        } else {
            None
        }
    }

    /// Longitudinal nutation (periodic variation in the inclination of the Earth's axis) at a given Moment.
    /// Argument comes from the result of calling `Self::julian_centuries(moment)` in an earlier function.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4037-L4047>
    fn nutation(julian_centuries: f64) -> f64 {
        let c = julian_centuries;
        let a = 124.90 - 1934.134 * c + 0.002063 * c * c;
        let b = 201.11 + 72001.5377 * c + 0.00057 * c * c;
        -0.004778 * a.to_radians().sin() - 0.0003667 * b.to_radians().sin()
    }

    /// The phase of the moon at a given Moment, defined as the difference in longitudes
    /// of the sun and the moon.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4397-L4414>
    pub fn lunar_phase(moment: Moment, julian_centuries: f64) -> f64 {
        let t0 = NEW_MOON_ZERO;
        let maybe_n = i64_to_i32(div_euclid_f64(moment - t0, MEAN_SYNODIC_MONTH).round() as i64);
        debug_assert!(
            maybe_n.is_ok(),
            "Lunar phase moment should be in range of i32"
        );
        let n = maybe_n.unwrap_or_else(|e| e.saturate());
        let a = (Self::lunar_longitude(julian_centuries) - Self::solar_longitude(julian_centuries))
            .rem_euclid(360.0);
        let b = 360.0 * ((moment - Self::nth_new_moon(n)) / MEAN_SYNODIC_MONTH).rem_euclid(1.0);
        if (a - b).abs() > 180.0 {
            b
        } else {
            a
        }
    }

    /// Moment in universal time of the last time at or before the given moment when the lunar phase
    /// was equal to the `phase` given.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4416-L4427>
    pub fn lunar_phase_at_or_before(phase: f64, moment: Moment) -> Moment {
        let julian_centuries = Self::julian_centuries(moment);
        let tau = moment.inner()
            - (MEAN_SYNODIC_MONTH / 360.0)
                * ((Self::lunar_phase(moment, julian_centuries) - phase) % 360.0);
        let a = tau - 2.0;
        let b = moment.inner().min(tau + 2.0);

        let lunar_phase_f64 = |x: f64| -> f64 {
            Self::lunar_phase(Moment::new(x), Self::julian_centuries(Moment::new(x)))
        };

        Moment::new(invert_angular(lunar_phase_f64, phase, (a, b)))
    }

    /// The longitude of the Sun at a given Moment in degrees.
    /// Moment is not directly used but is enconded from the argument `julian_centuries` which is the result of calling `Self::julian_centuries(moment) in an earlier function`.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz,
    /// originally from "Planetary Programs and Tables from -4000 to +2800" by Bretagnon & Simon, 1986.
    /// Reference code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3985-L4035>
    pub fn solar_longitude(julian_centuries: f64) -> f64 {
        let c: f64 = julian_centuries;
        let mut lt = (
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        );
        let [x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22, x23, x24, x25, x26, x27, x28, x29, x30, x31, x32, x33, x34, x35, x36, x37, x38, x39, x40, x41, x42, x43, x44, x45, x46, x47, x48] = [
            403406.0, 195207.0, 119433.0, 112392.0, 3891.0, 2819.0, 1721.0, 660.0, 350.0, 334.0,
            314.0, 268.0, 242.0, 234.0, 158.0, 132.0, 129.0, 114.0, 99.0, 93.0, 86.0, 78.0, 72.0,
            68.0, 64.0, 46.0, 38.0, 37.0, 32.0, 29.0, 28.0, 27.0, 27.0, 25.0, 24.0, 21.0, 21.0,
            20.0, 18.0, 17.0, 14.0, 13.0, 13.0, 13.0, 12.0, 10.0, 10.0, 10.0, 10.0,
        ];
        let [y0, y1, y2, y3, y4, y5, y6, y7, y8, y9, y10, y11, y12, y13, y14, y15, y16, y17, y18, y19, y20, y21, y22, y23, y24, y25, y26, y27, y28, y29, y30, y31, y32, y33, y34, y35, y36, y37, y38, y39, y40, y41, y42, y43, y44, y45, y46, y47, y48] = [
            270.54861, 340.19128, 63.91854, 331.26220, 317.843, 86.631, 240.052, 310.26, 247.23,
            260.87, 297.82, 343.14, 166.79, 81.53, 3.50, 132.75, 182.95, 162.03, 29.8, 266.4,
            249.2, 157.6, 257.8, 185.1, 69.9, 8.0, 197.1, 250.4, 65.3, 162.7, 341.5, 291.6, 98.5,
            146.7, 110.0, 5.2, 342.6, 230.9, 256.1, 45.3, 242.9, 115.2, 151.8, 285.3, 53.3, 126.6,
            205.7, 85.9, 146.1,
        ];
        let [z0, z1, z2, z3, z4, z5, z6, z7, z8, z9, z10, z11, z12, z13, z14, z15, z16, z17, z18, z19, z20, z21, z22, z23, z24, z25, z26, z27, z28, z29, z30, z31, z32, z33, z34, z35, z36, z37, z38, z39, z40, z41, z42, z43, z44, z45, z46, z47, z48] = [
            0.9287892,
            35999.1376958,
            35999.4089666,
            35998.7287385,
            71998.20261,
            71998.4403,
            36000.35726,
            71997.4812,
            32964.4678,
            -19.4410,
            445267.1117,
            45036.8840,
            3.1008,
            22518.4434,
            -19.9739,
            65928.9345,
            9038.0293,
            3034.7684,
            33718.148,
            3034.448,
            -2280.773,
            29929.992,
            31556.493,
            149.588,
            9037.750,
            107997.405,
            -4444.176,
            151.771,
            67555.316,
            31556.080,
            -4561.540,
            107996.706,
            1221.655,
            62894.167,
            31437.369,
            14578.298,
            -31931.757,
            34777.243,
            1221.999,
            62894.511,
            -4442.039,
            107997.909,
            119.066,
            16859.071,
            -4.578,
            26895.292,
            -39.127,
            12297.536,
            90073.778,
        ];

        lt.0 = x0 * (y0 + z0 * c).to_radians().sin();
        lt.1 = x1 * (y1 + z1 * c).to_radians().sin();
        lt.2 = x2 * (y2 + z2 * c).to_radians().sin();
        lt.3 = x3 * (y3 + z3 * c).to_radians().sin();
        lt.4 = x4 * (y4 + z4 * c).to_radians().sin();
        lt.5 = x5 * (y5 + z5 * c).to_radians().sin();
        lt.6 = x6 * (y6 + z6 * c).to_radians().sin();
        lt.7 = x7 * (y7 + z7 * c).to_radians().sin();
        lt.8 = x8 * (y8 + z8 * c).to_radians().sin();
        lt.9 = x9 * (y9 + z9 * c).to_radians().sin();
        lt.10 = x10 * (y10 + z10 * c).to_radians().sin();
        lt.11 = x11 * (y11 + z11 * c).to_radians().sin();
        lt.12 = x12 * (y12 + z12 * c).to_radians().sin();
        lt.13 = x13 * (y13 + z13 * c).to_radians().sin();
        lt.14 = x14 * (y14 + z14 * c).to_radians().sin();
        lt.15 = x15 * (y15 + z15 * c).to_radians().sin();
        lt.16 = x16 * (y16 + z16 * c).to_radians().sin();
        lt.17 = x17 * (y17 + z17 * c).to_radians().sin();
        lt.18 = x18 * (y18 + z18 * c).to_radians().sin();
        lt.19 = x19 * (y19 + z19 * c).to_radians().sin();
        lt.20 = x20 * (y20 + z20 * c).to_radians().sin();
        lt.21 = x21 * (y21 + z21 * c).to_radians().sin();
        lt.22 = x22 * (y22 + z22 * c).to_radians().sin();
        lt.23 = x23 * (y23 + z23 * c).to_radians().sin();
        lt.24 = x24 * (y24 + z24 * c).to_radians().sin();
        lt.25 = x25 * (y25 + z25 * c).to_radians().sin();
        lt.26 = x26 * (y26 + z26 * c).to_radians().sin();
        lt.27 = x27 * (y27 + z27 * c).to_radians().sin();
        lt.28 = x28 * (y28 + z28 * c).to_radians().sin();
        lt.29 = x29 * (y29 + z29 * c).to_radians().sin();
        lt.30 = x30 * (y30 + z30 * c).to_radians().sin();
        lt.31 = x31 * (y31 + z31 * c).to_radians().sin();
        lt.32 = x32 * (y32 + z32 * c).to_radians().sin();
        lt.33 = x33 * (y33 + z33 * c).to_radians().sin();
        lt.34 = x34 * (y34 + z34 * c).to_radians().sin();
        lt.35 = x35 * (y35 + z35 * c).to_radians().sin();
        lt.36 = x36 * (y36 + z36 * c).to_radians().sin();
        lt.37 = x37 * (y37 + z37 * c).to_radians().sin();
        lt.38 = x38 * (y38 + z38 * c).to_radians().sin();
        lt.39 = x39 * (y39 + z39 * c).to_radians().sin();
        lt.40 = x40 * (y40 + z40 * c).to_radians().sin();
        lt.41 = x41 * (y41 + z41 * c).to_radians().sin();
        lt.42 = x42 * (y42 + z42 * c).to_radians().sin();
        lt.43 = x43 * (y43 + z43 * c).to_radians().sin();
        lt.44 = x44 * (y44 + z44 * c).to_radians().sin();
        lt.45 = x45 * (y45 + z45 * c).to_radians().sin();
        lt.46 = x46 * (y46 + z46 * c).to_radians().sin();
        lt.47 = x47 * (y47 + z47 * c).to_radians().sin();
        lt.48 = x48 * (y48 + z48 * c).to_radians().sin();

        let mut lambda = lt.0
            + lt.1
            + lt.2
            + lt.3
            + lt.4
            + lt.5
            + lt.6
            + lt.7
            + lt.8
            + lt.9
            + lt.10
            + lt.11
            + lt.12
            + lt.13
            + lt.14
            + lt.15
            + lt.16
            + lt.17
            + lt.18
            + lt.19
            + lt.20
            + lt.21
            + lt.22
            + lt.23
            + lt.24
            + lt.25
            + lt.26
            + lt.27
            + lt.28
            + lt.29
            + lt.30
            + lt.31
            + lt.32
            + lt.33
            + lt.34
            + lt.35
            + lt.36
            + lt.37
            + lt.38
            + lt.39
            + lt.40
            + lt.41
            + lt.42
            + lt.43
            + lt.44
            + lt.45
            + lt.46
            + lt.47
            + lt.48;
        lambda *= 0.000005729577951308232;
        lambda += 282.7771834 + 36000.76953744 * c;
        (lambda + Self::aberration(c) + Self::nutation(julian_centuries)).rem_euclid(360.0)
    }

    /// The best viewing time (UT) in the evening for viewing the young moon from `location` on `date`. This is defined as
    /// the time when the sun is 4.5 degrees below the horizon, or `date + 1` if there is no such time.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L7337-L7346>
    fn simple_best_view(date: RataDie, location: Location) -> Moment {
        let dark = Self::dusk(date.to_f64_date(), location, 4.5);
        let best = dark.unwrap_or((date + 1).as_moment());

        Location::universal_from_standard(best, location)
    }

    /// Angular separation of the sun and moon at `moment`, for the purposes of determining the likely
    /// visibility of the crescent moon.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L7284-L7290>
    fn arc_of_light(moment: Moment) -> f64 {
        let julian_centuries = Self::julian_centuries(moment);
        (Self::lunar_latitude(julian_centuries).to_radians().cos()
            * Self::lunar_phase(moment, julian_centuries)
                .to_radians()
                .cos())
        .acos()
        .to_degrees()
    }

    /// Criterion for likely visibility of the crescent moon on the eve of `date` at `location`,
    /// not intended for high altitudes or polar regions, as defined by S.K. Shaukat.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Reference lisp code: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L7306-L7317>
    pub fn shaukat_criterion(date: Moment, location: Location) -> bool {
        let tee = Self::simple_best_view((date - 1.0).as_rata_die(), location);
        let phase = Self::lunar_phase(tee, Self::julian_centuries(tee));
        let h = Self::lunar_altitude(tee, location);
        let cap_arcl = Self::arc_of_light(tee);

        let new = 0.0;
        let first_quarter = 90.0;
        let deg_10_6 = 10.6;
        let deg_90 = 90.0;
        let deg_4_1 = 4.1;

        if phase > new
            && phase < first_quarter
            && cap_arcl >= deg_10_6
            && cap_arcl <= deg_90
            && h > deg_4_1
        {
            return true;
        }

        false
    }

    /// Criterion for possible visibility of crescent moon on the eve of `date` at `location`;
    /// currently, this calls `shaukat_criterion`, but this can be replaced with another implementation.
    pub fn visible_crescent(date: Moment, location: Location) -> bool {
        Self::shaukat_criterion(date, location)
    }

    /// Given an `angle` and a [`Moment`] `moment`, approximate the `Moment` at or before moment
    /// at which solar longitude exceeded the given angle.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4132-L4146>
    pub fn estimate_prior_solar_longitude(angle: f64, moment: Moment) -> Moment {
        let rate = MEAN_TROPICAL_YEAR / 360.0;
        let julian_centuries = Self::julian_centuries(moment);
        let tau =
            moment - rate * (Self::solar_longitude(julian_centuries) - angle).rem_euclid(360.0);
        let delta = (Self::solar_longitude(Self::julian_centuries(tau)) - angle + 180.0)
            .rem_euclid(360.0)
            - 180.0;
        let result_rhs = tau - rate * delta;
        if moment < result_rhs {
            moment
        } else {
            result_rhs
        }
    }

    /// Aberration at the time given in Julian centuries.
    /// See: https://sceweb.sce.uhcl.edu/helm/WEB-Positional%20Astronomy/Tutorial/Aberration/Aberration.html
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4049-L4057>
    fn aberration(c: f64) -> f64 {
        0.0000974 * (177.63 + 35999.01848 * c).to_radians().cos() - 0.005575
    }

    /// Find the time of the new moon preceding a given Moment (the last new moon before the moment)
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Most of the math performed in the equivalent book/lisp function is done in [`Self::num_of_new_moon_at_or_after`].
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4379-L4386>
    pub fn new_moon_before(moment: Moment) -> Moment {
        Self::nth_new_moon(Self::num_of_new_moon_at_or_after(moment) - 1)
    }

    /// Find the time of the new moon following a given Moment (the first new moon before the moment)
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Most of the math performed in the equivalent book/lisp function is done in [`Self::num_of_new_moon_at_or_after`].
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4388-L4395>
    pub fn new_moon_at_or_after(moment: Moment) -> Moment {
        Self::nth_new_moon(Self::num_of_new_moon_at_or_after(moment))
    }

    /// Function to find the number of the new moon at or after a given moment;
    /// helper function for `new_moon_before` and `new_moon_at_or_after`.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// This function incorporates code from the book/lisp equivalent functions
    /// of [`Self::new_moon_before`] and [`Self::new_moon_at_or_after`].
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L4379-L4395>
    pub fn num_of_new_moon_at_or_after(moment: Moment) -> i32 {
        let t0: Moment = NEW_MOON_ZERO;
        let phi = Self::lunar_phase(moment, Self::julian_centuries(moment));
        let maybe_n = i64_to_i32(
            (div_euclid_f64(moment - t0, MEAN_SYNODIC_MONTH) - phi / 360.0).round() as i64,
        );
        debug_assert!(maybe_n.is_ok(), "Num of new moon should be in range of i32");
        let n = maybe_n.unwrap_or_else(|e| e.saturate());
        let mut result = n;
        let mut iters = 0;
        let max_iters = 31;
        while iters < max_iters && Self::nth_new_moon(result) < moment {
            iters += 1;
            result += 1;
        }
        result
    }

    /// Sine of angle between the position of the sun at the given moment in local time and the moment
    /// at which the angle of depression of the sun from the given location is equal to `alpha`.
    ///
    /// Based on functions from _Calendrical Calculations_ by Reingold & Dershowitz.
    /// Lisp code reference: <https://github.com/EdReingold/calendar-code2/blob/9afc1f3/calendar.l#L3590-L3605>
    pub fn sine_offset(moment: Moment, location: Location, alpha: f64) -> f64 {
        let phi = location.latitude;
        let tee_prime = Location::universal_from_local(moment, location);
        let delta = Self::declination(
            tee_prime,
            0.0,
            Self::solar_longitude(Self::julian_centuries(tee_prime)),
        );

        phi.to_radians().tan() * delta.to_radians().tan()
            + alpha.to_radians().sin() / (delta.to_radians().cos() * phi.to_radians().cos())
    }
}
