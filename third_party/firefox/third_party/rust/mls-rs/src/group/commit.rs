// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::boxed::Box;
use alloc::vec;
use alloc::vec::Vec;
use core::fmt::Debug;
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::{crypto::SignatureSecretKey, error::IntoAnyError};

use crate::{
    cipher_suite::CipherSuite,
    client::MlsError,
    client_config::ClientConfig,
    extension::RatchetTreeExt,
    identity::SigningIdentity,
    protocol_version::ProtocolVersion,
    signer::Signable,
    time::MlsTime,
    tree_kem::{kem::TreeKem, path_secret::PathSecret, TreeKemPrivate, UpdatePath},
    ExtensionList, MlsRules,
};

#[cfg(all(not(mls_build_async), feature = "rayon"))]
use {crate::iter::ParallelIteratorExt, rayon::prelude::*};

use crate::tree_kem::leaf_node::LeafNode;

#[cfg(not(feature = "private_message"))]
use crate::WireFormat;

#[cfg(feature = "psk")]
use crate::{
    group::{JustPreSharedKeyID, PskGroupId, ResumptionPSKUsage, ResumptionPsk},
    psk::ExternalPskId,
};

use super::{
    confirmation_tag::ConfirmationTag,
    framing::{Content, MlsMessage, MlsMessagePayload, Sender},
    key_schedule::{KeySchedule, WelcomeSecret},
    message_hash::MessageHash,
    message_processor::{path_update_required, MessageProcessor},
    message_signature::AuthenticatedContent,
    mls_rules::CommitDirection,
    proposal::{Proposal, ProposalOrRef},
    CommitEffect, CommitMessageDescription, EncryptedGroupSecrets, EpochSecrets, ExportedTree,
    Group, GroupContext, GroupInfo, GroupState, InterimTranscriptHash, NewEpoch,
    PendingCommitSnapshot, Welcome,
};

#[cfg(not(feature = "by_ref_proposal"))]
use super::proposal_cache::prepare_commit;

#[cfg(feature = "custom_proposal")]
use super::proposal::CustomProposal;

#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(mls_rs_core::arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub(crate) struct Commit {
    pub proposals: Vec<ProposalOrRef>,
    pub path: Option<UpdatePath>,
}

#[derive(Clone, PartialEq, Debug, MlsEncode, MlsDecode, MlsSize)]
pub(crate) struct PendingCommit {
    pub(crate) state: GroupState,
    pub(crate) epoch_secrets: EpochSecrets,
    pub(crate) private_tree: TreeKemPrivate,
    pub(crate) key_schedule: KeySchedule,
    pub(crate) signer: SignatureSecretKey,

    pub(crate) output: CommitMessageDescription,

    pub(crate) commit_message_hash: MessageHash,
}

#[derive(Clone)]
pub struct CommitSecrets(pub(crate) PendingCommitSnapshot);

impl CommitSecrets {
    /// Deserialize the commit secrets from bytes
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, MlsError> {
        Ok(MlsDecode::mls_decode(&mut &*bytes).map(Self)?)
    }

    /// Serialize the commit secrets to bytes
    pub fn to_bytes(&self) -> Result<Vec<u8>, MlsError> {
        Ok(self.0.mls_encode_to_vec()?)
    }
}

#[derive(Clone, Debug)]
#[non_exhaustive]
/// Result of MLS commit operation using
/// [`Group::commit`](crate::group::Group::commit) or
/// [`CommitBuilder::build`](CommitBuilder::build).
pub struct CommitOutput {
    /// Commit message to send to other group members.
    pub commit_message: MlsMessage,
    /// Welcome messages to send to new group members. If the commit does not add members,
    /// this list is empty. Otherwise, if [`MlsRules::commit_options`] returns `single_welcome_message`
    /// set to true, then this list contains a single message sent to all members. Else, the list
    /// contains one message for each added member. Recipients of each message can be identified using
    /// [`MlsMessage::key_package_reference`] of their key packages and
    /// [`MlsMessage::welcome_key_package_references`].
    pub welcome_messages: Vec<MlsMessage>,
    /// Ratchet tree that can be sent out of band if
    /// `ratchet_tree_extension` is not used according to
    /// [`MlsRules::commit_options`].
    pub ratchet_tree: Option<ExportedTree<'static>>,
    /// A group info that can be provided to new members in order to enable external commit
    /// functionality. This value is set if [`MlsRules::commit_options`] returns
    /// `allow_external_commit` set to true.
    pub external_commit_group_info: Option<MlsMessage>,
    /// Proposals that were received in the prior epoch but not included in the following commit.
    #[cfg(feature = "by_ref_proposal")]
    pub unused_proposals: Vec<crate::mls_rules::ProposalInfo<Proposal>>,
    /// Indicator that the commit contains a path update
    pub contains_update_path: bool,
}

