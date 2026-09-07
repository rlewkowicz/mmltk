// See README.md and LICENSE.txt for details.

/*!
`strftime`/`strptime`-inspired date and time formatting syntax.

## Specifiers

The following specifiers are available both to formatting and parsing.

| Spec. | Example  | Description                                                                |
|-------|----------|----------------------------------------------------------------------------|
|       |          | **DATE SPECIFIERS:**                                                       |
| `%Y`  | `2001`   | The full proleptic Gregorian year, zero-padded to 4 digits. chrono supports years from -262144 to 262143. Note: years before 1 BCE or after 9999 CE, require an initial sign (+/-).|
| `%C`  | `20`     | The proleptic Gregorian year divided by 100, zero-padded to 2 digits. [^1] |
| `%y`  | `01`     | The proleptic Gregorian year modulo 100, zero-padded to 2 digits. [^1]     |
|       |          |                                                                            |
| `%q`  | `1`      | Quarter of year (1-4)                                                      |
| `%m`  | `07`     | Month number (01--12), zero-padded to 2 digits.                            |
| `%b`  | `Jul`    | Abbreviated month name. Always 3 letters.                                  |
| `%B`  | `July`   | Full month name. Also accepts corresponding abbreviation in parsing.       |
| `%h`  | `Jul`    | Same as `%b`.                                                              |
|       |          |                                                                            |
| `%d`  | `08`     | Day number (01--31), zero-padded to 2 digits.                              |
| `%e`  | ` 8`     | Same as `%d` but space-padded. Same as `%_d`.                              |
|       |          |                                                                            |
| `%a`  | `Sun`    | Abbreviated weekday name. Always 3 letters.                                |
| `%A`  | `Sunday` | Full weekday name. Also accepts corresponding abbreviation in parsing.     |
| `%w`  | `0`      | Sunday = 0, Monday = 1, ..., Saturday = 6.                                 |
| `%u`  | `7`      | Monday = 1, Tuesday = 2, ..., Sunday = 7. (ISO 8601)                       |
|       |          |                                                                            |
| `%U`  | `28`     | Week number starting with Sunday (00--53), zero-padded to 2 digits. [^2]   |
| `%W`  | `27`     | Same as `%U`, but week 1 starts with the first Monday in that year instead.|
|       |          |                                                                            |
| `%G`  | `2001`   | Same as `%Y` but uses the year number in ISO 8601 week date. [^3]          |
| `%g`  | `01`     | Same as `%y` but uses the year number in ISO 8601 week date. [^3]          |
| `%V`  | `27`     | Same as `%U` but uses the week number in ISO 8601 week date (01--53). [^3] |
|       |          |                                                                            |
| `%j`  | `189`    | Day of the year (001--366), zero-padded to 3 digits.                       |
|       |          |                                                                            |
| `%D`  | `07/08/01`    | Month-day-year format. Same as `%m/%d/%y`.                            |
| `%x`  | `07/08/01`    | Locale's date representation (e.g., 12/31/99).                        |
| `%F`  | `2001-07-08`  | Year-month-day format (ISO 8601). Same as `%Y-%m-%d`.                 |
| `%v`  | ` 8-Jul-2001` | Day-month-year format. Same as `%e-%b-%Y`.                            |
|       |          |                                                                            |
|       |          | **TIME SPECIFIERS:**                                                       |
| `%H`  | `00`     | Hour number (00--23), zero-padded to 2 digits.                             |
| `%k`  | ` 0`     | Same as `%H` but space-padded. Same as `%_H`.                              |
| `%I`  | `12`     | Hour number in 12-hour clocks (01--12), zero-padded to 2 digits.           |
| `%l`  | `12`     | Same as `%I` but space-padded. Same as `%_I`.                              |
|       |          |                                                                            |
| `%P`  | `am`     | `am` or `pm` in 12-hour clocks.                                            |
| `%p`  | `AM`     | `AM` or `PM` in 12-hour clocks.                                            |
|       |          |                                                                            |
| `%M`  | `34`     | Minute number (00--59), zero-padded to 2 digits.                           |
| `%S`  | `60`     | Second number (00--60), zero-padded to 2 digits. [^4]                      |
| `%f`  | `26490000`    | Number of nanoseconds since last whole second. [^7]                   |
| `%.f` | `.026490`| Decimal fraction of a second. Consumes the leading dot. [^7]               |
| `%.3f`| `.026`        | Decimal fraction of a second with a fixed length of 3.                |
| `%.6f`| `.026490`     | Decimal fraction of a second with a fixed length of 6.                |
| `%.9f`| `.026490000`  | Decimal fraction of a second with a fixed length of 9.                |
| `%3f` | `026`         | Decimal fraction of a second like `%.3f` but without the leading dot. |
| `%6f` | `026490`      | Decimal fraction of a second like `%.6f` but without the leading dot. |
| `%9f` | `026490000`   | Decimal fraction of a second like `%.9f` but without the leading dot. |
|       |               |                                                                       |
| `%R`  | `00:34`       | Hour-minute format. Same as `%H:%M`.                                  |
| `%T`  | `00:34:60`    | Hour-minute-second format. Same as `%H:%M:%S`.                        |
| `%X`  | `00:34:60`    | Locale's time representation (e.g., 23:13:48).                        |
| `%r`  | `12:34:60 AM` | Locale's 12 hour clock time. (e.g., 11:11:04 PM). Falls back to `%X` if the locale does not have a 12 hour clock format. |
|       |          |                                                                            |
|       |          | **TIME ZONE SPECIFIERS:**                                                  |
| `%Z`  | `ACST`   | Local time zone name. Skips all non-whitespace characters during parsing. Identical to `%:z` when formatting. [^8] |
| `%z`  | `+0930`  | Offset from the local time to UTC (with UTC being `+0000`).                |
| `%:z` | `+09:30` | Same as `%z` but with a colon.                                             |
|`%::z`|`+09:30:00`| Offset from the local time to UTC with seconds.                            |
|`%:::z`| `+09`    | Offset from the local time to UTC without minutes.                         |
| `%#z` | `+09`    | *Parsing only:* Same as `%z` but allows minutes to be missing or present.  |
|       |          |                                                                            |
|       |          | **DATE & TIME SPECIFIERS:**                                                |
|`%c`|`Sun Jul  8 00:34:60 2001`|Locale's date and time (e.g., Thu Mar  3 23:05:25 2005).       |
| `%+`  | `2001-07-08T00:34:60.026490+09:30` | ISO 8601 / RFC 3339 date & time format. [^5]     |
|       |               |                                                                       |
| `%s`  | `994518299`   | UNIX timestamp, the number of seconds since 1970-01-01 00:00 UTC. [^6]|
|       |          |                                                                            |
|       |          | **SPECIAL SPECIFIERS:**                                                    |
| `%t`  |          | Literal tab (`\t`).                                                        |
| `%n`  |          | Literal newline (`\n`).                                                    |
| `%%`  |          | Literal percent sign.                                                      |

It is possible to override the default padding behavior of numeric specifiers `%?`.
This is not allowed for other specifiers and will result in the `BAD_FORMAT` error.

Modifier | Description
-------- | -----------
`%-?`    | Suppresses any padding including spaces and zeroes. (e.g. `%j` = `012`, `%-j` = `12`)
`%_?`    | Uses spaces as a padding. (e.g. `%j` = `012`, `%_j` = ` 12`)
`%0?`    | Uses zeroes as a padding. (e.g. `%e` = ` 9`, `%0e` = `09`)

Notes:

[^1]: `%C`, `%y`:
   This is floor division, so 100 BCE (year number -99) will print `-1` and `99` respectively.
   For `%y`, values greater or equal to 70 are interpreted as being in the 20th century,
   values smaller than 70 in the 21st century.

[^2]: `%U`:
   Week 1 starts with the first Sunday in that year.
   It is possible to have week 0 for days before the first Sunday.

[^3]: `%G`, `%g`, `%V`:
   Week 1 is the first week with at least 4 days in that year.
   Week 0 does not exist, so this should be used with `%G` or `%g`.

[^4]: `%S`:
   It accounts for leap seconds, so `60` is possible.

[^5]: `%+`: Same as `%Y-%m-%dT%H:%M:%S%.f%:z`, i.e. 0, 3, 6 or 9 fractional
   digits for seconds and colons in the time zone offset.
   <br>
   <br>
   This format also supports having a `Z` or `UTC` in place of `%:z`. They
   are equivalent to `+00:00`.
   <br>
   <br>
   Note that all `T`, `Z`, and `UTC` are parsed case-insensitively.
   <br>
   <br>
   The typical `strftime` implementations have different (and locale-dependent)
   formats for this specifier. While Chrono's format for `%+` is far more
   stable, it is best to avoid this specifier if you want to control the exact
   output.

[^6]: `%s`:
   This is not padded and can be negative.
   For the purpose of Chrono, it only accounts for non-leap seconds
   so it slightly differs from ISO C `strftime` behavior.

[^7]: `%f`, `%.f`:
   <br>
   `%f` and `%.f` are notably different formatting specifiers.<br>
   `%f` counts the number of nanoseconds since the last whole second, while `%.f` is a fraction of a
   second.<br>
   Example: 7μs is formatted as `7000` with `%f`, and formatted as `.000007` with `%.f`.

[^8]: `%Z`:
   Since `chrono` is not aware of timezones beyond their offsets, this specifier
   **only prints the offset** when used for formatting. The timezone abbreviation
   will NOT be printed. See [this issue](https://github.com/chronotope/chrono/issues/960)
   for more information.
   <br>
   <br>
   Offset will not be populated from the parsed data, nor will it be validated.
   Timezone is completely ignored. Similar to the glibc `strptime` treatment of
   this format code.
   <br>
   <br>
   It is not possible to reliably convert from an abbreviation to an offset,
   for example CDT can mean either Central Daylight Time (North America) or
   China Daylight Time.
*/

