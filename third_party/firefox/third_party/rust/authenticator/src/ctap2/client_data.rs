use super::commands::CommandError;
use crate::transport::errors::HIDError;
use base64::Engine;
use serde::de::{self, Deserializer, Error as SerdeError, MapAccess, Visitor};
use serde::{Deserialize, Serialize, Serializer};
use serde_json as json;
use sha2::{Digest, Sha256};
use std::fmt;

/// https://w3c.github.io/webauthn/#dom-collectedclientdata-tokenbinding
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TokenBinding {
    Present(String),
    Supported,
}

impl Serialize for TokenBinding {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            TokenBinding::Supported => {
                serialize_map!(
                    serializer,
                    &"status" => &"supported",
                )
            }
            TokenBinding::Present(ref v) => {
                serialize_map!(
                    serializer,
                    &"status" => "present",
                    &"id" => &v,
                )
            }
        }
    }
}

impl<'de> Deserialize<'de> for TokenBinding {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct TokenBindingVisitor;

        impl<'de> Visitor<'de> for TokenBindingVisitor {
            type Value = TokenBinding;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte string")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut id = None;
                let mut status = None;

                while let Some(key) = map.next_key()? {
                    match key {
                        "status" => {
                            status = Some(map.next_value()?);
                        }
                        "id" => {
                            id = Some(map.next_value()?);
                        }
                        k => {
                            return Err(M::Error::custom(format!("unexpected key: {k:?}")));
                        }
                    }
                }

                if let Some(stat) = status {
                    match stat {
                        "present" => {
                            if let Some(id) = id {
                                Ok(TokenBinding::Present(id))
                            } else {
                                Err(SerdeError::missing_field("id"))
                            }
                        }
                        "supported" => Ok(TokenBinding::Supported),
                        k => Err(M::Error::custom(format!("unexpected status key: {k:?}"))),
                    }
                } else {
                    Err(SerdeError::missing_field("status"))
                }
            }
        }

        deserializer.deserialize_map(TokenBindingVisitor)
    }
}

/// https://w3c.github.io/webauthn/#dom-collectedclientdata-type
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum WebauthnType {
    Create,
    Get,
}

impl Serialize for WebauthnType {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match *self {
            WebauthnType::Create => serializer.serialize_str("webauthn.create"),
            WebauthnType::Get => serializer.serialize_str("webauthn.get"),
        }
    }
}

impl<'de> Deserialize<'de> for WebauthnType {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct WebauthnTypeVisitor;

        impl<'de> Visitor<'de> for WebauthnTypeVisitor {
            type Value = WebauthnType;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a string")
            }

            fn visit_str<E>(self, v: &str) -> Result<Self::Value, E>
            where
                E: de::Error,
            {
                match v {
                    "webauthn.create" => Ok(WebauthnType::Create),
                    "webauthn.get" => Ok(WebauthnType::Get),
                    _ => Err(E::custom("unexpected webauthn_type")),
                }
            }
        }

        deserializer.deserialize_str(WebauthnTypeVisitor)
    }
}

#[derive(Serialize, Deserialize, Clone, PartialEq, Eq, Debug)]
pub struct Challenge(pub String);

impl Challenge {
    pub fn new(input: Vec<u8>) -> Self {
        let value = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(input);
        Challenge(value)
    }
}

impl From<Vec<u8>> for Challenge {
    fn from(v: Vec<u8>) -> Challenge {
        Challenge::new(v)
    }
}

impl AsRef<[u8]> for Challenge {
    fn as_ref(&self) -> &[u8] {
        self.0.as_bytes()
    }
}

pub type Origin = String;

#[derive(Debug, Deserialize, Serialize, Clone, PartialEq, Eq)]
pub struct CollectedClientData {
    #[serde(rename = "type")]
    pub webauthn_type: WebauthnType,
    pub challenge: Challenge,
    pub origin: Origin,
    #[serde(rename = "crossOrigin", default)]
    pub cross_origin: bool,
    #[serde(rename = "tokenBinding", skip_serializing_if = "Option::is_none")]
    pub token_binding: Option<TokenBinding>,
}

impl CollectedClientData {
    pub fn hash(&self) -> Result<ClientDataHash, HIDError> {
        let json = json::to_vec(&self).map_err(CommandError::Json)?;
        let digest = Sha256::digest(json);
        Ok(ClientDataHash(digest.into()))
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ClientDataHash(pub [u8; 32]);

impl PartialEq<[u8]> for ClientDataHash {
    fn eq(&self, other: &[u8]) -> bool {
        self.0.eq(other)
    }
}

impl AsRef<[u8]> for ClientDataHash {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

impl Serialize for ClientDataHash {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_bytes(&self.0)
    }
}
