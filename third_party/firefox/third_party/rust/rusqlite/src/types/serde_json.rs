//! [`ToSql`] and [`FromSql`] implementation for JSON `Value`.

use serde_json::{Number, Value};

use crate::types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef};
use crate::{Error, Result};

/// Serialize JSON `Value` to text:
///
///
/// | JSON   | SQLite    |
/// |----------|---------|
/// | Null     | NULL    |
/// | Bool     | 'true' / 'false' |
/// | Number   | INT or REAL except u64 |
/// | _ | TEXT |
impl ToSql for Value {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        match self {
            Self::Null => Ok(ToSqlOutput::Borrowed(ValueRef::Null)),
            Self::Number(n) if n.is_i64() => Ok(ToSqlOutput::from(n.as_i64().unwrap())),
            Self::Number(n) if n.is_f64() => Ok(ToSqlOutput::from(n.as_f64().unwrap())),
            _ => serde_json::to_string(self)
                .map(ToSqlOutput::from)
                .map_err(|err| Error::ToSqlConversionFailure(err.into())),
        }
    }
}

/// Deserialize SQLite value to JSON `Value`:
///
/// | SQLite   | JSON    |
/// |----------|---------|
/// | NULL     | Null    |
/// | 'null'   | Null    |
/// | 'true'   | Bool    |
/// | 1        | Number  |
/// | 0.1      | Number  |
/// | '"text"' | String  |
/// | 'text'   | _Error_ |
/// | '[0, 1]' | Array   |
/// | '{"x": 1}' | Object  |
impl FromSql for Value {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        match value {
            ValueRef::Text(s) => serde_json::from_slice(s), 
            ValueRef::Blob(b) => serde_json::from_slice(b),
            ValueRef::Integer(i) => Ok(Self::Number(Number::from(i))),
            ValueRef::Real(f) => {
                match Number::from_f64(f) {
                    Some(n) => Ok(Self::Number(n)),
                    _ => return Err(FromSqlError::InvalidType), 
                }
            }
            ValueRef::Null => Ok(Self::Null),
        }
        .map_err(FromSqlError::other)
    }
}
