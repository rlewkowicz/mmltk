//! [`ToSql`] and [`FromSql`] implementation for [`Url`].
use crate::types::{FromSql, FromSqlError, FromSqlResult, ToSql, ToSqlOutput, ValueRef};
use crate::Result;
use url::Url;

/// Serialize `Url` to text.
impl ToSql for Url {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self.as_str()))
    }
}

/// Deserialize text to `Url`.
impl FromSql for Url {
    #[inline]
    fn column_result(value: ValueRef<'_>) -> FromSqlResult<Self> {
        match value {
            ValueRef::Text(s) => {
                let s = std::str::from_utf8(s).map_err(FromSqlError::other)?;
                Self::parse(s).map_err(FromSqlError::other)
            }
            _ => Err(FromSqlError::InvalidType),
        }
    }
}
