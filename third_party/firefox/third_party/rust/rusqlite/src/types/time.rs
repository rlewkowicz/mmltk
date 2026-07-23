//! Convert formats 1-10 in [Time Values](https://sqlite.org/lang_datefunc.html#time_values) to time types.
//! [`ToSql`] and [`FromSql`] implementation for [`OffsetDateTime`].
//! [`ToSql`] and [`FromSql`] implementation for [`PrimitiveDateTime`].
//! [`ToSql`] and [`FromSql`] implementation for [`Date`].
//! [`ToSql`] and [`FromSql`] implementation for [`Time`].
//! Time Strings in:
//!  - Format 2: "YYYY-MM-DD HH:MM"
//!  - Format 5: "YYYY-MM-DDTHH:MM"
//!  - Format 8: "HH:MM"
//!
//! without an explicit second value will assume 0 seconds.
//! Time String that contain an optional timezone without an explicit date are unsupported.
//! All other assumptions described in [Time Values](https://sqlite.org/lang_datefunc.html#time_values) section are unsupported.

use crate::types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef};
use crate::{Error, Result};
use time::format_description::FormatItem;
use time::macros::format_description;
use time::{Date, OffsetDateTime, PrimitiveDateTime, Time};

const OFFSET_DATE_TIME_ENCODING: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[year]-[month]-[day] [hour]:[minute]:[second].[subsecond][offset_hour sign:mandatory]:[offset_minute]"
);
const PRIMITIVE_DATE_TIME_ENCODING: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[year]-[month]-[day] [hour]:[minute]:[second].[subsecond]"
);
const TIME_ENCODING: &[FormatItem<'_>] =
    format_description!(version = 2, "[hour]:[minute]:[second].[subsecond]");

const DATE_FORMAT: &[FormatItem<'_>] = format_description!(version = 2, "[year]-[month]-[day]");
const TIME_FORMAT: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[hour]:[minute][optional [:[second][optional [.[subsecond]]]]]"
);
const PRIMITIVE_DATE_TIME_FORMAT: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[year]-[month]-[day][first [ ][T]][hour]:[minute][optional [:[second][optional [.[subsecond]]]]]"
);
const UTC_DATE_TIME_FORMAT: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[year]-[month]-[day][first [ ][T]][hour]:[minute][optional [:[second][optional [.[subsecond]]]]][optional [Z]]"
);
const OFFSET_DATE_TIME_FORMAT: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[year]-[month]-[day][first [ ][T]][hour]:[minute][optional [:[second][optional [.[subsecond]]]]][offset_hour sign:mandatory]:[offset_minute]"
);
const LEGACY_DATE_TIME_FORMAT: &[FormatItem<'_>] = format_description!(
    version = 2,
    "[year]-[month]-[day] [hour]:[minute]:[second]:[subsecond] [offset_hour sign:mandatory]:[offset_minute]"
);

/// `OffsetDatetime` => RFC3339 format ("YYYY-MM-DD HH:MM:SS.SSS[+-]HH:MM")
impl ToSql for OffsetDateTime {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let time_string = self
            .format(&OFFSET_DATE_TIME_ENCODING)
            .map_err(|err| Error::ToSqlConversionFailure(err.into()))?;
        Ok(ToSqlOutput::from(time_string))
    }
}

impl FromSql for OffsetDateTime {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value.as_str().and_then(|s| {
            if let Some(b' ') = s.as_bytes().get(23) {
                return Self::parse(s, &LEGACY_DATE_TIME_FORMAT).map_err(FromSqlError::other);
            }
            if s[8..].contains('+') || s[8..].contains('-') {
                return Self::parse(s, &OFFSET_DATE_TIME_FORMAT).map_err(FromSqlError::other);
            }
            PrimitiveDateTime::parse(s, &UTC_DATE_TIME_FORMAT)
                .map(|p| p.assume_utc())
                .map_err(FromSqlError::other)
        })
    }
}

/// ISO 8601 calendar date without timezone => "YYYY-MM-DD"
impl ToSql for Date {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self
            .format(&DATE_FORMAT)
            .map_err(|err| Error::ToSqlConversionFailure(err.into()))?;
        Ok(ToSqlOutput::from(date_str))
    }
}

/// "YYYY-MM-DD" => ISO 8601 calendar date without timezone.
impl FromSql for Date {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value.as_str().and_then(|s| {
            Self::parse(s, &DATE_FORMAT).map_err(|err| FromSqlError::Other(err.into()))
        })
    }
}

/// ISO 8601 time without timezone => "HH:MM:SS.SSS"
impl ToSql for Time {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let time_str = self
            .format(&TIME_ENCODING)
            .map_err(|err| Error::ToSqlConversionFailure(err.into()))?;
        Ok(ToSqlOutput::from(time_str))
    }
}

/// "HH:MM"/"HH:MM:SS"/"HH:MM:SS.SSS" => ISO 8601 time without timezone.
impl FromSql for Time {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value.as_str().and_then(|s| {
            Self::parse(s, &TIME_FORMAT).map_err(|err| FromSqlError::Other(err.into()))
        })
    }
}

/// ISO 8601 combined date and time without timezone => "YYYY-MM-DD HH:MM:SS.SSS"
impl ToSql for PrimitiveDateTime {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_time_str = self
            .format(&PRIMITIVE_DATE_TIME_ENCODING)
            .map_err(|err| Error::ToSqlConversionFailure(err.into()))?;
        Ok(ToSqlOutput::from(date_time_str))
    }
}

/// YYYY-MM-DD HH:MM
/// YYYY-MM-DDTHH:MM
/// YYYY-MM-DD HH:MM:SS
/// YYYY-MM-DDTHH:MM:SS
/// YYYY-MM-DD HH:MM:SS.SSS
/// YYYY-MM-DDTHH:MM:SS.SSS
/// => ISO 8601 combined date and time with timezone
impl FromSql for PrimitiveDateTime {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value.as_str().and_then(|s| {
            Self::parse(s, &PRIMITIVE_DATE_TIME_FORMAT)
                .map_err(|err| FromSqlError::Other(err.into()))
        })
    }
}