#[cfg(feature = "alloc")]
extern crate alloc;

#[cfg(any(feature = "alloc", feature = "std"))]
use super::{BAD_FORMAT, ParseError};
use super::{Fixed, InternalInternal, Item, Numeric, Pad};
#[cfg(feature = "unstable-locales")]
use super::{Locale, locales};
use super::{fixed, internal_fixed, num, num0, nums};
#[cfg(all(feature = "alloc", not(feature = "std")))]
use alloc::vec::Vec;

/// Parsing iterator for `strftime`-like format strings.
///
/// See the [`format::strftime` module](crate::format::strftime) for supported formatting
/// specifiers.
///
/// `StrftimeItems` is used in combination with more low-level methods such as [`format::parse()`]
/// or [`format_with_items`].
///
/// If formatting or parsing date and time values is not performance-critical, the methods
/// [`parse_from_str`] and [`format`] on types such as [`DateTime`](crate::DateTime) are easier to
/// use.
///
/// [`format`]: crate::DateTime::format
/// [`format_with_items`]: crate::DateTime::format
/// [`parse_from_str`]: crate::DateTime::parse_from_str
/// [`DateTime`]: crate::DateTime
/// [`format::parse()`]: crate::format::parse()
#[derive(Clone, Debug)]
pub struct StrftimeItems<'a> {
    /// Remaining portion of the string.
    remainder: &'a str,
    /// If the current specifier is composed of multiple formatting items (e.g. `%+`),
    /// `queue` stores a slice of `Item`s that have to be returned one by one.
    queue: &'static [Item<'static>],
    lenient: bool,
    #[cfg(feature = "unstable-locales")]
    locale_str: &'a str,
    #[cfg(feature = "unstable-locales")]
    locale: Option<Locale>,
}

