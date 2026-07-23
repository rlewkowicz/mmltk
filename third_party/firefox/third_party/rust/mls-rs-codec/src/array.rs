// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::{MlsDecode, MlsEncode, MlsSize};
use alloc::vec::Vec;

impl<const N: usize> MlsSize for [u8; N] {
    #[inline(always)]
    fn mls_encoded_len(&self) -> usize {
        N
    }
}

impl<const N: usize> MlsEncode for [u8; N] {
    #[inline(always)]
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
        writer.extend_from_slice(self);
        Ok(())
    }
}

impl<const N: usize> MlsDecode for [u8; N] {
    fn mls_decode(reader: &mut &[u8]) -> Result<Self, crate::Error> {
        let array = reader
            .get(..N)
            .and_then(|head| head.try_into().ok())
            .ok_or(crate::Error::UnexpectedEOF)?;

        *reader = &reader[N..];
        Ok(array)
    }
}
