// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec;
use alloc::vec::Vec;
#[cfg(feature = "std")]
use core::fmt::Display;
use itertools::Itertools;
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::extension::ExtensionList;

use mls_rs_core::{error::IntoAnyError, identity::IdentityProvider};

#[cfg(feature = "tree_index")]
use mls_rs_core::identity::SigningIdentity;

use math as tree_math;
use node::{LeafIndex, NodeIndex, NodeVec};

use self::leaf_node::LeafNode;

use crate::client::MlsError;
use crate::crypto::{self, CipherSuiteProvider, HpkeSecretKey};

#[cfg(feature = "by_ref_proposal")]
use crate::group::proposal::{AddProposal, UpdateProposal};

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal", feature = "self_remove_proposal"))]
use crate::group::proposal::SelfRemoveProposal;

#[cfg(feature = "by_ref_proposal")]
use crate::group::{proposal::RemoveProposal, proposal_filter::bundle::Proposable};

use crate::group::proposal_filter::ProposalBundle;
use crate::tree_kem::tree_hash::TreeHashes;

mod capabilities;
pub(crate) mod hpke_encryption;
mod lifetime;
pub(crate) mod math;
pub mod node;
pub mod parent_hash;
pub mod path_secret;
mod private;
mod tree_hash;
pub mod tree_validator;
pub mod update_path;

pub use capabilities::*;
pub use lifetime::*;
pub(crate) use private::*;
pub use update_path::*;

use tree_index::*;

pub mod kem;
pub mod leaf_node;
pub mod leaf_node_validator;
mod tree_index;

#[cfg(feature = "std")]
pub(crate) mod tree_utils;


#[cfg(feature = "custom_proposal")]
use crate::group::proposal::ProposalType;

#[derive(Clone, Debug, MlsEncode, MlsDecode, MlsSize, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct TreeKemPublic {
    #[cfg(feature = "tree_index")]
    #[cfg_attr(feature = "serde", serde(skip))]
    index: TreeIndex,
    pub(crate) nodes: NodeVec,
    tree_hashes: TreeHashes,
}

impl PartialEq for TreeKemPublic {
    fn eq(&self, other: &Self) -> bool {
        self.nodes == other.nodes
    }
}

impl TreeKemPublic {
    pub fn new() -> TreeKemPublic {
        Default::default()
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn import_node_data<IP>(
        nodes: NodeVec,
        identity_provider: &IP,
        extensions: &ExtensionList,
    ) -> Result<TreeKemPublic, MlsError>
    where
        IP: IdentityProvider,
    {
        let tree = TreeKemPublic {
            nodes,
            ..Default::default()
        };

        #[cfg(feature = "tree_index")]
        let mut tree = tree;
        #[cfg(feature = "tree_index")]
        tree.initialize_index_if_necessary(identity_provider, extensions)
            .await?;

        #[cfg(not(feature = "tree_index"))]
        for (leaf_index, leaf) in tree.nodes.non_empty_leaves() {
            index_insert(&tree.nodes, leaf, leaf_index, identity_provider, extensions).await?;
        }

        Ok(tree)
    }

    #[cfg(feature = "tree_index")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn initialize_index_if_necessary<IP: IdentityProvider>(
        &mut self,
        identity_provider: &IP,
        extensions: &ExtensionList,
    ) -> Result<(), MlsError> {
        if !self.index.is_initialized() {
            self.index = TreeIndex::new();

            for (leaf_index, leaf) in self.nodes.non_empty_leaves() {
                index_insert(
                    &mut self.index,
                    leaf,
                    leaf_index,
                    identity_provider,
                    extensions,
                )
                .await?;
            }
        }

        Ok(())
    }

    #[cfg(feature = "tree_index")]
    pub(crate) fn get_leaf_node_with_identity(&self, identity: &[u8]) -> Option<LeafIndex> {
        self.index.get_leaf_index_with_identity(identity)
    }

