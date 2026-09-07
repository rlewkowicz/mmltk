// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::{MlsDecode, MlsEncode, MlsSize};
use alloc::vec::Vec;

macro_rules! impl_stdint {
    ($t:ty) => {
        impl MlsSize for $t {
            fn mls_encoded_len(&self) -> usize {
                core::mem::size_of::<$t>()
            }
        }

        impl MlsEncode for $t {
            fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
                writer.extend(self.to_be_bytes());
                Ok(())
            }
        }

        impl MlsDecode for $t {
            fn mls_decode(reader: &mut &[u8]) -> Result<Self, crate::Error> {
                MlsDecode::mls_decode(reader).map(<$t>::from_be_bytes)
            }
        }
    };
}

impl_stdint!(u8);
impl_stdint!(u16);
impl_stdint!(u32);
impl_stdint!(u64);
impl_stdint!(u128);
