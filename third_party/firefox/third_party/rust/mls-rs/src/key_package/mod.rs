// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::cipher_suite::CipherSuite;
use crate::client::MlsError;
use crate::crypto::HpkePublicKey;
use crate::hash_reference::HashReference;
use crate::identity::SigningIdentity;
use crate::protocol_version::ProtocolVersion;
use crate::signer::Signable;
use crate::time::MlsTime;
use crate::tree_kem::leaf_node::{LeafNode, LeafNodeSource};
use crate::CipherSuiteProvider;
use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};
use mls_rs_codec::MlsDecode;
use mls_rs_codec::MlsEncode;
use mls_rs_codec::MlsSize;
use mls_rs_core::extension::ExtensionList;

mod validator;
pub(crate) use validator::*;

pub(crate) mod generator;
pub(crate) use generator::*;

#[non_exhaustive]
#[derive(Clone, MlsSize, MlsEncode, MlsDecode, PartialEq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct KeyPackage {
    pub version: ProtocolVersion,
    pub cipher_suite: CipherSuite,
    pub hpke_init_key: HpkePublicKey,
    pub(crate) leaf_node: LeafNode,
    pub extensions: ExtensionList,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    pub signature: Vec<u8>,
}

impl Debug for KeyPackage {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("KeyPackage")
            .field("version", &self.version)
            .field("cipher_suite", &self.cipher_suite)
            .field("hpke_init_key", &self.hpke_init_key)
            .field("leaf_node", &self.leaf_node)
            .field("extensions", &self.extensions)
            .field(
                "signature",
                &mls_rs_core::debug::pretty_bytes(&self.signature),
            )
            .finish()
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub struct KeyPackageRef(HashReference);

impl Deref for KeyPackageRef {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl From<Vec<u8>> for KeyPackageRef {
    fn from(v: Vec<u8>) -> Self {
        Self(HashReference::from(v))
    }
}

#[derive(MlsSize, MlsEncode)]
struct KeyPackageData<'a> {
    pub version: ProtocolVersion,
    pub cipher_suite: CipherSuite,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    pub hpke_init_key: &'a HpkePublicKey,
    pub leaf_node: &'a LeafNode,
    pub extensions: &'a ExtensionList,
}

impl KeyPackage {
    pub fn version(&self) -> ProtocolVersion {
        self.version
    }

    pub fn cipher_suite(&self) -> CipherSuite {
        self.cipher_suite
    }

    pub fn signing_identity(&self) -> &SigningIdentity {
        &self.leaf_node.signing_identity
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn to_reference<CP: CipherSuiteProvider>(
        &self,
        cipher_suite_provider: &CP,
    ) -> Result<KeyPackageRef, MlsError> {
        if cipher_suite_provider.cipher_suite() != self.cipher_suite {
            return Err(MlsError::CipherSuiteMismatch);
        }

        Ok(KeyPackageRef(
            HashReference::compute(
                &self.mls_encode_to_vec()?,
                b"MLS 1.0 KeyPackage Reference",
                cipher_suite_provider,
            )
            .await?,
        ))
    }

    /// Time after which the key package is expired.
    pub fn expiration(&self) -> Result<MlsTime, MlsError> {
        if let LeafNodeSource::KeyPackage(lifetime) = &self.leaf_node.leaf_node_source {
            Ok(lifetime.not_after)
        } else {
            Err(MlsError::InvalidLeafNodeSource)
        }
    }
}

impl Signable<'_> for KeyPackage {
    const SIGN_LABEL: &'static str = "KeyPackageTBS";

    type SigningContext = ();

    fn signature(&self) -> &[u8] {
        &self.signature
    }

    fn signable_content(
        &self,
        _context: &Self::SigningContext,
    ) -> Result<Vec<u8>, mls_rs_codec::Error> {
        KeyPackageData {
            version: self.version,
            cipher_suite: self.cipher_suite,
            hpke_init_key: &self.hpke_init_key,
            leaf_node: &self.leaf_node,
            extensions: &self.extensions,
        }
        .mls_encode_to_vec()
    }

    fn write_signature(&mut self, signature: Vec<u8>) {
        self.signature = signature
    }
}
