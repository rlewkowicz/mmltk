// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::crypto::{CipherSuiteProvider, HpkePublicKey, HpkeSecretKey};
use crate::group::key_schedule::kdf_derive_secret;
use alloc::vec;
use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
use zeroize::Zeroizing;

use super::hpke_encryption::HpkeEncryptable;

#[derive(Clone, Eq, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct PathSecret(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    Zeroizing<Vec<u8>>,
);

impl Debug for PathSecret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PathSecret").finish()
    }
}

impl Deref for PathSecret {
    type Target = Vec<u8>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl From<Vec<u8>> for PathSecret {
    fn from(data: Vec<u8>) -> Self {
        PathSecret(Zeroizing::new(data))
    }
}

impl From<Zeroizing<Vec<u8>>> for PathSecret {
    fn from(data: Zeroizing<Vec<u8>>) -> Self {
        PathSecret(data)
    }
}

impl PathSecret {
    pub fn random<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
    ) -> Result<PathSecret, MlsError> {
        cipher_suite_provider
            .random_bytes_vec(cipher_suite_provider.kdf_extract_size())
            .map(Into::into)
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    pub fn empty<P: CipherSuiteProvider>(cipher_suite_provider: &P) -> Self {
        PathSecret::from(vec![0u8; cipher_suite_provider.kdf_extract_size()])
    }
}

impl HpkeEncryptable for PathSecret {
    const ENCRYPT_LABEL: &'static str = "UpdatePathNode";

    fn from_bytes(bytes: Vec<u8>) -> Result<Self, MlsError> {
        Ok(Self(Zeroizing::new(bytes)))
    }

    fn get_bytes(&self) -> Result<Vec<u8>, MlsError> {
        Ok(self.to_vec())
    }
}

impl PathSecret {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn to_hpke_key_pair<P: CipherSuiteProvider>(
        &self,
        cs: &P,
    ) -> Result<(HpkeSecretKey, HpkePublicKey), MlsError> {
        let node_secret = Zeroizing::new(kdf_derive_secret(cs, self, b"node").await?);

        cs.kem_derive(&node_secret)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }
}

#[derive(Clone, Debug)]
pub struct PathSecretGenerator<'a, P> {
    cipher_suite_provider: &'a P,
    last: Option<PathSecret>,
    starting_with: Option<PathSecret>,
}

impl<'a, P: CipherSuiteProvider> PathSecretGenerator<'a, P> {
    pub fn new(cipher_suite_provider: &'a P) -> Self {
        Self {
            cipher_suite_provider,
            last: None,
            starting_with: None,
        }
    }

    pub fn starting_with(cipher_suite_provider: &'a P, secret: PathSecret) -> Self {
        Self {
            starting_with: Some(secret),
            ..Self::new(cipher_suite_provider)
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn next_secret(&mut self) -> Result<PathSecret, MlsError> {
        let secret = if let Some(starting_with) = self.starting_with.take() {
            Ok(starting_with)
        } else if let Some(last) = self.last.take() {
            kdf_derive_secret(self.cipher_suite_provider, &last, b"path")
                .await
                .map(PathSecret::from)
        } else {
            PathSecret::random(self.cipher_suite_provider)
        }?;

        self.last = Some(secret.clone());

        Ok(secret)
    }
}
