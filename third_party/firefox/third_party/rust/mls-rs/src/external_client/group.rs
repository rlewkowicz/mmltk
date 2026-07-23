// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::time::MlsTime;
use mls_rs_core::{
    crypto::SignatureSecretKey, error::IntoAnyError, extension::ExtensionList, group::Member,
    identity::IdentityProvider,
};

use crate::{
    cipher_suite::CipherSuite,
    client::MlsError,
    external_client::ExternalClientConfig,
    group::{
        cipher_suite_provider,
        confirmation_tag::ConfirmationTag,
        framing::PublicMessage,
        member_from_leaf_node,
        message_processor::{
            ApplicationMessageDescription, CommitMessageDescription, EventOrContent,
            MessageProcessor, ProposalMessageDescription, ProvisionalState,
        },
        proposal::RemoveProposal,
        proposal_filter::ProposalInfo,
        snapshot::RawGroupState,
        state::GroupState,
        transcript_hash::InterimTranscriptHash,
        validate_tree_and_info_joiner, ContentType, ExportedTree, GroupContext, GroupInfo, Roster,
        Welcome,
    },
    identity::SigningIdentity,
    protocol_version::ProtocolVersion,
    psk::AlwaysFoundPskStorage,
    tree_kem::{node::LeafIndex, path_secret::PathSecret, TreeKemPrivate},
    CryptoProvider, KeyPackage, MlsMessage,
};

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal", feature = "self_remove_proposal"))]
use crate::group::proposal::SelfRemoveProposal;

#[cfg(feature = "by_ref_proposal")]
use crate::{
    group::{
        framing::{Content, MlsMessagePayload},
        message_processor::CachedProposal,
        message_signature::AuthenticatedContent,
        proposal::Proposal,
        proposal_ref::ProposalRef,
        Sender,
    },
    WireFormat,
};

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal"))]
use crate::group::proposal::CustomProposal;

#[cfg(feature = "by_ref_proposal")]
use mls_rs_core::{crypto::CipherSuiteProvider, psk::ExternalPskId};

#[cfg(feature = "by_ref_proposal")]
use crate::{
    extension::ExternalSendersExt,
    group::proposal::{AddProposal, ReInitProposal},
};

#[cfg(all(feature = "by_ref_proposal", feature = "psk"))]
use crate::{
    group::proposal::PreSharedKeyProposal,
    psk::{
        JustPreSharedKeyID, PreSharedKeyID, PskGroupId, PskNonce, ResumptionPSKUsage, ResumptionPsk,
    },
};

#[cfg(feature = "private_message")]
use crate::group::framing::PrivateMessage;

use alloc::boxed::Box;

/// The result of processing an [ExternalGroup](ExternalGroup) message using
/// [process_incoming_message](ExternalGroup::process_incoming_message)
#[derive(Clone, Debug)]
#[allow(clippy::large_enum_variant)]
pub enum ExternalReceivedMessage {
    /// State update as the result of a successful commit.
    Commit(CommitMessageDescription),
    /// Received proposal and its unique identifier.
    Proposal(ProposalMessageDescription),
    /// Encrypted message that can not be processed.
    Ciphertext(ContentType),
    /// Validated GroupInfo object
    GroupInfo(GroupInfo),
    /// Validated welcome message
    Welcome,
    /// Validated key package
    KeyPackage(KeyPackage),
}

/// A handle to an observed group that can track plaintext control messages
/// and the resulting group state.
#[derive(Clone)]
pub struct ExternalGroup<C>
where
    C: ExternalClientConfig,
{
    pub(crate) config: C,
    pub(crate) cipher_suite_provider: <C::CryptoProvider as CryptoProvider>::CipherSuiteProvider,
    pub(crate) state: GroupState,
    pub(crate) signing_data: Option<(SignatureSecretKey, SigningIdentity)>,
}

