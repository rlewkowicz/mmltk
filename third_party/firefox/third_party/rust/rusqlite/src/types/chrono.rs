//! Convert most of the [Time Strings](http://sqlite.org/lang_datefunc.html) to chrono types.

use chrono::{DateTime, FixedOffset, Local, NaiveDate, NaiveDateTime, NaiveTime, TimeZone, Utc};

use crate::types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef};
use crate::Result;

/// ISO 8601 calendar date without timezone => "YYYY-MM-DD"
impl ToSql for NaiveDate {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.format("%F").to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// "YYYY-MM-DD" => ISO 8601 calendar date without timezone.
impl FromSql for NaiveDate {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value
            .as_str()
            .and_then(|s| Self::parse_from_str(s, "%F").map_err(FromSqlError::other))
    }
}

/// ISO 8601 time without timezone => "HH:MM:SS.SSS"
impl ToSql for NaiveTime {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.format("%T%.f").to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// "HH:MM"/"HH:MM:SS"/"HH:MM:SS.SSS" => ISO 8601 time without timezone.
impl FromSql for NaiveTime {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value.as_str().and_then(|s| {
            let fmt = match s.len() {
                5 => "%H:%M",
                8 => "%T",
                _ => "%T%.f",
            };
            Self::parse_from_str(s, fmt).map_err(FromSqlError::other)
        })
    }
}

/// ISO 8601 combined date and time without timezone =>
/// "YYYY-MM-DD HH:MM:SS.SSS"
impl ToSql for NaiveDateTime {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.format("%F %T%.f").to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// "YYYY-MM-DD HH:MM:SS"/"YYYY-MM-DD HH:MM:SS.SSS" => ISO 8601 combined date
/// and time without timezone. ("YYYY-MM-DDTHH:MM:SS"/"YYYY-MM-DDTHH:MM:SS.SSS"
/// also supported)
impl FromSql for NaiveDateTime {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value.as_str().and_then(|s| {
            let fmt = if s.len() >= 11 && s.as_bytes()[10] == b'T' {
                "%FT%T%.f"
            } else {
                "%F %T%.f"
            };

            Self::parse_from_str(s, fmt).map_err(FromSqlError::other)
        })
    }
}

/// UTC time => UTC RFC3339 timestamp
/// ("YYYY-MM-DD HH:MM:SS.SSS+00:00").
impl ToSql for DateTime<Utc> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.format("%F %T%.f%:z").to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// Local time => UTC RFC3339 timestamp
/// ("YYYY-MM-DD HH:MM:SS.SSS+00:00").
impl ToSql for DateTime<Local> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.with_timezone(&Utc).format("%F %T%.f%:z").to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// Date and time with time zone => RFC3339 timestamp
/// ("YYYY-MM-DD HH:MM:SS.SSS[+-]HH:MM").
impl ToSql for DateTime<FixedOffset> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.format("%F %T%.f%:z").to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// RFC3339 ("YYYY-MM-DD HH:MM:SS.SSS[+-]HH:MM") into `DateTime<Utc>`.
impl FromSql for DateTime<Utc> {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        {
            let s = value.as_str()?;

            let fmt = if s.len() >= 11 && s.as_bytes()[10] == b'T' {
                "%FT%T%.f%#z"
            } else {
                "%F %T%.f%#z"
            };

            if let Ok(dt) = DateTime::parse_from_str(s, fmt) {
                return Ok(dt.with_timezone(&Utc));
            }
        }

        NaiveDateTime::column_result(value).map(|dt| Utc.from_utc_datetime(&dt))
    }
}

/// RFC3339 ("YYYY-MM-DD HH:MM:SS.SSS[+-]HH:MM") into `DateTime<Local>`.
impl FromSql for DateTime<Local> {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        let utc_dt = DateTime::<Utc>::column_result(value)?;
        Ok(utc_dt.with_timezone(&Local))
    }
}

/// RFC3339 ("YYYY-MM-DD HH:MM:SS.SSS[+-]HH:MM") into `DateTime<FixedOffset>`.
impl FromSql for DateTime<FixedOffset> {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        let s = String::column_result(value)?;
        Self::parse_from_rfc3339(s.as_str())
            .or_else(|_| Self::parse_from_str(s.as_str(), "%F %T%.f%:z"))
            .map_err(FromSqlError::other)
    }
}
