// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};

use crate::CipherSuiteProvider;

const REUSE_GUARD_SIZE: usize = 4;

#[derive(Clone, Debug, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode)]
pub(crate) struct ReuseGuard([u8; REUSE_GUARD_SIZE]);

impl From<[u8; REUSE_GUARD_SIZE]> for ReuseGuard {
    fn from(value: [u8; REUSE_GUARD_SIZE]) -> Self {
        ReuseGuard(value)
    }
}

impl From<ReuseGuard> for [u8; REUSE_GUARD_SIZE] {
    fn from(value: ReuseGuard) -> Self {
        value.0
    }
}

impl AsRef<[u8]> for ReuseGuard {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

impl ReuseGuard {
    pub(crate) fn random<P: CipherSuiteProvider>(provider: &P) -> Result<Self, P::Error> {
        let mut data = [0u8; REUSE_GUARD_SIZE];
        provider.random_bytes(&mut data).map(|_| ReuseGuard(data))
    }

    pub(crate) fn apply(&self, nonce: &[u8]) -> Vec<u8> {
        let mut new_nonce = nonce.to_vec();

        new_nonce
            .iter_mut()
            .zip(self.as_ref().iter())
            .for_each(|(nonce_byte, guard_byte)| *nonce_byte ^= guard_byte);

        new_nonce
    }
}
