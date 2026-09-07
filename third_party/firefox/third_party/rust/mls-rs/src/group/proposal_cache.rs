// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;

use super::{
    message_processor::ProvisionalState,
    mls_rules::{CommitDirection, CommitSource, MlsRules},
    proposal_filter::prepare_proposals_for_mls_rules,
    GroupState, ProposalOrRef,
};
use crate::{
    client::MlsError,
    group::{
        proposal_filter::{ProposalApplier, ProposalBundle, ProposalSource},
        Proposal, Sender,
    },
    time::MlsTime,
};

#[cfg(feature = "by_ref_proposal")]
use crate::{
    group::{message_hash::MessageHash, ProposalMessageDescription, ProposalRef, ProtocolVersion},
    MlsMessage,
};

use crate::tree_kem::leaf_node::LeafNode;

#[cfg(feature = "by_ref_proposal")]
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};

use mls_rs_core::{
    crypto::CipherSuiteProvider, error::IntoAnyError, identity::IdentityProvider,
    psk::PreSharedKeyStorage,
};

#[cfg(feature = "by_ref_proposal")]
use core::fmt::{self, Debug};

#[cfg(feature = "by_ref_proposal")]
#[derive(Debug, Clone, MlsSize, MlsEncode, MlsDecode, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct CachedProposal {
    pub(crate) proposal: Proposal,
    pub(crate) sender: Sender,
}

#[cfg(feature = "by_ref_proposal")]
#[derive(Clone, MlsSize, MlsEncode, MlsDecode)]
pub(crate) struct ProposalCache {
    protocol_version: ProtocolVersion,
    group_id: Vec<u8>,
    pub(crate) proposals: crate::map::SmallMap<ProposalRef, CachedProposal>,
    pub(crate) own_proposals: crate::map::SmallMap<MessageHash, ProposalMessageDescription>,
}

#[cfg(feature = "by_ref_proposal")]
impl PartialEq for ProposalCache {
    fn eq(&self, other: &Self) -> bool {
        self.protocol_version == other.protocol_version
            && self.group_id == other.group_id
            && self.proposals == other.proposals
    }
}

#[cfg(feature = "by_ref_proposal")]
impl Debug for ProposalCache {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ProposalCache")
            .field("protocol_version", &self.protocol_version)
            .field(
                "group_id",
                &mls_rs_core::debug::pretty_group_id(&self.group_id),
            )
            .field("proposals", &self.proposals)
            .finish()
    }
}

#[cfg(feature = "by_ref_proposal")]
impl ProposalCache {
    pub fn new(protocol_version: ProtocolVersion, group_id: Vec<u8>) -> Self {
        Self {
            protocol_version,
            group_id,
            proposals: Default::default(),
            own_proposals: Default::default(),
        }
    }

    pub fn import(
        protocol_version: ProtocolVersion,
        group_id: Vec<u8>,
        proposals: crate::map::SmallMap<ProposalRef, CachedProposal>,
        own_proposals: crate::map::SmallMap<MessageHash, ProposalMessageDescription>,
    ) -> Self {
        Self {
            protocol_version,
            group_id,
            proposals,
            own_proposals,
        }
    }

    pub fn clear(&mut self) {
        self.proposals.clear();
        self.own_proposals.clear();
    }

