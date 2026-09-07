// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use core::fmt::Debug;

use mls_rs_core::{crypto::CipherSuite, error::IntoAnyError};
use mls_rs_crypto_traits::{AeadId, AeadType, AES_TAG_LEN};

use alloc::vec::Vec;

#[derive(Debug)]
#[cfg_attr(feature = "std", derive(thiserror::Error))]
pub enum AeadError {
    #[cfg_attr(feature = "std", error("NSS Error"))]
    NssError(nss_rs::Error),
#[cfg_attr(feature = "std", error("AEAD ciphertext of length {0} is too short to fit the tag"))]
InvalidCipherLen(usize),
    #[cfg_attr(feature = "std", error("encrypted message cannot be empty"))]
    EmptyPlaintext,
#[cfg_attr(feature = "std", error("AEAD key of invalid length {0}. Expected length {1}"))]
InvalidKeyLen(usize, usize),
    #[cfg_attr(feature = "std", error("unsupported cipher suite"))]
    UnsupportedCipherSuite,
}

impl From<nss_rs::Error> for AeadError {
    fn from(value: nss_rs::Error) -> Self {
        AeadError::NssError(value)
    }
}

impl IntoAnyError for AeadError {
    #[cfg(feature = "std")]
    fn into_dyn_error(self) -> Result<Box<dyn std::error::Error + Send + Sync>, Self> {
        Ok(self.into())
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Aead(AeadId);

impl Aead {
    pub fn new(cipher_suite: CipherSuite) -> Option<Self> {
        AeadId::new(cipher_suite).map(Self)
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl AeadType for Aead {
    type Error = AeadError;

    #[allow(clippy::needless_lifetimes)]
    async fn seal<'a>(
        &self,
        key: &[u8],
        data: &[u8],
        aad: Option<&'a [u8]>,
        nonce: &[u8],
    ) -> Result<Vec<u8>, AeadError> {
        nss_rs::init()?;
        let mode = nss_rs::aead::Mode::Encrypt;

        (!data.is_empty())
            .then_some(())
            .ok_or(AeadError::EmptyPlaintext)?;

        (key.len() == self.key_size())
            .then_some(())
            .ok_or_else(|| AeadError::InvalidKeyLen(key.len(), self.key_size()))?;

        match self.0 {
            AeadId::Aes128Gcm => {
                let alg = nss_rs::aead::AeadAlgorithms::Aes128Gcm;
                let key = nss_rs::aead::Aead::import_key(alg, key)?;
                let nonce_array: [u8; 12] =
                    nonce.try_into().expect("Nonce must be exactly 12 bytes");
                let aad_array = aad.unwrap_or(&[0; 0]);

                let mut cipher = nss_rs::aead::Aead::new(mode, alg, &key, nonce_array)?;

                let ciphertext = cipher
                    .encrypt(aad_array, data)
                    .map_err(|_| AeadError::NssError(nss_rs::Error::Aead))?;

                Ok(ciphertext)
            }
            AeadId::Aes256Gcm => {
                let alg = nss_rs::aead::AeadAlgorithms::Aes256Gcm;
                let key = nss_rs::aead::Aead::import_key(alg, key)?;
                let nonce_array: [u8; 12] =
                    nonce.try_into().expect("Nonce must be exactly 12 bytes");
                let aad_array = aad.unwrap_or(&[0; 0]);

                let mut cipher = nss_rs::aead::Aead::new(mode, alg, &key, nonce_array)?;

                let ciphertext = cipher
                    .encrypt(aad_array, data)
                    .map_err(|_| AeadError::NssError(nss_rs::Error::Aead))?;

                Ok(ciphertext)
            }
            AeadId::Chacha20Poly1305 => {
                let alg = nss_rs::aead::AeadAlgorithms::ChaCha20Poly1305;
                let key = nss_rs::aead::Aead::import_key(alg, key)?;
                let nonce_array: [u8; 12] =
                    nonce.try_into().expect("Nonce must be exactly 12 bytes");
                let aad_array = aad.unwrap_or(&[0; 0]);

                let mut cipher = nss_rs::aead::Aead::new(mode, alg, &key, nonce_array)?;

                let ciphertext = cipher
                    .encrypt(aad_array, data)
                    .map_err(|_| AeadError::NssError(nss_rs::Error::Aead))?;

                Ok(ciphertext)
            }
            _ => Err(AeadError::UnsupportedCipherSuite),
        }
    }

    #[allow(clippy::needless_lifetimes)]
    async fn open<'a>(
        &self,
        key: &[u8],
        ciphertext: &[u8],
        aad: Option<&'a [u8]>,
        nonce: &[u8],
    ) -> Result<Vec<u8>, AeadError> {
        nss_rs::init()?;
        let mode = nss_rs::aead::Mode::Decrypt;

        (ciphertext.len() > AES_TAG_LEN)
            .then_some(())
            .ok_or(AeadError::InvalidCipherLen(ciphertext.len()))?;

        (key.len() == self.key_size())
            .then_some(())
            .ok_or_else(|| AeadError::InvalidKeyLen(key.len(), self.key_size()))?;

        match self.0 {
            AeadId::Aes128Gcm => {
                let alg = nss_rs::aead::AeadAlgorithms::Aes128Gcm;
                let key = nss_rs::aead::Aead::import_key(alg, key)?;
                let nonce_array: [u8; 12] =
                    nonce.try_into().expect("Nonce must be exactly 12 bytes");
                let aad_array = aad.unwrap_or(&[0; 0]);

                let mut cipher = nss_rs::aead::Aead::new(mode, alg, &key, nonce_array)?;

                let plaintext = cipher
                    .decrypt(aad_array, 0, ciphertext)
                    .map_err(|_| AeadError::NssError(nss_rs::Error::Aead))?;

                Ok(plaintext)
            }
            AeadId::Aes256Gcm => {
                let alg = nss_rs::aead::AeadAlgorithms::Aes256Gcm;
                let key = nss_rs::aead::Aead::import_key(alg, key)?;
                let nonce_array: [u8; 12] =
                    nonce.try_into().expect("Nonce must be exactly 12 bytes");
                let aad_array = aad.unwrap_or(&[0; 0]);

                let mut cipher = nss_rs::aead::Aead::new(mode, alg, &key, nonce_array)?;

                let plaintext = cipher
                    .decrypt(aad_array, 0, ciphertext)
                    .map_err(|_| AeadError::NssError(nss_rs::Error::Aead))?;

                Ok(plaintext)
            }
            AeadId::Chacha20Poly1305 => {
                let alg = nss_rs::aead::AeadAlgorithms::ChaCha20Poly1305;
                let key = nss_rs::aead::Aead::import_key(alg, key)?;
                let nonce_array: [u8; 12] =
                    nonce.try_into().expect("Nonce must be exactly 12 bytes");
                let aad_array = aad.unwrap_or(&[0; 0]);

                let mut cipher = nss_rs::aead::Aead::new(mode, alg, &key, nonce_array)?;

                let plaintext = cipher
                    .decrypt(aad_array, 0, ciphertext)
                    .map_err(|_| AeadError::NssError(nss_rs::Error::Aead))?;

                Ok(plaintext)
            }
            _ => Err(AeadError::UnsupportedCipherSuite),
        }
    }

    #[inline(always)]
    fn key_size(&self) -> usize {
        self.0.key_size()
    }

    fn nonce_size(&self) -> usize {
        self.0.nonce_size()
    }

    fn aead_id(&self) -> u16 {
        self.0 as u16
    }
}
