// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use super::*;
#[cfg(feature = "tree_index")]
use core::fmt::{self, Debug};

#[cfg(all(feature = "tree_index", feature = "custom_proposal"))]
use crate::group::proposal::ProposalType;

#[cfg(feature = "tree_index")]
use crate::{
    identity::CredentialType,
    map::{LargeMap, LargeMapEntry},
};

#[cfg(feature = "tree_index")]
use mls_rs_core::crypto::SignaturePublicKey;

#[cfg(all(feature = "tree_index", feature = "std"))]
use itertools::Itertools;

#[cfg(all(feature = "tree_index", not(feature = "std")))]
use alloc::collections::BTreeSet;

#[cfg(feature = "tree_index")]
use mls_rs_core::crypto::HpkePublicKey;

#[cfg(feature = "tree_index")]
#[derive(Clone, Default, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode, Hash, PartialOrd, Ord)]
pub struct Identifier(#[mls_codec(with = "mls_rs_codec::byte_vec")] Vec<u8>);

#[cfg(feature = "tree_index")]
impl Debug for Identifier {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("Identifier")
            .fmt(f)
    }
}

#[cfg(feature = "tree_index")]
#[derive(Clone, Debug, Default, PartialEq, MlsSize, MlsEncode, MlsDecode)]
pub struct TreeIndex {
    credential_signature_key: LargeMap<SignaturePublicKey, LeafIndex>,
    hpke_key: LargeMap<HpkePublicKey, LeafIndex>,
    identities: LargeMap<Identifier, LeafIndex>,
    credential_type_counters: LargeMap<CredentialType, TypeCounter>,
    #[cfg(feature = "custom_proposal")]
    proposal_type_counter: LargeMap<ProposalType, u32>,
}

#[cfg(feature = "tree_index")]
#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(super) async fn index_insert<I: IdentityProvider>(
    tree_index: &mut TreeIndex,
    new_leaf: &LeafNode,
    new_leaf_idx: LeafIndex,
    id_provider: &I,
    extensions: &ExtensionList,
) -> Result<(), MlsError> {
    let new_id = id_provider
        .identity(&new_leaf.signing_identity, extensions)
        .await
        .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

    tree_index.insert(new_leaf_idx, new_leaf, new_id)
}

#[cfg(not(feature = "tree_index"))]
#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(super) async fn index_insert<I: IdentityProvider>(
    nodes: &NodeVec,
    new_leaf: &LeafNode,
    new_leaf_idx: LeafIndex,
    id_provider: &I,
    extensions: &ExtensionList,
) -> Result<(), MlsError> {
    let new_id = id_provider
        .identity(&new_leaf.signing_identity, extensions)
        .await
        .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

    for (i, leaf) in nodes.non_empty_leaves().filter(|(i, _)| i != &new_leaf_idx) {
        (new_leaf.public_key != leaf.public_key)
            .then_some(())
            .ok_or(MlsError::DuplicateLeafData(*i))?;

        (new_leaf.signing_identity.signature_key != leaf.signing_identity.signature_key)
            .then_some(())
            .ok_or(MlsError::DuplicateLeafData(*i))?;

        let id = id_provider
            .identity(&leaf.signing_identity, extensions)
            .await
            .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

        (new_id != id)
            .then_some(())
            .ok_or(MlsError::DuplicateLeafData(*i))?;

        let cred_type = leaf.signing_identity.credential.credential_type();

        new_leaf
            .capabilities
            .credentials
            .contains(&cred_type)
            .then_some(())
            .ok_or(MlsError::InUseCredentialTypeUnsupportedByNewLeaf)?;

        let new_cred_type = new_leaf.signing_identity.credential.credential_type();

        leaf.capabilities
            .credentials
            .contains(&new_cred_type)
            .then_some(())
            .ok_or(MlsError::CredentialTypeOfNewLeafIsUnsupported)?;
    }

    Ok(())
}

#[cfg(feature = "tree_index")]
impl TreeIndex {
    pub fn new() -> Self {
        Default::default()
    }

    pub fn is_initialized(&self) -> bool {
        !self.identities.is_empty()
    }