    #[cfg(feature = "by_ref_proposal")]
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.proposals.is_empty()
    }

    pub fn insert(&mut self, proposal_ref: ProposalRef, proposal: Proposal, sender: Sender) {
        let cached_proposal = CachedProposal { proposal, sender };

        #[cfg(feature = "std")]
        self.proposals.insert(proposal_ref, cached_proposal);

        #[cfg(not(feature = "std"))]
        self.proposals.push((proposal_ref, cached_proposal));
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn insert_own<CS: CipherSuiteProvider>(
        &mut self,
        proposal: ProposalMessageDescription,
        message: &MlsMessage,
        sender: Sender,
        cs: &CS,
    ) -> Result<(), MlsError> {
        self.insert(
            proposal.proposal_ref.clone(),
            proposal.proposal.clone(),
            sender,
        );

        let message_hash = MessageHash::compute(cs, message).await?;
        self.own_proposals.insert(message_hash, proposal);

        Ok(())
    }

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal", feature = "self_remove_proposal"))]
pub(crate) fn has_own_self_remove(&self) -> bool {
        self.own_proposals
            .values()
            .any(|p| matches!(p.proposal, Proposal::SelfRemove(_)))
    }

    pub fn prepare_commit(
        &self,
        sender: Sender,
        additional_proposals: Vec<Proposal>,
    ) -> ProposalBundle {
        self.proposals
            .iter()
            .map(|(r, p)| {
                (
                    p.proposal.clone(),
                    p.sender,
                    ProposalSource::ByReference(r.clone()),
                )
            })
            .chain(
                additional_proposals
                    .into_iter()
                    .map(|p| (p, sender, ProposalSource::ByValue)),
            )
            .collect()
    }

    pub fn resolve_for_commit(
        &self,
        sender: Sender,
        proposal_list: Vec<ProposalOrRef>,
    ) -> Result<ProposalBundle, MlsError> {
        let mut proposals = ProposalBundle::default();

        for p in proposal_list {
            match p {
                ProposalOrRef::Proposal(p) => proposals.add(*p, sender, ProposalSource::ByValue),
                ProposalOrRef::Reference(r) => {
                    #[cfg(feature = "std")]
                    let p = self
                        .proposals
                        .get(&r)
                        .ok_or(MlsError::ProposalNotFound)?
                        .clone();
                    #[cfg(not(feature = "std"))]
                    let p = self
                        .proposals
                        .iter()
                        .find_map(|(rr, p)| (rr == &r).then_some(p))
                        .ok_or(MlsError::ProposalNotFound)?
                        .clone();

                    proposals.add(p.proposal, p.sender, ProposalSource::ByReference(r));
                }
            };
        }

        Ok(proposals)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn get_own<CS: CipherSuiteProvider>(
        &self,
        cs: &CS,
        message: &MlsMessage,
    ) -> Result<Option<ProposalMessageDescription>, MlsError> {
        let message_hash = MessageHash::compute(cs, message).await?;

        Ok(self.own_proposals.get(&message_hash).cloned())
    }
}

#[cfg(not(feature = "by_ref_proposal"))]
pub(crate) fn prepare_commit(
    sender: Sender,
    additional_proposals: Vec<Proposal>,
) -> ProposalBundle {
    let mut proposals = ProposalBundle::default();

    for p in additional_proposals.into_iter() {
        proposals.add(p, sender, ProposalSource::ByValue);
    }

    proposals
}

#[cfg(not(feature = "by_ref_proposal"))]
pub(crate) fn resolve_for_commit(
    sender: Sender,
    proposal_list: Vec<ProposalOrRef>,
) -> Result<ProposalBundle, MlsError> {
    let mut proposals = ProposalBundle::default();

    for p in proposal_list {
        let ProposalOrRef::Proposal(p) = p;
        proposals.add(*p, sender, ProposalSource::ByValue);
    }

    Ok(proposals)
}

impl GroupState {
    #[inline(never)]
    #[allow(clippy::too_many_arguments)]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn apply_resolved<C, F, P, CSP>(
        &self,
        sender: Sender,
        mut proposals: ProposalBundle,
        external_leaf: Option<&LeafNode>,
        identity_provider: &C,
        cipher_suite_provider: &CSP,
        psk_storage: &P,
        user_rules: &F,
        commit_time: Option<MlsTime>,
        direction: CommitDirection,
    ) -> Result<ProvisionalState, MlsError>
    where
        C: IdentityProvider,
        F: MlsRules,
        P: PreSharedKeyStorage,
        CSP: CipherSuiteProvider,
    {
        let roster = self.public_tree.roster();

        #[cfg(feature = "by_ref_proposal")]
        let all_proposals = proposals.clone();

        let origin = match sender {
            Sender::Member(index) => Ok::<_, MlsError>(CommitSource::ExistingMember(
                roster.member_with_index(index)?,
            )),
            #[cfg(feature = "by_ref_proposal")]
            Sender::NewMemberProposal => Err(MlsError::InvalidSender),
            #[cfg(feature = "by_ref_proposal")]
            Sender::External(_) => Err(MlsError::InvalidSender),
            Sender::NewMemberCommit => Ok(CommitSource::NewMember(
                external_leaf
                    .map(|l| l.signing_identity.clone())
                    .ok_or(MlsError::ExternalCommitMustHaveNewLeaf)?,
            )),
        }?;

        prepare_proposals_for_mls_rules(&mut proposals, direction, &self.public_tree)?;

        proposals = user_rules
            .filter_proposals(direction, origin, &roster, &self.context, proposals)
            .await
            .map_err(|e| MlsError::MlsRulesError(e.into_any_error()))?;

        let applier = ProposalApplier::new(
            &self.public_tree,
            cipher_suite_provider,
            &self.context,
            external_leaf,
            identity_provider,
            psk_storage,
        );

        #[cfg(feature = "by_ref_proposal")]
        let applier_output = applier
            .apply_proposals(direction.into(), &sender, proposals, commit_time)
            .await?;

        #[cfg(not(feature = "by_ref_proposal"))]
        let applier_output = applier
            .apply_proposals(&sender, &proposals, commit_time)
            .await?;

        #[cfg(feature = "by_ref_proposal")]
        let unused_proposals = unused_proposals(
            match direction {
                CommitDirection::Send => all_proposals,
                CommitDirection::Receive => self.proposals.proposals.iter().collect(),
            },
            &applier_output.applied_proposals,
        );

        #[cfg(not(feature = "by_ref_proposal"))]
        let unused_proposals = alloc::vec::Vec::default();

        let mut group_context = self.context.clone();
        group_context.epoch += 1;

        if let Some(ext) = applier_output.new_context_extensions {
            group_context.extensions = ext;
        }

        #[cfg(feature = "by_ref_proposal")]
        let proposals = applier_output.applied_proposals;

        Ok(ProvisionalState {
            public_tree: applier_output.new_tree,
            group_context,
            applied_proposals: proposals,
            external_init_index: applier_output.external_init_index,
            indexes_of_added_kpkgs: applier_output.indexes_of_added_kpkgs,
            unused_proposals,
        })
    }
}

#[cfg(feature = "by_ref_proposal")]
impl Extend<(ProposalRef, CachedProposal)> for ProposalCache {
    fn extend<T>(&mut self, iter: T)
    where
        T: IntoIterator<Item = (ProposalRef, CachedProposal)>,
    {
        self.proposals.extend(iter);
    }
}

#[cfg(feature = "by_ref_proposal")]
fn has_ref(proposals: &ProposalBundle, reference: &ProposalRef) -> bool {
    proposals
        .iter_proposals()
        .any(|p| matches!(&p.source, ProposalSource::ByReference(r) if r == reference))
}

#[cfg(feature = "by_ref_proposal")]
fn unused_proposals(
    all_proposals: ProposalBundle,
    accepted_proposals: &ProposalBundle,
) -> Vec<crate::mls_rules::ProposalInfo<Proposal>> {
    all_proposals
        .into_proposals()
        .filter(|p| {
            matches!(p.source, ProposalSource::ByReference(ref r) if !has_ref(accepted_proposals, r)
            )
        })
        .collect()
}


