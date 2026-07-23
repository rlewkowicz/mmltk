// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use core::{
    fmt::{self, Debug},
    ops::Deref,
};

use crate::client::MlsError;
use crate::CipherSuiteProvider;
use alloc::vec::Vec;
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;

#[derive(MlsSize, MlsEncode)]
struct RefHashInput<'a> {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    pub label: &'a [u8],
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    pub value: &'a [u8],
}

impl Debug for RefHashInput<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("RefHashInput")
            .field("label", &mls_rs_core::debug::pretty_bytes(self.label))
            .field("value", &mls_rs_core::debug::pretty_bytes(self.value))
            .finish()
    }
}

#[derive(PartialEq, Eq, PartialOrd, Ord, Hash, Clone, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct HashReference(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    Vec<u8>,
);

impl Debug for HashReference {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("HashReference")
            .fmt(f)
    }
}

impl Deref for HashReference {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl AsRef<[u8]> for HashReference {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

impl From<Vec<u8>> for HashReference {
    fn from(val: Vec<u8>) -> Self {
        Self(val)
    }
}

impl HashReference {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn compute<P: CipherSuiteProvider>(
        value: &[u8],
        label: &[u8],
        cipher_suite: &P,
    ) -> Result<HashReference, MlsError> {
        let input = RefHashInput { label, value };
        let input_bytes = input.mls_encode_to_vec()?;

        cipher_suite
            .hash(&input_bytes)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
            .map(HashReference)
    }
}
