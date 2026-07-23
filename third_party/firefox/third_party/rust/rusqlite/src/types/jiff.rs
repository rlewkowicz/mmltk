//! Convert some `jiff` types.

use jiff::{
    civil::{Date, DateTime, Time},
    Timestamp,
};

use crate::types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef};
use crate::Result;

/// Gregorian calendar date => "YYYY-MM-DD"
impl ToSql for Date {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let s = self.to_string();
        Ok(ToSqlOutput::from(s))
    }
}

/// "YYYY-MM-DD" => Gregorian calendar date.
impl FromSql for Date {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value
            .as_str()
            .and_then(|s| s.parse().map_err(FromSqlError::other))
    }
}
/// time => "HH:MM:SS.SSS"
impl ToSql for Time {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let date_str = self.to_string();
        Ok(ToSqlOutput::from(date_str))
    }
}

/// "HH:MM:SS.SSS" => time.
impl FromSql for Time {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value
            .as_str()
            .and_then(|s| s.parse().map_err(FromSqlError::other))
    }
}

/// Gregorian datetime => "YYYY-MM-DDTHH:MM:SS.SSS"
impl ToSql for DateTime {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        let s = self.to_string();
        Ok(ToSqlOutput::from(s))
    }
}

/// "YYYY-MM-DDTHH:MM:SS.SSS" => Gregorian datetime.
impl FromSql for DateTime {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value
            .as_str()
            .and_then(|s| s.parse().map_err(FromSqlError::other))
    }
}

/// UTC time => UTC RFC3339 timestamp
/// ("YYYY-MM-DDTHH:MM:SS.SSSZ").
impl ToSql for Timestamp {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self.to_string()))
    }
}

/// RFC3339 ("YYYY-MM-DD HH:MM:SS.SSS[+-]HH:MM") into `Timestamp`.
impl FromSql for Timestamp {
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        value
            .as_str()?
            .parse::<Timestamp>()
            .map_err(FromSqlError::other)
    }
}
