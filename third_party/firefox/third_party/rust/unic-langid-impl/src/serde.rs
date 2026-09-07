use crate::LanguageIdentifier;
use serde::{Deserialize, Deserializer, Serialize, Serializer};

impl Serialize for LanguageIdentifier {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&self.to_string())
    }
}

impl<'de> Deserialize<'de> for LanguageIdentifier {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct LanguageIdentifierVisitor;

        impl<'de> serde::de::Visitor<'de> for LanguageIdentifierVisitor {
            type Value = LanguageIdentifier;

            fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                write!(formatter, "A valid Unicode Language Identifier")
            }

            fn visit_str<E>(self, s: &str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                s.parse::<LanguageIdentifier>()
                    .map_err(serde::de::Error::custom)
            }
        }

        deserializer.deserialize_string(LanguageIdentifierVisitor)
    }
}
