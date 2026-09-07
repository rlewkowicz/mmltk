// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::crypto::{CipherSuiteProvider, HpkePublicKey};
use crate::tree_kem::math as tree_math;
use crate::tree_kem::node::{LeafIndex, Node, NodeIndex};
use crate::tree_kem::tree_hash::TreeHash;
use crate::tree_kem::TreeKemPublic;
use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
use tree_math::TreeIndex;

use super::leaf_node::LeafNodeSource;

#[cfg(feature = "std")]
use std::collections::HashSet;

#[cfg(not(feature = "std"))]
use alloc::collections::BTreeSet;

#[derive(Clone, Debug, MlsSize, MlsEncode)]
struct ParentHashInput<'a> {
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    public_key: &'a HpkePublicKey,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    parent_hash: &'a [u8],
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    original_sibling_tree_hash: &'a [u8],
}

#[derive(Clone, MlsSize, MlsEncode, MlsDecode, PartialEq, Eq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct ParentHash(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    Vec<u8>,
);

impl Debug for ParentHash {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("ParentHash")
            .fmt(f)
    }
}

impl From<Vec<u8>> for ParentHash {
    fn from(v: Vec<u8>) -> Self {
        Self(v)
    }
}

impl Deref for ParentHash {
    type Target = Vec<u8>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl ParentHash {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn new<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
        public_key: &HpkePublicKey,
        parent_hash: &ParentHash,
        original_sibling_tree_hash: &[u8],
    ) -> Result<Self, MlsError> {
        let input = ParentHashInput {
            public_key,
            parent_hash,
            original_sibling_tree_hash,
        };

        let input_bytes = input.mls_encode_to_vec()?;

        let hash = cipher_suite_provider
            .hash(&input_bytes)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        Ok(Self(hash))
    }

    pub fn empty() -> Self {
        ParentHash(Vec::new())
    }

    pub fn matches(&self, hash: &ParentHash) -> bool {
        hash == self
    }
}

impl Node {
    fn get_parent_hash(&self) -> Option<ParentHash> {
        match self {
            Node::Parent(p) => Some(p.parent_hash.clone()),
            Node::Leaf(l) => match &l.leaf_node_source {
                LeafNodeSource::Commit(parent_hash) => Some(parent_hash.clone()),
                _ => None,
            },
        }
    }
}