impl CommitOutput {
    /// Commit message to send to other group members.
    pub fn commit_message(&self) -> &MlsMessage {
        &self.commit_message
    }

    /// Welcome message to send to new group members.
    pub fn welcome_messages(&self) -> &[MlsMessage] {
        &self.welcome_messages
    }

    /// Ratchet tree that can be sent out of band if
    /// `ratchet_tree_extension` is not used according to
    /// [`MlsRules::commit_options`].
    pub fn ratchet_tree(&self) -> Option<&ExportedTree<'static>> {
        self.ratchet_tree.as_ref()
    }

    /// A group info that can be provided to new members in order to enable external commit
    /// functionality. This value is set if [`MlsRules::commit_options`] returns
    /// `allow_external_commit` set to true.
    pub fn external_commit_group_info(&self) -> Option<&MlsMessage> {
        self.external_commit_group_info.as_ref()
    }

    /// Proposals that were received in the prior epoch but not included in the following commit.
    #[cfg(feature = "by_ref_proposal")]
    pub fn unused_proposals(&self) -> &[crate::mls_rules::ProposalInfo<Proposal>] {
        &self.unused_proposals
    }
}

/// Build a commit with multiple proposals by-value.
///
/// Proposals within a commit can be by-value or by-reference.
/// Proposals received during the current epoch will be added to the resulting
/// commit by-reference automatically so long as they pass the rules defined
/// in the current
/// [proposal rules](crate::client_builder::ClientBuilder::mls_rules).
pub struct CommitBuilder<'a, C>
where
    C: ClientConfig + Clone,
{
    group: &'a mut Group<C>,
    pub(super) proposals: Vec<Proposal>,
    authenticated_data: Vec<u8>,
    group_info_extensions: ExtensionList,
    new_signer: Option<SignatureSecretKey>,
    new_signing_identity: Option<SigningIdentity>,
    new_leaf_node_extensions: Option<ExtensionList>,
    commit_time: Option<MlsTime>,
}