impl<C: ExternalClientConfig + Clone> ExternalGroup<C> {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn join(
        config: C,
        signing_data: Option<(SignatureSecretKey, SigningIdentity)>,
        group_info: MlsMessage,
        tree_data: Option<ExportedTree<'_>>,
        maybe_time: Option<MlsTime>,
    ) -> Result<Self, MlsError> {
        let protocol_version = group_info.version;

        if !config.version_supported(protocol_version) {
            return Err(MlsError::UnsupportedProtocolVersion(protocol_version));
        }

        let group_info = group_info
            .into_group_info()
            .ok_or(MlsError::UnexpectedMessageType)?;

        let cipher_suite_provider = cipher_suite_provider(
            config.crypto_provider(),
            group_info.group_context.cipher_suite,
        )?;

        let public_tree = validate_tree_and_info_joiner(
            protocol_version,
            &group_info,
            tree_data,
            &config.identity_provider(),
            &cipher_suite_provider,
            maybe_time,
        )
        .await?;

        let interim_transcript_hash = InterimTranscriptHash::create(
            &cipher_suite_provider,
            &group_info.group_context.confirmed_transcript_hash,
            &group_info.confirmation_tag,
        )
        .await?;

        Ok(Self {
            config,
            signing_data,
            state: GroupState::new(
                group_info.group_context,
                public_tree,
                interim_transcript_hash,
                group_info.confirmation_tag,
            ),
            cipher_suite_provider,
        })
    }

