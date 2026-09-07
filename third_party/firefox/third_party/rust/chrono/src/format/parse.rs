// Portions copyright (c) 2015, John Nagle.
// See README.md and LICENSE.txt for details.

//! Date and time parsing routines.

use core::borrow::Borrow;
use core::str;

use super::scan;
use super::{BAD_FORMAT, INVALID, OUT_OF_RANGE, TOO_LONG, TOO_SHORT};
use super::{Fixed, InternalFixed, InternalInternal, Item, Numeric, Pad, Parsed};
use super::{ParseError, ParseResult};
use crate::{DateTime, FixedOffset, Weekday};

fn set_weekday_with_num_days_from_sunday(p: &mut Parsed, v: i64) -> ParseResult<()> {
    p.set_weekday(match v {
        0 => Weekday::Sun,
        1 => Weekday::Mon,
        2 => Weekday::Tue,
        3 => Weekday::Wed,
        4 => Weekday::Thu,
        5 => Weekday::Fri,
        6 => Weekday::Sat,
        _ => return Err(OUT_OF_RANGE),
    })
}

fn set_weekday_with_number_from_monday(p: &mut Parsed, v: i64) -> ParseResult<()> {
    p.set_weekday(match v {
        1 => Weekday::Mon,
        2 => Weekday::Tue,
        3 => Weekday::Wed,
        4 => Weekday::Thu,
        5 => Weekday::Fri,
        6 => Weekday::Sat,
        7 => Weekday::Sun,
        _ => return Err(OUT_OF_RANGE),
    })
}

fn parse_rfc2822<'a>(parsed: &mut Parsed, mut s: &'a str) -> ParseResult<(&'a str, ())> {
    macro_rules! try_consume {
        ($e:expr) => {{
            let (s_, v) = $e?;
            s = s_;
            v
        }};
    }


    s = s.trim_start();

    if let Ok((s_, weekday)) = scan::short_weekday(s) {
        if !s_.starts_with(',') {
            return Err(INVALID);
        }
        s = &s_[1..];
        parsed.set_weekday(weekday)?;
    }

    s = s.trim_start();
    parsed.set_day(try_consume!(scan::number(s, 1, 2)))?;
    s = scan::space(s)?; 
    parsed.set_month(1 + i64::from(try_consume!(scan::short_month0(s))))?;
    s = scan::space(s)?; 

    let prevlen = s.len();
    let mut year = try_consume!(scan::number(s, 2, usize::MAX));
    let yearlen = prevlen - s.len();
    match (yearlen, year) {
        (2, 0..=49) => {
            year += 2000;
        } 
        (2, 50..=99) => {
            year += 1900;
        } 
        (3, _) => {
            year += 1900;
        } 
        (_, _) => {} 
    }
    parsed.set_year(year)?;

    s = scan::space(s)?; 
    parsed.set_hour(try_consume!(scan::number(s, 2, 2)))?;
    s = scan::char(s.trim_start(), b':')?.trim_start(); 
    parsed.set_minute(try_consume!(scan::number(s, 2, 2)))?;
    if let Ok(s_) = scan::char(s.trim_start(), b':') {
        parsed.set_second(try_consume!(scan::number(s_, 2, 2)))?;
    }

    s = scan::space(s)?; 
    parsed.set_offset(i64::from(try_consume!(scan::timezone_offset_2822(s))))?;

    while let Ok((s_out, ())) = scan::comment_2822(s) {
        s = s_out;
    }

    Ok((s, ()))
}

pub(crate) fn parse_rfc3339<'a>(parsed: &mut Parsed, mut s: &'a str) -> ParseResult<(&'a str, ())> {
    macro_rules! try_consume {
        ($e:expr) => {{
            let (s_, v) = $e?;
            s = s_;
            v
        }};
    }


    parsed.set_year(try_consume!(scan::number(s, 4, 4)))?;
    s = scan::char(s, b'-')?;
    parsed.set_month(try_consume!(scan::number(s, 2, 2)))?;
    s = scan::char(s, b'-')?;
    parsed.set_day(try_consume!(scan::number(s, 2, 2)))?;

    s = match s.as_bytes().first() {
        Some(&b't' | &b'T' | &b' ') => &s[1..],
        Some(_) => return Err(INVALID),
        None => return Err(TOO_SHORT),
    };

    parsed.set_hour(try_consume!(scan::number(s, 2, 2)))?;
    s = scan::char(s, b':')?;
    parsed.set_minute(try_consume!(scan::number(s, 2, 2)))?;
    s = scan::char(s, b':')?;
    parsed.set_second(try_consume!(scan::number(s, 2, 2)))?;
    if s.starts_with('.') {
        let nanosecond = try_consume!(scan::nanosecond(&s[1..]));
        parsed.set_nanosecond(nanosecond)?;
    }

    let offset = try_consume!(scan::timezone_offset(s, |s| scan::char(s, b':'), true, false, true));
    const MAX_RFC3339_OFFSET: i32 = (23 * 60 + 59) * 60;
    if !(-MAX_RFC3339_OFFSET..=MAX_RFC3339_OFFSET).contains(&offset) {
        return Err(OUT_OF_RANGE);
    }
    parsed.set_offset(i64::from(offset))?;

    Ok((s, ()))
}

