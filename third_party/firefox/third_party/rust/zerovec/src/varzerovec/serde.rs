// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use super::{VarZeroSlice, VarZeroVec, VarZeroVecFormat};
use crate::ule::*;
use alloc::boxed::Box;
use alloc::vec::Vec;
use core::fmt;
use core::marker::PhantomData;
use serde::de::{self, Deserialize, Deserializer, SeqAccess, Visitor};
#[cfg(feature = "serde")]
use serde::ser::{Serialize, SerializeSeq, Serializer};

struct VarZeroVecVisitor<T: ?Sized, F: VarZeroVecFormat> {
    #[expect(clippy::type_complexity)] 
    marker: PhantomData<(fn() -> Box<T>, F)>,
}

impl<T: ?Sized, F: VarZeroVecFormat> Default for VarZeroVecVisitor<T, F> {
    fn default() -> Self {
        Self {
            marker: PhantomData,
        }
    }
}

impl<'de, T, F> Visitor<'de> for VarZeroVecVisitor<T, F>
where
    T: VarULE + ?Sized,
    Box<T>: Deserialize<'de>,
    F: VarZeroVecFormat,
{
    type Value = VarZeroVec<'de, T, F>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a sequence or borrowed buffer of bytes")
    }

    fn visit_borrowed_bytes<E>(self, bytes: &'de [u8]) -> Result<Self::Value, E>
    where
        E: de::Error,
    {
        VarZeroVec::parse_bytes(bytes).map_err(de::Error::custom)
    }

    fn visit_seq<A>(self, mut seq: A) -> Result<Self::Value, A::Error>
    where
        A: SeqAccess<'de>,
    {
        let mut vec: Vec<Box<T>> = if let Some(capacity) = seq.size_hint() {
            Vec::with_capacity(capacity)
        } else {
            Vec::new()
        };
        while let Some(value) = seq.next_element::<Box<T>>()? {
            vec.push(value);
        }
        Ok(VarZeroVec::from(&vec))
    }
}

/// This impl requires enabling the optional `serde` Cargo feature of the `zerovec` crate
impl<'de, 'a, T, F> Deserialize<'de> for VarZeroVec<'a, T, F>
where
    T: VarULE + ?Sized,
    Box<T>: Deserialize<'de>,
    F: VarZeroVecFormat,
    'de: 'a,
{
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let visitor = VarZeroVecVisitor::<T, F>::default();
        if deserializer.is_human_readable() {
            deserializer.deserialize_seq(visitor)
        } else {
            deserializer.deserialize_bytes(visitor)
        }
    }
}

/// This impl requires enabling the optional `serde` Cargo feature of the `zerovec` crate
impl<'de, 'a, T, F> Deserialize<'de> for &'a VarZeroSlice<T, F>
where
    T: VarULE + ?Sized,
    F: VarZeroVecFormat,
    'de: 'a,
{
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        if deserializer.is_human_readable() {
            Err(de::Error::custom(
                "&VarZeroSlice cannot be deserialized from human-readable formats",
            ))
        } else {
            let bytes = <&[u8]>::deserialize(deserializer)?;
            VarZeroSlice::<T, F>::parse_bytes(bytes).map_err(de::Error::custom)
        }
    }
}

/// This impl requires enabling the optional `serde` Cargo feature of the `zerovec` crate
impl<'de, T, F> Deserialize<'de> for Box<VarZeroSlice<T, F>>
where
    T: VarULE + ?Sized,
    Box<T>: Deserialize<'de>,
    F: VarZeroVecFormat,
{
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let deserialized = VarZeroVec::<T, F>::deserialize(deserializer)?;
        Ok(deserialized.to_boxed())
    }
}

/// This impl requires enabling the optional `serde` Cargo feature of the `zerovec` crate
#[cfg(feature = "serde")]
impl<T, F> Serialize for VarZeroVec<'_, T, F>
where
    T: Serialize + VarULE + ?Sized,
    F: VarZeroVecFormat,
{
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        if serializer.is_human_readable() {
            let mut seq = serializer.serialize_seq(Some(self.len()))?;
            for value in self.iter() {
                seq.serialize_element(value)?;
            }
            seq.end()
        } else {
            serializer.serialize_bytes(self.as_bytes())
        }
    }
}

/// This impl requires enabling the optional `serde` Cargo feature of the `zerovec` crate
#[cfg(feature = "serde")]
impl<T, F> Serialize for VarZeroSlice<T, F>
where
    T: Serialize + VarULE + ?Sized,
    F: VarZeroVecFormat,
{
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        self.as_varzerovec().serialize(serializer)
    }
}
