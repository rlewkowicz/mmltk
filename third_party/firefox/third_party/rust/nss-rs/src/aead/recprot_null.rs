// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::fmt;

use super::{Mode, RecordProtectionOps, split_tag};
use crate::{Cipher, Error, Res, SymKey, Version, err::sec::SEC_ERROR_BAD_DATA};

pub const AEAD_NULL_TAG: &[u8] = &[0x0a; 16];

pub struct RecordProtection {}

impl RecordProtection {
    fn decrypt_check(_count: u64, _aad: &[u8], input: &[u8]) -> Res<usize> {
        let (len_encrypted, tag) = split_tag(input)?;
        if tag.as_slice() == AEAD_NULL_TAG
            && (len_encrypted == 0 || input[..len_encrypted].iter().any(|x| *x != 0x0))
        {
            Ok(len_encrypted)
        } else {
            Err(Error::from(SEC_ERROR_BAD_DATA))
        }
    }

    /// Create a new AEAD instance.
    ///
    /// # Errors
    ///
    /// Returns `Error` when the underlying crypto operations fail.
    #[expect(
        clippy::unnecessary_wraps,
        reason = "uniform interface with other backends"
    )]
    pub const fn new(
        _version: Version,
        _cipher: Cipher,
        _secret: &SymKey,
        _prefix: &str,
        _mode: Mode,
    ) -> Res<Self> {
        Ok(Self {})
    }
}

impl RecordProtectionOps for RecordProtection {
    fn expansion(&self) -> usize {
        AEAD_NULL_TAG.len()
    }

    fn encrypt<'a>(
        &self,
        _count: u64,
        _aad: &[u8],
        input: &[u8],
        output: &'a mut [u8],
    ) -> Res<&'a [u8]> {
        let l = input.len();
        let total = l
            .checked_add(self.expansion())
            .ok_or(Error::IntegerOverflow)?;
        if output.len() < total {
            return Err(Error::from(SEC_ERROR_BAD_DATA));
        }
        output[..l].copy_from_slice(input);
        output[l..total].copy_from_slice(AEAD_NULL_TAG);
        Ok(&output[..total])
    }

    fn encrypt_in_place(&self, _count: u64, _aad: &[u8], data: &mut [u8]) -> Res<usize> {
        if data.len() < self.expansion() {
            return Err(Error::from(SEC_ERROR_BAD_DATA));
        }
        let pos = data.len() - self.expansion();
        data[pos..].copy_from_slice(AEAD_NULL_TAG);
        Ok(data.len())
    }

    fn decrypt<'a>(
        &self,
        count: u64,
        aad: &[u8],
        input: &[u8],
        output: &'a mut [u8],
    ) -> Res<&'a [u8]> {
        Self::decrypt_check(count, aad, input).map(|len| {
            output[..len].copy_from_slice(&input[..len]);
            &output[..len]
        })
    }

    fn decrypt_in_place(&self, count: u64, aad: &[u8], data: &mut [u8]) -> Res<usize> {
        Self::decrypt_check(count, aad, data)
    }
}

impl fmt::Debug for RecordProtection {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "[NULL AEAD]")
    }
}
