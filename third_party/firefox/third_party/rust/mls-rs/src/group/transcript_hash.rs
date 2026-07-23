// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};

use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::{
    crypto::CipherSuiteProvider, error::IntoAnyError, group::ConfirmedTranscriptHash,
};

use crate::{
    client::MlsError,
    group::{framing::FramedContent, MessageSignature},
    WireFormat,
};

use super::{AuthenticatedContent, ConfirmationTag};

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn create<P: CipherSuiteProvider>(
    cipher_suite_provider: &P,
    interim_transcript_hash: &InterimTranscriptHash,
    content: &AuthenticatedContent,
) -> Result<ConfirmedTranscriptHash, MlsError> {
    #[derive(Debug, MlsSize, MlsEncode)]
    struct ConfirmedTranscriptHashInput<'a> {
        wire_format: WireFormat,
        content: &'a FramedContent,
        signature: &'a MessageSignature,
    }

    let input = ConfirmedTranscriptHashInput {
        wire_format: content.wire_format,
        content: &content.content,
        signature: &content.auth.signature,
    };

    let hash_input = [
        interim_transcript_hash.deref(),
        input.mls_encode_to_vec()?.deref(),
    ]
    .concat();

    cipher_suite_provider
        .hash(&hash_input)
        .await
        .map(Into::into)
        .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
}

#[derive(Clone, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub(crate) struct InterimTranscriptHash(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    Vec<u8>,
);

impl Debug for InterimTranscriptHash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("InterimTranscriptHash")
            .fmt(f)
    }
}

impl Deref for InterimTranscriptHash {
    type Target = Vec<u8>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl From<Vec<u8>> for InterimTranscriptHash {
    fn from(value: Vec<u8>) -> Self {
        Self(value)
    }
}

impl InterimTranscriptHash {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn create<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        confirmed: &ConfirmedTranscriptHash,
        confirmation_tag: &ConfirmationTag,
    ) -> Result<Self, MlsError> {
        #[derive(Debug, MlsSize, MlsEncode)]
        struct InterimTranscriptHashInput<'a> {
            confirmation_tag: &'a ConfirmationTag,
        }

        let input = InterimTranscriptHashInput { confirmation_tag }.mls_encode_to_vec()?;

        cipher_suite_provider
            .hash(&[&confirmed[..], &input].concat())
            .await
            .map(Into::into)
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }
}

