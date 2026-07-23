// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsEncode, MlsSize};
use mls_rs_core::{
    crypto::{CipherSuiteProvider, HpkeCiphertext, HpkePublicKey, HpkeSecretKey},
    error::IntoAnyError,
};
use zeroize::Zeroizing;

use crate::client::MlsError;

#[derive(Clone, MlsSize, MlsEncode)]
struct EncryptContext<'a> {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    label: Vec<u8>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    context: &'a [u8],
}

impl Debug for EncryptContext<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("EncryptContext")
            .field("label", &mls_rs_core::debug::pretty_bytes(&self.label))
            .field("context", &mls_rs_core::debug::pretty_bytes(self.context))
            .finish()
    }
}

impl<'a> EncryptContext<'a> {
    pub fn new(label: &str, context: &'a [u8]) -> Self {
        Self {
            label: [b"MLS 1.0 ", label.as_bytes()].concat(),
            context,
        }
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
pub(crate) trait HpkeEncryptable: Sized {
    const ENCRYPT_LABEL: &'static str;

    async fn encrypt<P: CipherSuiteProvider>(
        &self,
        cipher_suite_provider: &P,
        public_key: &HpkePublicKey,
        context: &[u8],
    ) -> Result<HpkeCiphertext, MlsError> {
        let context = EncryptContext::new(Self::ENCRYPT_LABEL, context)
            .mls_encode_to_vec()
            .map(Zeroizing::new)?;

        let content = self.get_bytes().map(Zeroizing::new)?;

        cipher_suite_provider
            .hpke_seal(public_key, &context, None, &content)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    async fn decrypt<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        secret_key: &HpkeSecretKey,
        public_key: &HpkePublicKey,
        context: &[u8],
        ciphertext: &HpkeCiphertext,
    ) -> Result<Self, MlsError> {
        let context = EncryptContext::new(Self::ENCRYPT_LABEL, context).mls_encode_to_vec()?;

        let plaintext = cipher_suite_provider
            .hpke_open(ciphertext, secret_key, public_key, &context, None)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        Self::from_bytes(plaintext.to_vec())
    }

    fn from_bytes(bytes: Vec<u8>) -> Result<Self, MlsError>;
    fn get_bytes(&self) -> Result<Vec<u8>, MlsError>;
}
