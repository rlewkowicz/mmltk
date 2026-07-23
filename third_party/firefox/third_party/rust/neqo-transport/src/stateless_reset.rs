// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Stateless Reset Token implementation.

use neqo_common::Decoder;
use nss::random;

use crate::Error;

/// A stateless reset token is a 16-byte value that is used to identify
/// a stateless reset packet.
#[derive(Clone, Debug, Default, Eq)]
pub struct Token([u8; Self::LEN]);

impl Token {
    pub const LEN: usize = 16;

    /// Create a new stateless reset token from a byte array.
    #[must_use]
    pub const fn new(token: [u8; Self::LEN]) -> Self {
        Self(token)
    }

    /// Generate a random stateless reset token.
    #[must_use]
    pub fn random() -> Self {
        Self(random::<{ Self::LEN }>())
    }

    /// Get the token as a byte array.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; Self::LEN] {
        &self.0
    }
}

/// Compare two tokens in constant time to prevent timing attacks.
impl PartialEq for Token {
    fn eq(&self, other: &Self) -> bool {
        let mut c = 0;
        for (&a, &b) in self.0.iter().zip(&other.0) {
            c |= a ^ b;
        }
        c == 0
    }
}

impl TryFrom<&[u8]> for Token {
    type Error = Error;

    fn try_from(value: &[u8]) -> Result<Self, Self::Error> {
        Ok(Self(value.try_into()?))
    }
}

impl TryFrom<&mut Decoder<'_>> for Token {
    type Error = Error;

    fn try_from(d: &mut Decoder<'_>) -> Result<Self, Self::Error> {
        Ok(Self(
            d.decode(Self::LEN).ok_or(Error::NoMoreData)?.try_into()?,
        ))
    }
}

impl AsRef<[u8]> for Token {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}
