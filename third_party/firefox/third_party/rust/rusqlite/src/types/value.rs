use super::{Null, Type};

/// Owning [dynamic type value](http://sqlite.org/datatype3.html). Value's type is typically
/// dictated by SQLite (not by the caller).
///
/// See [`ValueRef`](crate::types::ValueRef) for a non-owning dynamic type
/// value.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    /// The value is a `NULL` value.
    Null,
    /// The value is a signed integer.
    Integer(i64),
    /// The value is a floating point number.
    Real(f64),
    /// The value is a text string.
    Text(String),
    /// The value is a blob of data
    Blob(Vec<u8>),
}

impl From<Null> for Value {
    #[inline]
    fn from(_: Null) -> Self {
        Self::Null
    }
}

impl From<bool> for Value {
    #[inline]
    fn from(i: bool) -> Self {
        Self::Integer(i as i64)
    }
}

impl From<isize> for Value {
    #[inline]
    fn from(i: isize) -> Self {
        Self::Integer(i as i64)
    }
}

#[cfg(feature = "i128_blob")]
impl From<i128> for Value {
    #[inline]
    fn from(i: i128) -> Self {
        Self::Blob(i128::to_be_bytes(i ^ (1_i128 << 127)).to_vec())
    }
}

#[cfg(feature = "uuid")]
impl From<uuid::Uuid> for Value {
    #[inline]
    fn from(id: uuid::Uuid) -> Self {
        Self::Blob(id.as_bytes().to_vec())
    }
}

macro_rules! from_i64(
    ($t:ty) => (
        impl From<$t> for Value {
            #[inline]
            fn from(i: $t) -> Value {
                Value::Integer(i64::from(i))
            }
        }
    )
);

from_i64!(i8);
from_i64!(i16);
from_i64!(i32);
from_i64!(u8);
from_i64!(u16);
from_i64!(u32);

impl From<i64> for Value {
    #[inline]
    fn from(i: i64) -> Self {
        Self::Integer(i)
    }
}

impl From<f32> for Value {
    #[inline]
    fn from(f: f32) -> Self {
        Self::Real(f.into())
    }
}

impl From<f64> for Value {
    #[inline]
    fn from(f: f64) -> Self {
        Self::Real(f)
    }
}

impl From<String> for Value {
    #[inline]
    fn from(s: String) -> Self {
        Self::Text(s)
    }
}

impl From<Vec<u8>> for Value {
    #[inline]
    fn from(v: Vec<u8>) -> Self {
        Self::Blob(v)
    }
}

impl<T> From<Option<T>> for Value
where
    T: Into<Self>,
{
    #[inline]
    fn from(v: Option<T>) -> Self {
        match v {
            Some(x) => x.into(),
            None => Self::Null,
        }
    }
}

impl Value {
    /// Returns SQLite fundamental datatype.
    #[inline]
    #[must_use]
    pub fn data_type(&self) -> Type {
        match *self {
            Self::Null => Type::Null,
            Self::Integer(_) => Type::Integer,
            Self::Real(_) => Type::Real,
            Self::Text(_) => Type::Text,
            Self::Blob(_) => Type::Blob,
        }
    }
}
