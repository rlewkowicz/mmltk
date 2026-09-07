// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::extension::ExternalPubExt;
use crate::group::{GroupContext, MembershipTag};
use crate::psk::secret::PskSecret;
#[cfg(feature = "psk")]
use crate::psk::PreSharedKey;
use crate::tree_kem::path_secret::PathSecret;
use crate::CipherSuiteProvider;

#[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
use crate::group::SecretTree;

use alloc::vec;
use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
use zeroize::Zeroizing;

use crate::crypto::{HpkeContextR, HpkeContextS, HpkePublicKey, HpkeSecretKey};

use super::epoch::{EpochSecrets, SenderDataSecret};
use super::message_signature::AuthenticatedContent;

#[derive(Clone, PartialEq, Eq, Default, MlsEncode, MlsDecode, MlsSize)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct KeySchedule {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    exporter_secret: Zeroizing<Vec<u8>>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    pub authentication_secret: Zeroizing<Vec<u8>>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    external_secret: Zeroizing<Vec<u8>>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    pub(crate) membership_key: Zeroizing<Vec<u8>>,
    init_secret: InitSecret,
}

impl Debug for KeySchedule {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("KeySchedule").finish()
    }
}

pub(crate) struct KeyScheduleDerivationResult {
    pub(crate) key_schedule: KeySchedule,
    pub(crate) confirmation_key: Zeroizing<Vec<u8>>,
    pub(crate) joiner_secret: JoinerSecret,
    pub(crate) epoch_secrets: EpochSecrets,
}

impl KeySchedule {
    pub fn new(init_secret: InitSecret) -> Self {
        KeySchedule {
            init_secret,
            ..Default::default()
        }
    }

