// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::{Deref, DerefMut},
};

use zeroize::Zeroizing;

use crate::{client::MlsError, map::LargeMap, tree_kem::math::TreeIndex, CipherSuiteProvider};

use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;

use super::key_schedule::kdf_expand_with_label;

pub(crate) const MAX_RATCHET_BACK_HISTORY: u32 = 1024;

#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(u8)]
enum SecretTreeNode {
    Secret(TreeSecret) = 0u8,
    Ratchet(SecretRatchets) = 1u8,
}

impl SecretTreeNode {
    fn into_secret(self) -> Option<TreeSecret> {
        if let SecretTreeNode::Secret(secret) = self {
            Some(secret)
        } else {
            None
        }
    }
}

#[derive(Clone, PartialEq, MlsEncode, MlsDecode, MlsSize)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
struct TreeSecret(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    Zeroizing<Vec<u8>>,
);

impl Debug for TreeSecret {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TreeSecret").finish()
    }
}

impl Deref for TreeSecret {
    type Target = Vec<u8>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl DerefMut for TreeSecret {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

impl AsRef<[u8]> for TreeSecret {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

impl From<Vec<u8>> for TreeSecret {
    fn from(vec: Vec<u8>) -> Self {
        TreeSecret(Zeroizing::new(vec))
    }
}

impl From<Zeroizing<Vec<u8>>> for TreeSecret {
    fn from(vec: Zeroizing<Vec<u8>>) -> Self {
        TreeSecret(vec)
    }
}

#[derive(Clone, Debug, PartialEq, MlsEncode, MlsDecode, MlsSize, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
struct TreeSecretsVec<T: TreeIndex> {
    inner: LargeMap<T, SecretTreeNode>,
}

impl<T: TreeIndex> TreeSecretsVec<T> {
    fn set_node(&mut self, index: T, value: SecretTreeNode) {
        self.inner.insert(index, value);
    }

    fn take_node(&mut self, index: &T) -> Option<SecretTreeNode> {
        self.inner.remove(index)
    }
}

#[derive(Clone, Debug, PartialEq, MlsEncode, MlsDecode, MlsSize)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct SecretTree<T: TreeIndex> {
    known_secrets: TreeSecretsVec<T>,
    leaf_count: T,
}

impl<T: TreeIndex> SecretTree<T> {
    pub(crate) fn empty() -> SecretTree<T> {
        SecretTree {
            known_secrets: Default::default(),
            leaf_count: T::zero(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct SecretRatchets {
    pub application: SecretKeyRatchet,
    pub handshake: SecretKeyRatchet,
}

impl SecretRatchets {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn message_key_generation<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        generation: u32,
        key_type: KeyType,
    ) -> Result<MessageKeyData, MlsError> {
        match key_type {
            KeyType::Handshake => {
                self.handshake
                    .get_message_key(cipher_suite_provider, generation)
                    .await
            }
            KeyType::Application => {
                self.application
                    .get_message_key(cipher_suite_provider, generation)
                    .await
            }
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn next_message_key<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite: &P,
        key_type: KeyType,
    ) -> Result<MessageKeyData, MlsError> {
        match key_type {
            KeyType::Handshake => self.handshake.next_message_key(cipher_suite).await,
            KeyType::Application => self.application.next_message_key(cipher_suite).await,
        }
    }

    /// Peeks at the next key generation for `key_type`, but does not increment the
    /// generation nor derive keys.
    #[cfg(feature = "export_key_generation")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync, allow(dead_code))]
    async fn peek_next_key_generation(&self, key_type: KeyType) -> u32 {
        match key_type {
            KeyType::Handshake => self.handshake.peek_next_key_generation().await,
            KeyType::Application => self.application.peek_next_key_generation().await,
        }
    }
}

impl<T: TreeIndex> SecretTree<T> {
    pub fn new(leaf_count: T, encryption_secret: Zeroizing<Vec<u8>>) -> SecretTree<T> {
        let mut known_secrets = TreeSecretsVec::default();

        let root_secret = SecretTreeNode::Secret(TreeSecret::from(encryption_secret));
        known_secrets.set_node(leaf_count.root(), root_secret);

        Self {
            known_secrets,
            leaf_count,
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn consume_node<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        index: &T,
    ) -> Result<(), MlsError> {
        let node = self.known_secrets.take_node(index);

        if let Some(secret) = node.and_then(|n| n.into_secret()) {
            let left_index = index.left().ok_or(MlsError::LeafNodeNoChildren)?;
            let right_index = index.right().ok_or(MlsError::LeafNodeNoChildren)?;

            let left_secret =
                kdf_expand_with_label(cipher_suite_provider, &secret, b"tree", b"left", None)
                    .await?;

            let right_secret =
                kdf_expand_with_label(cipher_suite_provider, &secret, b"tree", b"right", None)
                    .await?;

            self.known_secrets
                .set_node(left_index, SecretTreeNode::Secret(left_secret.into()));

            self.known_secrets
                .set_node(right_index, SecretTreeNode::Secret(right_secret.into()));
        }

        Ok(())
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn take_leaf_ratchet<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite: &P,
        leaf_index: &T,
    ) -> Result<SecretRatchets, MlsError> {
        let node_index = leaf_index;

        let node = match self.known_secrets.take_node(node_index) {
            Some(node) => node,
            None => {
                for i in node_index.direct_copath(&self.leaf_count).into_iter().rev() {
                    self.consume_node(cipher_suite, &i.path).await?;
                }

                self.known_secrets
                    .take_node(node_index)
                    .ok_or(MlsError::InvalidLeafConsumption)?
            }
        };

        Ok(match node {
            SecretTreeNode::Ratchet(ratchet) => ratchet,
            SecretTreeNode::Secret(secret) => SecretRatchets {
                application: SecretKeyRatchet::new(cipher_suite, &secret, KeyType::Application)
                    .await?,
                handshake: SecretKeyRatchet::new(cipher_suite, &secret, KeyType::Handshake).await?,
            },
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn next_message_key<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite: &P,
        leaf_index: T,
        key_type: KeyType,
    ) -> Result<MessageKeyData, MlsError> {
        let mut ratchet = self.take_leaf_ratchet(cipher_suite, &leaf_index).await?;
        let res = ratchet.next_message_key(cipher_suite, key_type).await?;

        self.known_secrets
            .set_node(leaf_index, SecretTreeNode::Ratchet(ratchet));

        Ok(res)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn message_key_generation<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite: &P,
        leaf_index: T,
        key_type: KeyType,
        generation: u32,
    ) -> Result<MessageKeyData, MlsError> {
        let mut ratchet = self.take_leaf_ratchet(cipher_suite, &leaf_index).await?;

        let res = ratchet
            .message_key_generation(cipher_suite, generation, key_type)
            .await;

        self.known_secrets
            .set_node(leaf_index, SecretTreeNode::Ratchet(ratchet));

        res
    }

    /// Peeks at the next key generation, but does not increment the generation nor
    /// derive keys.
    ///
    /// Takes &mut self since take_leaf_ratchet constructs and stores nodes in the
    /// SecretTree the first time they are requested.
    ///
    /// Called by [`Group::peek_next_key_generation`], which is used by clients to
    /// authenticate the generation to defend against in-group forgery attacks described
    /// in https://eprint.iacr.org/2025/554
    #[cfg(feature = "export_key_generation")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync, allow(dead_code))]
    pub async fn peek_next_key_generation<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite: &P,
        leaf_index: T,
        key_type: KeyType,
    ) -> Result<u32, MlsError> {
        let ratchet = self.take_leaf_ratchet(cipher_suite, &leaf_index).await?;
        let res = ratchet.peek_next_key_generation(key_type).await;

        self.known_secrets
            .set_node(leaf_index, SecretTreeNode::Ratchet(ratchet));

        Ok(res)
    }
}

#[derive(Clone, Copy)]
pub enum KeyType {
    Handshake,
    Application,
}

#[derive(Clone, PartialEq, Eq, MlsEncode, MlsDecode, MlsSize)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
/// AEAD key derived by the MLS secret tree.
pub struct MessageKeyData {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    pub(crate) nonce: Zeroizing<Vec<u8>>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::zeroizing_serde"))]
    pub(crate) key: Zeroizing<Vec<u8>>,
    pub(crate) generation: u32,
}

impl Debug for MessageKeyData {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("MessageKeyData")
            .field("generation", &self.generation)
            .finish()
    }
}

impl MessageKeyData {
    /// AEAD nonce.
    #[cfg_attr(not(feature = "secret_tree_access"), allow(dead_code))]
    pub fn nonce(&self) -> &[u8] {
        &self.nonce
    }

    /// AEAD key.
    #[cfg_attr(not(feature = "secret_tree_access"), allow(dead_code))]
    pub fn key(&self) -> &[u8] {
        &self.key
    }

    /// Generation of this key within the key schedule.
    #[cfg_attr(not(feature = "secret_tree_access"), allow(dead_code))]
    pub fn generation(&self) -> u32 {
        self.generation
    }
}

#[derive(Debug, Clone, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct SecretKeyRatchet {
    secret: TreeSecret,
    generation: u32,
    #[cfg(feature = "out_of_order")]
    history: LargeMap<u32, MessageKeyData>,
}

impl MlsSize for SecretKeyRatchet {
    fn mls_encoded_len(&self) -> usize {
        let len = mls_rs_codec::byte_vec::mls_encoded_len(&self.secret)
            + self.generation.mls_encoded_len();

        #[cfg(feature = "out_of_order")]
        return len + mls_rs_codec::iter::mls_encoded_len(self.history.values());
        #[cfg(not(feature = "out_of_order"))]
        return len;
    }
}

#[cfg(feature = "out_of_order")]
impl MlsEncode for SecretKeyRatchet {
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), mls_rs_codec::Error> {
        mls_rs_codec::byte_vec::mls_encode(&self.secret, writer)?;
        self.generation.mls_encode(writer)?;
        mls_rs_codec::iter::mls_encode(self.history.values(), writer)
    }
}

#[cfg(not(feature = "out_of_order"))]
impl MlsEncode for SecretKeyRatchet {
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), mls_rs_codec::Error> {
        mls_rs_codec::byte_vec::mls_encode(&self.secret, writer)?;
        self.generation.mls_encode(writer)
    }
}

impl MlsDecode for SecretKeyRatchet {
    fn mls_decode(reader: &mut &[u8]) -> Result<Self, mls_rs_codec::Error> {
        Ok(Self {
            secret: mls_rs_codec::byte_vec::mls_decode(reader)?,
            generation: u32::mls_decode(reader)?,
            #[cfg(feature = "out_of_order")]
            history: mls_rs_codec::iter::mls_decode_collection(reader, |data| {
                let mut items = LargeMap::default();

                while !data.is_empty() {
                    let item = MessageKeyData::mls_decode(data)?;
                    items.insert(item.generation, item);
                }

                Ok(items)
            })?,
        })
    }
}

impl SecretKeyRatchet {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn new<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        secret: &[u8],
        key_type: KeyType,
    ) -> Result<Self, MlsError> {
        let label = match key_type {
            KeyType::Handshake => b"handshake".as_slice(),
            KeyType::Application => b"application".as_slice(),
        };

        let secret = kdf_expand_with_label(cipher_suite_provider, secret, label, &[], None)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        Ok(Self {
            secret: TreeSecret::from(secret),
            generation: 0,
            #[cfg(feature = "out_of_order")]
            history: Default::default(),
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn get_message_key<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        generation: u32,
    ) -> Result<MessageKeyData, MlsError> {
        #[cfg(feature = "out_of_order")]
        if generation < self.generation {
            return self
                .history
                .remove_entry(&generation)
                .map(|(_, mk)| mk)
                .ok_or(MlsError::KeyMissing(generation));
        }

        #[cfg(not(feature = "out_of_order"))]
        if generation < self.generation {
            return Err(MlsError::KeyMissing(generation));
        }

        let max_generation_allowed = self.generation + MAX_RATCHET_BACK_HISTORY;

        if generation > max_generation_allowed {
            return Err(MlsError::InvalidFutureGeneration(generation));
        }

        #[cfg(not(feature = "out_of_order"))]
        while self.generation < generation {
            self.next_message_key(cipher_suite_provider)?;
        }

        #[cfg(feature = "out_of_order")]
        while self.generation < generation {
            let key_data = self.next_message_key(cipher_suite_provider).await?;
            self.history.insert(key_data.generation, key_data);
        }

        self.next_message_key(cipher_suite_provider).await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn next_message_key<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
    ) -> Result<MessageKeyData, MlsError> {
        let generation = self.generation;

        let key = MessageKeyData {
            nonce: self
                .derive_secret(
                    cipher_suite_provider,
                    b"nonce",
                    cipher_suite_provider.aead_nonce_size(),
                )
                .await?,
            key: self
                .derive_secret(
                    cipher_suite_provider,
                    b"key",
                    cipher_suite_provider.aead_key_size(),
                )
                .await?,
            generation,
        };

        self.secret = self
            .derive_secret(
                cipher_suite_provider,
                b"secret",
                cipher_suite_provider.kdf_extract_size(),
            )
            .await?
            .into();

        self.generation = generation + 1;

        Ok(key)
    }

    /// Peeks at the next key generation, but does not increment the generation nor
    /// derive keys.
    #[cfg(feature = "export_key_generation")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync, allow(dead_code))]
    async fn peek_next_key_generation(&self) -> u32 {
        self.generation
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn derive_secret<P: CipherSuiteProvider>(
        &self,
        cipher_suite_provider: &P,
        label: &[u8],
        len: usize,
    ) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        kdf_expand_with_label(
            cipher_suite_provider,
            self.secret.as_ref(),
            label,
            &self.generation.to_be_bytes(),
            Some(len),
        )
        .await
        .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }
}
