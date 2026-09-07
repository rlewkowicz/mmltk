// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use super::LiteMap;
use crate::store::*;
use alloc::vec::Vec;
use core::fmt;
use core::marker::PhantomData;
use serde::{
    de::{MapAccess, SeqAccess, Visitor},
    ser::{SerializeMap, SerializeSeq},
    Deserialize, Deserializer, Serialize, Serializer,
};

impl<K, V, R> Serialize for LiteMap<K, V, R>
where
    K: Serialize,
    V: Serialize,
    R: Store<K, V>,
{
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        if serializer.is_human_readable() {
            let k_is_num_or_string = self
                .values
                .lm_get(0)
                .is_some_and(|(k, _)| super::serde_helpers::is_num_or_string(k));
            if !k_is_num_or_string {
                let mut seq = serializer.serialize_seq(Some(self.len()))?;
                for index in 0..self.len() {
                    #[allow(clippy::unwrap_used)] 
                    seq.serialize_element(&self.get_indexed(index).unwrap())?;
                }
                return seq.end();
            }
        }

        let mut map = serializer.serialize_map(Some(self.len()))?;
        for index in 0..self.len() {
            #[allow(clippy::unwrap_used)] 
            let (k, v) = self.get_indexed(index).unwrap();
            map.serialize_entry(k, v)?;
        }
        map.end()
    }
}

/// Modified example from https://serde.rs/deserialize-map.html
#[allow(clippy::type_complexity)]
struct LiteMapVisitor<K, V, R> {
    marker: PhantomData<fn() -> LiteMap<K, V, R>>,
}

impl<K, V, R> LiteMapVisitor<K, V, R> {
    fn new() -> Self {
        Self {
            marker: PhantomData,
        }
    }
}

impl<'de, K, V, R> Visitor<'de> for LiteMapVisitor<K, V, R>
where
    K: Deserialize<'de> + Ord,
    V: Deserialize<'de>,
    R: StoreBulkMut<K, V>,
{
    type Value = LiteMap<K, V, R>;

    fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
        formatter.write_str("a map produced by LiteMap")
    }

    fn visit_seq<S>(self, mut access: S) -> Result<Self::Value, S::Error>
    where
        S: SeqAccess<'de>,
    {
        let mut map = LiteMap::with_capacity(access.size_hint().unwrap_or(0));
        let mut out_of_order = Vec::new();

        while let Some((key, value)) = access.next_element()? {
            if let Some((key, value)) = map.try_append(key, value) {
                out_of_order.push((key, value));
            }
        }

        if !out_of_order.is_empty() {
            map.extend(out_of_order);
        }

        Ok(map)
    }

    fn visit_map<M>(self, mut access: M) -> Result<Self::Value, M::Error>
    where
        M: MapAccess<'de>,
    {
        let mut map = LiteMap::with_capacity(access.size_hint().unwrap_or(0));
        let mut out_of_order = Vec::new();

        while let Some((key, value)) = access.next_entry()? {
            if let Some((key, value)) = map.try_append(key, value) {
                out_of_order.push((key, value));
            }
        }

        if !out_of_order.is_empty() {
            map.extend(out_of_order);
        }

        Ok(map)
    }
}

impl<'de, K, V, R> Deserialize<'de> for LiteMap<K, V, R>
where
    K: Ord + Deserialize<'de>,
    V: Deserialize<'de>,
    R: StoreBulkMut<K, V>,
{
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        if deserializer.is_human_readable() {
            deserializer.deserialize_any(LiteMapVisitor::new())
        } else {
            deserializer.deserialize_map(LiteMapVisitor::new())
        }
    }
}