/// Tries to parse given string into `parsed` with given formatting items.
/// Returns `Ok` when the entire string has been parsed (otherwise `parsed` should not be used).
/// There should be no trailing string after parsing;
/// use a stray [`Item::Space`](./enum.Item.html#variant.Space) to trim whitespaces.
///
/// This particular date and time parser is:
///
/// - Greedy. It will consume the longest possible prefix.
///   For example, `April` is always consumed entirely when the long month name is requested;
///   it equally accepts `Apr`, but prefers the longer prefix in this case.
///
/// - Padding-agnostic (for numeric items).
///   The [`Pad`](./enum.Pad.html) field is completely ignored,
///   so one can prepend any number of whitespace then any number of zeroes before numbers.
///
/// - (Still) obeying the intrinsic parsing width. This allows, for example, parsing `HHMMSS`.
pub fn parse<'a, I, B>(parsed: &mut Parsed, s: &str, items: I) -> ParseResult<()>
where
    I: Iterator<Item = B>,
    B: Borrow<Item<'a>>,
{
    match parse_internal(parsed, s, items) {
        Ok("") => Ok(()),
        Ok(_) => Err(TOO_LONG), 
        Err(e) => Err(e),
    }
}

/// Tries to parse given string into `parsed` with given formatting items.
/// Returns `Ok` with a slice of the unparsed remainder.
///
/// This particular date and time parser is:
///
/// - Greedy. It will consume the longest possible prefix.
///   For example, `April` is always consumed entirely when the long month name is requested;
///   it equally accepts `Apr`, but prefers the longer prefix in this case.
///
/// - Padding-agnostic (for numeric items).
///   The [`Pad`](./enum.Pad.html) field is completely ignored,
///   so one can prepend any number of zeroes before numbers.
///
/// - (Still) obeying the intrinsic parsing width. This allows, for example, parsing `HHMMSS`.
pub fn parse_and_remainder<'a, 'b, I, B>(
    parsed: &mut Parsed,
    s: &'b str,
    items: I,
) -> ParseResult<&'b str>
where
    I: Iterator<Item = B>,
    B: Borrow<Item<'a>>,
{
    parse_internal(parsed, s, items)
}

