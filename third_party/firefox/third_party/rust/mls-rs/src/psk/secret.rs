// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec;
use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};
use mls_rs_core::crypto::CipherSuiteProvider;
use zeroize::Zeroizing;

#[cfg(feature = "psk")]
use mls_rs_codec::MlsEncode;

#[cfg(feature = "psk")]
use mls_rs_core::{error::IntoAnyError, psk::PreSharedKey};

#[cfg(feature = "psk")]
use crate::{
    client::MlsError,
    group::key_schedule::kdf_expand_with_label,
    psk::{PSKLabel, PreSharedKeyID},
};

#[cfg(feature = "psk")]
#[derive(Clone)]
pub(crate) struct PskSecretInput {
    pub id: PreSharedKeyID,
    pub psk: PreSharedKey,
}

#[derive(PartialEq, Eq, Clone)]
pub(crate) struct PskSecret(Zeroizing<Vec<u8>>);

impl Debug for PskSecret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PskSecret").finish()
    }
}


impl Deref for PskSecret {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl PskSecret {
    pub(crate) fn new<P: CipherSuiteProvider>(provider: &P) -> PskSecret {
        PskSecret(Zeroizing::new(vec![0u8; provider.kdf_extract_size()]))
    }

    #[cfg(feature = "psk")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn calculate<P: CipherSuiteProvider>(
        input: &[PskSecretInput],
        cipher_suite_provider: &P,
    ) -> Result<PskSecret, MlsError> {
        let len = u16::try_from(input.len()).map_err(|_| MlsError::TooManyPskIds)?;
        let mut psk_secret = PskSecret::new(cipher_suite_provider);

        for (index, psk_secret_input) in input.iter().enumerate() {
            let index = index as u16;

            let label = PSKLabel {
                id: &psk_secret_input.id,
                index,
                count: len,
            };

            let psk_extracted = cipher_suite_provider
                .kdf_extract(
                    &vec![0; cipher_suite_provider.kdf_extract_size()],
                    &psk_secret_input.psk,
                )
                .await
                .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

            let psk_input = kdf_expand_with_label(
                cipher_suite_provider,
                &psk_extracted,
                b"derived psk",
                &label.mls_encode_to_vec()?,
                None,
            )
            .await?;

            psk_secret = cipher_suite_provider
                .kdf_extract(&psk_input, &psk_secret)
                .await
                .map(PskSecret)
                .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;
        }

        Ok(psk_secret)
    }
}
