// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec;
use alloc::vec::Vec;
use mls_rs_codec::{MlsDecode, MlsEncode};
use mls_rs_core::{error::IntoAnyError, key_package::KeyPackageData};

use crate::client::MlsError;
use crate::{
    crypto::{HpkeSecretKey, SignatureSecretKey},
    group::framing::MlsMessagePayload,
    identity::SigningIdentity,
    protocol_version::ProtocolVersion,
    signer::Signable,
    tree_kem::{
        leaf_node::{ConfigProperties, LeafNode},
        Capabilities, Lifetime,
    },
    CipherSuiteProvider, ExtensionList, MlsMessage,
};

use super::{KeyPackage, KeyPackageRef};

#[derive(Clone, Debug)]
pub struct KeyPackageGenerator<'a, CP>
where
    CP: CipherSuiteProvider,
{
    pub protocol_version: ProtocolVersion,
    pub cipher_suite_provider: &'a CP,
    pub signing_identity: &'a SigningIdentity,
    pub signing_key: &'a SignatureSecretKey,
}

#[derive(Clone, Debug)]
pub struct KeyPackageGeneration {
    pub(crate) reference: KeyPackageRef,
    pub(crate) key_package: KeyPackage,
    pub(crate) init_secret_key: HpkeSecretKey,
    pub(crate) leaf_node_secret_key: HpkeSecretKey,
}

impl KeyPackageGeneration {
    pub fn to_storage(&self) -> Result<(Vec<u8>, KeyPackageData), MlsError> {
        let id = self.reference.to_vec();

        let data = KeyPackageData::new(
            self.key_package.mls_encode_to_vec()?,
            self.init_secret_key.clone(),
            self.leaf_node_secret_key.clone(),
            self.key_package.expiration()?.seconds_since_epoch(),
        );

        Ok((id, data))
    }

    pub fn from_storage(id: Vec<u8>, data: KeyPackageData) -> Result<Self, MlsError> {
        Ok(KeyPackageGeneration {
            reference: KeyPackageRef::from(id),
            key_package: KeyPackage::mls_decode(&mut &*data.key_package_bytes)?,
            init_secret_key: data.init_key,
            leaf_node_secret_key: data.leaf_node_key,
        })
    }

    pub fn key_package_message(&self) -> MlsMessage {
        MlsMessage::new(
            self.key_package.version,
            MlsMessagePayload::KeyPackage(self.key_package.clone()),
        )
    }
}

impl<CP> KeyPackageGenerator<'_, CP>
where
    CP: CipherSuiteProvider,
{
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(super) async fn sign(&self, package: &mut KeyPackage) -> Result<(), MlsError> {
        package
            .sign(self.cipher_suite_provider, self.signing_key, &())
            .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn generate(
        &self,
        lifetime: Lifetime,
        capabilities: Capabilities,
        key_package_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
    ) -> Result<KeyPackageGeneration, MlsError> {
        let (init_secret_key, public_init) = self
            .cipher_suite_provider
            .kem_generate()
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        let properties = ConfigProperties {
            capabilities,
            extensions: leaf_node_extensions,
        };

        let (leaf_node, leaf_node_secret) = LeafNode::generate(
            self.cipher_suite_provider,
            properties,
            self.signing_identity.clone(),
            self.signing_key,
            lifetime,
        )
        .await?;

        let mut package = KeyPackage {
            version: self.protocol_version,
            cipher_suite: self.cipher_suite_provider.cipher_suite(),
            hpke_init_key: public_init,
            leaf_node,
            extensions: key_package_extensions,
            signature: vec![],
        };

        package.grease(self.cipher_suite_provider)?;

        self.sign(&mut package).await?;

        let reference = package.to_reference(self.cipher_suite_provider).await?;

        Ok(KeyPackageGeneration {
            key_package: package,
            init_secret_key,
            leaf_node_secret_key: leaf_node_secret,
            reference,
        })
    }
}