impl<'a> StrftimeItems<'a> {
    /// Creates a new parsing iterator from a `strftime`-like format string.
    ///
    /// # Errors
    ///
    /// While iterating [`Item::Error`] will be returned if the format string contains an invalid
    /// or unrecognized formatting specifier.
    ///
    /// # Example
    ///
    /// ```
    /// use chrono::format::*;
    ///
    /// let strftime_parser = StrftimeItems::new("%F"); // %F: year-month-day (ISO 8601)
    ///
    /// const ISO8601_YMD_ITEMS: &[Item<'static>] = &[
    ///     Item::Numeric(Numeric::Year, Pad::Zero),
    ///     Item::Literal("-"),
    ///     Item::Numeric(Numeric::Month, Pad::Zero),
    ///     Item::Literal("-"),
    ///     Item::Numeric(Numeric::Day, Pad::Zero),
    /// ];
    /// assert!(strftime_parser.eq(ISO8601_YMD_ITEMS.iter().cloned()));
    /// ```
    #[must_use]
    pub const fn new(s: &'a str) -> StrftimeItems<'a> {
        StrftimeItems {
            remainder: s,
            queue: &[],
            lenient: false,
            #[cfg(feature = "unstable-locales")]
            locale_str: "",
            #[cfg(feature = "unstable-locales")]
            locale: None,
        }
    }

    /// The same as [`StrftimeItems::new`], but returns [`Item::Literal`] instead of [`Item::Error`].
    ///
    /// Useful for formatting according to potentially invalid format strings.
    ///
    /// # Example
    ///
    /// ```
    /// use chrono::format::*;
    ///
    /// let strftime_parser = StrftimeItems::new_lenient("%Y-%Q"); // %Y: year, %Q: invalid
    ///
    /// const ITEMS: &[Item<'static>] = &[
    ///     Item::Numeric(Numeric::Year, Pad::Zero),
    ///     Item::Literal("-"),
    ///     Item::Literal("%Q"),
    /// ];
    /// println!("{:?}", strftime_parser.clone().collect::<Vec<_>>());
    /// assert!(strftime_parser.eq(ITEMS.iter().cloned()));
    /// ```
    #[must_use]
    pub const fn new_lenient(s: &'a str) -> StrftimeItems<'a> {
        StrftimeItems {
            remainder: s,
            queue: &[],
            lenient: true,
            #[cfg(feature = "unstable-locales")]
            locale_str: "",
            #[cfg(feature = "unstable-locales")]
            locale: None,
        }
    }

    /// Creates a new parsing iterator from a `strftime`-like format string, with some formatting
    /// specifiers adjusted to match [`Locale`].
    ///
    /// Note: `StrftimeItems::new_with_locale` only localizes the *format*. You usually want to
    /// combine it with other locale-aware methods such as
    /// [`DateTime::format_localized_with_items`] to get things like localized month or day names.
    ///
    /// The `%x` formatting specifier will use the local date format, `%X` the local time format,
    ///  and `%c` the local format for date and time.
    /// `%r` will use the local 12-hour clock format (e.g., 11:11:04 PM). Not all locales have such
    /// a format, in which case we fall back to a 24-hour clock (`%X`).
    ///
    /// See the [`format::strftime` module](crate::format::strftime) for all supported formatting
    /// specifiers.
    ///
    ///  [`DateTime::format_localized_with_items`]: crate::DateTime::format_localized_with_items
    ///
    /// # Errors
    ///
    /// While iterating [`Item::Error`] will be returned if the format string contains an invalid
    /// or unrecognized formatting specifier.
    ///
    /// # Example
    ///
    /// ```
    /// # #[cfg(feature = "alloc")] {
    /// use chrono::format::{Locale, StrftimeItems};
    /// use chrono::{FixedOffset, TimeZone};
    ///
    /// let dt = FixedOffset::east_opt(9 * 60 * 60)
    ///     .unwrap()
    ///     .with_ymd_and_hms(2023, 7, 11, 0, 34, 59)
    ///     .unwrap();
    ///
    /// // Note: you usually want to combine `StrftimeItems::new_with_locale` with other
    /// // locale-aware methods such as `DateTime::format_localized_with_items`.
    /// // We use the regular `format_with_items` to show only how the formatting changes.
    ///
    /// let fmtr = dt.format_with_items(StrftimeItems::new_with_locale("%x", Locale::en_US));
    /// assert_eq!(fmtr.to_string(), "07/11/2023");
    /// let fmtr = dt.format_with_items(StrftimeItems::new_with_locale("%x", Locale::ko_KR));
    /// assert_eq!(fmtr.to_string(), "2023년 07월 11일");
    /// let fmtr = dt.format_with_items(StrftimeItems::new_with_locale("%x", Locale::ja_JP));
    /// assert_eq!(fmtr.to_string(), "2023年07月11日");
    /// # }
    /// ```
    #[cfg(feature = "unstable-locales")]
    #[must_use]
    pub const fn new_with_locale(s: &'a str, locale: Locale) -> StrftimeItems<'a> {
        StrftimeItems {
            remainder: s,
            queue: &[],
            lenient: false,
            locale_str: "",
            locale: Some(locale),
        }
    }

    /// Parse format string into a `Vec` of formatting [`Item`]'s.
    ///
    /// If you need to format or parse multiple values with the same format string, it is more
    /// efficient to convert it to a `Vec` of formatting [`Item`]'s than to re-parse the format
    /// string on every use.
    ///
    /// The `format_with_items` methods on [`DateTime`], [`NaiveDateTime`], [`NaiveDate`] and
    /// [`NaiveTime`] accept the result for formatting. [`format::parse()`] can make use of it for
    /// parsing.
    ///
    /// [`DateTime`]: crate::DateTime::format_with_items
    /// [`NaiveDateTime`]: crate::NaiveDateTime::format_with_items
    /// [`NaiveDate`]: crate::NaiveDate::format_with_items
    /// [`NaiveTime`]: crate::NaiveTime::format_with_items
    /// [`format::parse()`]: crate::format::parse()
    ///
    /// # Errors
    ///
    /// Returns an error if the format string contains an invalid or unrecognized formatting
    /// specifier and the [`StrftimeItems`] wasn't constructed with [`new_lenient`][Self::new_lenient].
    ///
    /// # Example
    ///
    /// ```
    /// use chrono::format::{parse, Parsed, StrftimeItems};
    /// use chrono::NaiveDate;
    ///
    /// let fmt_items = StrftimeItems::new("%e %b %Y %k.%M").parse()?;
    /// let datetime = NaiveDate::from_ymd_opt(2023, 7, 11).unwrap().and_hms_opt(9, 0, 0).unwrap();
    ///
    /// // Formatting
    /// assert_eq!(
    ///     datetime.format_with_items(fmt_items.as_slice().iter()).to_string(),
    ///     "11 Jul 2023  9.00"
    /// );
    ///
    /// // Parsing
    /// let mut parsed = Parsed::new();
    /// parse(&mut parsed, "11 Jul 2023  9.00", fmt_items.as_slice().iter())?;
    /// let parsed_dt = parsed.to_naive_datetime_with_offset(0)?;
    /// assert_eq!(parsed_dt, datetime);
    /// # Ok::<(), chrono::ParseError>(())
    /// ```
    #[cfg(any(feature = "alloc", feature = "std"))]
    pub fn parse(self) -> Result<Vec<Item<'a>>, ParseError> {
        self.into_iter()
            .map(|item| match item == Item::Error {
                false => Ok(item),
                true => Err(BAD_FORMAT),
            })
            .collect()
    }

    /// Parse format string into a `Vec` of [`Item`]'s that contain no references to slices of the
    /// format string.
    ///
    /// A `Vec` created with [`StrftimeItems::parse`] contains references to the format string,
    /// binding the lifetime of the `Vec` to that string. [`StrftimeItems::parse_to_owned`] will
    /// convert the references to owned types.
    ///
    /// # Errors
    ///
    /// Returns an error if the format string contains an invalid or unrecognized formatting
    /// specifier and the [`StrftimeItems`] wasn't constructed with [`new_lenient`][Self::new_lenient].
    ///
    /// # Example
    ///
    /// ```
    /// use chrono::format::{Item, ParseError, StrftimeItems};
    /// use chrono::NaiveDate;
    ///
    /// fn format_items(date_fmt: &str, time_fmt: &str) -> Result<Vec<Item<'static>>, ParseError> {
    ///     // `fmt_string` is dropped at the end of this function.
    ///     let fmt_string = format!("{} {}", date_fmt, time_fmt);
    ///     StrftimeItems::new(&fmt_string).parse_to_owned()
    /// }
    ///
    /// let fmt_items = format_items("%e %b %Y", "%k.%M")?;
    /// let datetime = NaiveDate::from_ymd_opt(2023, 7, 11).unwrap().and_hms_opt(9, 0, 0).unwrap();
    ///
    /// assert_eq!(
    ///     datetime.format_with_items(fmt_items.as_slice().iter()).to_string(),
    ///     "11 Jul 2023  9.00"
    /// );
    /// # Ok::<(), ParseError>(())
    /// ```
    #[cfg(any(feature = "alloc", feature = "std"))]
    pub fn parse_to_owned(self) -> Result<Vec<Item<'static>>, ParseError> {
        self.into_iter()
            .map(|item| match item == Item::Error {
                false => Ok(item.to_owned()),
                true => Err(BAD_FORMAT),
            })
            .collect()
    }

    fn parse_next_item(&mut self, mut remainder: &'a str) -> Option<(&'a str, Item<'a>)> {
        use InternalInternal::*;
        use Item::{Literal, Space};
        use Numeric::*;

        let (original, mut remainder) = match remainder.chars().next()? {
            '%' => (remainder, &remainder[1..]),

            c if c.is_whitespace() => {
                let nextspec =
                    remainder.find(|c: char| !c.is_whitespace()).unwrap_or(remainder.len());
                assert!(nextspec > 0);
                let item = Space(&remainder[..nextspec]);
                remainder = &remainder[nextspec..];
                return Some((remainder, item));
            }

            _ => {
                let nextspec = remainder
                    .find(|c: char| c.is_whitespace() || c == '%')
                    .unwrap_or(remainder.len());
                assert!(nextspec > 0);
                let item = Literal(&remainder[..nextspec]);
                remainder = &remainder[nextspec..];
                return Some((remainder, item));
            }
        };

        macro_rules! next {
            () => {
                match remainder.chars().next() {
                    Some(x) => {
                        remainder = &remainder[x.len_utf8()..];
                        x
                    }
                    None => return Some((remainder, self.error(original, remainder))), 
                }
            };
        }

        let spec = next!();
        let pad_override = match spec {
            '-' => Some(Pad::None),
            '0' => Some(Pad::Zero),
            '_' => Some(Pad::Space),
            _ => None,
        };

        let is_alternate = spec == '#';
        let spec = if pad_override.is_some() || is_alternate { next!() } else { spec };
        if is_alternate && !HAVE_ALTERNATES.contains(spec) {
            return Some((remainder, self.error(original, remainder)));
        }

        macro_rules! queue {
            [$head:expr, $($tail:expr),+ $(,)*] => ({
                const QUEUE: &'static [Item<'static>] = &[$($tail),+];
                self.queue = QUEUE;
                $head
            })
        }

        #[cfg(not(feature = "unstable-locales"))]
        macro_rules! queue_from_slice {
            ($slice:expr) => {{
                self.queue = &$slice[1..];
                $slice[0].clone()
            }};
        }

        let item = match spec {
            'A' => fixed(Fixed::LongWeekdayName),
            'B' => fixed(Fixed::LongMonthName),
            'C' => num0(YearDiv100),
            'D' => {
                queue![num0(Month), Literal("/"), num0(Day), Literal("/"), num0(YearMod100)]
            }
            'F' => queue![num0(Year), Literal("-"), num0(Month), Literal("-"), num0(Day)],
            'G' => num0(IsoYear),
            'H' => num0(Hour),
            'I' => num0(Hour12),
            'M' => num0(Minute),
            'P' => fixed(Fixed::LowerAmPm),
            'R' => queue![num0(Hour), Literal(":"), num0(Minute)],
            'S' => num0(Second),
            'T' => {
                queue![num0(Hour), Literal(":"), num0(Minute), Literal(":"), num0(Second)]
            }
            'U' => num0(WeekFromSun),
            'V' => num0(IsoWeek),
            'W' => num0(WeekFromMon),
            #[cfg(not(feature = "unstable-locales"))]
            'X' => queue_from_slice!(T_FMT),
            #[cfg(feature = "unstable-locales")]
            'X' => self.switch_to_locale_str(locales::t_fmt, T_FMT),
            'Y' => num0(Year),
            'Z' => fixed(Fixed::TimezoneName),
            'a' => fixed(Fixed::ShortWeekdayName),
            'b' | 'h' => fixed(Fixed::ShortMonthName),
            #[cfg(not(feature = "unstable-locales"))]
            'c' => queue_from_slice!(D_T_FMT),
            #[cfg(feature = "unstable-locales")]
            'c' => self.switch_to_locale_str(locales::d_t_fmt, D_T_FMT),
            'd' => num0(Day),
            'e' => nums(Day),
            'f' => num0(Nanosecond),
            'g' => num0(IsoYearMod100),
            'j' => num0(Ordinal),
            'k' => nums(Hour),
            'l' => nums(Hour12),
            'm' => num0(Month),
            'n' => Space("\n"),
            'p' => fixed(Fixed::UpperAmPm),
            'q' => num(Quarter),
            #[cfg(not(feature = "unstable-locales"))]
            'r' => queue_from_slice!(T_FMT_AMPM),
            #[cfg(feature = "unstable-locales")]
            'r' => {
                if self.locale.is_some() && locales::t_fmt_ampm(self.locale.unwrap()).is_empty() {
                    self.switch_to_locale_str(locales::t_fmt, T_FMT)
                } else {
                    self.switch_to_locale_str(locales::t_fmt_ampm, T_FMT_AMPM)
                }
            }
            's' => num(Timestamp),
            't' => Space("\t"),
            'u' => num(WeekdayFromMon),
            'v' => {
                queue![
                    nums(Day),
                    Literal("-"),
                    fixed(Fixed::ShortMonthName),
                    Literal("-"),
                    num0(Year)
                ]
            }
            'w' => num(NumDaysFromSun),
            #[cfg(not(feature = "unstable-locales"))]
            'x' => queue_from_slice!(D_FMT),
            #[cfg(feature = "unstable-locales")]
            'x' => self.switch_to_locale_str(locales::d_fmt, D_FMT),
            'y' => num0(YearMod100),
            'z' => {
                if is_alternate {
                    internal_fixed(TimezoneOffsetPermissive)
                } else {
                    fixed(Fixed::TimezoneOffset)
                }
            }
            '+' => fixed(Fixed::RFC3339),
            ':' => {
                if remainder.starts_with("::z") {
                    remainder = &remainder[3..];
                    fixed(Fixed::TimezoneOffsetTripleColon)
                } else if remainder.starts_with(":z") {
                    remainder = &remainder[2..];
                    fixed(Fixed::TimezoneOffsetDoubleColon)
                } else if remainder.starts_with('z') {
                    remainder = &remainder[1..];
                    fixed(Fixed::TimezoneOffsetColon)
                } else {
                    self.error(original, remainder)
                }
            }
            '.' => match next!() {
                '3' => match next!() {
                    'f' => fixed(Fixed::Nanosecond3),
                    _ => self.error(original, remainder),
                },
                '6' => match next!() {
                    'f' => fixed(Fixed::Nanosecond6),
                    _ => self.error(original, remainder),
                },
                '9' => match next!() {
                    'f' => fixed(Fixed::Nanosecond9),
                    _ => self.error(original, remainder),
                },
                'f' => fixed(Fixed::Nanosecond),
                _ => self.error(original, remainder),
            },
            '3' => match next!() {
                'f' => internal_fixed(Nanosecond3NoDot),
                _ => self.error(original, remainder),
            },
            '6' => match next!() {
                'f' => internal_fixed(Nanosecond6NoDot),
                _ => self.error(original, remainder),
            },
            '9' => match next!() {
                'f' => internal_fixed(Nanosecond9NoDot),
                _ => self.error(original, remainder),
            },
            '%' => Literal("%"),
            _ => self.error(original, remainder),
        };

        if let Some(new_pad) = pad_override {
            match item {
                Item::Numeric(ref kind, _pad) if self.queue.is_empty() => {
                    Some((remainder, Item::Numeric(kind.clone(), new_pad)))
                }
                _ => Some((remainder, self.error(original, remainder))),
            }
        } else {
            Some((remainder, item))
        }
    }