fn parse_internal<'a, 'b, I, B>(
    parsed: &mut Parsed,
    mut s: &'b str,
    items: I,
) -> Result<&'b str, ParseError>
where
    I: Iterator<Item = B>,
    B: Borrow<Item<'a>>,
{
    macro_rules! try_consume {
        ($e:expr) => {{
            match $e {
                Ok((s_, v)) => {
                    s = s_;
                    v
                }
                Err(e) => return Err(e),
            }
        }};
    }

    for item in items {
        match *item.borrow() {
            Item::Literal(prefix) => {
                if s.len() < prefix.len() {
                    return Err(TOO_SHORT);
                }
                if !s.starts_with(prefix) {
                    return Err(INVALID);
                }
                s = &s[prefix.len()..];
            }

            #[cfg(feature = "alloc")]
            Item::OwnedLiteral(ref prefix) => {
                if s.len() < prefix.len() {
                    return Err(TOO_SHORT);
                }
                if !s.starts_with(&prefix[..]) {
                    return Err(INVALID);
                }
                s = &s[prefix.len()..];
            }

            Item::Space(_) => {
                s = s.trim_start();
            }

            #[cfg(feature = "alloc")]
            Item::OwnedSpace(_) => {
                s = s.trim_start();
            }

            Item::Numeric(ref spec, ref _pad) => {
                use super::Numeric::*;
                type Setter = fn(&mut Parsed, i64) -> ParseResult<()>;

                let (width, signed, set): (usize, bool, Setter) = match *spec {
                    Year => (4, true, Parsed::set_year),
                    YearDiv100 => (2, false, Parsed::set_year_div_100),
                    YearMod100 => (2, false, Parsed::set_year_mod_100),
                    IsoYear => (4, true, Parsed::set_isoyear),
                    IsoYearDiv100 => (2, false, Parsed::set_isoyear_div_100),
                    IsoYearMod100 => (2, false, Parsed::set_isoyear_mod_100),
                    Quarter => (1, false, Parsed::set_quarter),
                    Month => (2, false, Parsed::set_month),
                    Day => (2, false, Parsed::set_day),
                    WeekFromSun => (2, false, Parsed::set_week_from_sun),
                    WeekFromMon => (2, false, Parsed::set_week_from_mon),
                    IsoWeek => (2, false, Parsed::set_isoweek),
                    NumDaysFromSun => (1, false, set_weekday_with_num_days_from_sunday),
                    WeekdayFromMon => (1, false, set_weekday_with_number_from_monday),
                    Ordinal => (3, false, Parsed::set_ordinal),
                    Hour => (2, false, Parsed::set_hour),
                    Hour12 => (2, false, Parsed::set_hour12),
                    Minute => (2, false, Parsed::set_minute),
                    Second => (2, false, Parsed::set_second),
                    Nanosecond => (9, false, Parsed::set_nanosecond),
                    Timestamp => (usize::MAX, false, Parsed::set_timestamp),

                    Internal(ref int) => match int._dummy {},
                };

                s = s.trim_start();
                let v = if signed {
                    if s.starts_with('-') {
                        let v = try_consume!(scan::number(&s[1..], 1, usize::MAX));
                        0i64.checked_sub(v).ok_or(OUT_OF_RANGE)?
                    } else if s.starts_with('+') {
                        try_consume!(scan::number(&s[1..], 1, usize::MAX))
                    } else {
                        try_consume!(scan::number(s, 1, width))
                    }
                } else {
                    try_consume!(scan::number(s, 1, width))
                };
                set(parsed, v)?;
            }

            Item::Fixed(ref spec) => {
                use super::Fixed::*;

                match spec {
                    &ShortMonthName => {
                        let month0 = try_consume!(scan::short_month0(s));
                        parsed.set_month(i64::from(month0) + 1)?;
                    }

                    &LongMonthName => {
                        let month0 = try_consume!(scan::short_or_long_month0(s));
                        parsed.set_month(i64::from(month0) + 1)?;
                    }

                    &ShortWeekdayName => {
                        let weekday = try_consume!(scan::short_weekday(s));
                        parsed.set_weekday(weekday)?;
                    }

                    &LongWeekdayName => {
                        let weekday = try_consume!(scan::short_or_long_weekday(s));
                        parsed.set_weekday(weekday)?;
                    }

                    &LowerAmPm | &UpperAmPm => {
                        if s.len() < 2 {
                            return Err(TOO_SHORT);
                        }
                        let ampm = match (s.as_bytes()[0] | 32, s.as_bytes()[1] | 32) {
                            (b'a', b'm') => false,
                            (b'p', b'm') => true,
                            _ => return Err(INVALID),
                        };
                        parsed.set_ampm(ampm)?;
                        s = &s[2..];
                    }

                    &Nanosecond => {
                        if s.starts_with('.') {
                            let nano = try_consume!(scan::nanosecond(&s[1..]));
                            parsed.set_nanosecond(nano)?;
                        }
                    }

                    &Nanosecond3 => {
                        if s.starts_with('.') {
                            let nano = try_consume!(scan::nanosecond_fixed(&s[1..], 3));
                            parsed.set_nanosecond(nano)?;
                        }
                    }

                    &Nanosecond6 => {
                        if s.starts_with('.') {
                            let nano = try_consume!(scan::nanosecond_fixed(&s[1..], 6));
                            parsed.set_nanosecond(nano)?;
                        }
                    }

                    &Nanosecond9 => {
                        if s.starts_with('.') {
                            let nano = try_consume!(scan::nanosecond_fixed(&s[1..], 9));
                            parsed.set_nanosecond(nano)?;
                        }
                    }

                    &Internal(InternalFixed { val: InternalInternal::Nanosecond3NoDot }) => {
                        if s.len() < 3 {
                            return Err(TOO_SHORT);
                        }
                        let nano = try_consume!(scan::nanosecond_fixed(s, 3));
                        parsed.set_nanosecond(nano)?;
                    }

                    &Internal(InternalFixed { val: InternalInternal::Nanosecond6NoDot }) => {
                        if s.len() < 6 {
                            return Err(TOO_SHORT);
                        }
                        let nano = try_consume!(scan::nanosecond_fixed(s, 6));
                        parsed.set_nanosecond(nano)?;
                    }

                    &Internal(InternalFixed { val: InternalInternal::Nanosecond9NoDot }) => {
                        if s.len() < 9 {
                            return Err(TOO_SHORT);
                        }
                        let nano = try_consume!(scan::nanosecond_fixed(s, 9));
                        parsed.set_nanosecond(nano)?;
                    }

                    &TimezoneName => {
                        try_consume!(Ok((s.trim_start_matches(|c: char| !c.is_whitespace()), ())));
                    }

                    &TimezoneOffsetColon
                    | &TimezoneOffsetDoubleColon
                    | &TimezoneOffsetTripleColon
                    | &TimezoneOffset => {
                        let offset = try_consume!(scan::timezone_offset(
                            s.trim_start(),
                            scan::colon_or_space,
                            false,
                            false,
                            true,
                        ));
                        parsed.set_offset(i64::from(offset))?;
                    }

                    &TimezoneOffsetColonZ | &TimezoneOffsetZ => {
                        let offset = try_consume!(scan::timezone_offset(
                            s.trim_start(),
                            scan::colon_or_space,
                            true,
                            false,
                            true,
                        ));
                        parsed.set_offset(i64::from(offset))?;
                    }
                    &Internal(InternalFixed {
                        val: InternalInternal::TimezoneOffsetPermissive,
                    }) => {
                        let offset = try_consume!(scan::timezone_offset(
                            s.trim_start(),
                            scan::colon_or_space,
                            true,
                            true,
                            true,
                        ));
                        parsed.set_offset(i64::from(offset))?;
                    }

                    &RFC2822 => try_consume!(parse_rfc2822(parsed, s)),
                    &RFC3339 => {
                        try_consume!(parse_rfc3339_relaxed(parsed, s))
                    }
                }
            }

            Item::Error => {
                return Err(BAD_FORMAT);
            }
        }
    }
    Ok(s)
}

