// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::{vec, vec::Vec};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::{
    error::IntoAnyError,
    group::GroupContext,
    identity::{IdentityProvider, MemberValidationContext},
};

use super::{
    leaf_node::LeafNode,
    leaf_node_validator::{LeafNodeValidator, ValidationContext},
    node::LeafIndex,
};
use crate::{
    client::MlsError,
    crypto::{CipherSuiteProvider, HpkeCiphertext, HpkePublicKey},
};
use crate::{group::message_processor::ProvisionalState, time::MlsTime};

#[derive(Clone, Debug, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct UpdatePathNode {
    pub public_key: HpkePublicKey,
    pub encrypted_path_secret: Vec<HpkeCiphertext>,
}

#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct UpdatePath {
    pub leaf_node: LeafNode,
    pub nodes: Vec<UpdatePathNode>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ValidatedUpdatePath {
    pub leaf_node: LeafNode,
    pub nodes: Vec<Option<UpdatePathNode>>,
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn validate_update_path<C: IdentityProvider, CSP: CipherSuiteProvider>(
    identity_provider: &C,
    cipher_suite_provider: &CSP,
    path: UpdatePath,
    state: &ProvisionalState,
    sender: LeafIndex,
    commit_time: Option<MlsTime>,
    current_context: &GroupContext,
) -> Result<ValidatedUpdatePath, MlsError> {
    let member_validation_context = MemberValidationContext::ForCommit {
        current_context,
        new_extensions: &state.group_context.extensions,
    };

    let leaf_validator = LeafNodeValidator::new(
        cipher_suite_provider,
        identity_provider,
        member_validation_context,
    );

    leaf_validator
        .check_if_valid(
            &path.leaf_node,
            ValidationContext::Commit((&state.group_context.group_id, *sender, commit_time)),
        )
        .await?;

    let check_identity_eq = state.applied_proposals.external_initializations.is_empty();

    if check_identity_eq {
        let existing_leaf = state.public_tree.nodes.borrow_as_leaf(sender)?;
        let original_leaf_node = existing_leaf.clone();

        identity_provider
            .valid_successor(
                &original_leaf_node.signing_identity,
                &path.leaf_node.signing_identity,
                &state.group_context.extensions,
            )
            .await
            .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?
            .then_some(())
            .ok_or(MlsError::InvalidSuccessor)?;

        (existing_leaf.public_key != path.leaf_node.public_key)
            .then_some(())
            .ok_or(MlsError::SameHpkeKey(*sender))?;
    }

    let filtered = state.public_tree.nodes.filtered(sender)?;
    let mut unfiltered_nodes = vec![];
    let mut i = 0;

    for n in path.nodes {
        while *filtered.get(i).ok_or(MlsError::WrongPathLen)? {
            unfiltered_nodes.push(None);
            i += 1;
        }

        unfiltered_nodes.push(Some(n));
        i += 1;
    }

    Ok(ValidatedUpdatePath {
        leaf_node: path.leaf_node,
        nodes: unfiltered_nodes,
    })
}
