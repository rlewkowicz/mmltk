// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;

use crate::client::MlsError;
use crate::crypto::{CipherSuiteProvider, SignaturePublicKey, SignatureSecretKey};

#[derive(Clone, MlsSize, MlsEncode)]
struct SignContent {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    label: Vec<u8>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    content: Vec<u8>,
}

impl Debug for SignContent {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SignContent")
            .field("label", &mls_rs_core::debug::pretty_bytes(&self.label))
            .field("content", &mls_rs_core::debug::pretty_bytes(&self.content))
            .finish()
    }
}

impl SignContent {
    pub fn new(label: &str, content: Vec<u8>) -> Self {
        Self {
            label: [b"MLS 1.0 ", label.as_bytes()].concat(),
            content,
        }
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
pub(crate) trait Signable<'a> {
    const SIGN_LABEL: &'static str;

    type SigningContext: Send + Sync;

    fn signature(&self) -> &[u8];

    fn signable_content(
        &self,
        context: &Self::SigningContext,
    ) -> Result<Vec<u8>, mls_rs_codec::Error>;

    fn write_signature(&mut self, signature: Vec<u8>);

    async fn sign<P: CipherSuiteProvider>(
        &mut self,
        signature_provider: &P,
        signer: &SignatureSecretKey,
        context: &Self::SigningContext,
    ) -> Result<(), MlsError> {
        let sign_content = SignContent::new(Self::SIGN_LABEL, self.signable_content(context)?);

        let signature = signature_provider
            .sign(signer, &sign_content.mls_encode_to_vec()?)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        self.write_signature(signature);

        Ok(())
    }

    async fn verify<P: CipherSuiteProvider>(
        &self,
        signature_provider: &P,
        public_key: &SignaturePublicKey,
        context: &Self::SigningContext,
    ) -> Result<(), MlsError> {
        let sign_content = SignContent::new(Self::SIGN_LABEL, self.signable_content(context)?);

        signature_provider
            .verify(
                public_key,
                self.signature(),
                &sign_content.mls_encode_to_vec()?,
            )
            .await
            .map_err(|_| MlsError::InvalidSignature)
    }
}
