// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec;
use alloc::vec::Vec;
use core::fmt::{self, Debug};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
#[cfg(feature = "last_resort_key_package_ext")]
use mls_rs_core::extension::MlsExtension;
use mls_rs_core::identity::MemberValidationContext;
use mls_rs_core::secret::Secret;
use mls_rs_core::time::MlsTime;
use snapshot::PendingCommitSnapshot;
use zeroize::Zeroizing;

use crate::cipher_suite::CipherSuite;
use crate::client::MlsError;
use crate::client_config::ClientConfig;
use crate::crypto::{HpkeCiphertext, SignatureSecretKey};
#[cfg(feature = "last_resort_key_package_ext")]
use crate::extension::LastResortKeyPackageExt;
use crate::extension::RatchetTreeExt;
use crate::identity::SigningIdentity;
use crate::key_package::{KeyPackage, KeyPackageGeneration, KeyPackageRef};
use crate::protocol_version::ProtocolVersion;
use crate::psk::secret::PskSecret;
use crate::psk::PreSharedKeyID;
use crate::signer::Signable;
use crate::tree_kem::hpke_encryption::HpkeEncryptable;
use crate::tree_kem::kem::TreeKem;
use crate::tree_kem::leaf_node::LeafNode;
use crate::tree_kem::leaf_node_validator::{LeafNodeValidator, ValidationContext};
use crate::tree_kem::node::LeafIndex;
use crate::tree_kem::path_secret::PathSecret;
pub use crate::tree_kem::Capabilities;
use crate::tree_kem::{math as tree_math, ValidatedUpdatePath};
use crate::tree_kem::{TreeKemPrivate, TreeKemPublic};
use crate::{CipherSuiteProvider, CryptoProvider};
pub use state::GroupState;

#[cfg(feature = "by_ref_proposal")]
use crate::crypto::{HpkePublicKey, HpkeSecretKey};

use crate::extension::ExternalPubExt;

use self::message_hash::MessageHash;
#[cfg(feature = "private_message")]
use self::mls_rules::{EncryptionOptions, MlsRules};

#[cfg(feature = "psk")]
pub use self::resumption::ReinitClient;

#[cfg(feature = "psk")]
use crate::psk::{
    resolver::PskResolver, secret::PskSecretInput, ExternalPskId, JustPreSharedKeyID, PskGroupId,
    ResumptionPSKUsage, ResumptionPsk,
};

#[cfg(feature = "private_message")]
use ciphertext_processor::*;

use component_operation::{ComponentID, ComponentOperationLabel};
use confirmation_tag::*;
use framing::*;
use key_schedule::*;
use membership_tag::*;
use message_signature::*;
use message_verifier::*;
use proposal::*;
#[cfg(feature = "by_ref_proposal")]
use proposal_cache::*;
use transcript_hash::*;



use self::proposal_filter::ProposalInfo;

#[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
use secret_tree::*;

#[cfg(feature = "prior_epoch")]
use self::epoch::PriorEpoch;

use self::epoch::EpochSecrets;
pub use self::message_processor::{
    ApplicationMessageDescription, CommitEffect, CommitMessageDescription, NewEpoch,
    ProposalMessageDescription, ProposalSender, ReceivedMessage,
};
use self::message_processor::{EventOrContent, MessageProcessor, ProvisionalState};
#[cfg(feature = "by_ref_proposal")]
use self::proposal_ref::ProposalRef;
use self::state_repo::GroupStateRepository;
pub use group_info::GroupInfo;

pub use self::framing::{ContentType, Sender};
pub use commit::*;
pub use mls_rs_core::group::GroupContext;
pub use roster::*;

pub(crate) use mls_rs_core::group::ConfirmedTranscriptHash;
pub(crate) use util::*;

#[cfg(all(feature = "by_ref_proposal", feature = "external_client"))]
pub use self::message_processor::CachedProposal;

#[cfg(feature = "private_message")]
mod ciphertext_processor;

mod commit;
pub mod component_operation;
pub(crate) mod confirmation_tag;
pub(crate) mod epoch;
pub(crate) mod framing;
mod group_info;
pub(crate) mod key_schedule;
mod membership_tag;
pub(crate) mod message_hash;
pub(crate) mod message_processor;
pub(crate) mod message_signature;
pub(crate) mod message_verifier;
pub mod mls_rules;
#[cfg(feature = "private_message")]
pub(crate) mod padding;
/// Proposals to evolve a MLS [`Group`]
pub mod proposal;
mod proposal_cache;
pub(crate) mod proposal_filter;
#[cfg(feature = "by_ref_proposal")]
pub(crate) mod proposal_ref;
#[cfg(feature = "psk")]
mod resumption;
mod roster;
pub(crate) mod snapshot;
pub(crate) mod state;

#[cfg(feature = "prior_epoch")]
pub(crate) mod state_repo;
#[cfg(not(feature = "prior_epoch"))]
pub(crate) mod state_repo_light;
#[cfg(not(feature = "prior_epoch"))]
pub(crate) use state_repo_light as state_repo;

pub(crate) mod transcript_hash;
mod util;

/// External commit building.
pub mod external_commit;

#[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
pub(crate) mod secret_tree;

#[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
pub use secret_tree::MessageKeyData as MessageKey;


mod exported_tree;

pub use exported_tree::ExportedTree;

#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode, MlsDecode)]
struct GroupSecrets {
    joiner_secret: JoinerSecret,
    path_secret: Option<PathSecret>,
    psks: Vec<PreSharedKeyID>,
}

impl HpkeEncryptable for GroupSecrets {
    const ENCRYPT_LABEL: &'static str = "Welcome";

    fn from_bytes(bytes: Vec<u8>) -> Result<Self, MlsError> {
        Self::mls_decode(&mut bytes.as_slice()).map_err(Into::into)
    }

    fn get_bytes(&self) -> Result<Vec<u8>, MlsError> {
        self.mls_encode_to_vec().map_err(Into::into)
    }
}

#[derive(Clone, Debug, PartialEq, Eq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub(crate) struct EncryptedGroupSecrets {
    pub new_member: KeyPackageRef,
    pub encrypted_group_secrets: HpkeCiphertext,
}

#[derive(Clone, Eq, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub(crate) struct Welcome {
    pub cipher_suite: CipherSuite,
    pub secrets: Vec<EncryptedGroupSecrets>,
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    pub encrypted_group_info: Vec<u8>,
}

impl Debug for Welcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Welcome")
            .field("cipher_suite", &self.cipher_suite)
            .field("secrets", &self.secrets)
            .field(
                "encrypted_group_info",
                &mls_rs_core::debug::pretty_bytes(&self.encrypted_group_info),
            )
            .finish()
    }
}

#[derive(Clone, Debug)]
#[non_exhaustive]
/// Information provided to new members upon joining a group.
pub struct NewMemberInfo {
    /// Group info extensions found within the Welcome message used to join
    /// the group.
    pub group_info_extensions: ExtensionList,
    /// The group member who generated the Commit adding the joiner to the
    /// group. This may not be the party who generated the corresponding
    /// add proposal
    pub sender: u32,
}

impl NewMemberInfo {
    pub(crate) fn new(group_info_extensions: ExtensionList, sender: u32) -> Self {
        let mut new_member_info = Self {
            group_info_extensions,
            sender,
        };

        new_member_info.ungrease();

        new_member_info
    }

    /// Group info extensions found within the Welcome message used to join
    /// the group.
    pub fn group_info_extensions(&self) -> &ExtensionList {
        &self.group_info_extensions
    }
}

/// An MLS end-to-end encrypted group.
///
/// # Group Evolution
///
/// MLS Groups are evolved via a propose-then-commit system. Each group state
/// produced by a commit is called an epoch and can produce and consume
/// application, proposal, and commit messages. A [commit](Group::commit) is used
/// to advance to the next epoch by applying existing proposals sent in
/// the current epoch by-reference along with an optional set of proposals
/// that are included by-value using a [`CommitBuilder`].
#[derive(Clone)]
pub struct Group<C>
where
    C: ClientConfig,
{
    config: C,
    cipher_suite_provider: <C::CryptoProvider as CryptoProvider>::CipherSuiteProvider,
    state_repo: GroupStateRepository<C::GroupStateStorage, C::KeyPackageRepository>,
    pub(crate) state: GroupState,
    epoch_secrets: EpochSecrets,
    private_tree: TreeKemPrivate,
    key_schedule: KeySchedule,
    #[cfg(feature = "by_ref_proposal")]
    pending_updates:
        crate::map::SmallMap<HpkePublicKey, (HpkeSecretKey, Option<SignatureSecretKey>)>,
    pending_commit: PendingCommitSnapshot,
    #[cfg(feature = "psk")]
    previous_psk: Option<PskSecretInput>,
#[cfg(any())]









    pub(crate) commit_modifiers: CommitModifiers,
    pub(crate) signer: SignatureSecretKey,
}

