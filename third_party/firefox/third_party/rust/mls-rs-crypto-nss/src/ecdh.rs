// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use core::ops::Deref;

use alloc::vec::Vec;

use mls_rs_core::{
    crypto::{CipherSuite, HpkePublicKey, HpkeSecretKey},
    error::IntoAnyError,
};
use mls_rs_crypto_traits::{Curve, DhType, SamplingMethod};

use crate::ec::{
    generate_keypair, private_key_bytes_to_public, private_key_ecdh, private_key_from_bytes,
    pub_key_from_uncompressed, EcError, EcPublicKey,
};

#[derive(Debug)]
#[cfg_attr(feature = "std", derive(thiserror::Error))]
pub enum EcdhKemError {
    #[cfg_attr(feature = "std", error(transparent))]
    EcError(EcError),
    #[cfg_attr(feature = "std", error("unsupported cipher suite"))]
    UnsupportedCipherSuite,
}

impl From<EcError> for EcdhKemError {
    fn from(e: EcError) -> Self {
        EcdhKemError::EcError(e)
    }
}

impl IntoAnyError for EcdhKemError {
    #[cfg(feature = "std")]
    fn into_dyn_error(self) -> Result<Box<dyn std::error::Error + Send + Sync>, Self> {
        Ok(self.into())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Ecdh(Curve);

impl Deref for Ecdh {
    type Target = Curve;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl Ecdh {
    pub fn new(cipher_suite: CipherSuite) -> Option<Self> {
        Curve::from_ciphersuite(cipher_suite, false).map(Self)
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl DhType for Ecdh {
    type Error = EcdhKemError;

    async fn dh(
        &self,
        secret_key: &HpkeSecretKey,
        public_key: &HpkePublicKey,
    ) -> Result<Vec<u8>, Self::Error> {
        Ok(private_key_ecdh(
            &private_key_from_bytes(secret_key.to_vec(), self.0)?,
            &self.to_ec_public_key(public_key)?,
        )?)
    }
    async fn to_public(&self, secret_key: &HpkeSecretKey) -> Result<HpkePublicKey, Self::Error> {
        Ok(private_key_bytes_to_public(secret_key.to_vec(), self.0)?.into())
    }

    async fn generate(&self) -> Result<(HpkeSecretKey, HpkePublicKey), Self::Error> {
        let key_pair = generate_keypair(self.0)?;
        Ok((key_pair.secret.into(), key_pair.public.into()))
    }

    fn bitmask_for_rejection_sampling(&self) -> SamplingMethod {
        self.0.hpke_sampling_method()
    }

    fn public_key_validate(&self, key: &HpkePublicKey) -> Result<(), Self::Error> {
        self.to_ec_public_key(key).map(|_| ())
    }

    fn secret_key_size(&self) -> usize {
        self.0.secret_key_size()
    }

    fn public_key_size(&self) -> usize {
        self.0.public_key_size()
    }
}

impl Ecdh {
    fn to_ec_public_key(&self, public_key: &HpkePublicKey) -> Result<EcPublicKey, EcdhKemError> {
        Ok(pub_key_from_uncompressed(public_key.to_vec(), self.0)?)
    }
}
