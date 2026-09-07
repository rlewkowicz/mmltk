//! Value module.

use alloc::{borrow::Cow, boxed::Box, format, string::String, vec::Vec};
use core::{cmp::Eq, hash::Hash};

use serde::{
    de::{DeserializeOwned, DeserializeSeed, Deserializer, MapAccess, SeqAccess, Visitor},
    forward_to_deserialize_any,
};

use crate::{de::Error, error::Result};

mod map;
mod number;
pub(crate) mod raw;

pub use map::Map;
pub use number::{Number, F32, F64};
#[allow(clippy::useless_attribute, clippy::module_name_repetitions)]
pub use raw::RawValue;

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum Value {
    Bool(bool),
    Char(char),
    Map(Map),
    Number(Number),
    Option(Option<Box<Value>>),
    String(String),
    Bytes(Vec<u8>),
    Seq(Vec<Value>),
    Unit,
}

impl From<bool> for Value {
    fn from(value: bool) -> Self {
        Self::Bool(value)
    }
}

impl From<char> for Value {
    fn from(value: char) -> Self {
        Self::Char(value)
    }
}

impl<K: Into<Value>, V: Into<Value>> FromIterator<(K, V)> for Value {
    fn from_iter<T: IntoIterator<Item = (K, V)>>(iter: T) -> Self {
        Self::Map(iter.into_iter().collect())
    }
}

impl From<Map> for Value {
    fn from(value: Map) -> Self {
        Self::Map(value)
    }
}

impl<T: Into<Number>> From<T> for Value {
    fn from(value: T) -> Self {
        Self::Number(value.into())
    }
}

impl<T: Into<Value>> From<Option<T>> for Value {
    fn from(value: Option<T>) -> Self {
        Self::Option(value.map(Into::into).map(Box::new))
    }
}

impl<'a> From<&'a str> for Value {
    fn from(value: &'a str) -> Self {
        String::from(value).into()
    }
}

impl<'a> From<Cow<'a, str>> for Value {
    fn from(value: Cow<'a, str>) -> Self {
        String::from(value).into()
    }
}

impl From<String> for Value {
    fn from(value: String) -> Self {
        Self::String(value)
    }
}

/// Special case to allow `Value::from(b"byte string")`
impl<const N: usize> From<&'static [u8; N]> for Value {
    fn from(value: &'static [u8; N]) -> Self {
        Self::Bytes(Vec::from(*value))
    }
}

impl<T: Into<Value>> FromIterator<T> for Value {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        Self::Seq(iter.into_iter().map(Into::into).collect())
    }
}

impl<'a, T: Clone + Into<Value>> From<&'a [T]> for Value {
    fn from(value: &'a [T]) -> Self {
        value.iter().map(Clone::clone).map(Into::into).collect()
    }
}

impl<T: Into<Value>> From<Vec<T>> for Value {
    fn from(value: Vec<T>) -> Self {
        value.into_iter().collect()
    }
}

impl From<()> for Value {
    fn from(_value: ()) -> Self {
        Value::Unit
    }
}

impl Value {
    /// Tries to deserialize this [`Value`] into `T`.
    pub fn into_rust<T>(self) -> Result<T>
    where
        T: DeserializeOwned,
    {
        T::deserialize(self)
    }
}

/// Deserializer implementation for RON [`Value`].
/// This does not support enums (because [`Value`] does not store them).
impl<'de> Deserializer<'de> for Value {
    type Error = Error;

    forward_to_deserialize_any! {
        bool i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 char str string bytes
        byte_buf option unit unit_struct newtype_struct seq tuple
        tuple_struct map struct enum identifier ignored_any
    }

    #[cfg(feature = "integer128")]
    forward_to_deserialize_any! {
        i128 u128
    }

    fn deserialize_any<V>(self, visitor: V) -> Result<V::Value>
    where
        V: Visitor<'de>,
    {
        match self {
            Value::Bool(b) => visitor.visit_bool(b),
            Value::Char(c) => visitor.visit_char(c),
            Value::Map(m) => {
                let old_len = m.len();

                let mut items: Vec<(Value, Value)> = m.into_iter().collect();
                items.reverse();

                let value = visitor.visit_map(MapAccessor {
                    items: &mut items,
                    value: None,
                })?;

                if items.is_empty() {
                    Ok(value)
                } else {
                    Err(Error::ExpectedDifferentLength {
                        expected: format!("a map of length {}", old_len - items.len()),
                        found: old_len,
                    })
                }
            }
            Value::Number(number) => number.visit(visitor),
            Value::Option(Some(o)) => visitor.visit_some(*o),
            Value::Option(None) => visitor.visit_none(),
            Value::String(s) => visitor.visit_string(s),
            Value::Bytes(b) => visitor.visit_byte_buf(b),
            Value::Seq(mut seq) => {
                let old_len = seq.len();

                seq.reverse();
                let value = visitor.visit_seq(SeqAccessor { seq: &mut seq })?;

                if seq.is_empty() {
                    Ok(value)
                } else {
                    Err(Error::ExpectedDifferentLength {
                        expected: format!("a sequence of length {}", old_len - seq.len()),
                        found: old_len,
                    })
                }
            }
            Value::Unit => visitor.visit_unit(),
        }
    }
}

struct SeqAccessor<'a> {
    seq: &'a mut Vec<Value>,
}

impl<'a, 'de> SeqAccess<'de> for SeqAccessor<'a> {
    type Error = Error;

    fn next_element_seed<T>(&mut self, seed: T) -> Result<Option<T::Value>>
    where
        T: DeserializeSeed<'de>,
    {
        self.seq
            .pop()
            .map_or(Ok(None), |v| seed.deserialize(v).map(Some))
    }

    fn size_hint(&self) -> Option<usize> {
        Some(self.seq.len())
    }
}

struct MapAccessor<'a> {
    items: &'a mut Vec<(Value, Value)>,
    value: Option<Value>,
}

impl<'a, 'de> MapAccess<'de> for MapAccessor<'a> {
    type Error = Error;

    fn next_key_seed<K>(&mut self, seed: K) -> Result<Option<K::Value>>
    where
        K: DeserializeSeed<'de>,
    {
        match self.items.pop() {
            Some((key, value)) => {
                self.value = Some(value);
                seed.deserialize(key).map(Some)
            }
            None => Ok(None),
        }
    }

    #[allow(clippy::panic)]
    fn next_value_seed<V>(&mut self, seed: V) -> Result<V::Value>
    where
        V: DeserializeSeed<'de>,
    {
        match self.value.take() {
            Some(value) => seed.deserialize(value),
            None => panic!("Contract violation: value before key"),
        }
    }

    fn size_hint(&self) -> Option<usize> {
        Some(self.items.len())
    }
}