impl<C> Group<C>
where
    C: ClientConfig + Clone,
{
    #[allow(clippy::too_many_arguments)]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn new(
        config: C,
        group_id: Option<Vec<u8>>,
        cipher_suite: CipherSuite,
        protocol_version: ProtocolVersion,
        signing_identity: SigningIdentity,
        group_context_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
        signer: SignatureSecretKey,
        maybe_now_time: Option<MlsTime>,
    ) -> Result<Self, MlsError> {
        let cipher_suite_provider = cipher_suite_provider(config.crypto_provider(), cipher_suite)?;

        let (leaf_node, leaf_node_secret) = LeafNode::generate(
            &cipher_suite_provider,
            config.leaf_properties(leaf_node_extensions),
            signing_identity,
            &signer,
            config.lifetime(maybe_now_time),
        )
        .await?;

        let (mut public_tree, private_tree) = TreeKemPublic::derive(
            leaf_node,
            leaf_node_secret,
            &config.identity_provider(),
            &group_context_extensions,
        )
        .await?;

        let tree_hash = public_tree.tree_hash(&cipher_suite_provider).await?;

        let group_id = group_id.map(Ok).unwrap_or_else(|| {
            cipher_suite_provider
                .random_bytes_vec(cipher_suite_provider.kdf_extract_size())
                .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
        })?;

        let context = GroupContext::new(
            protocol_version,
            cipher_suite,
            group_id,
            tree_hash,
            group_context_extensions,
        );

        let identity_provider = config.identity_provider();

        let member_validation_context = MemberValidationContext::ForNewGroup {
            current_context: &context,
        };

        let leaf_node_validator = LeafNodeValidator::new(
            &cipher_suite_provider,
            &identity_provider,
            member_validation_context,
        );

        leaf_node_validator
            .check_if_valid(
                public_tree.get_leaf_node(LeafIndex::unchecked(0))?,
                ValidationContext::Add(maybe_now_time),
            )
            .await?;

        let state_repo = GroupStateRepository::new(
            #[cfg(feature = "prior_epoch")]
            context.group_id.clone(),
            config.group_state_storage(),
            config.key_package_repo(),
            None,
        )?;

        let key_schedule_result = KeySchedule::from_random_epoch_secret(
            &cipher_suite_provider,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            public_tree.total_leaf_count(),
        )
        .await?;

        let confirmation_tag = ConfirmationTag::create(
            &key_schedule_result.confirmation_key,
            &vec![].into(),
            &cipher_suite_provider,
        )
        .await?;

        let interim_hash = InterimTranscriptHash::create(
            &cipher_suite_provider,
            &vec![].into(),
            &confirmation_tag,
        )
        .await?;

        Ok(Self {
            config,
            state: GroupState::new(context, public_tree, interim_hash, confirmation_tag),
            private_tree,
            key_schedule: key_schedule_result.key_schedule,
            #[cfg(feature = "by_ref_proposal")]
            pending_updates: Default::default(),
            pending_commit: Default::default(),
#[cfg(any())]









            commit_modifiers: Default::default(),
            epoch_secrets: key_schedule_result.epoch_secrets,
            state_repo,
            cipher_suite_provider,
            #[cfg(feature = "psk")]
            previous_psk: None,
            signer,
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn join(
        welcome: &MlsMessage,
        tree_data: Option<ExportedTree<'_>>,
        config: C,
        signer: SignatureSecretKey,
        maybe_time: Option<MlsTime>,
    ) -> Result<(Self, NewMemberInfo), MlsError> {
        Self::from_welcome_message(
            welcome,
            tree_data,
            config,
            signer,
            #[cfg(feature = "psk")]
            None,
            maybe_time,
        )
        .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn from_welcome_message(
        welcome: &MlsMessage,
        tree_data: Option<ExportedTree<'_>>,
        config: C,
        signer: SignatureSecretKey,
        #[cfg(feature = "psk")] additional_psk: Option<PskSecretInput>,
        maybe_time: Option<MlsTime>,
    ) -> Result<(Self, NewMemberInfo), MlsError> {
        let (group_info, key_package_generation, group_secrets, psk_secret) =
            Self::decrypt_group_info_internal(
                welcome,
                &config,
                #[cfg(feature = "psk")]
                additional_psk,
            )
            .await?;

        let cipher_suite_provider = cipher_suite_provider(
            config.crypto_provider(),
            group_info.group_context.cipher_suite,
        )?;

        let id_provider = config.identity_provider();

        let public_tree = validate_tree_and_info_joiner(
            welcome.version,
            &group_info,
            tree_data,
            &id_provider,
            &cipher_suite_provider,
            maybe_time,
        )
        .await?;

        let key_package = key_package_generation.key_package;

        let self_index = public_tree
            .find_leaf_node(&key_package.leaf_node)
            .ok_or(MlsError::WelcomeKeyPackageNotFound)?;

        #[cfg(not(feature = "last_resort_key_package_ext"))]
        let is_last_resort = false;
        #[cfg(feature = "last_resort_key_package_ext")]
        let is_last_resort = key_package
            .extensions
            .has_extension(LastResortKeyPackageExt::extension_type());
        let used_key_package_ref = (!is_last_resort).then_some(key_package_generation.reference);

        let mut private_tree =
            TreeKemPrivate::new_self_leaf(self_index, key_package_generation.leaf_node_secret_key);

        if let Some(path_secret) = group_secrets.path_secret {
            private_tree
                .update_secrets(
                    &cipher_suite_provider,
                    group_info.signer,
                    path_secret,
                    &public_tree,
                )
                .await?;
        }

        let key_schedule_result = KeySchedule::from_joiner(
            &cipher_suite_provider,
            &group_secrets.joiner_secret,
            &group_info.group_context,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            public_tree.total_leaf_count(),
            &psk_secret,
        )
        .await?;

        if !group_info
            .confirmation_tag
            .matches(
                &key_schedule_result.confirmation_key,
                &group_info.group_context.confirmed_transcript_hash,
                &cipher_suite_provider,
            )
            .await?
        {
            return Err(MlsError::InvalidConfirmationTag);
        }

        Self::join_with(
            config,
            group_info,
            public_tree,
            key_schedule_result.key_schedule,
            key_schedule_result.epoch_secrets,
            private_tree,
            used_key_package_ref,
            signer,
        )
        .await
    }

    #[allow(clippy::too_many_arguments)]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn join_with(
        config: C,
        group_info: GroupInfo,
        public_tree: TreeKemPublic,
        key_schedule: KeySchedule,
        epoch_secrets: EpochSecrets,
        private_tree: TreeKemPrivate,
        used_key_package_ref: Option<KeyPackageRef>,
        signer: SignatureSecretKey,
    ) -> Result<(Self, NewMemberInfo), MlsError> {
        let cs = group_info.group_context.cipher_suite;

        let cs = config
            .crypto_provider()
            .cipher_suite_provider(cs)
            .ok_or(MlsError::UnsupportedCipherSuite(cs))?;

        let interim_transcript_hash = InterimTranscriptHash::create(
            &cs,
            &group_info.group_context.confirmed_transcript_hash,
            &group_info.confirmation_tag,
        )
        .await?;

        let state_repo = GroupStateRepository::new(
            #[cfg(feature = "prior_epoch")]
            group_info.group_context.group_id.clone(),
            config.group_state_storage(),
            config.key_package_repo(),
            used_key_package_ref,
        )?;

        let group = Group {
            config,
            state: GroupState::new(
                group_info.group_context,
                public_tree,
                interim_transcript_hash,
                group_info.confirmation_tag,
            ),
            private_tree,
            key_schedule,
            #[cfg(feature = "by_ref_proposal")]
            pending_updates: Default::default(),
            pending_commit: Default::default(),
#[cfg(any())]









            commit_modifiers: Default::default(),
            epoch_secrets,
            state_repo,
            cipher_suite_provider: cs,
            #[cfg(feature = "psk")]
            previous_psk: None,
            signer,
        };

        Ok((
            group,
            NewMemberInfo::new(group_info.extensions, *group_info.signer),
        ))
    }

    #[inline(always)]
    pub(crate) fn current_epoch_tree(&self) -> &TreeKemPublic {
        &self.state.public_tree
    }

    /// The current epoch of the group. This value is incremented each
    /// time a [`Group::commit`] message is processed.
    #[inline(always)]
    pub fn current_epoch(&self) -> u64 {
        self.context().epoch
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn hpke_encrypt_to_recipient_with_generic_context(
        &self,
        recipient_index: u32,
        context_info: &[u8],
        associated_data: Option<&[u8]>,
        plaintext: &[u8],
    ) -> Result<HpkeCiphertext, MlsError> {
        let member_leaf_node = self
            .group_state()
            .public_tree
            .get_leaf_node(LeafIndex::try_from(recipient_index)?)?;
        let member_public_key = &member_leaf_node.public_key;
        let hpke_ciphertext = self
            .cipher_suite_provider
            .hpke_seal(member_public_key, context_info, associated_data, plaintext)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;
        Ok(hpke_ciphertext)
    }

    /// HPKE encrypts a message to the member at the specified `recipient_index` in the group.
    ///
    /// Takes `context_info`, `associated_data`, and `plaintext`.
    /// Returns `ciphertext` and `kem_output` inside `HpkeCiphertext`.
    ///
    /// WARNING: The message sender is not authenticated.
    #[cfg(feature = "non_domain_separated_hpke_encrypt_decrypt")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn hpke_encrypt_to_recipient(
        &self,
        recipient_index: u32,
        context_info: &[u8],
        associated_data: Option<&[u8]>,
        plaintext: &[u8],
    ) -> Result<HpkeCiphertext, MlsError> {
        self.hpke_encrypt_to_recipient_with_generic_context(
            recipient_index,
            context_info,
            associated_data,
            plaintext,
        )
        .await
    }

    /// HPKE encrypts a message to the member at the specified `recipient_index` in the group.
    ///
    /// Takes a `component_id` and `context` to construct a `ComponentOperationLabel`, to be used as
    /// the HPKE seal context, to ensure domain separation.
    /// Also takes an `associated_data` and `plaintext`.
    /// Returns `ciphertext` and `kem_output` inside `HpkeCiphertext`.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn safe_encrypt_with_context_to_recipient(
        &self,
        recipient_index: u32,
        component_id: ComponentID,
        context: &[u8],
        associated_data: Option<&[u8]>,
        plaintext: &[u8],
    ) -> Result<HpkeCiphertext, MlsError> {
        let component_operation_label = ComponentOperationLabel::new(component_id, context);
        self.hpke_encrypt_to_recipient_with_generic_context(
            recipient_index,
            &component_operation_label.get_bytes()?,
            associated_data,
            plaintext,
        )
        .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn hpke_decrypt_for_current_member_with_generic_context(
        &self,
        context_info: &[u8],
        associated_data: Option<&[u8]>,
        hpke_ciphertext: HpkeCiphertext,
    ) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        let self_private_key = &self.private_tree.secret_keys[0]
            .as_ref()
            .ok_or(MlsError::InvalidTreeKemPrivateKey)?;

        let self_public_key = &self.current_user_leaf_node()?.public_key;

        self.cipher_suite_provider
            .hpke_open(
                &hpke_ciphertext,
                self_private_key,
                self_public_key,
                context_info,
                associated_data,
            )
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
    }

    /// HPKE decrypts a message sent to the current member.
    ///
    /// Takes `HpkeCiphertext` generated by `hpke_encrypt_to_recipient` intended for the
    /// current member.
    ///
    /// WARNING: The message sender is not authenticated.
    #[cfg(feature = "non_domain_separated_hpke_encrypt_decrypt")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn hpke_decrypt_for_current_member(
        &self,
        context_info: &[u8],
        associated_data: Option<&[u8]>,
        hpke_ciphertext: HpkeCiphertext,
    ) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        self.hpke_decrypt_for_current_member_with_generic_context(
            context_info,
            associated_data,
            hpke_ciphertext,
        )
        .await
    }

    /// HPKE decrypts a message sent to the current member.
    ///
    /// Takes `HpkeCiphertext` generated by `hpke_encrypt_to_recipient` intended for the
    /// current member.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn safe_decrypt_with_context_for_current_member(
        &self,
        component_id: ComponentID,
        context: &[u8],
        associated_data: Option<&[u8]>,
        hpke_ciphertext: HpkeCiphertext,
    ) -> Result<Zeroizing<Vec<u8>>, MlsError> {
        let component_operation_label = ComponentOperationLabel::new(component_id, context);
        self.hpke_decrypt_for_current_member_with_generic_context(
            &component_operation_label.get_bytes()?,
            associated_data,
            hpke_ciphertext,
        )
        .await
    }

    /// Index within the group's state for the local group instance.
    ///
    /// This index corresponds to indexes in content descriptions within
    /// [`ReceivedMessage`].
    #[inline(always)]
    pub fn current_member_index(&self) -> u32 {
        *self.current_member_leaf_index()
    }

    #[inline(always)]
    fn current_member_leaf_index(&self) -> LeafIndex {
        self.private_tree.self_index
    }

    fn current_user_leaf_node(&self) -> Result<&LeafNode, MlsError> {
        self.current_epoch_tree()
            .get_leaf_node(self.private_tree.self_index)
    }

    /// Signing identity currently in use by the local group instance.
    pub fn current_member_signing_identity(&self) -> Result<&SigningIdentity, MlsError> {
        self.current_user_leaf_node().map(|ln| &ln.signing_identity)
    }

    /// Member at a specific index in the group state.
    ///
    /// These indexes correspond to indexes in content descriptions within
    /// [`ReceivedMessage`].
    pub fn member_at_index(&self, index: u32) -> Option<Member> {
        self.group_state().member_at_index(index)
    }

    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn proposal_message(
        &mut self,
        proposal: Proposal,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let sender = Sender::Member(*self.private_tree.self_index);

        let auth_content = AuthenticatedContent::new_signed(
            &self.cipher_suite_provider,
            self.context(),
            sender,
            Content::Proposal(alloc::boxed::Box::new(proposal.clone())),
            &self.signer,
            #[cfg(feature = "private_message")]
            self.encryption_options()?.control_wire_format(sender),
            #[cfg(not(feature = "private_message"))]
            WireFormat::PublicMessage,
            authenticated_data,
        )
        .await?;

        let sender = auth_content.content.sender;

        let proposal_desc =
            ProposalMessageDescription::new(&self.cipher_suite_provider, &auth_content, proposal)
                .await?;

        let message = self.format_for_wire(auth_content).await?;

        self.state
            .proposals
            .insert_own(proposal_desc, &message, sender, &self.cipher_suite_provider)
            .await?;

        Ok(message)
    }

    /// Unique identifier for this group.
    pub fn group_id(&self) -> &[u8] {
        &self.context().group_id
    }

    fn provisional_private_tree(
        &self,
        provisional_state: &ProvisionalState,
    ) -> Result<(TreeKemPrivate, Option<SignatureSecretKey>), MlsError> {
        let mut provisional_private_tree = self.private_tree.clone();
        let self_index = provisional_private_tree.self_index;

        let path = provisional_state
            .public_tree
            .nodes
            .direct_copath(self_index);

        provisional_private_tree
            .secret_keys
            .resize(path.len() + 1, None);

        for (i, n) in path.iter().enumerate() {
            if provisional_state.public_tree.nodes.is_blank(n.path)? {
                provisional_private_tree.secret_keys[i + 1] = None;
            }
        }

        let new_signer = None;

        #[cfg(feature = "by_ref_proposal")]
        let mut new_signer = new_signer;

        #[cfg(feature = "by_ref_proposal")]
        for p in &provisional_state.applied_proposals.updates {
            if p.sender == Sender::Member(*self_index) {
                let leaf_pk = &p.proposal.leaf_node.public_key;

                #[cfg(feature = "std")]
                let new_leaf_sk_and_signer = self.pending_updates.get(leaf_pk);

                #[cfg(not(feature = "std"))]
                let new_leaf_sk_and_signer = self
                    .pending_updates
                    .iter()
                    .find_map(|(pk, sk)| (pk == leaf_pk).then_some(sk));

                let new_leaf_sk = new_leaf_sk_and_signer.map(|(sk, _)| sk.clone());
                new_signer = new_leaf_sk_and_signer.and_then(|(_, sk)| sk.clone());

                provisional_private_tree
                    .update_leaf(new_leaf_sk.ok_or(MlsError::UpdateErrorNoSecretKey)?);

                break;
            }
        }

        Ok((provisional_private_tree, new_signer))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn encrypt_group_secrets(
        &self,
        key_package: &KeyPackage,
        leaf_index: LeafIndex,
        joiner_secret: &JoinerSecret,
        path_secrets: Option<&Vec<Option<PathSecret>>>,
        #[cfg(feature = "psk")] psks: Vec<PreSharedKeyID>,
        encrypted_group_info: &[u8],
    ) -> Result<EncryptedGroupSecrets, MlsError> {
        let path_secret = path_secrets
            .map(|secrets| {
                secrets
                    .get(
                        tree_math::leaf_lca_level(*self.private_tree.self_index, *leaf_index)
                            as usize
                            - 1,
                    )
                    .cloned()
                    .flatten()
                    .ok_or(MlsError::InvalidTreeKemPrivateKey)
            })
            .transpose()?;

        #[cfg(not(feature = "psk"))]
        let psks = Vec::new();

        let group_secrets = GroupSecrets {
            joiner_secret: joiner_secret.clone(),
            path_secret,
            psks,
        };

        let encrypted_group_secrets = group_secrets
            .encrypt(
                &self.cipher_suite_provider,
                &key_package.hpke_init_key,
                encrypted_group_info,
            )
            .await?;

        Ok(EncryptedGroupSecrets {
            new_member: key_package
                .to_reference(&self.cipher_suite_provider)
                .await?,
            encrypted_group_secrets,
        })
    }

    /// Create a proposal message that adds a new member to the group.
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_add(
        &mut self,
        key_package: MlsMessage,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self.add_proposal(key_package)?;
        self.proposal_message(proposal, authenticated_data).await
    }

    fn add_proposal(&self, key_package: MlsMessage) -> Result<Proposal, MlsError> {
        Ok(Proposal::Add(alloc::boxed::Box::new(AddProposal {
            key_package: key_package
                .into_key_package()
                .ok_or(MlsError::UnexpectedMessageType)?,
        })))
    }

    /// Create a proposal message that updates your own public keys.
    ///
    /// This proposal is useful for contributing additional forward secrecy
    /// and post-compromise security to the group without having to perform
    /// the necessary computation of a [`Group::commit`].
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_update(
        &mut self,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self.update_proposal(None, None, None).await?;
        self.proposal_message(proposal, authenticated_data).await
    }

    /// Create a proposal message that updates your own public keys
    /// as well as your credential.
    ///
    /// This proposal is useful for contributing additional forward secrecy
    /// and post-compromise security to the group without having to perform
    /// the necessary computation of a [`Group::commit`].
    ///
    /// Identity updates are allowed by the group by default assuming that the
    /// new identity provided is considered
    /// [valid](crate::IdentityProvider::validate_member)
    /// by and matches the output of the
    /// [identity](crate::IdentityProvider)
    /// function of the current
    /// [`IdentityProvider`](crate::IdentityProvider).
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_update_with_identity(
        &mut self,
        signer: SignatureSecretKey,
        signing_identity: SigningIdentity,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self
            .update_proposal(Some(signer), Some(signing_identity), None)
            .await?;

        self.proposal_message(proposal, authenticated_data).await
    }

    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn update_proposal(
        &mut self,
        signer: Option<SignatureSecretKey>,
        signing_identity: Option<SigningIdentity>,
        leaf_node_extensions: Option<ExtensionList>,
    ) -> Result<Proposal, MlsError> {
        let mut new_leaf_node: LeafNode = self.current_user_leaf_node()?.clone();

        let new_leaf_node_extensions =
            leaf_node_extensions.unwrap_or(new_leaf_node.ungreased_extensions());
        let secret_key = new_leaf_node
            .update(
                &self.cipher_suite_provider,
                self.group_id(),
                self.current_member_index(),
                Some(self.config.leaf_properties(new_leaf_node_extensions)),
                signing_identity,
                signer.as_ref().unwrap_or(&self.signer),
            )
            .await?;

        #[cfg(feature = "std")]
        self.pending_updates
            .insert(new_leaf_node.public_key.clone(), (secret_key, signer));

        #[cfg(not(feature = "std"))]
        self.pending_updates
            .push((new_leaf_node.public_key.clone(), (secret_key, signer)));

        Ok(Proposal::Update(UpdateProposal {
            leaf_node: new_leaf_node,
        }))
    }

    /// Create a proposal message that removes an existing member from the
    /// group.
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_remove(
        &mut self,
        index: u32,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self.remove_proposal(index)?;
        self.proposal_message(proposal, authenticated_data).await
    }

    fn remove_proposal(&self, index: u32) -> Result<Proposal, MlsError> {
        let leaf_index = LeafIndex::try_from(index)?;

        self.current_epoch_tree().get_leaf_node(leaf_index)?;

        Ok(Proposal::Remove(RemoveProposal {
            to_remove: leaf_index,
        }))
    }

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal", feature = "self_remove_proposal"))]
#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_self_remove(
        &mut self,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        if self.state.proposals.has_own_self_remove() {
            return Err(MlsError::SelfRemoveAlreadyProposed);
        }
        let proposal = Proposal::SelfRemove(SelfRemoveProposal {});
        self.proposal_message(proposal, authenticated_data).await
    }

    /// Create a proposal message that adds an external pre shared key to the group.
    ///
    /// Each group member will need to have the PSK associated with
    /// [`ExternalPskId`](mls_rs_core::psk::ExternalPskId) installed within
    /// the [`PreSharedKeyStorage`](mls_rs_core::psk::PreSharedKeyStorage)
    /// in use by this group upon processing a [commit](Group::commit) that
    /// contains this proposal.
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(all(feature = "by_ref_proposal", feature = "psk"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_external_psk(
        &mut self,
        psk: ExternalPskId,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self.psk_proposal(JustPreSharedKeyID::External(psk))?;
        self.proposal_message(proposal, authenticated_data).await
    }

    #[cfg(feature = "psk")]
    fn psk_proposal(&self, key_id: JustPreSharedKeyID) -> Result<Proposal, MlsError> {
        Ok(Proposal::Psk(PreSharedKeyProposal {
            psk: PreSharedKeyID::new(key_id, &self.cipher_suite_provider)?,
        }))
    }

    /// Create a proposal message that adds a pre shared key from a previous
    /// epoch to the current group state.
    ///
    /// Each group member will need to have the secret state from `psk_epoch`.
    /// In particular, the members who joined between `psk_epoch` and the
    /// current epoch cannot process a commit containing this proposal.
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
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
            psk_group_id: PskGroupId(self.group_id().to_vec()),
        };

        let proposal = self.psk_proposal(JustPreSharedKeyID::Resumption(key_id))?;
        self.proposal_message(proposal, authenticated_data).await
    }

    /// Create a proposal message that requests for this group to be
    /// reinitialized.
    ///
    /// Once a [`ReInitProposal`](proposal::ReInitProposal)
    /// has been sent, another group member can complete reinitialization of
    /// the group by calling [`Group::get_reinit_client`].
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
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
        let proposal = self.reinit_proposal(group_id, version, cipher_suite, extensions)?;
        self.proposal_message(proposal, authenticated_data).await
    }

    fn reinit_proposal(
        &self,
        group_id: Option<Vec<u8>>,
        version: ProtocolVersion,
        cipher_suite: CipherSuite,
        extensions: ExtensionList,
    ) -> Result<Proposal, MlsError> {
        let group_id = group_id.map(Ok).unwrap_or_else(|| {
            self.cipher_suite_provider
                .random_bytes_vec(self.cipher_suite_provider.kdf_extract_size())
                .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))
        })?;

        Ok(Proposal::ReInit(ReInitProposal {
            group_id,
            version,
            cipher_suite,
            extensions,
        }))
    }

    /// Create a proposal message that sets extensions stored in the group
    /// state.
    ///
    /// # Warning
    ///
    /// This function does not create a diff that will be applied to the
    /// current set of extension that are in use. In order for an existing
    /// extension to not be overwritten by this proposal, it must be included
    /// in the new set of extensions being proposed.
    ///
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_group_context_extensions(
        &mut self,
        extensions: ExtensionList,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        let proposal = self.group_context_extensions_proposal(extensions);
        self.proposal_message(proposal, authenticated_data).await
    }

    fn group_context_extensions_proposal(&self, extensions: ExtensionList) -> Proposal {
        Proposal::GroupContextExtensions(extensions)
    }

    /// Create a custom proposal message.
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(all(feature = "custom_proposal", feature = "by_ref_proposal"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn propose_custom(
        &mut self,
        proposal: CustomProposal,
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        self.proposal_message(Proposal::Custom(proposal), authenticated_data)
            .await
    }

    /// Validate a custom proposal message, verifying its signature and membership
    /// tag. Can be a proposal from the current or a prior epoch.
    /// Can also pass in a ProposalType to check the custom proposal against.
    /// Returns the `authenticated_data` and sender of the custom proposal, or an error if
    /// it fails to validate or if the message is not a custom proposal.
    ///
    /// WARNING: This API is not recommend for general purpose usage as it reduces the guarantees of RFC9420.
    /// Since prior epochs' membership keys are stored, an attacker that compromises a client's signature key
    /// and membership key is able to send valid MLS PublicMessages appearing to originate from the compromised
    /// client from prior epochs.
    /// This API is used by the GSMA's Rich Communication Suite for certain types of messages. See section 7.6
    /// for specifics https://www.gsma.com/solutions-and-impact/technologies/networks/wp-content/uploads/2025/03/RCC.16-v1.0.pdf.
#[cfg(all(feature = "custom_proposal", feature = "by_ref_proposal", feature = "prior_epoch_membership_key"))]
#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn validate_custom_proposal(
        &mut self,
        msg: &MlsMessage,
        proposal_type: Option<ProposalType>,
    ) -> Result<(Vec<u8>, Sender), MlsError> {
        let auth_content = self.validate_public_message(msg).await?;
        let proposal = match auth_content.content.content {
            Content::Proposal(p) => match *p {
                Proposal::Custom(c) => c,
                _ => return Err(MlsError::UnexpectedMessageType),
            },
            _ => return Err(MlsError::UnexpectedMessageType),
        };
        if proposal_type.is_some() && Some(proposal.proposal_type()) != proposal_type {
            return Err(MlsError::UnsupportedCustomProposal(
                proposal.proposal_type(),
            ));
        }
        Ok((
            auth_content.content.authenticated_data,
            auth_content.content.sender,
        ))
    }

    #[cfg(all(feature = "custom_proposal", feature = "prior_epoch_membership_key"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn validate_public_message(
        &mut self,
        msg: &MlsMessage,
    ) -> Result<AuthenticatedContent, MlsError> {
        let plaintext = match msg.payload {
            MlsMessagePayload::Plain(ref plaintext) => plaintext.clone(),
            _ => return Err(MlsError::UnexpectedMessageType),
        };
        let epoch_id = msg.epoch().ok_or(MlsError::EpochNotFound)?;
        let auth_content = if epoch_id == self.context().epoch {
            let auth_content = verify_plaintext_authentication(
                &self.cipher_suite_provider,
                plaintext,
                Some(&self.key_schedule.membership_key),
                &self.state.context,
                SignaturePublicKeysContainer::RatchetTree(&self.state.public_tree),
            )
            .await?;

            Ok::<_, MlsError>(auth_content)
        } else {
            #[cfg(feature = "prior_epoch")]
            {
                let epoch = self
                    .state_repo
                    .get_epoch_mut(epoch_id)
                    .await?
                    .ok_or(MlsError::EpochNotFound)?;

                let auth_content = verify_plaintext_authentication(
                    &self.cipher_suite_provider,
                    plaintext,
                    Some(&epoch.membership_key),
                    &epoch.context,
                    SignaturePublicKeysContainer::List(&epoch.signature_public_keys),
                )
                .await?;

                Ok(auth_content)
            }

            #[cfg(not(feature = "prior_epoch"))]
            Err(MlsError::EpochNotFound)
        }?;

        Ok(auth_content)
    }

    /// Delete all sent and received proposals cached for commit.
    #[cfg(feature = "by_ref_proposal")]
    pub fn clear_proposal_cache(&mut self) {
        self.state.proposals.clear()
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn format_for_wire(
        &mut self,
        content: AuthenticatedContent,
    ) -> Result<MlsMessage, MlsError> {
        #[cfg(feature = "private_message")]
        let payload = if content.wire_format == WireFormat::PrivateMessage {
            MlsMessagePayload::Cipher(self.create_ciphertext(content).await?)
        } else {
            MlsMessagePayload::Plain(self.create_plaintext(content).await?)
        };
        #[cfg(not(feature = "private_message"))]
        let payload = MlsMessagePayload::Plain(self.create_plaintext(content).await?);

        Ok(MlsMessage::new(self.protocol_version(), payload))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn create_plaintext(
        &self,
        auth_content: AuthenticatedContent,
    ) -> Result<PublicMessage, MlsError> {
        let membership_tag = if matches!(auth_content.content.sender, Sender::Member(_)) {
            let tag = self
                .key_schedule
                .get_membership_tag(&auth_content, self.context(), &self.cipher_suite_provider)
                .await?;

            Some(tag)
        } else {
            None
        };

        Ok(PublicMessage {
            content: auth_content.content,
            auth: auth_content.auth,
            membership_tag,
        })
    }

    #[cfg(feature = "private_message")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn create_ciphertext(
        &mut self,
        auth_content: AuthenticatedContent,
    ) -> Result<PrivateMessage, MlsError> {
        let padding_mode = self.encryption_options()?.padding_mode;

        let mut encryptor = CiphertextProcessor::new(self, self.cipher_suite_provider.clone());

        encryptor.seal(auth_content, padding_mode).await
    }

    /// Encrypt an application message using the current group state.
    ///
    /// `authenticated_data` will be sent unencrypted along with the contents
    /// of the proposal message.
    #[cfg(feature = "private_message")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn encrypt_application_message(
        &mut self,
        message: &[u8],
        authenticated_data: Vec<u8>,
    ) -> Result<MlsMessage, MlsError> {
        #[cfg(feature = "by_ref_proposal")]
        if !self.state.proposals.is_empty() {
            return Err(MlsError::CommitRequired);
        }

        let auth_content = AuthenticatedContent::new_signed(
            &self.cipher_suite_provider,
            self.context(),
            Sender::Member(*self.private_tree.self_index),
            Content::Application(message.to_vec().into()),
            &self.signer,
            WireFormat::PrivateMessage,
            authenticated_data,
        )
        .await?;

        self.format_for_wire(auth_content).await
    }

    #[cfg(feature = "private_message")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn decrypt_incoming_ciphertext(
        &mut self,
        message: &PrivateMessage,
    ) -> Result<AuthenticatedContent, MlsError> {
        let epoch_id = message.epoch;

        let auth_content = if epoch_id == self.context().epoch {
            let content = CiphertextProcessor::new(self, self.cipher_suite_provider.clone())
                .open(message)
                .await?;

            verify_auth_content_signature(
                &self.cipher_suite_provider,
                SignaturePublicKeysContainer::RatchetTree(&self.state.public_tree),
                self.context(),
                &content,
                #[cfg(feature = "by_ref_proposal")]
                &[],
            )
            .await?;

            Ok::<_, MlsError>(content)
        } else {
            #[cfg(feature = "prior_epoch")]
            {
                let epoch = self
                    .state_repo
                    .get_epoch_mut(epoch_id)
                    .await?
                    .ok_or(MlsError::EpochNotFound)?;

                let content = CiphertextProcessor::new(epoch, self.cipher_suite_provider.clone())
                    .open(message)
                    .await?;

                verify_auth_content_signature(
                    &self.cipher_suite_provider,
                    SignaturePublicKeysContainer::List(&epoch.signature_public_keys),
                    &epoch.context,
                    &content,
                    #[cfg(feature = "by_ref_proposal")]
                    &[],
                )
                .await?;

                Ok(content)
            }

            #[cfg(not(feature = "prior_epoch"))]
            Err(MlsError::EpochNotFound)
        }?;

        Ok(auth_content)
    }

    /// Apply a pending commit that was created by [`Group::commit`] or
    /// [`CommitBuilder::build`].
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn apply_pending_commit(&mut self) -> Result<CommitMessageDescription, MlsError> {
        let pending = core::mem::take(&mut self.pending_commit);
        self.apply_detached_commit(CommitSecrets(pending)).await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn apply_pending_commit_backwards_compatible(
        &mut self,
    ) -> Result<CommitMessageDescription, MlsError> {
        let pending = core::mem::take(&mut self.pending_commit);
        self.apply_detached_commit_backwards_compatible(CommitSecrets(pending))
            .await
    }

    /// Apply a detached commit that was created by [`Group::commit_detached`] or
    /// [`CommitBuilder::build_detached`].
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn apply_detached_commit(
        &mut self,
        commit_secrets: CommitSecrets,
    ) -> Result<CommitMessageDescription, MlsError> {
        let pending = match commit_secrets.0 {
            PendingCommitSnapshot::PendingCommit(bytes) => PendingCommit::mls_decode(&mut &*bytes)?,
            _ => return Err(MlsError::PendingCommitNotFound),
        };

        self.insert_past_epoch().await?;

        self.state = pending.state;
        self.epoch_secrets = pending.epoch_secrets;
        self.private_tree = pending.private_tree;
        self.key_schedule = pending.key_schedule;
        self.signer = pending.signer;

        Ok(pending.output)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn apply_detached_commit_backwards_compatible(
        &mut self,
        commit_secrets: CommitSecrets,
    ) -> Result<CommitMessageDescription, MlsError> {
        match commit_secrets.0 {
            PendingCommitSnapshot::None | PendingCommitSnapshot::PendingCommit(_) => {
                self.apply_detached_commit(commit_secrets).await
            }
            PendingCommitSnapshot::LegacyPendingCommit(legacy_pending) => {
                let content = legacy_pending.content.clone();
                self.pending_commit = PendingCommitSnapshot::LegacyPendingCommit(legacy_pending);
                self.process_commit(content, None).await
            }
        }
    }

    /// Returns true if a commit has been created but not yet applied
    /// with [`Group::apply_pending_commit`] or cleared with [`Group::clear_pending_commit`]
    pub fn has_pending_commit(&self) -> bool {
        !self.pending_commit.is_none()
    }

    /// Clear the currently pending commit.
    ///
    /// This function will automatically be called in the event that a
    /// commit message is processed using [`Group::process_incoming_message`]
    /// before [`Group::apply_pending_commit`] is called.
    pub fn clear_pending_commit(&mut self) {
        self.pending_commit = Default::default()
    }

    /// Returns true if the client has received or issued a proposal
    /// that needs to be committed to with [`Group::commit`] before encrypting an
    /// application message.
    #[cfg(feature = "by_ref_proposal")]
    pub fn commit_required(&self) -> bool {
        !self.state.proposals.is_empty()
    }

    /// Process an inbound message for this group.
    ///
    /// # Warning
    ///
    /// Changes to the group's state as a result of processing `message` will
    /// not be persisted by the
    /// [`GroupStateStorage`](crate::GroupStateStorage)
    /// in use by this group until [`Group::write_to_storage`] is called.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    #[inline(never)]
    pub async fn process_incoming_message(
        &mut self,
        message: MlsMessage,
    ) -> Result<ReceivedMessage, MlsError> {
        if let Some(pending) = self.pending_commit.commit_hash()? {
            let message_hash = MessageHash::compute(&self.cipher_suite_provider, &message).await?;

            if message_hash == pending {
                let message_description = self.apply_pending_commit().await?;

                return Ok(ReceivedMessage::Commit(message_description));
            }
        }

        #[cfg(feature = "by_ref_proposal")]
        if message.wire_format() == WireFormat::PrivateMessage {
            let cached_own_proposal = self
                .state
                .proposals
                .get_own(&self.cipher_suite_provider, &message)
                .await?;

            if let Some(cached) = cached_own_proposal {
                return Ok(ReceivedMessage::Proposal(cached));
            }
        }

        MessageProcessor::process_incoming_message(
            self,
            message,
            #[cfg(feature = "by_ref_proposal")]
            true,
        )
        .await
    }

    /// Process an inbound message for this group, providing additional context
    /// with a message timestamp.
    ///
    /// Providing a timestamp is useful when the
    /// [`IdentityProvider`](crate::IdentityProvider)
    /// in use by the group can determine validity based on a timestamp.
    /// For example, this allows for checking X.509 certificate expiration
    /// at the time when `message` was received by a server rather than when
    /// a specific client asynchronously received `message`
    ///
    /// # Warning
    ///
    /// Changes to the group's state as a result of processing `message` will
    /// not be persisted by the
    /// [`GroupStateStorage`](crate::GroupStateStorage)
    /// in use by this group until [`Group::write_to_storage`] is called.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn process_incoming_message_with_time(
        &mut self,
        message: MlsMessage,
        time: MlsTime,
    ) -> Result<ReceivedMessage, MlsError> {
        if let Some(pending) = self.pending_commit.commit_hash()? {
            let message_hash = MessageHash::compute(&self.cipher_suite_provider, &message).await?;

            if message_hash == pending {
                let message_description = self.apply_pending_commit().await?;

                return Ok(ReceivedMessage::Commit(message_description));
            }
        }

        #[cfg(feature = "by_ref_proposal")]
        if message.wire_format() == WireFormat::PrivateMessage {
            let cached_own_proposal = self
                .state
                .proposals
                .get_own(&self.cipher_suite_provider, &message)
                .await?;

            if let Some(cached) = cached_own_proposal {
                return Ok(ReceivedMessage::Proposal(cached));
            }
        }

        MessageProcessor::process_incoming_message_with_time(
            self,
            message,
            #[cfg(feature = "by_ref_proposal")]
            true,
            Some(time),
        )
        .await
    }

    /// Find a group member by
    /// [identity](crate::IdentityProvider::identity)
    ///
    /// This function determines identity by calling the
    /// [`IdentityProvider`](crate::IdentityProvider)
    /// currently in use by the group.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn member_with_identity(&self, identity: &[u8]) -> Result<Member, MlsError> {
        let tree = &self.state.public_tree;

        #[cfg(feature = "tree_index")]
        let index = tree.get_leaf_node_with_identity(identity);

        #[cfg(not(feature = "tree_index"))]
        let index = tree
            .get_leaf_node_with_identity(
                identity,
                &self.identity_provider(),
                &self.state.context.extensions,
            )
            .await?;

        let index = index.ok_or(MlsError::MemberNotFound)?;
        let node = self.state.public_tree.get_leaf_node(index)?;

        Ok(member_from_leaf_node(node, index))
    }

    /// Create a group info message that can be used for external proposals and commits.
    ///
    /// The returned `GroupInfo` is suitable for one external commit for the current epoch.
    /// If `with_tree_in_extension` is set to true, the returned `GroupInfo` contains the
    /// ratchet tree and therefore contains all information needed to join the group. Otherwise,
    /// the ratchet tree must be obtained separately, e.g. via
    /// (ExternalClient::export_tree)[crate::external_client::ExternalGroup::export_tree].
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn group_info_message_allowing_ext_commit(
        &self,
        with_tree_in_extension: bool,
    ) -> Result<MlsMessage, MlsError> {
        let exts = ExtensionList::new();
        self.group_info_message_allowing_ext_commit_with_extensions(with_tree_in_extension, exts)
            .await
    }

    /// Create a group info message that can be used for external proposals and commits,
    /// and that includes a user-specified list of group info extensions.
    ///
    /// The returned `GroupInfo` is suitable for one external commit for the current epoch.
    /// If `with_tree_in_extension` is set to true, the returned `GroupInfo` contains the
    /// ratchet tree and therefore contains all information needed to join the group. Otherwise,
    /// the ratchet tree must be obtained separately, e.g. via
    /// (ExternalClient::export_tree)[crate::external_client::ExternalGroup::export_tree].
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn group_info_message_allowing_ext_commit_with_extensions(
        &self,
        with_tree_in_extension: bool,
        mut extensions: ExtensionList,
    ) -> Result<MlsMessage, MlsError> {
        extensions.set_from({
            self.key_schedule
                .get_external_key_pair_ext(&self.cipher_suite_provider)
                .await?
        })?;

        self.group_info_message_internal(extensions, with_tree_in_extension)
            .await
    }

    /// Create a group info message that can be used for external proposals.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn group_info_message(
        &self,
        with_tree_in_extension: bool,
    ) -> Result<MlsMessage, MlsError> {
        self.group_info_message_internal(ExtensionList::new(), with_tree_in_extension)
            .await
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn group_info_message_internal(
        &self,
        mut initial_extensions: ExtensionList,
        with_tree_in_extension: bool,
    ) -> Result<MlsMessage, MlsError> {
        if with_tree_in_extension {
            initial_extensions.set_from(RatchetTreeExt {
                tree_data: ExportedTree::new(self.state.public_tree.nodes.clone()),
            })?;
        }

        let mut info = GroupInfo {
            group_context: self.context().clone(),
            extensions: initial_extensions,
            confirmation_tag: self.state.confirmation_tag.clone(),
            signer: self.private_tree.self_index,
            signature: Vec::new(),
        };

        info.grease(self.cipher_suite_provider())?;

        info.sign(&self.cipher_suite_provider, &self.signer, &())
            .await?;

        Ok(MlsMessage::new(
            self.protocol_version(),
            MlsMessagePayload::GroupInfo(info),
        ))
    }

    /// Get the current group context summarizing various information about the group.
    #[inline(always)]
    pub fn context(&self) -> &GroupContext {
        &self.group_state().context
    }

    /// Get the
    /// [epoch_authenticator](https://messaginglayersecurity.rocks/mls-protocol/draft-ietf-mls-protocol.html#name-key-schedule)
    /// of the current epoch.
    pub fn epoch_authenticator(&self) -> Result<Secret, MlsError> {
        Ok(self.key_schedule.authentication_secret.clone().into())
    }

    /// Export a secret for use outside of MLS. Each epoch, label, context
    /// combination has a unique and independent secret. Secrets for all
    /// epochs, labels and contexts can be derived until either the epoch
    /// changes, i.e. a commit is received (or own commit is applied), or
    /// [Group::delete_exporter] is called.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn export_secret(
        &self,
        label: &[u8],
        context: &[u8],
        len: usize,
    ) -> Result<Secret, MlsError> {
        self.key_schedule
            .export_secret(label, context, len, &self.cipher_suite_provider)
            .await
            .map(Into::into)
    }

    /// Delete the exporter secret. Afterwards the state contains no information
    /// about any secrets outputted by [Group::export_secret] (for the current or
    /// past epochs). This means that after calling this function, [Group::export_secret]
    /// can no longer be used and we get forward secrecy for all secrets derived using
    /// [Group::export_secret].
    pub fn delete_exporter(&mut self) {
        self.key_schedule.delete_exporter();
    }

    /// Export the current epoch's ratchet tree in serialized format.
    ///
    /// This function is used to provide the current group tree to new members
    /// when the `ratchet_tree_extension` is not used according to [`MlsRules::commit_options`].
    pub fn export_tree(&self) -> ExportedTree<'_> {
        ExportedTree::new_borrowed(&self.current_epoch_tree().nodes)
    }

    /// Current version of the MLS protocol in use by this group.
    pub fn protocol_version(&self) -> ProtocolVersion {
        self.context().protocol_version
    }

    /// Current cipher suite in use by this group.
    pub fn cipher_suite(&self) -> CipherSuite {
        self.context().cipher_suite
    }

    /// Current roster
    pub fn roster(&self) -> Roster<'_> {
        self.group_state().public_tree.roster()
    }

    /// Determines equality of two different groups internal states.
    /// Useful for testing.
    ///
    pub fn equal_group_state(a: &Group<C>, b: &Group<C>) -> bool {
        a.state == b.state && a.key_schedule == b.key_schedule && a.epoch_secrets == b.epoch_secrets
    }

    #[cfg(feature = "psk")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn get_psk(
        &self,
        psks: &[ProposalInfo<PreSharedKeyProposal>],
    ) -> Result<(PskSecret, Vec<PreSharedKeyID>), MlsError> {
        if let Some(psk) = self.previous_psk.clone() {
            let psk_id = vec![psk.id.clone()];
            let psk = PskSecret::calculate(&[psk], self.cipher_suite_provider()).await?;

            Ok((psk, psk_id))
        } else {
            let psks = psks
                .iter()
                .map(|psk| psk.proposal.psk.clone())
                .collect::<Vec<_>>();

            let psk = PskResolver {
                group_context: Some(self.context()),
                current_epoch: Some(&self.epoch_secrets),
                prior_epochs: Some(&self.state_repo),
                psk_store: &self.config.secret_store(),
            }
            .resolve_to_secret(&psks, self.cipher_suite_provider())
            .await?;

            Ok((psk, psks))
        }
    }

    #[cfg(feature = "private_message")]
    pub(crate) fn encryption_options(&self) -> Result<EncryptionOptions, MlsError> {
        self.config
            .mls_rules()
            .encryption_options(&self.roster(), self.group_context())
            .map_err(|e| MlsError::MlsRulesError(e.into_any_error()))
    }

    #[cfg(not(feature = "psk"))]
    fn get_psk(&self) -> PskSecret {
        PskSecret::new(self.cipher_suite_provider())
    }

    /// Returns the key generation used by the next invocation of
    /// [`Group::encrypt_application_message`]. Does not increment the generation nor
    /// derive keys.
    ///
    /// Used by clients to authenticate the generation to defend against in-group forgery
    /// attacks described in https://eprint.iacr.org/2025/554. This may be accomplished
    /// by placing the generation in the `message` or `authenticated_data` parameters of
    /// [`Group::encrypt_application_message`], as both fields are signed by the sender's
    /// signature key.
    ///
    /// To verify, get the unauthenticated generation from ApplicationMessageDescription
    /// returned from [`Group::process_incoming_message`], which is the value used to
    /// derive keys to decrypt the message, and check that it equals the authenticated
    /// generation.
    ///
    /// WARNING: This is only safe for synchronous usage of [`Group`] APIs.
    #[cfg(all(
        feature = "export_key_generation",
        feature = "private_message",
        feature = "secret_tree_access",
    ))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub fn peek_next_key_generation(&mut self) -> Result<u32, MlsError> {
        self.epoch_secrets
            .secret_tree
            .peek_next_key_generation(
                &self.cipher_suite_provider,
                crate::tree_kem::node::NodeIndex::from(self.private_tree.self_index),
                KeyType::Application,
            )
            .await
    }

    #[cfg(feature = "secret_tree_access")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    #[inline(never)]
    pub async fn next_encryption_key(&mut self) -> Result<MessageKey, MlsError> {
        self.epoch_secrets
            .secret_tree
            .next_message_key(
                &self.cipher_suite_provider,
                crate::tree_kem::node::NodeIndex::from(self.private_tree.self_index),
                KeyType::Application,
            )
            .await
    }

    #[cfg(feature = "secret_tree_access")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn derive_decryption_key(
        &mut self,
        sender: u32,
        generation: u32,
    ) -> Result<MessageKey, MlsError> {
        self.epoch_secrets
            .secret_tree
            .message_key_generation(
                &self.cipher_suite_provider,
                crate::tree_kem::node::NodeIndex::from(sender),
                KeyType::Application,
                generation,
            )
            .await
    }
}