    #[cfg(not(feature = "tree_index"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn get_leaf_node_with_identity<I: IdentityProvider>(
        &self,
        identity: &[u8],
        id_provider: &I,
        extensions: &ExtensionList,
    ) -> Result<Option<LeafIndex>, MlsError> {
        for (i, leaf) in self.nodes.non_empty_leaves() {
            let leaf_id = id_provider
                .identity(&leaf.signing_identity, extensions)
                .await
                .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

            if leaf_id == identity {
                return Ok(Some(i));
            }
        }

        Ok(None)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn derive<I: IdentityProvider>(
        leaf_node: LeafNode,
        secret_key: HpkeSecretKey,
        identity_provider: &I,
        extensions: &ExtensionList,
    ) -> Result<(TreeKemPublic, TreeKemPrivate), MlsError> {
        let mut public_tree = TreeKemPublic::new();

        public_tree
            .add_leaf(leaf_node, identity_provider, extensions, None)
            .await?;

        let private_tree = TreeKemPrivate::new_self_leaf(LeafIndex::unchecked(0), secret_key);

        Ok((public_tree, private_tree))
    }

    pub fn total_leaf_count(&self) -> u32 {
        self.nodes.total_leaf_count()
    }

#[cfg(all(feature = "custom_proposal", feature = "tree_index"))]
pub fn occupied_leaf_count(&self) -> u32 {
        self.nodes.occupied_leaf_count()
    }

    pub fn get_leaf_node(&self, index: LeafIndex) -> Result<&LeafNode, MlsError> {
        self.nodes.borrow_as_leaf(index)
    }

    pub fn find_leaf_node(&self, leaf_node: &LeafNode) -> Option<LeafIndex> {
        self.nodes.non_empty_leaves().find_map(
            |(index, node)| {
                if node == leaf_node {
                    Some(index)
                } else {
                    None
                }
            },
        )
    }

    #[cfg(feature = "custom_proposal")]
    pub fn can_support_proposal(&self, proposal_type: ProposalType) -> bool {
        #[cfg(feature = "tree_index")]
        return self.index.count_supporting_proposal(proposal_type) == self.occupied_leaf_count();

        #[cfg(not(feature = "tree_index"))]
        self.nodes
            .non_empty_leaves()
            .all(|(_, l)| l.capabilities.proposals.contains(&proposal_type))
    }

#[cfg(any())]









    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn add_leaves<I: IdentityProvider, CP: CipherSuiteProvider>(
        &mut self,
        leaf_nodes: Vec<LeafNode>,
        id_provider: &I,
        cipher_suite_provider: &CP,
    ) -> Result<Vec<LeafIndex>, MlsError> {
        let mut start = LeafIndex::unchecked(0);
        let mut added = vec![];

        for leaf in leaf_nodes.into_iter() {
            start = self
                .add_leaf(leaf, id_provider, &Default::default(), Some(start))
                .await?;
            added.push(start);
        }

        self.update_hashes(&added, cipher_suite_provider).await?;

        Ok(added)
    }

    pub fn non_empty_leaves(&self) -> impl Iterator<Item = (LeafIndex, &LeafNode)> + '_ {
        self.nodes.non_empty_leaves()
    }

    #[cfg(feature = "prior_epoch")]
    pub fn leaves(&self) -> impl Iterator<Item = Option<&LeafNode>> + '_ {
        self.nodes.leaves()
    }

    pub(crate) fn update_node(
        &mut self,
        pub_key: crypto::HpkePublicKey,
        index: NodeIndex,
    ) -> Result<(), MlsError> {
        self.nodes
            .borrow_or_fill_node_as_parent(index, &pub_key)
            .map(|p| {
                p.public_key = pub_key;
                p.unmerged_leaves = vec![];
            })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn apply_update_path<IP, CP>(
        &mut self,
        sender: LeafIndex,
        update_path: &ValidatedUpdatePath,
        extensions: &ExtensionList,
        identity_provider: IP,
        cipher_suite_provider: &CP,
    ) -> Result<(), MlsError>
    where
        IP: IdentityProvider,
        CP: CipherSuiteProvider,
    {
        let existing_leaf = self.nodes.borrow_as_leaf_mut(sender)?;

        #[cfg(feature = "tree_index")]
        let original_leaf_node = existing_leaf.clone();

        #[cfg(feature = "tree_index")]
        let original_identity = identity_provider
            .identity(&original_leaf_node.signing_identity, extensions)
            .await
            .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

        *existing_leaf = update_path.leaf_node.clone();

        let path = self.nodes.direct_copath(sender);

        for (node, pn) in update_path.nodes.iter().zip(path) {
            node.as_ref()
                .map(|n| self.update_node(n.public_key.clone(), pn.path))
                .transpose()?;
        }

        #[cfg(feature = "tree_index")]
        self.index.remove(&original_leaf_node, &original_identity);

        index_insert(
            #[cfg(feature = "tree_index")]
            &mut self.index,
            #[cfg(not(feature = "tree_index"))]
            &self.nodes,
            &update_path.leaf_node,
            sender,
            &identity_provider,
            extensions,
        )
        .await?;

        self.update_parent_hashes(sender, true, cipher_suite_provider)
            .await?;

        Ok(())
    }

    fn update_unmerged(&mut self, index: LeafIndex) -> Result<(), MlsError> {
        for i in self.nodes.direct_copath(index) {
            if let Ok(p) = self.nodes.borrow_as_parent_mut(i.path) {
                match p.unmerged_leaves.binary_search(&index) {
                    Ok(_) => return Err(MlsError::ParentHashMismatch),
                    Err(to_insert) => p.unmerged_leaves.insert(to_insert, index),
                }
            }
        }

        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn apply_remove<T, I>(
        &mut self,
        index: LeafIndex,
        p_index: usize,
        is_by_value: bool,
        proposal_bundle: &mut ProposalBundle,
        extensions: &ExtensionList,
        id_provider: &I,
        filter: bool,
    ) -> Result<(), MlsError>
    where
        I: IdentityProvider,
        T: Proposable,
    {
        let res = self.nodes.blank_leaf_node(index);

        if res.is_ok() {
            self.nodes.blank_direct_path(index)?;
        }

        #[cfg(feature = "tree_index")]
        if let Ok(old_leaf) = &res {
            let identity = identity(&old_leaf.signing_identity, id_provider, extensions).await?;

            self.index.remove(old_leaf, &identity);
        }

        if is_by_value || !filter {
            res?;
        } else if res.is_err() {
            proposal_bundle.remove::<T>(p_index);
        }

        Ok(())
    }

    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn batch_edit<I, CP>(
        &mut self,
        proposal_bundle: &mut ProposalBundle,
        extensions: &ExtensionList,
        id_provider: &I,
        cipher_suite_provider: &CP,
        filter: bool,
    ) -> Result<Vec<LeafIndex>, MlsError>
    where
        I: IdentityProvider,
        CP: CipherSuiteProvider,
    {
        #[cfg(all(feature = "custom_proposal", feature = "self_remove_proposal"))]
        let mut self_removed = vec![];
        #[cfg(all(feature = "custom_proposal", feature = "self_remove_proposal"))]
        for i in (0..proposal_bundle.self_removes.len()).rev() {
            let index = match proposal_bundle.self_removes[i].sender {
                crate::group::Sender::Member(idx) => LeafIndex::try_from(idx)?,
                _ => continue,
            };
            self_removed.push(index);
            self.apply_remove::<SelfRemoveProposal, I>(
                index,
                i,
                proposal_bundle.self_removes[i].is_by_value(),
                proposal_bundle,
                extensions,
                id_provider,
                filter,
            )
            .await?;
        }
        for i in (0..proposal_bundle.remove_proposals().len()).rev() {
            let index = proposal_bundle.remove_proposals()[i].proposal.to_remove;
            self.apply_remove::<RemoveProposal, I>(
                index,
                i,
                proposal_bundle.remove_proposals()[i].is_by_value(),
                proposal_bundle,
                extensions,
                id_provider,
                filter,
            )
            .await?;
        }

        let mut partial_updates = vec![];
        let senders = proposal_bundle.update_senders.iter().copied();

        for (i, (p, index)) in proposal_bundle.updates.iter().zip(senders).enumerate() {
            let new_leaf = p.proposal.leaf_node.clone();

            match self.nodes.blank_leaf_node(index) {
                Ok(old_leaf) => {
                    #[cfg(feature = "tree_index")]
                    let old_id =
                        identity(&old_leaf.signing_identity, id_provider, extensions).await?;

                    #[cfg(feature = "tree_index")]
                    self.index.remove(&old_leaf, &old_id);

                    partial_updates.push((index, old_leaf, new_leaf, i));
                }
                _ => {
                    if !filter || !p.is_by_reference() {
                        return Err(MlsError::UpdatingNonExistingMember);
                    }
                }
            }
        }

        #[cfg(feature = "tree_index")]
        let index_clone = self.index.clone();

        let mut removed_leaves = vec![];
        let mut updated_indices = vec![];
        let mut bad_indices = vec![];

        for (index, old_leaf, new_leaf, i) in partial_updates.into_iter() {
            #[cfg(feature = "tree_index")]
            let res =
                index_insert(&mut self.index, &new_leaf, index, id_provider, extensions).await;

            #[cfg(not(feature = "tree_index"))]
            let res = index_insert(&self.nodes, &new_leaf, index, id_provider, extensions).await;

            let err = res.is_err();

            if !filter {
                res?;
            }

            if !err {
                self.nodes.insert_leaf(index, new_leaf);
                removed_leaves.push(old_leaf);
                updated_indices.push(index);
            } else {
                #[cfg(feature = "tree_index")]
                let res =
                    index_insert(&mut self.index, &old_leaf, index, id_provider, extensions).await;

                #[cfg(not(feature = "tree_index"))]
                let res =
                    index_insert(&self.nodes, &old_leaf, index, id_provider, extensions).await;

                if res.is_ok() {
                    self.nodes.insert_leaf(index, old_leaf);
                    bad_indices.push(i);
                } else {
                    #[cfg(feature = "tree_index")]
                    {
                        self.index = index_clone;
                    }

                    removed_leaves
                        .into_iter()
                        .zip(updated_indices.iter())
                        .for_each(|(leaf, index)| self.nodes.insert_leaf(*index, leaf));

                    updated_indices = vec![];
                    break;
                }
            }
        }

        updated_indices
            .iter()
            .try_for_each(|index| self.nodes.blank_direct_path(*index).map(|_| ()))?;

        if updated_indices.is_empty() {
            proposal_bundle.updates = vec![];
        } else {
            for i in bad_indices.into_iter().rev() {
                proposal_bundle.remove::<UpdateProposal>(i);
                proposal_bundle.update_senders.remove(i);
            }
        }

        let mut start = LeafIndex::unchecked(0);
        let mut added = vec![];
        let mut bad_indexes = vec![];

        for i in 0..proposal_bundle.additions.len() {
            let leaf = proposal_bundle.additions[i]
                .proposal
                .key_package
                .leaf_node
                .clone();

            let res = self
                .add_leaf(leaf, id_provider, extensions, Some(start))
                .await;

            if let Ok(index) = res {
                start = index;
                added.push(start);
            } else if proposal_bundle.additions[i].is_by_value() || !filter {
                res?;
            } else {
                bad_indexes.push(i);
            }
        }

        for i in bad_indexes.into_iter().rev() {
            proposal_bundle.remove::<AddProposal>(i);
        }

        self.nodes.trim();

        let chained = proposal_bundle
            .remove_proposals()
            .iter()
            .map(|p| p.proposal.to_remove)
            .chain(updated_indices)
            .chain(added.iter().copied());

        #[cfg(all(feature = "custom_proposal", feature = "self_remove_proposal"))]
        let chained = chained.chain(self_removed);

        let updated_leaves = chained.collect_vec();

        self.update_hashes(&updated_leaves, cipher_suite_provider)
            .await?;

        Ok(added)
    }

    #[cfg(not(feature = "by_ref_proposal"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn batch_edit_lite<I, CP>(
        &mut self,
        proposal_bundle: &ProposalBundle,
        extensions: &ExtensionList,
        id_provider: &I,
        cipher_suite_provider: &CP,
    ) -> Result<Vec<LeafIndex>, MlsError>
    where
        I: IdentityProvider,
        CP: CipherSuiteProvider,
    {
        for p in &proposal_bundle.removals {
            let index = p.proposal.to_remove;

            #[cfg(feature = "tree_index")]
            {
                let old_leaf = self.nodes.blank_leaf_node(index)?;

                let identity =
                    identity(&old_leaf.signing_identity, id_provider, extensions).await?;

                self.index.remove(&old_leaf, &identity);
            }

            #[cfg(not(feature = "tree_index"))]
            self.nodes.blank_leaf_node(index)?;

            self.nodes.blank_direct_path(index)?;
        }

        let mut start = LeafIndex::unchecked(0);
        let mut added = vec![];

        for p in &proposal_bundle.additions {
            let leaf = p.proposal.key_package.leaf_node.clone();
            start = self
                .add_leaf(leaf, id_provider, extensions, Some(start))
                .await?;
            added.push(start);
        }

        self.nodes.trim();

        let updated_leaves = proposal_bundle
            .remove_proposals()
            .iter()
            .map(|p| p.proposal.to_remove)
            .chain(added.iter().copied())
            .collect_vec();

        self.update_hashes(&updated_leaves, cipher_suite_provider)
            .await?;

        Ok(added)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn add_leaf<I: IdentityProvider>(
        &mut self,
        leaf: LeafNode,
        id_provider: &I,
        extensions: &ExtensionList,
        start: Option<LeafIndex>,
    ) -> Result<LeafIndex, MlsError> {
        let index = self
            .nodes
            .next_empty_leaf(start.unwrap_or(LeafIndex::unchecked(0)));

        #[cfg(feature = "tree_index")]
        index_insert(&mut self.index, &leaf, index, id_provider, extensions).await?;

        #[cfg(not(feature = "tree_index"))]
        index_insert(&self.nodes, &leaf, index, id_provider, extensions).await?;

        self.nodes.insert_leaf(index, leaf);
        self.update_unmerged(index)?;

        Ok(index)
    }
}

#[cfg(feature = "tree_index")]
#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
async fn identity<I: IdentityProvider>(
    signing_id: &SigningIdentity,
    provider: &I,
    extensions: &ExtensionList,
) -> Result<Vec<u8>, MlsError> {
    provider
        .identity(signing_id, extensions)
        .await
        .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))
}

#[cfg(feature = "std")]
impl Display for TreeKemPublic {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", tree_utils::build_ascii_tree(&self.nodes))
    }
}
