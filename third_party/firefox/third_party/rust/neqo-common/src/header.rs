// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::str::FromStr;

use thiserror::Error;

#[derive(Debug, PartialEq, PartialOrd, Eq, Ord, Clone)]
pub struct Header {
    name: String,
    /// The raw header field value as bytes.
    ///
    /// HTTP allows field values to contain any visible ASCII characters and
    /// arbitrary 0x80–0xFF bytes (`obs-text`).  Unlike field *names*, field
    /// values are not guaranteed to be valid UTF-8.
    ///
    /// See also <https://www.rfc-editor.org/rfc/rfc9110#section-5.5>.
    value: Vec<u8>,
}

impl Header {
    pub fn new<N, V>(name: N, value: V) -> Self
    where
        N: Into<String>,
        V: Into<Vec<u8>>,
    {
        Self {
            name: name.into(),
            value: value.into(),
        }
    }

    #[must_use]
    pub fn is_allowed_for_response(&self) -> bool {
        !matches!(
            self.name.as_str(),
            "connection"
                | "host"
                | "keep-alive"
                | "proxy-connection"
                | "te"
                | "transfer-encoding"
                | "upgrade"
        )
    }

    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub fn value(&self) -> &[u8] {
        &self.value
    }

    /// Try to interpret the header value as UTF-8.
    ///
    /// # Errors
    ///
    /// Returns an error if the value contains invalid UTF-8.
    pub fn value_utf8(&self) -> Result<&str, std::str::Utf8Error> {
        std::str::from_utf8(&self.value)
    }
}

impl<T: AsRef<str>, U: AsRef<[u8]>> PartialEq<(T, U)> for Header {
    fn eq(&self, other: &(T, U)) -> bool {
        self.name == other.0.as_ref() && self.value == other.1.as_ref()
    }
}

pub trait HeadersExt<'h> {
    fn contains_header<T: AsRef<str>, U: AsRef<[u8]>>(self, name: T, value: U) -> bool;
    fn find_header<T: AsRef<str> + 'h>(self, name: T) -> Option<&'h Header>;
}

impl<'h, H> HeadersExt<'h> for H
where
    H: IntoIterator<Item = &'h Header> + 'h,
{
    fn contains_header<T: AsRef<str>, U: AsRef<[u8]>>(self, name: T, value: U) -> bool {
        let (name, value) = (name.as_ref(), value.as_ref());
        self.into_iter().any(|h| h == &(name, value))
    }

    fn find_header<T: AsRef<str> + 'h>(self, name: T) -> Option<&'h Header> {
        let name = name.as_ref();
        self.into_iter().find(|h| h.name == name)
    }
}

#[derive(Debug, PartialEq, Eq, Error)]
pub enum FromStrError {
    #[error("Header string missing colon")]
    MissingColon,
    #[error("Header string missing name")]
    MissingName,
}

impl FromStr for Header {
    type Err = FromStrError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let (seperator, _) = s
            .match_indices(':')
            .find(|(i, _)| *i != 0)
            .ok_or(FromStrError::MissingColon)?;

        let name = s[..seperator].trim().to_ascii_lowercase();
        if name.is_empty() {
            return Err(FromStrError::MissingName);
        }

        let value = s[seperator + 1..].trim();

        Ok(Self::new(name, value))
    }
}
