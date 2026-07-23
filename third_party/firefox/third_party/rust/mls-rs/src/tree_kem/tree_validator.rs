// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

#[cfg(feature = "std")]
use std::collections::HashSet;

#[cfg(not(feature = "std"))]
use alloc::{vec, vec::Vec};
use tree_math::TreeIndex;

use super::node::{Node, NodeIndex};
use crate::client::MlsError;
use crate::crypto::CipherSuiteProvider;
use crate::group::GroupContext;
use crate::iter::wrap_impl_iter;
use crate::time::MlsTime;
use crate::tree_kem::math as tree_math;
use crate::tree_kem::{leaf_node_validator::LeafNodeValidator, TreeKemPublic};
use mls_rs_core::identity::{IdentityProvider, MemberValidationContext};

#[cfg(all(not(mls_build_async), feature = "rayon"))]
use rayon::prelude::*;

#[cfg(mls_build_async)]
use futures::{StreamExt, TryStreamExt};

pub(crate) struct TreeValidator<'a, C, CSP>
where
    C: IdentityProvider,
    CSP: CipherSuiteProvider,
{
    expected_tree_hash: &'a [u8],
    leaf_node_validator: LeafNodeValidator<'a, C, CSP>,
    group_id: &'a [u8],
    cipher_suite_provider: &'a CSP,
}

impl<'a, C: IdentityProvider, CSP: CipherSuiteProvider> TreeValidator<'a, C, CSP> {
    pub fn new(
        cipher_suite_provider: &'a CSP,
        context: &'a GroupContext,
        identity_provider: &'a C,
    ) -> Self {
        let member_validation_context = MemberValidationContext::ForNewGroup {
            current_context: context,
        };

        TreeValidator {
            expected_tree_hash: &context.tree_hash,
            leaf_node_validator: LeafNodeValidator::new(
                cipher_suite_provider,
                identity_provider,
                member_validation_context,
            ),
            group_id: &context.group_id,
            cipher_suite_provider,
        }
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn validate(
        &self,
        tree: &mut TreeKemPublic,
        maybe_time: Option<MlsTime>,
    ) -> Result<(), MlsError> {
        self.validate_tree_hash(tree).await?;

        tree.validate_parent_hashes(self.cipher_suite_provider)
            .await?;

        self.validate_no_trailing_blanks(tree)?;
        self.validate_leaves(tree, maybe_time).await?;
        validate_unmerged(tree)
    }

    fn validate_no_trailing_blanks(&self, tree: &TreeKemPublic) -> Result<(), MlsError> {
        tree.nodes
            .last()
            .ok_or(MlsError::UnexpectedEmptyTree)?
            .is_some()
            .then_some(())
            .ok_or(MlsError::UnexpectedTrailingBlanks)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn validate_tree_hash(&self, tree: &mut TreeKemPublic) -> Result<(), MlsError> {
        let tree_hash = tree.tree_hash(self.cipher_suite_provider).await?;

        if tree_hash != self.expected_tree_hash {
            return Err(MlsError::TreeHashMismatch);
        }

        Ok(())
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn validate_leaves(
        &self,
        tree: &TreeKemPublic,
        maybe_time: Option<MlsTime>,
    ) -> Result<(), MlsError> {
        let leaves = wrap_impl_iter(tree.nodes.non_empty_leaves());

        #[cfg(mls_build_async)]
        let leaves = leaves.map(Ok);

        { leaves }
            .try_for_each(|(index, leaf_node)| async move {
                self.leaf_node_validator
                    .revalidate(leaf_node, self.group_id, *index, maybe_time)
                    .await
            })
            .await
    }
}

fn validate_unmerged(tree: &TreeKemPublic) -> Result<(), MlsError> {
    tree.nodes
        .iter()
        .flatten()
        .all(|n| match n {
            Node::Leaf(_) => true,
            Node::Parent(p) => p.unmerged_leaves.is_sorted(),
        })
        .then_some(())
        .ok_or(MlsError::ParentHashMismatch)?;

    let unmerged_sets = tree.nodes.iter().map(|n| {
        #[cfg(feature = "std")]
        if let Some(Node::Parent(p)) = n {
            HashSet::from_iter(p.unmerged_leaves.iter())
        } else {
            HashSet::new()
        }

        #[cfg(not(feature = "std"))]
        if let Some(Node::Parent(p)) = n {
            p.unmerged_leaves.iter().collect()
        } else {
            vec![]
        }
    });

    let mut unmerged_sets = unmerged_sets.collect::<Vec<_>>();

    let leaf_count = tree.total_leaf_count();

    for (index, _) in tree.nodes.non_empty_leaves() {
        let mut n = NodeIndex::from(index);

        while let Some(ps) = n.parent_sibling(&leaf_count) {
            if tree.nodes.is_blank(ps.parent)? {
                n = ps.parent;
                continue;
            }

            let parent_node = tree.nodes.borrow_as_parent(ps.parent)?;

            if parent_node.unmerged_leaves.contains(&index) {
                unmerged_sets[ps.parent as usize].retain(|i| **i != index);

                n = ps.parent;
            } else {
                break;
            }
        }
    }

    let unmerged_sets = unmerged_sets.iter().all(|set| set.is_empty());

    unmerged_sets
        .then_some(())
        .ok_or(MlsError::UnmergedLeavesMismatch)
}
