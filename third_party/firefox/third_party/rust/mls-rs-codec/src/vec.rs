// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;

use crate::{MlsDecode, MlsEncode, MlsSize};

impl<T> MlsSize for [T]
where
    T: MlsSize,
{
    fn mls_encoded_len(&self) -> usize {
        crate::iter::mls_encoded_len(self.iter())
    }
}

impl<T> MlsSize for Vec<T>
where
    T: MlsSize,
{
    #[inline]
    fn mls_encoded_len(&self) -> usize {
        self.as_slice().mls_encoded_len()
    }
}

impl<T> MlsEncode for [T]
where
    T: MlsEncode,
{
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
        crate::iter::mls_encode(self.iter(), writer)
    }
}

impl<T> MlsEncode for Vec<T>
where
    T: MlsEncode,
{
    #[inline]
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
        self.as_slice().mls_encode(writer)
    }
}

impl<T> MlsDecode for Vec<T>
where
    T: MlsDecode,
{
    fn mls_decode(reader: &mut &[u8]) -> Result<Self, crate::Error> {
        crate::iter::mls_decode_collection(reader, |data| {
            let mut items = Vec::new();

            while !data.is_empty() {
                items.push(T::mls_decode(data)?);
            }

            Ok(items)
        })
    }
}
