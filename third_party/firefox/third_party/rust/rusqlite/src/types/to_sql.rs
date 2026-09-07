use super::{Null, Value, ValueRef};
#[cfg(feature = "array")]
use crate::vtab::array::Array;
use crate::{Error, Result};
use std::borrow::Cow;

/// `ToSqlOutput` represents the possible output types for implementers of the
/// [`ToSql`] trait.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub enum ToSqlOutput<'a> {
    /// A borrowed SQLite-representable value.
    Borrowed(ValueRef<'a>),

    /// An owned SQLite-representable value.
    Owned(Value),

    /// A BLOB of the given length that is filled with
    /// zeroes.
    #[cfg(feature = "blob")]
    ZeroBlob(i32),

    /// n-th arg of an SQL scalar function
    #[cfg(feature = "functions")]
    Arg(usize),

    /// `feature = "array"`
    #[cfg(feature = "array")]
    Array(Array),
}

impl<'a, T: ?Sized> From<&'a T> for ToSqlOutput<'a>
where
    &'a T: Into<ValueRef<'a>>,
{
    #[inline]
    fn from(t: &'a T) -> Self {
        ToSqlOutput::Borrowed(t.into())
    }
}

macro_rules! from_value(
    ($t:ty) => (
        impl From<$t> for ToSqlOutput<'_> {
            #[inline]
            fn from(t: $t) -> Self { ToSqlOutput::Owned(t.into())}
        }
    );
    (non_zero $t:ty) => (
        impl From<$t> for ToSqlOutput<'_> {
            #[inline]
            fn from(t: $t) -> Self { ToSqlOutput::Owned(t.get().into())}
        }
    )
);
from_value!(String);
from_value!(Null);
from_value!(bool);
from_value!(i8);
from_value!(i16);
from_value!(i32);
from_value!(i64);
from_value!(isize);
from_value!(u8);
from_value!(u16);
from_value!(u32);
from_value!(f32);
from_value!(f64);
from_value!(Vec<u8>);

from_value!(non_zero std::num::NonZeroI8);
from_value!(non_zero std::num::NonZeroI16);
from_value!(non_zero std::num::NonZeroI32);
from_value!(non_zero std::num::NonZeroI64);
from_value!(non_zero std::num::NonZeroIsize);
from_value!(non_zero std::num::NonZeroU8);
from_value!(non_zero std::num::NonZeroU16);
from_value!(non_zero std::num::NonZeroU32);

#[cfg(feature = "i128_blob")]
from_value!(i128);

#[cfg(feature = "i128_blob")]
from_value!(non_zero std::num::NonZeroI128);

#[cfg(feature = "uuid")]
from_value!(uuid::Uuid);

impl ToSql for ToSqlOutput<'_> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(match *self {
            ToSqlOutput::Borrowed(v) => ToSqlOutput::Borrowed(v),
            ToSqlOutput::Owned(ref v) => ToSqlOutput::Borrowed(ValueRef::from(v)),

            #[cfg(feature = "blob")]
            ToSqlOutput::ZeroBlob(i) => ToSqlOutput::ZeroBlob(i),
            #[cfg(feature = "functions")]
            ToSqlOutput::Arg(i) => ToSqlOutput::Arg(i),
            #[cfg(feature = "array")]
            ToSqlOutput::Array(ref a) => ToSqlOutput::Array(a.clone()),
        })
    }
}

/// A trait for types that can be converted into SQLite values. Returns
/// [`Error::ToSqlConversionFailure`] if the conversion fails.
pub trait ToSql {
    /// Converts Rust value to SQLite value
    fn to_sql(&self) -> Result<ToSqlOutput<'_>>;
}

impl<T: ToSql + ToOwned + ?Sized> ToSql for Cow<'_, T> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        self.as_ref().to_sql()
    }
}

impl<T: ToSql + ?Sized> ToSql for Box<T> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        self.as_ref().to_sql()
    }
}

impl<T: ToSql + ?Sized> ToSql for std::rc::Rc<T> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        self.as_ref().to_sql()
    }
}

impl<T: ToSql + ?Sized> ToSql for std::sync::Arc<T> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        self.as_ref().to_sql()
    }
}


macro_rules! to_sql_self(
    ($t:ty) => (
        impl ToSql for $t {
            #[inline]
            fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
                Ok(ToSqlOutput::from(*self))
            }
        }
    )
);

to_sql_self!(Null);
to_sql_self!(bool);
to_sql_self!(i8);
to_sql_self!(i16);
to_sql_self!(i32);
to_sql_self!(i64);
to_sql_self!(isize);
to_sql_self!(u8);
to_sql_self!(u16);
to_sql_self!(u32);
to_sql_self!(f32);
to_sql_self!(f64);

to_sql_self!(std::num::NonZeroI8);
to_sql_self!(std::num::NonZeroI16);
to_sql_self!(std::num::NonZeroI32);
to_sql_self!(std::num::NonZeroI64);
to_sql_self!(std::num::NonZeroIsize);
to_sql_self!(std::num::NonZeroU8);
to_sql_self!(std::num::NonZeroU16);
to_sql_self!(std::num::NonZeroU32);

#[cfg(feature = "i128_blob")]
to_sql_self!(i128);

#[cfg(feature = "i128_blob")]
to_sql_self!(std::num::NonZeroI128);

#[cfg(feature = "uuid")]
to_sql_self!(uuid::Uuid);

macro_rules! to_sql_self_fallible(
    ($t:ty) => (
        impl ToSql for $t {
            #[inline]
            fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
                Ok(ToSqlOutput::Owned(Value::Integer(
                    i64::try_from(*self).map_err(
                        |err| Error::ToSqlConversionFailure(err.into())
                    )?
                )))
            }
        }
    );
    (non_zero $t:ty) => (
        impl ToSql for $t {
            #[inline]
            fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
                Ok(ToSqlOutput::Owned(Value::Integer(
                    i64::try_from(self.get()).map_err(
                        |err| Error::ToSqlConversionFailure(err.into())
                    )?
                )))
            }
        }
    )
);

to_sql_self_fallible!(u64);
to_sql_self_fallible!(usize);
to_sql_self_fallible!(non_zero std::num::NonZeroU64);
to_sql_self_fallible!(non_zero std::num::NonZeroUsize);

impl<T: ?Sized> ToSql for &'_ T
where
    T: ToSql,
{
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        (*self).to_sql()
    }
}

impl ToSql for String {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self.as_str()))
    }
}

impl ToSql for str {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self))
    }
}

impl ToSql for Vec<u8> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self.as_slice()))
    }
}

impl<const N: usize> ToSql for [u8; N] {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(&self[..]))
    }
}

impl ToSql for [u8] {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self))
    }
}

impl ToSql for Value {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        Ok(ToSqlOutput::from(self))
    }
}

impl<T: ToSql> ToSql for Option<T> {
    #[inline]
    fn to_sql(&self) -> Result<ToSqlOutput<'_>> {
        match *self {
            None => Ok(ToSqlOutput::from(Null)),
            Some(ref t) => t.to_sql(),
        }
    }
}