impl TreeKemPublic {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn parent_hash_for_leaf<P: CipherSuiteProvider>(
        &mut self,
        cipher_suite_provider: &P,
        index: LeafIndex,
    ) -> Result<ParentHash, MlsError> {
        let mut hash = ParentHash::empty();

        for node in self.nodes.direct_copath(index).into_iter().rev() {
            if self.nodes.is_resolution_empty(node.copath) {
                continue;
            }

            let parent = self.nodes.borrow_as_parent_mut(node.path)?;

            let calculated = ParentHash::new(
                cipher_suite_provider,
                &parent.public_key,
                &hash,
                &self.tree_hashes.current[node.copath as usize],
            )
            .await?;

            parent.parent_hash = core::mem::replace(&mut hash, calculated);
        }

        Ok(hash)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn update_parent_hashes<P: CipherSuiteProvider>(
        &mut self,
        index: LeafIndex,
        verify_leaf_hash: bool,
        cipher_suite_provider: &P,
    ) -> Result<(), MlsError> {
        self.update_hashes(&[index], cipher_suite_provider).await?;

        let leaf_hash = self
            .parent_hash_for_leaf(cipher_suite_provider, index)
            .await?;

        let leaf = self.nodes.borrow_as_leaf_mut(index)?;

        if verify_leaf_hash {
            if let LeafNodeSource::Commit(parent_hash) = &leaf.leaf_node_source {
                if !leaf_hash.matches(parent_hash) {
                    return Err(MlsError::ParentHashMismatch);
                }
            } else {
                return Err(MlsError::InvalidLeafNodeSource);
            }
        } else {
            leaf.leaf_node_source = LeafNodeSource::Commit(leaf_hash);
        }

        self.update_hashes(&[index], cipher_suite_provider).await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(super) async fn validate_parent_hashes<P: CipherSuiteProvider>(
        &self,
        cipher_suite_provider: &P,
    ) -> Result<(), MlsError> {
        let original_hashes = self.compute_original_hashes(cipher_suite_provider).await?;

        let nodes_to_validate = self
            .nodes
            .non_empty_parents()
            .filter_map(|(node_index, node)| {
                if node.parent_hash.is_empty() {
                    None
                } else {
                    Some(node_index)
                }
            });

        #[cfg(feature = "std")]
        let mut nodes_to_validate = nodes_to_validate.collect::<HashSet<_>>();
        #[cfg(not(feature = "std"))]
        let mut nodes_to_validate = nodes_to_validate.collect::<BTreeSet<_>>();

        let num_leaves = self.total_leaf_count();

        for (leaf_index, _) in self.nodes.non_empty_leaves() {
            self.validate_chain(
                leaf_index,
                num_leaves,
                cipher_suite_provider,
                &original_hashes,
                &mut nodes_to_validate,
            )
            .await?;
        }

        if nodes_to_validate.is_empty() {
            Ok(())
        } else {
            Err(MlsError::ParentHashMismatch)
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn validate_chain<P: CipherSuiteProvider>(
        &self,
        leaf_index: LeafIndex,
        num_leaves: u32,
        cipher_suite_provider: &P,
        original_hashes: &[TreeHash],
        #[cfg(feature = "std")] nodes_to_validate: &mut HashSet<u32>,
        #[cfg(not(feature = "std"))] nodes_to_validate: &mut BTreeSet<u32>,
    ) -> Result<(), MlsError> {
        let mut n = NodeIndex::from(leaf_index);

        while let Some(mut ps) = n.parent_sibling(&num_leaves) {
            while self.nodes.is_blank(ps.parent)? {
                let Some(ps_parent) = ps.parent.parent_sibling(&num_leaves) else {
                    return Ok(());
                };

                ps = ps_parent;
            }

            let p_parent = self.nodes.borrow_as_parent(ps.parent)?;

            let n_node = self
                .nodes
                .borrow_node(n)?
                .as_ref()
                .ok_or(MlsError::ExpectedNode)?;

            let calculated = ParentHash::new(
                cipher_suite_provider,
                &p_parent.public_key,
                &p_parent.parent_hash,
                &original_hashes[ps.sibling as usize],
            )
            .await?;

            if n_node.get_parent_hash() == Some(calculated) {
                let Some(cp) = ps.sibling.parent_sibling(&num_leaves) else {
                    return Err(MlsError::ParentHashMismatch);
                };

                let c = cp.sibling;
                let c_resolution = self.nodes.get_resolution_index(c)?.into_iter();

                #[cfg(feature = "std")]
                let mut c_resolution = c_resolution.collect::<HashSet<_>>();
                #[cfg(not(feature = "std"))]
                let mut c_resolution = c_resolution.collect::<BTreeSet<_>>();

                let p_unmerged_in_c_subtree = self
                    .unmerged_in_subtree(ps.parent, c)?
                    .iter()
                    .copied()
                    .map(|x| *x * 2);

                #[cfg(feature = "std")]
                let p_unmerged_in_c_subtree = p_unmerged_in_c_subtree.collect::<HashSet<_>>();
                #[cfg(not(feature = "std"))]
                let p_unmerged_in_c_subtree = p_unmerged_in_c_subtree.collect::<BTreeSet<_>>();

                if c_resolution.remove(&n) && c_resolution == p_unmerged_in_c_subtree {
                    let removed = nodes_to_validate.remove(&ps.parent);
                    if !removed && !p_parent.parent_hash.is_empty() {
                        return Err(MlsError::ParentHashMismatch);
                    }
                    n = ps.parent;
                } else {
                    return Err(MlsError::ParentHashMismatch);
                }
            } else {
                break;
            }
        }

        Ok(())
    }
}
