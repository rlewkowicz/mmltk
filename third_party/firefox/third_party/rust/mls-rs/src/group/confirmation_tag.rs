// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::CipherSuiteProvider;
use crate::{client::MlsError, group::ConfirmedTranscriptHash};
use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
use subtle::ConstantTimeEq;

#[derive(Clone, MlsSize, MlsEncode, MlsDecode, Default)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct ConfirmationTag(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    Vec<u8>,
);

impl PartialEq for ConfirmationTag {
    fn eq(&self, other: &Self) -> bool {
        self.0.ct_eq(&other.0).into()
    }
}

impl Debug for ConfirmationTag {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("ConfirmationTag")
            .fmt(f)
    }
}

impl Deref for ConfirmationTag {
    type Target = Vec<u8>;
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl ConfirmationTag {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn create<P: CipherSuiteProvider>(
        confirmation_key: &[u8],
        confirmed_transcript_hash: &ConfirmedTranscriptHash,
        cipher_suite_provider: &P,
    ) -> Result<Self, MlsError> {
        cipher_suite_provider
            .mac(confirmation_key, confirmed_transcript_hash)
            .await
            .map(ConfirmationTag)
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn matches<P: CipherSuiteProvider>(
        &self,
        confirmation_key: &[u8],
        confirmed_transcript_hash: &ConfirmedTranscriptHash,
        cipher_suite_provider: &P,
    ) -> Result<bool, MlsError> {
        let tag = ConfirmationTag::create(
            confirmation_key,
            confirmed_transcript_hash,
            cipher_suite_provider,
        )
        .await?;

        Ok(&tag == self)
    }
}
