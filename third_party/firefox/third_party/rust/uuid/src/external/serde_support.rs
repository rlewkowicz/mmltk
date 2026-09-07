// Copyright 2013-2014 The Rust Project Developers.
// Copyright 2018 The Uuid Project Developers.
// See the COPYRIGHT file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use crate::{
    error::*,
    fmt::{Braced, Hyphenated, Simple, Urn},
    std::fmt,
    Uuid,
};
use serde::{
    de::{self, Error as _},
    Deserialize, Deserializer, Serialize, Serializer,
};

impl Serialize for Uuid {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        if serializer.is_human_readable() {
            serializer.serialize_str(self.hyphenated().encode_lower(&mut Uuid::encode_buffer()))
        } else {
            serializer.serialize_bytes(self.as_bytes())
        }
    }
}

impl Serialize for Hyphenated {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(self.encode_lower(&mut Uuid::encode_buffer()))
    }
}

impl Serialize for Simple {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(self.encode_lower(&mut Uuid::encode_buffer()))
    }
}

impl Serialize for Urn {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(self.encode_lower(&mut Uuid::encode_buffer()))
    }
}

impl Serialize for Braced {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(self.encode_lower(&mut Uuid::encode_buffer()))
    }
}

impl<'de> Deserialize<'de> for Uuid {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        fn de_error<E: de::Error>(e: Error) -> E {
            E::custom(format_args!("UUID parsing failed: {}", e))
        }

        if deserializer.is_human_readable() {
            struct UuidVisitor;

            impl<'vi> de::Visitor<'vi> for UuidVisitor {
                type Value = Uuid;

                fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                    write!(formatter, "a UUID string")
                }

                fn visit_str<E: de::Error>(self, value: &str) -> Result<Uuid, E> {
                    value.parse::<Uuid>().map_err(de_error)
                }

                fn visit_bytes<E: de::Error>(self, value: &[u8]) -> Result<Uuid, E> {
                    Uuid::from_slice(value).map_err(de_error)
                }

                fn visit_seq<A>(self, mut seq: A) -> Result<Uuid, A::Error>
                where
                    A: de::SeqAccess<'vi>,
                {
                    #[rustfmt::skip]
                    let bytes = [
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                        match seq.next_element()? { Some(e) => e, None => return Err(A::Error::invalid_length(16, &self)) },
                    ];

                    Ok(Uuid::from_bytes(bytes))
                }
            }

            deserializer.deserialize_str(UuidVisitor)
        } else {
            struct UuidBytesVisitor;

            impl<'vi> de::Visitor<'vi> for UuidBytesVisitor {
                type Value = Uuid;

                fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                    write!(formatter, "bytes")
                }

                fn visit_bytes<E: de::Error>(self, value: &[u8]) -> Result<Uuid, E> {
                    Uuid::from_slice(value).map_err(de_error)
                }
            }

            deserializer.deserialize_bytes(UuidBytesVisitor)
        }
    }
}

pub mod compact {
    //! Serialize a [`Uuid`] as a `[u8; 16]`.
    //!
    //! [`Uuid`]: ../../struct.Uuid.html

    /// Serialize from a [`Uuid`] as a `[u8; 16]`
    ///
    /// [`Uuid`]: ../../struct.Uuid.html
    pub fn serialize<S>(u: &crate::Uuid, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serde::Serialize::serialize(u.as_bytes(), serializer)
    }

    /// Deserialize a `[u8; 16]` as a [`Uuid`]
    ///
    /// [`Uuid`]: ../../struct.Uuid.html
    pub fn deserialize<'de, D>(deserializer: D) -> Result<crate::Uuid, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let bytes: [u8; 16] = serde::Deserialize::deserialize(deserializer)?;

        Ok(crate::Uuid::from_bytes(bytes))
    }

}