impl<'a, C> CommitBuilder<'a, C>
where
    C: ClientConfig + Clone,
{
    /// Insert an [`AddProposal`](crate::group::proposal::AddProposal) into
    /// the current commit that is being built.
    pub fn add_member(mut self, key_package: MlsMessage) -> Result<CommitBuilder<'a, C>, MlsError> {
        let proposal = self.group.add_proposal(key_package)?;
        self.proposals.push(proposal);
        Ok(self)
    }

    /// Set group info extensions that will be inserted into the resulting
    /// [welcome messages](CommitOutput::welcome_messages) for new members.
    ///
    /// Group info extensions that are transmitted as part of a welcome message
    /// are encrypted along with other private values.
    ///
    /// These extensions can be retrieved as part of
    /// [`NewMemberInfo`](crate::group::NewMemberInfo) that is returned
    /// by joining the group via
    /// [`Client::join_group`](crate::Client::join_group).
    pub fn set_group_info_ext(self, extensions: ExtensionList) -> Self {
        Self {
            group_info_extensions: extensions,
            ..self
        }
    }

    /// Insert a [`RemoveProposal`](crate::group::proposal::RemoveProposal) into
    /// the current commit that is being built.
    pub fn remove_member(mut self, index: u32) -> Result<Self, MlsError> {
        let proposal = self.group.remove_proposal(index)?;
        self.proposals.push(proposal);
        Ok(self)
    }

    /// Insert a
    /// [`GroupContextExtensions`](crate::group::proposal::Proposal::GroupContextExtensions)
    /// into the current commit that is being built.
    pub fn set_group_context_ext(mut self, extensions: ExtensionList) -> Result<Self, MlsError> {
        let proposal = self.group.group_context_extensions_proposal(extensions);
        self.proposals.push(proposal);
        Ok(self)
    }

    /// Insert a
    /// [`PreSharedKeyProposal`](crate::group::proposal::PreSharedKeyProposal) with
    /// an external PSK into the current commit that is being built.
    #[cfg(feature = "psk")]
    pub fn add_external_psk(mut self, psk_id: ExternalPskId) -> Result<Self, MlsError> {
        let key_id = JustPreSharedKeyID::External(psk_id);
        let proposal = self.group.psk_proposal(key_id)?;
        self.proposals.push(proposal);
        Ok(self)
    }

    /// Insert a
    /// [`PreSharedKeyProposal`](crate::group::proposal::PreSharedKeyProposal) with
    /// a resumption PSK into the current commit that is being built.
    #[cfg(feature = "psk")]
    pub fn add_resumption_psk(mut self, psk_epoch: u64) -> Result<Self, MlsError> {
        let psk_id = ResumptionPsk {
            psk_epoch,
            usage: ResumptionPSKUsage::Application,
            psk_group_id: PskGroupId(self.group.group_id().to_vec()),
        };

        let key_id = JustPreSharedKeyID::Resumption(psk_id);
        let proposal = self.group.psk_proposal(key_id)?;
        self.proposals.push(proposal);
        Ok(self)
    }

    /// Insert a [`ReInitProposal`](crate::group::proposal::ReInitProposal) into
    /// the current commit that is being built.
    pub fn reinit(
        mut self,
        group_id: Option<Vec<u8>>,
        version: ProtocolVersion,
        cipher_suite: CipherSuite,
        extensions: ExtensionList,
    ) -> Result<Self, MlsError> {
        let proposal = self
            .group
            .reinit_proposal(group_id, version, cipher_suite, extensions)?;

        self.proposals.push(proposal);
        Ok(self)
    }

    /// Insert a [`CustomProposal`](crate::group::proposal::CustomProposal) into
    /// the current commit that is being built.
    #[cfg(feature = "custom_proposal")]
    pub fn custom_proposal(mut self, proposal: CustomProposal) -> Self {
        self.proposals.push(Proposal::Custom(proposal));
        self
    }

    /// Insert a proposal that was previously constructed such as when a
    /// proposal is returned from
    /// [`NewEpoch::unused_proposals`](super::NewEpoch::unused_proposals).
    pub fn raw_proposal(mut self, proposal: Proposal) -> Self {
        self.proposals.push(proposal);
        self
    }

    /// Insert proposals that were previously constructed such as when a
    /// proposal is returned from
    /// [`NewEpoch::unused_proposals`](super::NewEpoch::unused_proposals).
    pub fn raw_proposals(mut self, mut proposals: Vec<Proposal>) -> Self {
        self.proposals.append(&mut proposals);
        self
    }

    /// Add additional authenticated data to the commit.
    ///
    /// # Warning
    ///
    /// The data provided here is always sent unencrypted.
    pub fn authenticated_data(self, authenticated_data: Vec<u8>) -> Self {
        Self {
            authenticated_data,
            ..self
        }
    }

    /// Change the committer's signing identity as part of making this commit.
    /// This will only succeed if the [`IdentityProvider`](crate::IdentityProvider)
    /// in use by the group considers the credential inside this signing_identity
    /// [valid](crate::IdentityProvider::validate_member)
    /// and results in the same
    /// [identity](crate::IdentityProvider::identity)
    /// being used.
    pub fn set_new_signing_identity(
        self,
        signer: SignatureSecretKey,
        signing_identity: SigningIdentity,
    ) -> Self {
        Self {
            new_signer: Some(signer),
            new_signing_identity: Some(signing_identity),
            ..self
        }
    }

    /// Change the committer's leaf node extensions as part of making this commit.
    pub fn set_leaf_node_extensions(self, new_leaf_node_extensions: ExtensionList) -> Self {
        Self {
            new_leaf_node_extensions: Some(new_leaf_node_extensions),
            ..self
        }
    }

    /// Add a time to associate with the commit creation.
    pub fn commit_time(self, commit_time: MlsTime) -> Self {
        Self {
            commit_time: Some(commit_time),
            ..self
        }
    }

    /// Finalize the commit to send.
    ///
    /// # Errors
    ///
    /// This function will return an error if any of the proposals provided
    /// are not contextually valid according to the rules defined by the
    /// MLS RFC, or if they do not pass the custom rules defined by the current
    /// [proposal rules](crate::client_builder::ClientBuilder::mls_rules).
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn build(self) -> Result<CommitOutput, MlsError> {
        let (output, pending_commit) = self
            .group
            .commit_internal(
                self.proposals,
                None,
                self.authenticated_data,
                self.group_info_extensions,
                self.new_signer,
                self.new_signing_identity,
                self.new_leaf_node_extensions,
                self.commit_time,
            )
            .await?;

        self.group.pending_commit = pending_commit.try_into()?;

        Ok(output)
    }

    /// The same function as `GroupBuilder::build` except the secrets generated
    /// for the commit are outputted instead of being cached internally.
    ///
    /// A detached commit can be applied using `Group::apply_detached_commit`.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn build_detached(self) -> Result<(CommitOutput, CommitSecrets), MlsError> {
        let (output, pending_commit) = self
            .group
            .commit_internal(
                self.proposals,
                None,
                self.authenticated_data,
                self.group_info_extensions,
                self.new_signer,
                self.new_signing_identity,
                self.new_leaf_node_extensions,
                self.commit_time,
            )
            .await?;

        Ok((
            output,
            CommitSecrets(PendingCommitSnapshot::PendingCommit(
                pending_commit.mls_encode_to_vec()?,
            )),
        ))
    }
}