    fn insert(
        &mut self,
        index: LeafIndex,
        leaf_node: &LeafNode,
        identity: Vec<u8>,
    ) -> Result<(), MlsError> {
        let old_leaf_count = self.credential_signature_key.len();

        let pub_key = leaf_node.signing_identity.signature_key.clone();
        let credential_entry = self.credential_signature_key.entry(pub_key);

        if let LargeMapEntry::Occupied(entry) = credential_entry {
            return Err(MlsError::DuplicateLeafData(**entry.get()));
        }

        let hpke_entry = self.hpke_key.entry(leaf_node.public_key.clone());

        if let LargeMapEntry::Occupied(entry) = hpke_entry {
            return Err(MlsError::DuplicateLeafData(**entry.get()));
        }

        let identity_entry = self.identities.entry(Identifier(identity));
        if let LargeMapEntry::Occupied(entry) = identity_entry {
            return Err(MlsError::DuplicateLeafData(**entry.get()));
        }

        let in_use_cred_type_unsupported_by_new_leaf = self
            .credential_type_counters
            .iter()
            .filter_map(|(cred_type, counters)| Some(*cred_type).filter(|_| counters.used > 0))
            .find(|cred_type| !leaf_node.capabilities.credentials.contains(cred_type));

        if in_use_cred_type_unsupported_by_new_leaf.is_some() {
            return Err(MlsError::InUseCredentialTypeUnsupportedByNewLeaf);
        }

        let new_leaf_cred_type = leaf_node.signing_identity.credential.credential_type();

        let cred_type_counters = self
            .credential_type_counters
            .entry(new_leaf_cred_type)
            .or_default();

        if cred_type_counters.supported != old_leaf_count as u32 {
            return Err(MlsError::CredentialTypeOfNewLeafIsUnsupported);
        }

        cred_type_counters.used += 1;

        let credential_type_iter = leaf_node.capabilities.credentials.iter().copied();

        #[cfg(feature = "std")]
        let credential_type_iter = credential_type_iter.unique();

        #[cfg(not(feature = "std"))]
        let credential_type_iter = credential_type_iter.collect::<BTreeSet<_>>().into_iter();

        credential_type_iter.for_each(|cred_type| {
            self.credential_type_counters
                .entry(cred_type)
                .or_default()
                .supported += 1;
        });

        #[cfg(feature = "custom_proposal")]
        {
            let proposal_type_iter = leaf_node.capabilities.proposals.iter().copied();

            #[cfg(feature = "std")]
            let proposal_type_iter = proposal_type_iter.unique();

            #[cfg(not(feature = "std"))]
            let proposal_type_iter = proposal_type_iter.collect::<BTreeSet<_>>().into_iter();

            proposal_type_iter.for_each(|proposal_type| {
                *self.proposal_type_counter.entry(proposal_type).or_default() += 1;
            });
        }

        identity_entry.or_insert(index);
        credential_entry.or_insert(index);
        hpke_entry.or_insert(index);

        Ok(())
    }

    pub(crate) fn get_leaf_index_with_identity(&self, identity: &[u8]) -> Option<LeafIndex> {
        self.identities.get(&Identifier(identity.to_vec())).copied()
    }

    pub fn remove(&mut self, leaf_node: &LeafNode, identity: &[u8]) {
        let existed = self
            .identities
            .remove(&Identifier(identity.to_vec()))
            .is_some();

        self.credential_signature_key
            .remove(&leaf_node.signing_identity.signature_key);

        self.hpke_key.remove(&leaf_node.public_key);

        if !existed {
            return;
        }

        let leaf_cred_type = leaf_node.signing_identity.credential.credential_type();

        if let Some(counters) = self.credential_type_counters.get_mut(&leaf_cred_type) {
            counters.used -= 1;
        }

        let credential_type_iter = leaf_node.capabilities.credentials.iter();

        #[cfg(feature = "std")]
        let credential_type_iter = credential_type_iter.unique();

        #[cfg(not(feature = "std"))]
        let credential_type_iter = credential_type_iter.collect::<BTreeSet<_>>().into_iter();

        credential_type_iter.for_each(|cred_type| {
            if let Some(counters) = self.credential_type_counters.get_mut(cred_type) {
                counters.supported -= 1;
            }
        });

        #[cfg(feature = "custom_proposal")]
        {
            let proposal_type_iter = leaf_node.capabilities.proposals.iter();

            #[cfg(feature = "std")]
            let proposal_type_iter = proposal_type_iter.unique();

            #[cfg(not(feature = "std"))]
            let proposal_type_iter = proposal_type_iter.collect::<BTreeSet<_>>().into_iter();

            proposal_type_iter.for_each(|proposal_type| {
                if let Some(supported) = self.proposal_type_counter.get_mut(proposal_type) {
                    *supported -= 1;
                }
            })
        }
    }

    #[cfg(feature = "custom_proposal")]
    pub fn count_supporting_proposal(&self, proposal_type: ProposalType) -> u32 {
        self.proposal_type_counter
            .get(&proposal_type)
            .copied()
            .unwrap_or_default()
    }

}

#[cfg(feature = "tree_index")]
#[derive(Clone, Debug, Default, PartialEq, MlsEncode, MlsDecode, MlsSize)]
struct TypeCounter {
    supported: u32,
    used: u32,
}
