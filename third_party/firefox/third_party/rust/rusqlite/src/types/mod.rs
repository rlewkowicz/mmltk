//! Traits dealing with SQLite data types.
//!
//! SQLite uses a [dynamic type system](https://www.sqlite.org/datatype3.html). Implementations of
//! the [`ToSql`] and [`FromSql`] traits are provided for the basic types that
//! SQLite provides methods for:
//!
//! * Strings (`String` and `&str`)
//! * Blobs (`Vec<u8>` and `&[u8]`)
//! * Numbers
//!
//! The number situation is a little complicated due to the fact that all
//! numbers in SQLite are stored as `INTEGER` (`i64`) or `REAL` (`f64`).
//!
//! [`ToSql`] and [`FromSql`] are implemented for all primitive number types.
//! [`FromSql`] has different behaviour depending on the SQL and Rust types, and
//! the value.
//!
//! * `INTEGER` to integer: returns an
//!   [`Error::IntegralValueOutOfRange`](crate::Error::IntegralValueOutOfRange)
//!   error if the value does not fit in the Rust type.
//! * `REAL` to integer: always returns an
//!   [`Error::InvalidColumnType`](crate::Error::InvalidColumnType) error.
//! * `INTEGER` to float: casts using `as` operator. Never fails.
//! * `REAL` to float: casts using `as` operator. Never fails.
//!
//! [`ToSql`] always succeeds except when storing a `u64` or `usize` value that
//! cannot fit in an `INTEGER` (`i64`). Also note that SQLite ignores column
//! types, so if you store an `i64` in a column with type `REAL` it will be
//! stored as an `INTEGER`, not a `REAL` (unless the column is part of a
//! [STRICT table](https://www.sqlite.org/stricttables.html)).
//!
//! If the `time` feature is enabled, implementations are
//! provided for `time::OffsetDateTime` that use the RFC 3339 date/time format,
//! `"%Y-%m-%dT%H:%M:%S.%fZ"`, to store time values as strings.  These values
//! can be parsed by SQLite's builtin
//! [datetime](https://www.sqlite.org/lang_datefunc.html) functions.  If you
//! want different storage for datetimes, you can use a newtype.
#![cfg_attr(
    feature = "time",
    doc = r##"
For example, to store datetimes as `i64`s counting the number of seconds since
the Unix epoch:

```
use rusqlite::types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef};
use rusqlite::Result;

pub struct DateTimeSql(pub time::OffsetDateTime);

impl FromSql for DateTimeSql {
    fn column_result(value: ValueRef) -> FromSqlResult<Self> {
        i64::column_result(value).and_then(|as_i64| {
            time::OffsetDateTime::from_unix_timestamp(as_i64)
            .map(|odt| DateTimeSql(odt))
            .map_err(FromSqlError::other)
        })
    }
}

impl ToSql for DateTimeSql {
    fn to_sql(&self) -> Result<ToSqlOutput> {
        Ok(self.0.unix_timestamp().into())
    }
}
```

"##
)]
//! [`ToSql`] and [`FromSql`] are also implemented for `Option<T>` where `T`
//! implements [`ToSql`] or [`FromSql`] for the cases where you want to know if
//! a value was NULL (which gets translated to `None`).

pub use self::from_sql::{FromSql, FromSqlError, FromSqlResult};
pub use self::to_sql::{ToSql, ToSqlOutput};
pub use self::value::Value;
pub use self::value_ref::ValueRef;

use std::fmt;

#[cfg(feature = "chrono")]
mod chrono;
mod from_sql;
#[cfg(feature = "jiff")]
mod jiff;
#[cfg(feature = "serde_json")]
mod serde_json;
#[cfg(feature = "time")]
mod time;
mod to_sql;
#[cfg(feature = "url")]
mod url;
mod value;
mod value_ref;

/// Empty struct that can be used to fill in a query parameter as `NULL`.
///
/// ## Example
///
/// ```rust,no_run
/// # use rusqlite::{Connection, Result};
/// # use rusqlite::types::{Null};
///
/// fn insert_null(conn: &Connection) -> Result<usize> {
///     conn.execute("INSERT INTO people (name) VALUES (?1)", [Null])
/// }
/// ```
#[derive(Copy, Clone)]
pub struct Null;

/// SQLite data types.
/// See [Fundamental Datatypes](https://sqlite.org/c3ref/c_blob.html).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Type {
    /// NULL
    Null,
    /// 64-bit signed integer
    Integer,
    /// 64-bit IEEE floating point number
    Real,
    /// String
    Text,
    /// BLOB
    Blob,
}

impl fmt::Display for Type {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::Null => f.pad("Null"),
            Self::Integer => f.pad("Integer"),
            Self::Real => f.pad("Real"),
            Self::Text => f.pad("Text"),
            Self::Blob => f.pad("Blob"),
        }
    }
}