    /// Process a message that was sent to the group.
    ///
    /// * Proposals will be stored in the group state and processed by the
    /// same rules as a standard group.
    ///
    /// * Commits will result in the same outcome as a standard group.
    /// However, the integrity of the resulting group state can only be partially
    /// verified, since the external group does have access to the group
    /// secrets required to do a complete check.
    ///
    /// * Application messages are always encrypted so they result in a no-op
    /// that returns [ExternalReceivedMessage::Ciphertext]
    ///
    /// # Warning
    ///
    /// Processing an encrypted commit or proposal message has the same result
    /// as processing an encrypted application message. Proper tracking of
    /// the group state requires that all proposal and commit messages are
    /// readable.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn process_incoming_message(
        &mut self,
        message: MlsMessage,
    ) -> Result<ExternalReceivedMessage, MlsError> {
        MessageProcessor::process_incoming_message(
            self,
            message,
            #[cfg(feature = "by_ref_proposal")]
            self.config.cache_proposals(),
        )
        .await
    }

    /// Process an inbound message for this group, providing additional context
    /// with a message timestamp.
    ///
    /// Providing a timestamp is useful when the
    /// [`IdentityProvider`](crate::IdentityProvider) in use by the group can
    /// determine validity based on a timestamp. For example, this allows for
    /// checking X.509 certificate expiration at the time when `message` was
    /// received by a server rather than when a specific client asynchronously
    /// received `message`
    ///
    /// See [`process_incoming_message`](Self::process_incoming_message) for
    /// full details.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn process_incoming_message_with_time(
        &mut self,
        message: MlsMessage,
        time: MlsTime,
    ) -> Result<ExternalReceivedMessage, MlsError> {
        MessageProcessor::process_incoming_message_with_time(
            self,
            message,
            #[cfg(feature = "by_ref_proposal")]
            self.config.cache_proposals(),
            Some(time),
        )
        .await
    }

    /// Replay a proposal message into the group skipping all validation steps.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn insert_proposal_from_message(
        &mut self,
        message: MlsMessage,
    ) -> Result<(), MlsError> {
        let ptxt = match message.payload {
            MlsMessagePayload::Plain(p) => Ok(p),
            _ => Err(MlsError::UnexpectedMessageType),
        }?;

        let auth_content: AuthenticatedContent = ptxt.into();

        let proposal_ref =
            ProposalRef::from_content(&self.cipher_suite_provider, &auth_content).await?;

        let sender = auth_content.content.sender;

        let proposal = match auth_content.content.content {
            Content::Proposal(p) => Ok(*p),
            _ => Err(MlsError::UnexpectedMessageType),
        }?;

        self.group_state_mut()
            .proposals
            .insert(proposal_ref, proposal, sender);

        Ok(())
    }

    /// Force insert a proposal directly into the internal state of the group
    /// with no validation.
    #[cfg(feature = "by_ref_proposal")]
    pub fn insert_proposal(&mut self, proposal: CachedProposal) {
        self.group_state_mut().proposals.insert(
            proposal.proposal_ref,
            proposal.proposal,
            proposal.sender,
        )
    }

    /// Create an external proposal to request that a group add a new member
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_add(
        &mut self,
        key_package: MlsMessage,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let key_package = key_package
            .into_key_package()
            .ok_or(MlsError::UnexpectedMessageType)?;

        self.propose(
            Proposal::Add(alloc::boxed::Box::new(AddProposal { key_package })),
            authenticated_data,
        )
        .await
    }

    /// Create an external proposal to request that a group remove an existing member
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_remove(
        &mut self,
        index: u32,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let to_remove = LeafIndex::try_from(index)?;

        self.group_state().public_tree.get_leaf_node(to_remove)?;

        self.propose(
            Proposal::Remove(RemoveProposal { to_remove }),
            authenticated_data,
        )
        .await
    }

    /// Create an external proposal to request that a group inserts an external
    /// pre shared key into its state.
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(all(feature = "by_ref_proposal", feature = "psk"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_external_psk(
        &mut self,
        psk: ExternalPskId,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self.psk_proposal(JustPreSharedKeyID::External(psk))?;
        self.propose(proposal, authenticated_data).await
    }

    /// Create an external proposal to request that a group adds a pre shared key
    /// from a previous epoch to the current group state.
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(all(feature = "by_ref_proposal", feature = "psk"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_resumption_psk(
        &mut self,
        psk_epoch: u64,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let key_id = ResumptionPsk {
            psk_epoch,
            usage: ResumptionPSKUsage::Application,
            psk_group_id: PskGroupId(self.group_context().group_id().to_vec()),
        };

        let proposal = self.psk_proposal(JustPreSharedKeyID::Resumption(key_id))?;
        self.propose(proposal, authenticated_data).await
    }

    #[cfg(all(feature = "by_ref_proposal", feature = "psk"))]
    fn psk_proposal(&self, key_id: JustPreSharedKeyID) -> Result<Proposal, MlsError> {
        Ok(Proposal::Psk(PreSharedKeyProposal {
            psk: PreSharedKeyID {
                key_id,
                psk_nonce: PskNonce::random(&self.cipher_suite_provider)
                    .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?,
            },
        }))
    }

    /// Create an external proposal to request that a group sets extensions stored in the group
    /// state.
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_group_context_extensions(
        &mut self,
        extensions: ExtensionList,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = Proposal::GroupContextExtensions(extensions);
        self.propose(proposal, authenticated_data).await
    }

    /// Create an external proposal to request that a group is reinitialized.
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_reinit(
        &mut self,
        group_id: Option<Vec<u8>>,
        version: ProtocolVersion,
        cipher_suite: CipherSuite,
        extensions: ExtensionList,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let group_id = group_id.map(Ok).unwrap_or_else(|| {
            self.cipher_suite_provider
                .random_bytes_vec(self.cipher_suite_provider.kdf_extract_size())
                .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
        })?;

        let proposal = Proposal::ReInit(ReInitProposal {
            group_id,
            version,
            cipher_suite,
            extensions,
        });

        self.propose(proposal, authenticated_data).await
    }

    /// Create a custom proposal message.
    ///
    /// # Warning
    ///
    /// In order for the proposal generated by this function to be successfully
    /// committed, the group needs to have `signing_identity` as an entry
    /// within an [ExternalSendersExt](crate::extension::built_in::ExternalSendersExt)
    /// as part of its group context extensions.
    #[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_custom(
        &mut self,
        proposal: CustomProposal,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        self.propose(Proposal::Custom(proposal), authenticated_data)
            .await
    }

    /// Issue an external proposal.
    ///
    /// This function is useful for reissuing external proposals that
    /// are returned in [crate::group::NewEpoch::unused_proposals]
    /// after a commit is processed.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose(
        &mut self,
        proposal: Proposal,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let (signer, signing_identity) =
            self.signing_data.as_ref().ok_or(MlsError::SignerNotFound)?;

        let external_senders_ext = self
            .state
            .context
            .extensions
            .get_as::<ExternalSendersExt>()?
            .ok_or(MlsError::ExternalProposalsDisabled)?;

        let sender_index = external_senders_ext
            .allowed_senders
            .iter()
            .position(|allowed_signer| signing_identity == allowed_signer)
            .ok_or(MlsError::InvalidExternalSigningIdentity)?;

        let sender = Sender::External(sender_index as u32);

        let auth_content = AuthenticatedContent::new_signed(
            &self.cipher_suite_provider,
            &self.state.context,
            sender,
            Content::Proposal(Box::new(proposal.clone())),
            signer,
            WireFormat::PublicMessage,
            authenticated_data,
        )
        .await?;

        let proposal_ref =
            ProposalRef::from_content(&self.cipher_suite_provider, &auth_content).await?;

        let plaintext = PublicMessage {
            content: auth_content.content,
            auth: auth_content.auth,
            membership_tag: None,
        };

        let message = MlsMessage::new(
            self.group_context().version(),
            MlsMessagePayload::Plain(plaintext),
        );

        self.state.proposals.insert(proposal_ref, proposal, sender);

        Ok(message)
    }

    /// Delete all sent and received proposals cached for commit.
    #[cfg(feature = "by_ref_proposal")]
    pub fn clear_proposal_cache(&mut self) {
        self.state.proposals.clear()
    }

    #[inline(always)]
    pub(crate) fn group_state(&self) -> &GroupState {
        &self.state
    }

    /// Get the current group context summarizing various information about the group.
    #[inline(always)]
    pub fn group_context(&self) -> &GroupContext {
        &self.group_state().context
    }

    /// Export the current ratchet tree used within the group.
    pub fn export_tree(&self) -> Result<Vec<u8>, MlsError> {
        self.group_state()
            .public_tree
            .nodes
            .mls_encode_to_vec()
            .map_err(Into::into)
    }

    /// Get the current roster of the group.
    #[inline(always)]
    pub fn roster(&self) -> Roster<'_> {
        self.group_state().public_tree.roster()
    }

    /// Get the
    /// [transcript hash](https://messaginglayersecurity.rocks/mls-protocol/draft-ietf-mls-protocol.html#name-transcript-hashes)
    /// for the current epoch that the group is in.
    #[inline(always)]
    pub fn transcript_hash(&self) -> &Vec<u8> {
        &self.group_state().context.confirmed_transcript_hash
    }

    /// Get the
    /// [tree hash](https://www.rfc-editor.org/rfc/rfc9420.html#name-tree-hashes)
    /// for the current epoch that the group is in.
    #[inline(always)]
    pub fn tree_hash(&self) -> &[u8] {
        &self.group_state().context.tree_hash
    }

    /// Find a member based on their identity.
    ///
    /// Identities are matched based on the
    /// [IdentityProvider](crate::IdentityProvider)
    /// that this group was configured with.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn get_member_with_identity(
        &self,
        identity_id: &SigningIdentity,
    ) -> Result<Member, MlsError> {
        let identity = self
            .identity_provider()
            .identity(identity_id, self.group_context().extensions())
            .await
            .map_err(|error| MlsError::IdentityProviderError(error.into_any_error()))?;

        let tree = &self.group_state().public_tree;

        #[cfg(feature = "tree_index")]
        let index = tree.get_leaf_node_with_identity(&identity);

        #[cfg(not(feature = "tree_index"))]
        let index = tree
            .get_leaf_node_with_identity(
                &identity,
                &self.identity_provider(),
                self.group_context().extensions(),
            )
            .await?;

        let index = index.ok_or(MlsError::MemberNotFound)?;
        let node = self.group_state().public_tree.get_leaf_node(index)?;

        Ok(member_from_leaf_node(node, index))
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl<C> MessageProcessor for ExternalGroup<C>
where
    C: ExternalClientConfig + Clone,
{
    type MlsRules = C::MlsRules;
    type IdentityProvider = C::IdentityProvider;
    type PreSharedKeyStorage = AlwaysFoundPskStorage;
    type OutputType = ExternalReceivedMessage;
    type CipherSuiteProvider = <C::CryptoProvider as CryptoProvider>::CipherSuiteProvider;

    fn mls_rules(&self) -> Self::MlsRules {
        self.config.mls_rules()
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn verify_plaintext_authentication(
        &self,
        message: PublicMessage,
    ) -> Result<EventOrContent<Self::OutputType>, MlsError> {
        let auth_content = crate::group::message_verifier::verify_plaintext_authentication(
            &self.cipher_suite_provider,
            message,
            None,
            &self.state.context,
            crate::group::message_verifier::SignaturePublicKeysContainer::RatchetTree(
                &self.state.public_tree,
            ),
        )
        .await?;

        Ok(EventOrContent::Content(auth_content))
    }

    #[cfg(all(feature = "export_key_generation", feature = "private_message"))]
    async fn get_unauthenticated_key_generation_from_sender_data(
        &mut self,
        _cipher_text: &PrivateMessage,
    ) -> Result<Option<u32>, MlsError> {
        Ok(None)
    }

    #[cfg(feature = "private_message")]
    async fn process_ciphertext(
        &mut self,
        cipher_text: &PrivateMessage,
    ) -> Result<EventOrContent<Self::OutputType>, MlsError> {
        Ok(EventOrContent::Event(ExternalReceivedMessage::Ciphertext(
            cipher_text.content_type,
        )))
    }

    async fn update_key_schedule(
        &mut self,
        _secrets: Option<(TreeKemPrivate, PathSecret)>,
        interim_transcript_hash: InterimTranscriptHash,
        confirmation_tag: &ConfirmationTag,
        provisional_public_state: ProvisionalState,
    ) -> Result<(), MlsError> {
        self.state.context = provisional_public_state.group_context;
        #[cfg(feature = "by_ref_proposal")]
        self.state.proposals.clear();
        self.state.interim_transcript_hash = interim_transcript_hash;
        self.state.public_tree = provisional_public_state.public_tree;
        self.state.confirmation_tag = confirmation_tag.clone();

        Ok(())
    }

    fn identity_provider(&self) -> Self::IdentityProvider {
        self.config.identity_provider()
    }

    fn psk_storage(&self) -> Self::PreSharedKeyStorage {
        AlwaysFoundPskStorage
    }

    fn group_state(&self) -> &GroupState {
        &self.state
    }

    fn group_state_mut(&mut self) -> &mut GroupState {
        &mut self.state
    }

    fn removal_proposal(
        &self,
        _provisional_state: &ProvisionalState,
    ) -> Option<ProposalInfo<RemoveProposal>> {
        None
    }

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal", feature = "self_remove_proposal"))]
fn self_removal_proposal(
        &self,
        _provisional_state: &ProvisionalState,
    ) -> Option<ProposalInfo<SelfRemoveProposal>> {
        None
    }

    #[cfg(feature = "private_message")]
    fn min_epoch_available(&self) -> Option<u64> {
        self.config
            .max_epoch_jitter()
            .map(|j| self.state.context.epoch - j)
    }

    fn cipher_suite_provider(&self) -> &Self::CipherSuiteProvider {
        &self.cipher_suite_provider
    }
}

/// Serializable snapshot of an [ExternalGroup](ExternalGroup) state.
#[derive(Debug, MlsEncode, MlsSize, MlsDecode, PartialEq, Clone)]
pub struct ExternalSnapshot {
    version: u16,
    pub(crate) state: RawGroupState,
}

impl ExternalSnapshot {
    /// Serialize the snapshot
    pub fn to_bytes(&self) -> Result<Vec<u8>, MlsError> {
        Ok(self.mls_encode_to_vec()?)
    }

    /// Deserialize the snapshot
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, MlsError> {
        Ok(Self::mls_decode(&mut &*bytes)?)
    }

    /// Group context encoded in the snapshot
    pub fn context(&self) -> &GroupContext {
        &self.state.context
    }
}

impl<C> ExternalGroup<C>
where
    C: ExternalClientConfig + Clone,
{
    /// Create a snapshot of this group's current internal state.
    pub fn snapshot(&self) -> ExternalSnapshot {
        ExternalSnapshot {
            state: RawGroupState::export(self.group_state()),
            version: 1,
        }
    }

    /// Create a snapshot of this group's current internal state.
    /// The tree is not included in the state and can be stored
    /// separately by calling [`Group::export_tree`].
    pub fn snapshot_without_ratchet_tree(&mut self) -> ExternalSnapshot {
        let tree = std::mem::take(&mut self.state.public_tree.nodes);

        let snapshot = ExternalSnapshot {
            state: RawGroupState::export(&self.state),
            version: 1,
        };

        self.state.public_tree.nodes = tree;

        snapshot
    }
}

impl From<CommitMessageDescription> for ExternalReceivedMessage {
    fn from(value: CommitMessageDescription) -> Self {
        ExternalReceivedMessage::Commit(value)
    }
}

impl TryFrom<ApplicationMessageDescription> for ExternalReceivedMessage {
    type Error = MlsError;

    fn try_from(_: ApplicationMessageDescription) -> Result<Self, Self::Error> {
        Err(MlsError::UnencryptedApplicationMessage)
    }
}

impl From<ProposalMessageDescription> for ExternalReceivedMessage {
    fn from(value: ProposalMessageDescription) -> Self {
        ExternalReceivedMessage::Proposal(value)
    }
}

impl From<GroupInfo> for ExternalReceivedMessage {
    fn from(value: GroupInfo) -> Self {
        ExternalReceivedMessage::GroupInfo(value)
    }
}

impl From<Welcome> for ExternalReceivedMessage {
    fn from(_: Welcome) -> Self {
        ExternalReceivedMessage::Welcome
    }
}

impl From<KeyPackage> for ExternalReceivedMessage {
    fn from(value: KeyPackage) -> Self {
        ExternalReceivedMessage::KeyPackage(value)
    }
}
