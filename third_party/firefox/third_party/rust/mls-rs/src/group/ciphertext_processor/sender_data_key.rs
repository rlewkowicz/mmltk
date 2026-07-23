// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
use zeroize::Zeroizing;

use crate::{
    client::MlsError,
    crypto::CipherSuiteProvider,
    group::{epoch::SenderDataSecret, framing::ContentType, key_schedule::kdf_expand_with_label},
    tree_kem::node::LeafIndex,
};

use super::ReuseGuard;

#[derive(Clone, Debug, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode)]
pub(crate) struct SenderData {
    pub sender: LeafIndex,
    pub generation: u32,
    pub reuse_guard: ReuseGuard,
}

#[derive(Clone, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode)]
pub(crate) struct SenderDataAAD {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    pub group_id: Vec<u8>,
    pub epoch: u64,
    pub content_type: ContentType,
}

impl Debug for SenderDataAAD {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SenderDataAAD")
            .field(
                "group_id",
                &mls_rs_core::debug::pretty_group_id(&self.group_id),
            )
            .field("epoch", &self.epoch)
            .field("content_type", &self.content_type)
            .finish()
    }
}

pub(crate) struct SenderDataKey<'a, CP: CipherSuiteProvider> {
    pub(crate) key: Zeroizing<Vec<u8>>,
    pub(crate) nonce: Zeroizing<Vec<u8>>,
    cipher_suite_provider: &'a CP,
}

impl<CP: CipherSuiteProvider + Debug> Debug for SenderDataKey<'_, CP> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SenderDataKey").finish()
    }
}

impl<'a, CP: CipherSuiteProvider> SenderDataKey<'a, CP> {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(super) async fn new(
        sender_data_secret: &SenderDataSecret,
        ciphertext: &[u8],
        cipher_suite_provider: &'a CP,
    ) -> Result<SenderDataKey<'a, CP>, MlsError> {
        let extract_size = cipher_suite_provider.kdf_extract_size();
        let ciphertext_sample = ciphertext.get(0..extract_size).unwrap_or(ciphertext);

        let key = kdf_expand_with_label(
            cipher_suite_provider,
            sender_data_secret,
            b"key",
            ciphertext_sample,
            Some(cipher_suite_provider.aead_key_size()),
        )
        .await?;

        let nonce = kdf_expand_with_label(
            cipher_suite_provider,
            sender_data_secret,
            b"nonce",
            ciphertext_sample,
            Some(cipher_suite_provider.aead_nonce_size()),
        )
        .await?;

        Ok(Self {
            key,
            nonce,
            cipher_suite_provider,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn seal(
        &self,
        sender_data: &SenderData,
        aad: &SenderDataAAD,
    ) -> Result<Vec<u8>, MlsError> {
        self.cipher_suite_provider
            .aead_seal(
                &self.key,
                &sender_data.mls_encode_to_vec()?,
                Some(&aad.mls_encode_to_vec()?),
                &self.nonce,
            )
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn open(
        &self,
        sender_data: &[u8],
        aad: &SenderDataAAD,
    ) -> Result<SenderData, MlsError> {
        self.cipher_suite_provider
            .aead_open(
                &self.key,
                sender_data,
                Some(&aad.mls_encode_to_vec()?),
                &self.nonce,
            )
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
            .and_then(|data| SenderData::mls_decode(&mut &**data).map_err(From::from))
    }
}
