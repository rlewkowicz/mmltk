//! Integrates `ArrayString` with other crates' traits

use crate::prelude::*;

#[cfg(all(feature = "diesel-traits", feature = "std"))]
use std::io::Write;

#[cfg(feature = "diesel-traits")]
use diesel::{expression::*, prelude::*, query_builder::*, row::Row, sql_types::*};

#[cfg(feature = "diesel-traits")]
use diesel::backend::Backend;

#[cfg(feature = "diesel-traits")]
use diesel::deserialize::{self, FromSql, FromSqlRow, Queryable};

#[cfg(all(feature = "diesel-traits", feature = "std"))]
use diesel::serialize::{self, Output, ToSql};

#[cfg(feature = "serde-traits")]
use serde::{de::Deserializer, ser::Serializer, Deserialize, Serialize};

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "serde-traits")))]
#[cfg(feature = "serde-traits")]
impl<SIZE> Serialize for ArrayString<SIZE>
where
    SIZE: Capacity,
{
    #[inline]
    fn serialize<S: Serializer>(&self, ser: S) -> Result<S::Ok, S::Error> {
        Serialize::serialize(self.as_str(), ser)
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "serde-traits")))]
#[cfg(feature = "serde-traits")]
impl<'a, SIZE> Deserialize<'a> for ArrayString<SIZE>
where
    SIZE: Capacity,
{
    #[inline]
    fn deserialize<D: Deserializer<'a>>(des: D) -> Result<Self, D::Error> {
        <&str>::deserialize(des).map(Self::from_str_truncate)
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE: Capacity> Expression for ArrayString<SIZE> {
    type SqlType = VarChar;
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE: Capacity, QS> SelectableExpression<QS> for ArrayString<SIZE> {}
#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE: Capacity, QS> AppearsOnTable<QS> for ArrayString<SIZE> {}
#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE: Capacity> NonAggregate for ArrayString<SIZE> {}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE, DB> QueryFragment<DB> for ArrayString<SIZE>
where
    SIZE: Capacity,
    DB: Backend + HasSqlType<VarChar>,
{
    #[inline]
    fn walk_ast(&self, mut pass: AstPass<DB>) -> QueryResult<()> {
        pass.push_bind_param::<Varchar, _>(&self.as_str())?;
        Ok(())
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE, ST, DB> FromSql<ST, DB> for ArrayString<SIZE>
where
    SIZE: Capacity,
    DB: Backend,
    *const str: FromSql<ST, DB>,
{
    #[inline]
    fn from_sql(bytes: Option<&DB::RawValue>) -> deserialize::Result<Self> {
        let ptr: *const str = FromSql::<ST, DB>::from_sql(bytes)?;
        Ok(Self::from_str_truncate(unsafe { &*ptr }))
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE, ST, DB> FromSqlRow<ST, DB> for ArrayString<SIZE>
where
    SIZE: Capacity,
    DB: Backend,
    *const str: FromSql<ST, DB>,
{
    const FIELDS_NEEDED: usize = 1;

    #[inline]
    fn build_from_row<T: Row<DB>>(row: &mut T) -> deserialize::Result<Self> {
        FromSql::<ST, DB>::from_sql(row.take())
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<SIZE, ST, DB> Queryable<ST, DB> for ArrayString<SIZE>
where
    SIZE: Capacity,
    DB: Backend,
    *const str: FromSql<ST, DB>,
{
    type Row = Self;

    #[inline]
    fn build(row: Self::Row) -> Self {
        row
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(all(feature = "diesel-traits", feature = "std"))))]
#[cfg(all(feature = "diesel-traits", feature = "std"))]
impl<SIZE, DB> ToSql<VarChar, DB> for ArrayString<SIZE>
where
    SIZE: Capacity,
    DB: Backend,
{
    #[inline]
    fn to_sql<W: Write>(&self, out: &mut Output<W, DB>) -> serialize::Result {
        ToSql::<VarChar, DB>::to_sql(self.as_str(), out)
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl Expression for CacheString {
    type SqlType = VarChar;
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<QS> SelectableExpression<QS> for CacheString {}
#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<QS> AppearsOnTable<QS> for CacheString {}
#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl NonAggregate for CacheString {}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<DB> QueryFragment<DB> for CacheString
where
    DB: Backend + HasSqlType<VarChar>,
{
    #[inline]
    fn walk_ast(&self, pass: AstPass<DB>) -> QueryResult<()> {
        self.0.walk_ast(pass)
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<ST, DB> FromSql<ST, DB> for CacheString
where
    DB: Backend,
    *const str: FromSql<ST, DB>,
{
    #[inline]
    fn from_sql(bytes: Option<&DB::RawValue>) -> deserialize::Result<Self> {
        Ok(CacheString(FromSql::from_sql(bytes)?))
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<ST, DB> FromSqlRow<ST, DB> for CacheString
where
    DB: Backend,
    *const str: FromSql<ST, DB>,
{
    const FIELDS_NEEDED: usize = 1;

    #[inline]
    fn build_from_row<T: Row<DB>>(row: &mut T) -> deserialize::Result<Self> {
        Ok(CacheString(FromSqlRow::build_from_row(row)?))
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "diesel-traits")))]
#[cfg(feature = "diesel-traits")]
impl<ST, DB> Queryable<ST, DB> for CacheString
where
    DB: Backend,
    *const str: FromSql<ST, DB>,
{
    type Row = Self;

    #[inline]
    fn build(row: Self::Row) -> Self {
        row
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(all(feature = "diesel-traits", feature = "std"))))]
#[cfg(all(feature = "diesel-traits", feature = "std"))]
impl<DB> ToSql<VarChar, DB> for CacheString
where
    DB: Backend,
{
    #[inline]
    fn to_sql<W: Write>(&self, out: &mut Output<W, DB>) -> serialize::Result {
        ToSql::to_sql(&self.0, out)
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "serde-traits")))]
#[cfg(feature = "serde-traits")]
impl Serialize for CacheString {
    #[inline]
    fn serialize<S: Serializer>(&self, ser: S) -> Result<S::Ok, S::Error> {
        self.0.serialize(ser)
    }
}

#[cfg_attr(docs_rs_workaround, doc(cfg(feature = "serde-traits")))]
#[cfg(feature = "serde-traits")]
impl<'a> Deserialize<'a> for CacheString {
    #[inline]
    fn deserialize<D: Deserializer<'a>>(des: D) -> Result<Self, D::Error> {
        Ok(CacheString(Deserialize::deserialize(des)?))
    }
}