/// Accepts a relaxed form of RFC3339.
/// A space or a 'T' are accepted as the separator between the date and time
/// parts. Additional spaces are allowed between each component.
///
/// All of these examples are equivalent:
/// ```
/// # use chrono::{DateTime, offset::FixedOffset};
/// "2012-12-12T12:12:12Z".parse::<DateTime<FixedOffset>>()?;
/// "2012-12-12 12:12:12Z".parse::<DateTime<FixedOffset>>()?;
/// "2012-  12-12T12:  12:12Z".parse::<DateTime<FixedOffset>>()?;
/// # Ok::<(), chrono::ParseError>(())
/// ```
impl str::FromStr for DateTime<FixedOffset> {
    type Err = ParseError;

    fn from_str(s: &str) -> ParseResult<DateTime<FixedOffset>> {
        let mut parsed = Parsed::new();
        let (s, _) = parse_rfc3339_relaxed(&mut parsed, s)?;
        if !s.trim_start().is_empty() {
            return Err(TOO_LONG);
        }
        parsed.to_datetime()
    }
}

/// Accepts a relaxed form of RFC3339.
///
/// Differences with RFC3339:
/// - Values don't require padding to two digits.
/// - Years outside the range 0...=9999 are accepted, but they must include a sign.
/// - `UTC` is accepted as a valid timezone name/offset (for compatibility with the debug format of
///   `DateTime<Utc>`.
/// - There can be spaces between any of the components.
/// - The colon in the offset may be missing.
fn parse_rfc3339_relaxed<'a>(parsed: &mut Parsed, mut s: &'a str) -> ParseResult<(&'a str, ())> {
    const DATE_ITEMS: &[Item<'static>] = &[
        Item::Numeric(Numeric::Year, Pad::Zero),
        Item::Space(""),
        Item::Literal("-"),
        Item::Numeric(Numeric::Month, Pad::Zero),
        Item::Space(""),
        Item::Literal("-"),
        Item::Numeric(Numeric::Day, Pad::Zero),
    ];
    const TIME_ITEMS: &[Item<'static>] = &[
        Item::Numeric(Numeric::Hour, Pad::Zero),
        Item::Space(""),
        Item::Literal(":"),
        Item::Numeric(Numeric::Minute, Pad::Zero),
        Item::Space(""),
        Item::Literal(":"),
        Item::Numeric(Numeric::Second, Pad::Zero),
        Item::Fixed(Fixed::Nanosecond),
        Item::Space(""),
    ];

    s = parse_internal(parsed, s, DATE_ITEMS.iter())?;

    s = match s.as_bytes().first() {
        Some(&b't' | &b'T' | &b' ') => &s[1..],
        Some(_) => return Err(INVALID),
        None => return Err(TOO_SHORT),
    };

    s = parse_internal(parsed, s, TIME_ITEMS.iter())?;
    s = s.trim_start();
    let (s, offset) = if s.len() >= 3 && "UTC".as_bytes().eq_ignore_ascii_case(&s.as_bytes()[..3]) {
        (&s[3..], 0)
    } else {
        scan::timezone_offset(s, scan::colon_or_space, true, false, true)?
    };
    parsed.set_offset(i64::from(offset))?;
    Ok((s, ()))
}