    fn error<'b>(&mut self, original: &'b str, remainder: &'b str) -> Item<'b> {
        match self.lenient {
            false => Item::Error,
            true => Item::Literal(&original[..original.len() - remainder.len()]),
        }
    }

    #[cfg(feature = "unstable-locales")]
    fn switch_to_locale_str(
        &mut self,
        localized_fmt_str: impl Fn(Locale) -> &'static str,
        fallback: &'static [Item<'static>],
    ) -> Item<'a> {
        if let Some(locale) = self.locale {
            assert!(self.locale_str.is_empty());
            let (fmt_str, item) = self.parse_next_item(localized_fmt_str(locale)).unwrap();
            self.locale_str = fmt_str;
            item
        } else {
            self.queue = &fallback[1..];
            fallback[0].clone()
        }
    }
}

impl<'a> Iterator for StrftimeItems<'a> {
    type Item = Item<'a>;

    fn next(&mut self) -> Option<Item<'a>> {
        if let Some((item, remainder)) = self.queue.split_first() {
            self.queue = remainder;
            return Some(item.clone());
        }

        #[cfg(feature = "unstable-locales")]
        if !self.locale_str.is_empty() {
            let (remainder, item) = self.parse_next_item(self.locale_str)?;
            self.locale_str = remainder;
            return Some(item);
        }