    pub fn delete_exporter(&mut self) {
        self.exporter_secret = Default::default();
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn derive_for_external<P: CipherSuiteProvider>(
        &self,
        kem_output: &[u8],
        cipher_suite: &P,
    ) -> Result<KeySchedule, MlsError> {
        let (secret, public) = self.get_external_key_pair(cipher_suite).await?;

        let init_secret =
            InitSecret::decode_for_external(cipher_suite, kem_output, &secret, &public).await?;

        Ok(KeySchedule::new(init_secret))
    }

    /// Returns the derived epoch as well as the joiner secret required for building welcome
    /// messages
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn from_key_schedule<P: CipherSuiteProvider>(
        last_key_schedule: &KeySchedule,
        commit_secret: &PathSecret,
        context: &GroupContext,
        #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
        secret_tree_size: u32,
        psk_secret: &PskSecret,
        cipher_suite_provider: &P,
    ) -> Result<KeyScheduleDerivationResult, MlsError> {
        let joiner_seed = cipher_suite_provider
            .kdf_extract(&last_key_schedule.init_secret.0, commit_secret)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        let joiner_secret = kdf_expand_with_label(
            cipher_suite_provider,
            &joiner_seed,
            b"joiner",
            &context.mls_encode_to_vec()?,
            None,
        )
        .await?
        .into();

        let key_schedule_result = Self::from_joiner(
            cipher_suite_provider,
            &joiner_secret,
            context,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            secret_tree_size,
            psk_secret,
        )
        .await?;

        Ok(KeyScheduleDerivationResult {
            key_schedule: key_schedule_result.key_schedule,
            confirmation_key: key_schedule_result.confirmation_key,
            joiner_secret,
            epoch_secrets: key_schedule_result.epoch_secrets,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn from_joiner<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        joiner_secret: &JoinerSecret,
        context: &GroupContext,
        #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
        secret_tree_size: u32,
        psk_secret: &PskSecret,
    ) -> Result<KeyScheduleDerivationResult, MlsError> {
        let epoch_seed =
            get_pre_epoch_secret(cipher_suite_provider, psk_secret, joiner_secret).await?;
        let context = context.mls_encode_to_vec()?;

        let epoch_secret =
            kdf_expand_with_label(cipher_suite_provider, &epoch_seed, b"epoch", &context, None)
                .await?;

        Self::from_epoch_secret(
            cipher_suite_provider,
            &epoch_secret,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            secret_tree_size,
        )
        .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn from_random_epoch_secret<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
        secret_tree_size: u32,
    ) -> Result<KeyScheduleDerivationResult, MlsError> {
        let epoch_secret = cipher_suite_provider
            .random_bytes_vec(cipher_suite_provider.kdf_extract_size())
            .map(Zeroizing::new)
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        Self::from_epoch_secret(
            cipher_suite_provider,
            &epoch_secret,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            secret_tree_size,
        )
        .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn from_epoch_secret<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        epoch_secret: &[u8],
        #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
        secret_tree_size: u32,
    ) -> Result<KeyScheduleDerivationResult, MlsError> {
        let secrets_producer = SecretsProducer::new(cipher_suite_provider, epoch_secret);

        let epoch_secrets = EpochSecrets {
            #[cfg(feature = "psk")]
            resumption_secret: PreSharedKey::from(secrets_producer.derive(b"resumption").await?),
            sender_data_secret: SenderDataSecret::from(
                secrets_producer.derive(b"sender data").await?,
            ),
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            secret_tree: SecretTree::new(
                secret_tree_size,
                secrets_producer.derive(b"encryption").await?,
            ),
        };

        let key_schedule = Self {
            exporter_secret: secrets_producer.derive(b"exporter").await?,
            authentication_secret: secrets_producer.derive(b"authentication").await?,
            external_secret: secrets_producer.derive(b"external").await?,
            membership_key: secrets_producer.derive(b"membership").await?,
            init_secret: InitSecret(secrets_producer.derive(b"init").await?),
        };

        Ok(KeyScheduleDerivationResult {
            key_schedule,
            confirmation_key: secrets_producer.derive(b"confirm").await?,
            joiner_secret: Zeroizing::new(vec![]).into(),
            epoch_secrets,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn export_secret<P: CipherSuiteProvider>(
        &self,
        label: &[u8],
        context: &[u8],
        len: usize,
        cipher_suite: &P,
    ) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        if self.exporter_secret.is_empty() {
            return Err(MlsError::ExporterDeleted);
        }

        let secret = kdf_derive_secret(cipher_suite, &self.exporter_secret, label).await?;

        let context_hash = cipher_suite
            .hash(context)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        kdf_expand_with_label(cipher_suite, &secret, b"exported", &context_hash, Some(len)).await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn get_membership_tag<P: CipherSuiteProvider>(
        &self,
        content: &AuthenticatedContent,
        context: &GroupContext,
        cipher_suite_provider: &P,
    ) -> Result<MembershipTag, MlsError> {
        MembershipTag::create(
            content,
            context,
            &self.membership_key,
            cipher_suite_provider,
        )
        .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn get_external_key_pair<P: CipherSuiteProvider>(
        &self,
        cipher_suite: &P,
    ) -> Result<(HpkeSecretKey, HpkePublicKey), MlsError> {
        cipher_suite
            .kem_derive(&self.external_secret)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn get_external_key_pair_ext<P: CipherSuiteProvider>(
        &self,
        cipher_suite: &P,
    ) -> Result<ExternalPubExt, MlsError> {
        let (_external_secret, external_pub) = self.get_external_key_pair(cipher_suite).await?;

        Ok(ExternalPubExt { external_pub })
    }
}

#[derive(MlsEncode, MlsSize)]
struct Label<'a> {
    length: u16,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    label: Vec<u8>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    context: &'a [u8],
}

impl<'a> Label<'a> {
    fn new(length: u16, label: &'a [u8], context: &'a [u8]) -> Self {
        Self {
            length,
            label: [b"MLS 1.0 ", label].concat(),
            context,
        }
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn kdf_expand_with_label<P: CipherSuiteProvider>(
    cipher_suite_provider: &P,
    secret: &[u8],
    label: &[u8],
    context: &[u8],
    len: Option<usize>,
) -> Result<Zeroizing<Vec<u8>>, MlsError> {
    let extract_size = cipher_suite_provider.kdf_extract_size();
    let len = len.unwrap_or(extract_size);
    let label = Label::new(len as u16, label, context);

    cipher_suite_provider
        .kdf_expand(secret, &label.mls_encode_to_vec()?, len)
        .await
        .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn kdf_derive_secret<P: CipherSuiteProvider>(
    cipher_suite_provider: &P,
    secret: &[u8],
    label: &[u8],
) -> Result<Zeroizing<Vec<u8>>, MlsError> {
    kdf_expand_with_label(cipher_suite_provider, secret, label, &[], None).await
}

#[derive(Clone, PartialEq, MlsSize, MlsEncode, MlsDecode)]
pub(crate) struct JoinerSecret(#[mls_codec(with = "mls_rs_codec::byte_vec")] Zeroizing<Vec<u8>>);

impl Debug for JoinerSecret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("JoinerSecret").finish()
    }
}

impl From<Zeroizing<Vec<u8>>> for JoinerSecret {
    fn from(bytes: Zeroizing<Vec<u8>>) -> Self {
        Self(bytes)
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn get_pre_epoch_secret<P: CipherSuiteProvider>(
    cipher_suite_provider: &P,
    psk_secret: &PskSecret,
    joiner_secret: &JoinerSecret,
) -> Result<Zeroizing<Vec<u8>>, MlsError> {
    cipher_suite_provider
        .kdf_extract(&joiner_secret.0, psk_secret)
        .await
        .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
}

struct SecretsProducer<'a, P: CipherSuiteProvider> {
    cipher_suite_provider: &'a P,
    epoch_secret: &'a [u8],
}

impl<'a, P: CipherSuiteProvider> SecretsProducer<'a, P> {
    fn new(cipher_suite_provider: &'a P, epoch_secret: &'a [u8]) -> Self {
        Self {
            cipher_suite_provider,
            epoch_secret,
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn derive(&self, label: &[u8]) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        kdf_derive_secret(self.cipher_suite_provider, self.epoch_secret, label).await
    }
}

const EXPORTER_CONTEXT: &[u8] = b"MLS 1.0 external init secret";

#[derive(Clone, Eq, PartialEq, MlsEncode, MlsDecode, MlsSize, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct InitSecret(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    Zeroizing<Vec<u8>>,
);

impl Debug for InitSecret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("InitSecret").finish()
    }
}

impl InitSecret {
    /// Returns init secret and KEM output to be used when creating an external commit.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn encode_for_external<P: CipherSuiteProvider>(
        cipher_suite: &P,
        external_pub: &HpkePublicKey,
    ) -> Result<(Self, Vec<u8>), MlsError> {
        let (kem_output, context) = cipher_suite
            .hpke_setup_s(external_pub, &[])
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        let init_secret = context
            .export(EXPORTER_CONTEXT, cipher_suite.kdf_extract_size())
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        Ok((InitSecret(init_secret), kem_output))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn decode_for_external<P: CipherSuiteProvider>(
        cipher_suite: &P,
        kem_output: &[u8],
        external_secret: &HpkeSecretKey,
        external_pub: &HpkePublicKey,
    ) -> Result<Self, MlsError> {
        let context = cipher_suite
            .hpke_setup_r(kem_output, external_secret, external_pub, &[])
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        context
            .export(EXPORTER_CONTEXT, cipher_suite.kdf_extract_size())
            .await
            .map(InitSecret)
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }
}

pub(crate) struct WelcomeSecret<'a, P: CipherSuiteProvider> {
    cipher_suite: &'a P,
    key: Zeroizing<Vec<u8>>,
    nonce: Zeroizing<Vec<u8>>,
}

impl<'a, P: CipherSuiteProvider> WelcomeSecret<'a, P> {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn from_joiner_secret(
        cipher_suite: &'a P,
        joiner_secret: &JoinerSecret,
        psk_secret: &PskSecret,
    ) -> Result<WelcomeSecret<'a, P>, MlsError> {
        let welcome_secret = get_welcome_secret(cipher_suite, joiner_secret, psk_secret).await?;

        let key_len = cipher_suite.aead_key_size();
        let key = kdf_expand_with_label(cipher_suite, &welcome_secret, b"key", &[], Some(key_len))
            .await?;

        let nonce_len = cipher_suite.aead_nonce_size();

        let nonce = kdf_expand_with_label(
            cipher_suite,
            &welcome_secret,
            b"nonce",
            &[],
            Some(nonce_len),
        )
        .await?;

        Ok(Self {
            cipher_suite,
            key,
            nonce,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn encrypt(&self, plaintext: &[u8]) -> Result<Vec<u8>, MlsError> {
        self.cipher_suite
            .aead_seal(&self.key, plaintext, None, &self.nonce)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn decrypt(&self, ciphertext: &[u8]) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        self.cipher_suite
            .aead_open(&self.key, ciphertext, None, &self.nonce)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
async fn get_welcome_secret<P: CipherSuiteProvider>(
    cipher_suite: &P,
    joiner_secret: &JoinerSecret,
    psk_secret: &PskSecret,
) -> Result<Zeroizing<Vec<u8>>, MlsError> {
    let epoch_seed = get_pre_epoch_secret(cipher_suite, psk_secret, joiner_secret).await?;
    kdf_derive_secret(cipher_suite, &epoch_seed, b"welcome").await
}