impl<C: ClientConfig> Group<C> {
    #[cfg(feature = "psk")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn psk_secret<CS: CipherSuiteProvider>(
        config: &C,
        cipher_suite_provider: &CS,
        psks: &[PreSharedKeyID],
        additional_psk: Option<PskSecretInput>,
    ) -> Result<PskSecret, MlsError> {
        if let Some(psk) = additional_psk {
            let psk_id = psks.first().ok_or(MlsError::UnexpectedPskId)?;

            match &psk_id.key_id {
                JustPreSharedKeyID::Resumption(r) if r.usage != ResumptionPSKUsage::Application => {
                    Ok(())
                }
                _ => Err(MlsError::UnexpectedPskId),
            }?;

            let mut psk = psk;
            psk.id.psk_nonce = psk_id.psk_nonce.clone();
            PskSecret::calculate(&[psk], cipher_suite_provider).await
        } else {
            PskResolver::<
                <C as ClientConfig>::GroupStateStorage,
                <C as ClientConfig>::KeyPackageRepository,
                <C as ClientConfig>::PskStore,
            > {
                group_context: None,
                current_epoch: None,
                prior_epochs: None,
                psk_store: &config.secret_store(),
            }
            .resolve_to_secret(psks, cipher_suite_provider)
            .await
        }
    }

    #[cfg(not(feature = "psk"))]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn psk_secret<CS: CipherSuiteProvider>(
        _config: &C,
        cipher_suite_provider: &CS,
        _psks: &[PreSharedKeyID],
    ) -> Result<PskSecret, MlsError> {
        Ok(PskSecret::new(cipher_suite_provider))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn decrypt_group_info(
        welcome: &MlsMessage,
        config: &C,
    ) -> Result<GroupInfo, MlsError> {
        Self::decrypt_group_info_internal(
            welcome,
            config,
            #[cfg(feature = "psk")]
            None,
        )
        .await
        .map(|info| info.0)
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn decrypt_group_info_internal(
        welcome: &MlsMessage,
        config: &C,
        #[cfg(feature = "psk")] additional_psk: Option<PskSecretInput>,
    ) -> Result<(GroupInfo, KeyPackageGeneration, GroupSecrets, PskSecret), MlsError> {
        let protocol_version = welcome.version;

        if !config.version_supported(protocol_version) {
            return Err(MlsError::UnsupportedProtocolVersion(protocol_version));
        }

        let MlsMessagePayload::Welcome(welcome) = &welcome.payload else {
            return Err(MlsError::UnexpectedMessageType);
        };

        let cipher_suite_provider =
            cipher_suite_provider(config.crypto_provider(), welcome.cipher_suite)?;

        let (encrypted_group_secrets, key_package_generation) =
            find_key_package_generation(&config.key_package_repo(), &welcome.secrets).await?;

        let key_package_version = key_package_generation.key_package.version;

        if key_package_version != protocol_version {
            return Err(MlsError::ProtocolVersionMismatch);
        }

        let group_secrets = GroupSecrets::decrypt(
            &cipher_suite_provider,
            &key_package_generation.init_secret_key,
            &key_package_generation.key_package.hpke_init_key,
            &welcome.encrypted_group_info,
            &encrypted_group_secrets.encrypted_group_secrets,
        )
        .await?;

        let psk_secret = Self::psk_secret(
            config,
            &cipher_suite_provider,
            &group_secrets.psks,
            #[cfg(feature = "psk")]
            additional_psk,
        )
        .await?;

        let welcome_secret = WelcomeSecret::from_joiner_secret(
            &cipher_suite_provider,
            &group_secrets.joiner_secret,
            &psk_secret,
        )
        .await?;

        let decrypted_group_info = welcome_secret
            .decrypt(&welcome.encrypted_group_info)
            .await?;

        let group_info = GroupInfo::mls_decode(&mut &**decrypted_group_info)?;

        Ok((
            group_info,
            key_package_generation,
            group_secrets,
            psk_secret,
        ))
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    #[cfg(feature = "prior_epoch")]
    pub(crate) async fn insert_past_epoch(&mut self) -> Result<(), MlsError> {
        let signature_public_keys = self
            .state
            .public_tree
            .leaves()
            .map(|l| l.map(|n| n.signing_identity.signature_key.clone()))
            .collect();

        let past_epoch = PriorEpoch {
            context: self.context().clone(),
            self_index: self.private_tree.self_index,
            secrets: self.epoch_secrets.clone(),
            signature_public_keys,
            #[cfg(feature = "prior_epoch_membership_key")]
            membership_key: self.key_schedule.membership_key.clone(),
        };

        self.state_repo.insert(past_epoch).await?;

        Ok(())
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    #[cfg(not(feature = "prior_epoch"))]
    pub(crate) async fn insert_past_epoch(&mut self) -> Result<(), MlsError> {
        Ok(())
    }
}

#[cfg(feature = "private_message")]
impl<C> GroupStateProvider for Group<C>
where
    C: ClientConfig + Clone,
{
    fn group_context(&self) -> &GroupContext {
        self.context()
    }

    fn self_index(&self) -> LeafIndex {
        self.private_tree.self_index
    }

    fn epoch_secrets_mut(&mut self) -> &mut EpochSecrets {
        &mut self.epoch_secrets
    }

    fn epoch_secrets(&self) -> &EpochSecrets {
        &self.epoch_secrets
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl<C> MessageProcessor for Group<C>
where
    C: ClientConfig + Clone,
{
    type MlsRules = C::MlsRules;
    type IdentityProvider = C::IdentityProvider;
    type PreSharedKeyStorage = C::PskStore;
    type OutputType = ReceivedMessage;
    type CipherSuiteProvider = <C::CryptoProvider as CryptoProvider>::CipherSuiteProvider;

    #[cfg(feature = "private_message")]
    async fn process_ciphertext(
        &mut self,
        cipher_text: &PrivateMessage,
    ) -> Result<EventOrContent<Self::OutputType>, MlsError> {
        self.decrypt_incoming_ciphertext(cipher_text)
            .await
            .map(EventOrContent::Content)
    }

    async fn verify_plaintext_authentication(
        &self,
        message: PublicMessage,
    ) -> Result<EventOrContent<Self::OutputType>, MlsError> {
        let auth_content = verify_plaintext_authentication(
            &self.cipher_suite_provider,
            message,
            Some(&self.key_schedule.membership_key),
            &self.state.context,
            SignaturePublicKeysContainer::RatchetTree(&self.state.public_tree),
        )
        .await?;

        Ok(EventOrContent::Content(auth_content))
    }

    #[cfg(all(feature = "export_key_generation", feature = "private_message"))]
    /// Returns the unauthenticated key generation used to decrypt the private message.
    async fn get_unauthenticated_key_generation_from_sender_data(
        &mut self,
        cipher_text: &PrivateMessage,
    ) -> Result<Option<u32>, MlsError> {
        let epoch_id = cipher_text.epoch;
        let sender_data = if epoch_id == self.context().epoch {
            CiphertextProcessor::new(self, self.cipher_suite_provider.clone())
                .open_sender_data(cipher_text)
                .await?
        } else {
            #[cfg(feature = "prior_epoch")]
            {
                let epoch = self
                    .state_repo
                    .get_epoch_mut(epoch_id)
                    .await?
                    .ok_or(MlsError::EpochNotFound)?;
                CiphertextProcessor::new(epoch, self.cipher_suite_provider.clone())
                    .open_sender_data(cipher_text)
                    .await?
            }
            #[cfg(not(feature = "prior_epoch"))]
            Err(MlsError::EpochNotFound)
        };
        Ok(Some(sender_data.generation))
    }

    async fn apply_update_path(
        &mut self,
        sender: LeafIndex,
        update_path: &ValidatedUpdatePath,
        provisional_state: &mut ProvisionalState,
    ) -> Result<Option<(TreeKemPrivate, PathSecret)>, MlsError> {
        let (mut provisional_private_tree, new_signer) =
            self.provisional_private_tree(provisional_state)?;

        if let Some(signer) = new_signer {
            self.signer = signer;
        }

        provisional_state
            .public_tree
            .apply_update_path(
                sender,
                update_path,
                &provisional_state.group_context.extensions,
                self.identity_provider(),
                self.cipher_suite_provider(),
            )
            .await?;

        if sender == self.private_tree.self_index {
            let PendingCommitSnapshot::LegacyPendingCommit(legacy_pending) = &self.pending_commit
            else {
                return Err(MlsError::CantProcessMessageFromSelf);
            };

            return Ok(Some((
                legacy_pending.private_tree.clone(),
                legacy_pending.commit_secret.clone(),
            )));
        }

        provisional_state.group_context.tree_hash = provisional_state
            .public_tree
            .tree_hash(&self.cipher_suite_provider)
            .await?;

        let context_bytes = provisional_state.group_context.mls_encode_to_vec()?;

        TreeKem::new(
            &mut provisional_state.public_tree,
            &mut provisional_private_tree,
        )
        .decap(
            sender,
            update_path,
            &provisional_state.indexes_of_added_kpkgs,
            &context_bytes,
            &self.cipher_suite_provider,
        )
        .await
        .map(|root_secret| Some((provisional_private_tree, root_secret)))
    }

    async fn update_key_schedule(
        &mut self,
        secrets: Option<(TreeKemPrivate, PathSecret)>,
        interim_transcript_hash: InterimTranscriptHash,
        confirmation_tag: &ConfirmationTag,
        provisional_state: ProvisionalState,
    ) -> Result<(), MlsError> {
        let commit_secret = if let Some(secrets) = secrets {
            self.private_tree = secrets.0;
            secrets.1
        } else {
            PathSecret::empty(&self.cipher_suite_provider)
        };


        let key_schedule = match provisional_state
            .applied_proposals
            .external_initializations
            .first()
        {
            Some(ext_init) => {
                self.key_schedule
                    .derive_for_external(&ext_init.proposal.kem_output, &self.cipher_suite_provider)
                    .await?
            }
            _ => self.key_schedule.clone(),
        };

        #[cfg(feature = "psk")]
        let (psk, _) = self
            .get_psk(&provisional_state.applied_proposals.psks)
            .await?;

        #[cfg(not(feature = "psk"))]
        let psk = self.get_psk();

        let key_schedule_result = KeySchedule::from_key_schedule(
            &key_schedule,
            &commit_secret,
            &provisional_state.group_context,
            #[cfg(any(feature = "secret_tree_access", feature = "private_message"))]
            provisional_state.public_tree.total_leaf_count(),
            &psk,
            &self.cipher_suite_provider,
        )
        .await?;

        let new_confirmation_tag = ConfirmationTag::create(
            &key_schedule_result.confirmation_key,
            &provisional_state.group_context.confirmed_transcript_hash,
            &self.cipher_suite_provider,
        )
        .await?;

        if &new_confirmation_tag != confirmation_tag {
            return Err(MlsError::InvalidConfirmationTag);
        }

        self.insert_past_epoch().await?;

        self.epoch_secrets = key_schedule_result.epoch_secrets;
        self.state.context = provisional_state.group_context;
        self.state.interim_transcript_hash = interim_transcript_hash;
        self.key_schedule = key_schedule_result.key_schedule;
        self.state.public_tree = provisional_state.public_tree;
        self.state.confirmation_tag = new_confirmation_tag;

        #[cfg(feature = "by_ref_proposal")]
        self.state.proposals.clear();

        #[cfg(feature = "by_ref_proposal")]
        {
            self.pending_updates = Default::default();
        }

        self.pending_commit = Default::default();

        Ok(())
    }

    fn mls_rules(&self) -> Self::MlsRules {
        self.config.mls_rules()
    }

    fn identity_provider(&self) -> Self::IdentityProvider {
        self.config.identity_provider()
    }

    fn psk_storage(&self) -> Self::PreSharedKeyStorage {
        self.config.secret_store()
    }

    fn group_state(&self) -> &GroupState {
        &self.state
    }

    fn group_state_mut(&mut self) -> &mut GroupState {
        &mut self.state
    }

    fn removal_proposal(
        &self,
        provisional_state: &ProvisionalState,
    ) -> Option<ProposalInfo<RemoveProposal>> {
        provisional_state
            .applied_proposals
            .removals
            .iter()
            .find(|p| p.proposal.to_remove == self.private_tree.self_index)
            .cloned()
    }

#[cfg(all(feature = "by_ref_proposal", feature = "custom_proposal", feature = "self_remove_proposal"))]
fn self_removal_proposal(
        &self,
        provisional_state: &ProvisionalState,
    ) -> Option<ProposalInfo<SelfRemoveProposal>> {
        provisional_state
            .applied_proposals
            .self_removes
            .iter()
            .find(|p| p.sender == Sender::Member(*self.private_tree.self_index))
            .cloned()
    }

    #[cfg(feature = "private_message")]
    fn min_epoch_available(&self) -> Option<u64> {
        None
    }

    fn cipher_suite_provider(&self) -> &Self::CipherSuiteProvider {
        &self.cipher_suite_provider
    }
}