        let (remainder, item) = self.parse_next_item(self.remainder)?;
        self.remainder = remainder;
        Some(item)
    }
}

static D_FMT: &[Item<'static>] = &[
    num0(Numeric::Month),
    Item::Literal("/"),
    num0(Numeric::Day),
    Item::Literal("/"),
    num0(Numeric::YearMod100),
];
static D_T_FMT: &[Item<'static>] = &[
    fixed(Fixed::ShortWeekdayName),
    Item::Space(" "),
    fixed(Fixed::ShortMonthName),
    Item::Space(" "),
    nums(Numeric::Day),
    Item::Space(" "),
    num0(Numeric::Hour),
    Item::Literal(":"),
    num0(Numeric::Minute),
    Item::Literal(":"),
    num0(Numeric::Second),
    Item::Space(" "),
    num0(Numeric::Year),
];
static T_FMT: &[Item<'static>] = &[
    num0(Numeric::Hour),
    Item::Literal(":"),
    num0(Numeric::Minute),
    Item::Literal(":"),
    num0(Numeric::Second),
];
static T_FMT_AMPM: &[Item<'static>] = &[
    num0(Numeric::Hour12),
    Item::Literal(":"),
    num0(Numeric::Minute),
    Item::Literal(":"),
    num0(Numeric::Second),
    Item::Space(" "),
    fixed(Fixed::UpperAmPm),
];

const HAVE_ALTERNATES: &str = "z";