impl<C> Group<C>
where
    C: ClientConfig + Clone,
{
    /// Perform a commit of received proposals.
    ///
    /// This function is the equivalent of [`Group::commit_builder`] immediately
    /// followed by [`CommitBuilder::build`]. Any received proposals since the
    /// last commit will be included in the resulting message by-reference.
    ///
    /// Data provided in the `authenticated_data` field will be placed into
    /// the resulting commit message unencrypted.
    ///
    /// # Pending Commits
    ///
    /// When a commit is created, it is not applied immediately in order to
    /// allow for the resolution of conflicts when multiple members of a group
    /// attempt to make commits at the same time. For example, a central relay
    /// can be used to decide which commit should be accepted by the group by
    /// determining a consistent view of commit packet order for all clients.
    ///
    /// Pending commits are stored internally as part of the group's state
    /// so they do not need to be tracked outside of this library. Any commit
    /// message that is processed before calling [Group::apply_pending_commit]
    /// will clear the currently pending commit.
    ///
    /// # Empty Commits
    ///
    /// Sending a commit that contains no proposals is a valid operation
    /// within the MLS protocol. It is useful for providing stronger forward
    /// secrecy and post-compromise security, especially for long running
    /// groups when group membership does not change often.
    ///
    /// # Path Updates
    ///
    /// Path updates provide forward secrecy and post-compromise security
    /// within the MLS protocol.
    /// The `path_required` option returned by [`MlsRules::commit_options`](`crate::MlsRules::commit_options`)
    /// controls the ability of a group to send a commit without a path update.
    /// An update path will automatically be sent if there are no proposals
    /// in the commit, or if any proposal other than
    /// [`Add`](crate::group::proposal::Proposal::Add),
    /// [`Psk`](crate::group::proposal::Proposal::Psk),
    /// or [`ReInit`](crate::group::proposal::Proposal::ReInit) are part of the commit.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn commit(&mut self, authenticated_data: Vec<u8>) -> Result<CommitOutput, MlsError> {
        self.commit_builder()
            .authenticated_data(authenticated_data)
            .build()
            .await
    }

    /// The same function as `Group::commit` except the secrets generated
    /// for the commit are outputted instead of being cached internally.
    ///
    /// A detached commit can be applied using `Group::apply_detached_commit`.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn commit_detached(
        &mut self,
        authenticated_data: Vec<u8>,
    ) -> Result<(CommitOutput, CommitSecrets), MlsError> {
        self.commit_builder()
            .authenticated_data(authenticated_data)
            .build_detached()
            .await
    }

    /// Create a new commit builder that can include proposals
    /// by-value.
    pub fn commit_builder(&mut self) -> CommitBuilder<'_, C> {
        CommitBuilder {
            group: self,
            proposals: Default::default(),
            authenticated_data: Default::default(),
            group_info_extensions: Default::default(),
            new_signer: Default::default(),
            new_signing_identity: Default::default(),
            new_leaf_node_extensions: Default::default(),
            commit_time: None,
        }
    }

    /// Returns commit and optional [`MlsMessage`] containing a welcome message
    /// for newly added members.
    #[allow(clippy::too_many_arguments)]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(super) async fn commit_internal(
        &mut self,
        proposals: Vec<Proposal>,
        external_leaf: Option<&LeafNode>,
        authenticated_data: Vec<u8>,
        mut welcome_group_info_extensions: ExtensionList,
        new_signer: Option<SignatureSecretKey>,
        new_signing_identity: Option<SigningIdentity>,
        new_leaf_node_extensions: Option<ExtensionList>,
        commit_time: Option<MlsTime>,
    ) -> Result<(CommitOutput, PendingCommit), MlsError> {
        if !self.pending_commit.is_none() {
            return Err(MlsError::ExistingPendingCommit);
        }

        if self.state.pending_reinit.is_some() {
            return Err(MlsError::GroupUsedAfterReInit);
        }

        let mls_rules = self.config.mls_rules();

        let is_external = external_leaf.is_some();

        let sender = if is_external {
            Sender::NewMemberCommit
        } else {
            Sender::Member(*self.private_tree.self_index)
        };

        let new_signer = new_signer.unwrap_or_else(|| self.signer.clone());
        let old_signer = &self.signer;

        #[cfg(feature = "std")]
        let time = Some(crate::time::MlsTime::now());

        #[cfg(not(feature = "std"))]
        let time = None;

        let time = if commit_time.is_some() {
            commit_time
        } else {
            time
        };

        #[cfg(feature = "by_ref_proposal")]
        let proposals = self.state.proposals.prepare_commit(sender, proposals);

        #[cfg(not(feature = "by_ref_proposal"))]
        let proposals = prepare_commit(sender, proposals);

        let mut provisional_state = self
            .state
            .apply_resolved(
                sender,
                proposals,
                external_leaf,
                &self.config.identity_provider(),
                &self.cipher_suite_provider,
                &self.config.secret_store(),
                &mls_rules,
                time,
                CommitDirection::Send,
            )
            .await?;

        let (mut provisional_private_tree, _) =
            self.provisional_private_tree(&provisional_state)?;

        if is_external {
            provisional_private_tree.self_index = provisional_state
                .external_init_index
                .ok_or(MlsError::ExternalCommitMissingExternalInit)?;

            self.private_tree.self_index = provisional_private_tree.self_index;
        }

        let commit_options = mls_rules
            .commit_options(
                &provisional_state.public_tree.roster(),
                &provisional_state.group_context,
                &provisional_state.applied_proposals,
            )
            .map_err(|e| MlsError::MlsRulesError(e.into_any_error()))?;

        let perform_path_update = commit_options.path_required
            || path_update_required(&provisional_state.applied_proposals);

        let (update_path, path_secrets, commit_secret) = if perform_path_update {

            let new_leaf_node_extensions =
                new_leaf_node_extensions.or(external_leaf.map(|ln| ln.ungreased_extensions()));

            let new_leaf_node_extensions = match new_leaf_node_extensions {
                Some(extensions) => extensions,
                None => self.current_user_leaf_node()?.ungreased_extensions(),
            };

            let encap_gen = TreeKem::new(
                &mut provisional_state.public_tree,
                &mut provisional_private_tree,
            )
            .encap(
                &mut provisional_state.group_context,
                &provisional_state.indexes_of_added_kpkgs,
                &new_signer,
                Some(self.config.leaf_properties(new_leaf_node_extensions)),
                new_signing_identity,
                &self.cipher_suite_provider,
#[cfg(any())]









                &self.commit_modifiers,
            )
            .await?;

            (
                Some(encap_gen.update_path),
                Some(encap_gen.path_secrets),
                encap_gen.commit_secret,
            )
        } else {
            provisional_state
                .public_tree
                .update_hashes(
                    &[provisional_private_tree.self_index],
                    &self.cipher_suite_provider,
                )
                .await?;

            provisional_state.group_context.tree_hash = provisional_state
                .public_tree
                .tree_hash(&self.cipher_suite_provider)
                .await?;

            (None, None, PathSecret::empty(&self.cipher_suite_provider))
        };

        #[cfg(feature = "psk")]
        let (psk_secret, psks) = self
            .get_psk(&provisional_state.applied_proposals.psks)
            .await?;

        #[cfg(not(feature = "psk"))]
        let psk_secret = self.get_psk();

        let added_key_pkgs: Vec<_> = provisional_state
            .applied_proposals
            .additions
            .iter()
            .map(|info| info.proposal.key_package.clone())
            .collect();

        let commit = Commit {
            proposals: provisional_state.applied_proposals.proposals_or_refs(),
            path: update_path,
        };

        let mut auth_content = AuthenticatedContent::new_signed(
            &self.cipher_suite_provider,
            self.context(),
            sender,
            Content::Commit(Box::new(commit)),
            old_signer,
            #[cfg(feature = "private_message")]
            self.encryption_options()?.control_wire_format(sender),
            #[cfg(not(feature = "private_message"))]
            WireFormat::PublicMessage,
            authenticated_data,
        )
        .await?;

        let confirmed_transcript_hash = super::transcript_hash::create(
            self.cipher_suite_provider(),
            &self.state.interim_transcript_hash,
            &auth_content,
        )
        .await?;

        provisional_state.group_context.confirmed_transcript_hash = confirmed_transcript_hash;

        let key_schedule_result = KeySchedule::from_key_schedule(
            &self.key_schedule,
            &commit_secret,
            &provisional_state.group_context,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            provisional_state.public_tree.total_leaf_count(),
            &psk_secret,
            &self.cipher_suite_provider,
        )
        .await?;

        let confirmation_tag = ConfirmationTag::create(
            &key_schedule_result.confirmation_key,
            &provisional_state.group_context.confirmed_transcript_hash,
            &self.cipher_suite_provider,
        )
        .await?;

        let interim_transcript_hash = InterimTranscriptHash::create(
            self.cipher_suite_provider(),
            &provisional_state.group_context.confirmed_transcript_hash,
            &confirmation_tag,
        )
        .await?;

        auth_content.auth.confirmation_tag = Some(confirmation_tag.clone());

        let ratchet_tree_ext = commit_options
            .ratchet_tree_extension
            .then(|| RatchetTreeExt {
                tree_data: ExportedTree::new(provisional_state.public_tree.nodes.clone()),
            });

        let external_commit_group_info = match commit_options.allow_external_commit {
            true => {
                let mut extensions = ExtensionList::new();

                extensions.set_from({
                    key_schedule_result
                        .key_schedule
                        .get_external_key_pair_ext(&self.cipher_suite_provider)
                        .await?
                })?;

                if let Some(ref ratchet_tree_ext) = ratchet_tree_ext {
                    if !commit_options.always_out_of_band_ratchet_tree {
                        extensions.set_from(ratchet_tree_ext.clone())?;
                    }
                }

                let info = self
                    .make_group_info(
                        &provisional_state.group_context,
                        extensions,
                        &confirmation_tag,
                        &new_signer,
                    )
                    .await?;

                let msg =
                    MlsMessage::new(self.protocol_version(), MlsMessagePayload::GroupInfo(info));

                Some(msg)
            }
            false => None,
        };

        if let Some(ratchet_tree_ext) = ratchet_tree_ext {
            welcome_group_info_extensions.set_from(ratchet_tree_ext)?;
        }

        let welcome_group_info = self
            .make_group_info(
                &provisional_state.group_context,
                welcome_group_info_extensions,
                &confirmation_tag,
                &new_signer,
            )
            .await?;

        let welcome_secret = WelcomeSecret::from_joiner_secret(
            &self.cipher_suite_provider,
            &key_schedule_result.joiner_secret,
            &psk_secret,
        )
        .await?;

        let encrypted_group_info = welcome_secret
            .encrypt(&welcome_group_info.mls_encode_to_vec()?)
            .await?;

        let path_secrets = path_secrets.as_ref();

        #[cfg(not(any(mls_build_async, not(feature = "rayon"))))]
        let encrypted_path_secrets: Vec<_> = added_key_pkgs
            .into_par_iter()
            .zip(&provisional_state.indexes_of_added_kpkgs)
            .map(|(key_package, leaf_index)| {
                self.encrypt_group_secrets(
                    &key_package,
                    *leaf_index,
                    &key_schedule_result.joiner_secret,
                    path_secrets,
                    #[cfg(feature = "psk")]
                    psks.clone(),
                    &encrypted_group_info,
                )
            })
            .try_collect()?;

        #[cfg(any(mls_build_async, not(feature = "rayon")))]
        let encrypted_path_secrets = {
            let mut secrets = Vec::new();

            for (key_package, leaf_index) in added_key_pkgs
                .into_iter()
                .zip(&provisional_state.indexes_of_added_kpkgs)
            {
                secrets.push(
                    self.encrypt_group_secrets(
                        &key_package,
                        *leaf_index,
                        &key_schedule_result.joiner_secret,
                        path_secrets,
                        #[cfg(feature = "psk")]
                        psks.clone(),
                        &encrypted_group_info,
                    )
                    .await?,
                );
            }

            secrets
        };

        let welcome_messages =
            if commit_options.single_welcome_message && !encrypted_path_secrets.is_empty() {
                vec![self.make_welcome_message(encrypted_path_secrets, encrypted_group_info)]
            } else {
                encrypted_path_secrets
                    .into_iter()
                    .map(|s| self.make_welcome_message(vec![s], encrypted_group_info.clone()))
                    .collect()
            };

        let commit_message = self.format_for_wire(auth_content.clone()).await?;

        let ratchet_tree = (!commit_options.ratchet_tree_extension
            || commit_options.always_out_of_band_ratchet_tree)
            .then(|| ExportedTree::new(provisional_state.public_tree.nodes.clone()));

        let pending_reinit = provisional_state
            .applied_proposals
            .reinitializations
            .first();

        let pending_commit = PendingCommit {
            output: CommitMessageDescription {
                is_external: matches!(auth_content.content.sender, Sender::NewMemberCommit),
                authenticated_data: auth_content.content.authenticated_data,
                committer: *provisional_private_tree.self_index,
                effect: match pending_reinit {
                    Some(r) => CommitEffect::ReInit(r.clone()),
                    None => CommitEffect::NewEpoch(
                        NewEpoch::new(self.state.clone(), &provisional_state).into(),
                    ),
                },
            },

            state: GroupState {
                #[cfg(feature = "by_ref_proposal")]
                proposals: crate::group::ProposalCache::new(
                    self.protocol_version(),
                    self.group_id().to_vec(),
                ),
                context: provisional_state.group_context,
                public_tree: provisional_state.public_tree,
                interim_transcript_hash,
                pending_reinit: pending_reinit.map(|r| r.proposal.clone()),
                confirmation_tag,
            },

            commit_message_hash: MessageHash::compute(&self.cipher_suite_provider, &commit_message)
                .await?,
            signer: new_signer,
            epoch_secrets: key_schedule_result.epoch_secrets,
            key_schedule: key_schedule_result.key_schedule,

            private_tree: provisional_private_tree,
        };

        let output = CommitOutput {
            commit_message,
            welcome_messages,
            ratchet_tree,
            external_commit_group_info,
            contains_update_path: perform_path_update,
            #[cfg(feature = "by_ref_proposal")]
            unused_proposals: provisional_state.unused_proposals,
        };

        Ok((output, pending_commit))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn make_group_info(
        &self,
        group_context: &GroupContext,
        extensions: ExtensionList,
        confirmation_tag: &ConfirmationTag,
        signer: &SignatureSecretKey,
    ) -> Result<GroupInfo, MlsError> {
        let mut group_info = GroupInfo {
            group_context: group_context.clone(),
            extensions,
            confirmation_tag: confirmation_tag.clone(), 
            signer: self.current_member_leaf_index(),
            signature: vec![],
        };

        group_info.grease(self.cipher_suite_provider())?;

        group_info
            .sign(&self.cipher_suite_provider, signer, &())
            .await?;

        Ok(group_info)
    }

    fn make_welcome_message(
        &self,
        secrets: Vec<EncryptedGroupSecrets>,
        encrypted_group_info: Vec<u8>,
    ) -> MlsMessage {
        MlsMessage::new(
            self.context().protocol_version,
            MlsMessagePayload::Welcome(Welcome {
                cipher_suite: self.context().cipher_suite,
                secrets,
                encrypted_group_info,
            }),
        )
    }
}
