// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use super::{parent_hash::ParentHash, Capabilities, Lifetime};
use crate::client::MlsError;
use crate::crypto::{CipherSuiteProvider, HpkePublicKey, HpkeSecretKey, SignatureSecretKey};
use crate::{identity::SigningIdentity, signer::Signable, ExtensionList};
use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;

#[derive(Debug, Clone, MlsSize, MlsEncode, MlsDecode, PartialEq, Eq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(u8)]
pub enum LeafNodeSource {
    KeyPackage(Lifetime) = 1u8,
    Update = 2u8,
    Commit(ParentHash) = 3u8,
}

#[derive(Clone, MlsSize, MlsEncode, MlsDecode, PartialEq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[non_exhaustive]
pub struct LeafNode {
    pub public_key: HpkePublicKey,
    pub signing_identity: SigningIdentity,
    pub capabilities: Capabilities,
    pub leaf_node_source: LeafNodeSource,
    pub extensions: ExtensionList,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    pub signature: Vec<u8>,
}

impl Debug for LeafNode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LeafNode")
            .field("public_key", &self.public_key)
            .field("signing_identity", &self.signing_identity)
            .field("capabilities", &self.capabilities)
            .field("leaf_node_source", &self.leaf_node_source)
            .field("extensions", &self.extensions)
            .field(
                "signature",
                &mls_rs_core::debug::pretty_bytes(&self.signature),
            )
            .finish()
    }
}

#[derive(Clone, Debug)]
pub struct ConfigProperties {
    pub capabilities: Capabilities,
    pub extensions: ExtensionList,
}

impl LeafNode {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn generate<CSP>(
        cipher_suite_provider: &CSP,
        properties: ConfigProperties,
        signing_identity: SigningIdentity,
        signer: &SignatureSecretKey,
        lifetime: Lifetime,
    ) -> Result<(Self, HpkeSecretKey), MlsError>
    where
        CSP: CipherSuiteProvider,
    {
        let (secret_key, public_key) = cipher_suite_provider
            .kem_generate()
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        let mut leaf_node = LeafNode {
            public_key,
            signing_identity,
            capabilities: properties.capabilities,
            leaf_node_source: LeafNodeSource::KeyPackage(lifetime),
            extensions: properties.extensions,
            signature: Default::default(),
        };

        leaf_node.grease(cipher_suite_provider)?;

        leaf_node
            .sign(
                cipher_suite_provider,
                signer,
                &LeafNodeSigningContext::default(),
            )
            .await?;

        Ok((leaf_node, secret_key))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn update<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        group_id: &[u8],
        leaf_index: u32,
        new_properties: Option<ConfigProperties>,
        signing_identity: Option<SigningIdentity>,
        signer: &SignatureSecretKey,
    ) -> Result<HpkeSecretKey, MlsError> {
        let (secret, public) = cipher_suite_provider
            .kem_generate()
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        self.public_key = public;

        if let Some(new_properties) = new_properties {
            self.capabilities = new_properties.capabilities;
            self.extensions = new_properties.extensions;
        }

        self.leaf_node_source = LeafNodeSource::Update;

        self.grease(cipher_suite_provider)?;

        if let Some(signing_identity) = signing_identity {
            self.signing_identity = signing_identity;
        }

        self.sign(
            cipher_suite_provider,
            signer,
            &(group_id, leaf_index).into(),
        )
        .await?;

        Ok(secret)
    }

    #[allow(clippy::too_many_arguments)]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn commit<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        group_id: &[u8],
        leaf_index: u32,
        new_properties: Option<ConfigProperties>,
        new_signing_identity: Option<SigningIdentity>,
        signer: &SignatureSecretKey,
    ) -> Result<HpkeSecretKey, MlsError> {
        let (secret, public) = cipher_suite_provider
            .kem_generate()
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        self.public_key = public;

        if let Some(new_properties) = new_properties {
            self.capabilities = new_properties.capabilities;
            self.extensions = new_properties.extensions;
        }

        if let Some(new_signing_identity) = new_signing_identity {
            self.signing_identity = new_signing_identity;
        }

        self.sign(
            cipher_suite_provider,
            signer,
            &(group_id, leaf_index).into(),
        )
        .await?;

        Ok(secret)
    }
}

#[derive(Debug)]
struct LeafNodeTBS<'a> {
    public_key: &'a HpkePublicKey,
    signing_identity: &'a SigningIdentity,
    capabilities: &'a Capabilities,
    leaf_node_source: &'a LeafNodeSource,
    extensions: &'a ExtensionList,
    group_id: Option<&'a [u8]>,
    leaf_index: Option<u32>,
}

impl MlsSize for LeafNodeTBS<'_> {
    fn mls_encoded_len(&self) -> usize {
        self.public_key.mls_encoded_len()
            + self.signing_identity.mls_encoded_len()
            + self.capabilities.mls_encoded_len()
            + self.leaf_node_source.mls_encoded_len()
            + self.extensions.mls_encoded_len()
            + self
                .group_id
                .as_ref()
                .map_or(0, mls_rs_codec::byte_vec::mls_encoded_len)
            + self.leaf_index.map_or(0, |i| i.mls_encoded_len())
    }
}

impl MlsEncode for LeafNodeTBS<'_> {
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), mls_rs_codec::Error> {
        self.public_key.mls_encode(writer)?;
        self.signing_identity.mls_encode(writer)?;
        self.capabilities.mls_encode(writer)?;
        self.leaf_node_source.mls_encode(writer)?;
        self.extensions.mls_encode(writer)?;

        if let Some(ref group_id) = self.group_id {
            mls_rs_codec::byte_vec::mls_encode(group_id, writer)?;
        }

        if let Some(leaf_index) = self.leaf_index {
            leaf_index.mls_encode(writer)?;
        }

        Ok(())
    }
}

#[derive(Clone, Debug, Default)]
pub(crate) struct LeafNodeSigningContext<'a> {
    pub group_id: Option<&'a [u8]>,
    pub leaf_index: Option<u32>,
}

impl<'a> From<(&'a [u8], u32)> for LeafNodeSigningContext<'a> {
    fn from((group_id, leaf_index): (&'a [u8], u32)) -> Self {
        Self {
            group_id: Some(group_id),
            leaf_index: Some(leaf_index),
        }
    }
}

impl<'a> Signable<'a> for LeafNode {
    const SIGN_LABEL: &'static str = "LeafNodeTBS";

    type SigningContext = LeafNodeSigningContext<'a>;

    fn signature(&self) -> &[u8] {
        &self.signature
    }

    fn signable_content(
        &self,
        context: &Self::SigningContext,
    ) -> Result<Vec<u8>, mls_rs_codec::Error> {
        LeafNodeTBS {
            public_key: &self.public_key,
            signing_identity: &self.signing_identity,
            capabilities: &self.capabilities,
            leaf_node_source: &self.leaf_node_source,
            extensions: &self.extensions,
            group_id: context.group_id,
            leaf_index: context.leaf_index,
        }
        .mls_encode_to_vec()
    }

    fn write_signature(&mut self, signature: Vec<u8>) {
        self.signature = signature
    }
}
