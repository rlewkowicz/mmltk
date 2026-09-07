// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::{MlsDecode, MlsEncode, MlsSize};
use alloc::vec::Vec;

impl<T: MlsSize> MlsSize for Option<T> {
    #[inline]
    fn mls_encoded_len(&self) -> usize {
        1 + match self {
            Some(v) => v.mls_encoded_len(),
            None => 0,
        }
    }
}

impl<T: MlsEncode> MlsEncode for Option<T> {
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
        if let Some(item) = self {
            writer.push(1);
            item.mls_encode(writer)
        } else {
            writer.push(0);
            Ok(())
        }
    }
}

impl<T: MlsDecode> MlsDecode for Option<T> {
    fn mls_decode(reader: &mut &[u8]) -> Result<Self, crate::Error> {
        match u8::mls_decode(reader)? {
            0 => Ok(None),
            1 => T::mls_decode(reader).map(Some),
            n => Err(crate::Error::OptionOutOfRange(n)),
        }
    }
}
