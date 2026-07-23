// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::cipher_suite::CipherSuite;
use crate::client_builder::{recreate_config, BaseConfig, ClientBuilder, MakeConfig};
use crate::client_config::ClientConfig;
use crate::group::framing::MlsMessage;

use crate::group::{cipher_suite_provider, validate_group_info_joiner, GroupInfo};
use crate::group::{
    framing::MlsMessagePayload, snapshot::Snapshot, ExportedTree, Group, NewMemberInfo,
};
#[cfg(feature = "by_ref_proposal")]
use crate::group::{
    framing::{Content, PublicMessage, Sender, WireFormat},
    message_signature::AuthenticatedContent,
    proposal::{AddProposal, Proposal},
};
use crate::identity::SigningIdentity;
use crate::key_package::{KeyPackageGeneration, KeyPackageGenerator};
use crate::protocol_version::ProtocolVersion;
use crate::time::MlsTime;
use crate::tree_kem::node::NodeIndex;
use alloc::vec::Vec;
use mls_rs_codec::MlsDecode;
use mls_rs_core::crypto::{CryptoProvider, SignatureSecretKey};
use mls_rs_core::error::{AnyError, IntoAnyError};
use mls_rs_core::extension::{ExtensionError, ExtensionList, ExtensionType};
use mls_rs_core::group::{GroupStateStorage, ProposalType};
use mls_rs_core::identity::{CredentialType, IdentityProvider, MemberValidationContext};
use mls_rs_core::key_package::KeyPackageStorage;

use crate::group::external_commit::ExternalCommitBuilder;

#[cfg(feature = "by_ref_proposal")]
use alloc::boxed::Box;

#[derive(Debug)]
#[cfg_attr(feature = "std", derive(thiserror::Error))]
#[non_exhaustive]
pub enum MlsError {
    #[cfg_attr(feature = "std", error(transparent))]
    IdentityProviderError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    CryptoProviderError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    KeyPackageRepoError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    GroupStorageError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    PskStoreError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    MlsRulesError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    SerializationError(AnyError),
    #[cfg_attr(feature = "std", error(transparent))]
    ExtensionError(AnyError),
    #[cfg_attr(feature = "std", error("Cipher suite does not match"))]
    CipherSuiteMismatch,
    #[cfg_attr(feature = "std", error("Initial epoch must be 1"))]
    InitialEpochNotOne,
    #[cfg_attr(feature = "std", error("Invalid commit, missing required path"))]
    CommitMissingPath,
    #[cfg_attr(feature = "std", error("plaintext message for incorrect epoch"))]
    InvalidEpoch,
    #[cfg_attr(feature = "std", error("invalid signature found"))]
    InvalidSignature,
    #[cfg_attr(feature = "std", error("invalid confirmation tag"))]
    InvalidConfirmationTag,
    #[cfg_attr(feature = "std", error("invalid membership tag"))]
    InvalidMembershipTag,
    #[cfg_attr(feature = "std", error("corrupt private key, missing required values"))]
    InvalidTreeKemPrivateKey,
    #[cfg_attr(feature = "std", error("key package not found, unable to process"))]
    WelcomeKeyPackageNotFound,
    #[cfg_attr(feature = "std", error("leaf not found in tree for index {0}"))]
    LeafNotFound(u32),
    #[cfg_attr(feature = "std", error("message from self can't be processed"))]
    CantProcessMessageFromSelf,
#[cfg_attr(feature = "std", error("pending proposals found, commit required before application messages can be sent"))]
CommitRequired,
#[cfg_attr(feature = "std", error("ratchet tree not provided or discovered in GroupInfo"))]
RatchetTreeNotFound,
    #[cfg_attr(feature = "std", error("External sender cannot commit"))]
    ExternalSenderCannotCommit,
    #[cfg_attr(feature = "std", error("Unsupported protocol version {0:?}"))]
    UnsupportedProtocolVersion(ProtocolVersion),
    #[cfg_attr(feature = "std", error("Protocol version mismatch"))]
    ProtocolVersionMismatch,
    #[cfg_attr(feature = "std", error("Unsupported cipher suite {0:?}"))]
    UnsupportedCipherSuite(CipherSuite),
    #[cfg_attr(feature = "std", error("Signing key of external sender is unknown"))]
    UnknownSigningIdentityForExternalSender,
#[cfg_attr(feature = "std", error("External proposals are disabled for this group"))]
ExternalProposalsDisabled,
#[cfg_attr(feature = "std", error("Signing identity is not allowed to externally propose"))]
InvalidExternalSigningIdentity,
    #[cfg_attr(feature = "std", error("Missing ExternalPub extension"))]
    MissingExternalPubExtension,
    #[cfg_attr(feature = "std", error("Epoch not found"))]
    EpochNotFound,
    #[cfg_attr(feature = "std", error("Unencrypted application message"))]
    UnencryptedApplicationMessage,
#[cfg_attr(feature = "std", error("NewMemberCommit sender type can only be used to send Commit content"))]
ExpectedCommitForNewMemberCommit,
#[cfg_attr(feature = "std", error("NewMemberProposal sender type can only be used to send add proposals"))]
ExpectedAddProposalForNewMemberProposal,
#[cfg_attr(feature = "std", error("External commit missing ExternalInit proposal"))]
ExternalCommitMissingExternalInit,
#[cfg_attr(feature = "std", error(
            "A ReIinit has been applied. The next action must be creating or receiving a welcome."
        ))]
GroupUsedAfterReInit,
    #[cfg_attr(feature = "std", error("Pending ReIinit not found."))]
    PendingReInitNotFound,
#[cfg_attr(feature = "std", error("The extensions in the welcome message and in the reinit do not match."))]
ReInitExtensionsMismatch,
    #[cfg_attr(feature = "std", error("signer not found for given identity"))]
    SignerNotFound,
    #[cfg_attr(feature = "std", error("commit already pending"))]
    ExistingPendingCommit,
    #[cfg_attr(feature = "std", error("pending commit not found"))]
    PendingCommitNotFound,
    #[cfg_attr(feature = "std", error("unexpected message type for action"))]
    UnexpectedMessageType,
#[cfg_attr(feature = "std", error("membership tag on MlsPlaintext for non-member sender"))]
MembershipTagForNonMember,
    #[cfg_attr(feature = "std", error("No member found for given identity id."))]
    MemberNotFound,
    #[cfg_attr(feature = "std", error("group not found"))]
    GroupNotFound,
    #[cfg_attr(feature = "std", error("unexpected PSK ID"))]
    UnexpectedPskId,
    #[cfg_attr(feature = "std", error("invalid sender for content type"))]
    InvalidSender,
    #[cfg_attr(feature = "std", error("GroupID mismatch"))]
    GroupIdMismatch,
    #[cfg_attr(feature = "std", error("storage retention can not be zero"))]
    NonZeroRetentionRequired,
    #[cfg_attr(feature = "std", error("Too many PSK IDs to compute PSK secret"))]
    TooManyPskIds,
    #[cfg_attr(feature = "std", error("Missing required Psk"))]
    MissingRequiredPsk,
    #[cfg_attr(feature = "std", error("Old group state not found"))]
    OldGroupStateNotFound,
    #[cfg_attr(feature = "std", error("leaf secret already consumed"))]
    InvalidLeafConsumption,
    #[cfg_attr(feature = "std", error("key not available, invalid generation {0}"))]
    KeyMissing(u32),
#[cfg_attr(feature = "std", error("requested generation {0} is too far ahead of current generation"))]
InvalidFutureGeneration(u32),
    #[cfg_attr(feature = "std", error("leaf node has no children"))]
    LeafNodeNoChildren,
    #[cfg_attr(feature = "std", error("root node has no parent"))]
    LeafNodeNoParent,
    #[cfg_attr(feature = "std", error("index out of range"))]
    InvalidTreeIndex,
    #[cfg_attr(feature = "std", error("time overflow"))]
    TimeOverflow,
    #[cfg_attr(feature = "std", error("invalid leaf_node_source"))]
    InvalidLeafNodeSource,
#[cfg_attr(feature = "std", error("current time ({}) is not within key package lifetime ({} to {})",
              timestamp.seconds_since_epoch(),
              not_before.seconds_since_epoch(),
              not_after.seconds_since_epoch(),
        ))]
InvalidLifetime {
        not_before: MlsTime,
        not_after: MlsTime,
        timestamp: MlsTime,
    },
    #[cfg_attr(feature = "std", error("required extension not found"))]
    RequiredExtensionNotFound(ExtensionType),
    #[cfg_attr(feature = "std", error("required proposal not found"))]
    RequiredProposalNotFound(ProposalType),
    #[cfg_attr(feature = "std", error("required credential not found"))]
    RequiredCredentialNotFound(CredentialType),
    #[cfg_attr(feature = "std", error("capabilities must describe extensions used"))]
    ExtensionNotInCapabilities(ExtensionType),
    #[cfg_attr(feature = "std", error("expected non-blank node"))]
    ExpectedNode,
    #[cfg_attr(feature = "std", error("node index is out of bounds {0}"))]
    InvalidNodeIndex(NodeIndex),
    #[cfg_attr(feature = "std", error("unexpected empty node found"))]
    UnexpectedEmptyNode,
#[cfg_attr(feature = "std", error("duplicate signature key, hpke key or identity found at index {0}"))]
DuplicateLeafData(u32),
#[cfg_attr(feature = "std", error("In-use credential type not supported by new leaf at index"))]
InUseCredentialTypeUnsupportedByNewLeaf,
#[cfg_attr(feature = "std", error("Not all members support the credential type used by new leaf"))]
CredentialTypeOfNewLeafIsUnsupported,
#[cfg_attr(feature = "std", error("the length of the update path is different than the length of the direct path"))]
WrongPathLen,
#[cfg_attr(feature = "std", error("same HPKE leaf key before and after applying the update path for leaf {0}"))]
SameHpkeKey(u32),
    #[cfg_attr(feature = "std", error("init key is not valid for cipher suite"))]
    InvalidInitKey,
#[cfg_attr(feature = "std", error("init key can not be equal to leaf node public key"))]
InitLeafKeyEquality,
    #[cfg_attr(feature = "std", error("different identity in update for leaf {0}"))]
    DifferentIdentityInUpdate(u32),
    #[cfg_attr(feature = "std", error("update path pub key mismatch"))]
    PubKeyMismatch,
    #[cfg_attr(feature = "std", error("tree hash mismatch"))]
    TreeHashMismatch,
    #[cfg_attr(feature = "std", error("bad update: no suitable secret key"))]
    UpdateErrorNoSecretKey,
    #[cfg_attr(feature = "std", error("invalid lca, not found on direct path"))]
    LcaNotFoundInDirectPath,
    #[cfg_attr(feature = "std", error("update path parent hash mismatch"))]
    ParentHashMismatch,
    #[cfg_attr(feature = "std", error("unexpected pattern of unmerged leaves"))]
    UnmergedLeavesMismatch,
    #[cfg_attr(feature = "std", error("empty tree"))]
    UnexpectedEmptyTree,
    #[cfg_attr(feature = "std", error("trailing blanks"))]
    UnexpectedTrailingBlanks,
#[cfg_attr(feature = "std", error("Commiter must not include any update proposals generated by the commiter"))]
InvalidCommitSelfUpdate,
    #[cfg_attr(feature = "std", error("A PreSharedKey proposal must have a PSK of type External or type Resumption and usage Application"))]
    InvalidTypeOrUsageInPreSharedKeyProposal,
    #[cfg_attr(feature = "std", error("psk nonce length does not match cipher suite"))]
    InvalidPskNonceLength,
#[cfg_attr(feature = "std", error("ReInit proposal protocol version is less than the version of the original group"))]
InvalidProtocolVersionInReInit,
    #[cfg_attr(feature = "std", error("More than one proposal applying to leaf: {0}"))]
    MoreThanOneProposalForLeaf(u32),
#[cfg_attr(feature = "std", error("More than one GroupContextExtensions proposal"))]
MoreThanOneGroupContextExtensionsProposal,
    #[cfg_attr(feature = "std", error("Invalid proposal type for sender"))]
    InvalidProposalTypeForSender,
#[cfg_attr(feature = "std", error("External commit must have exactly one ExternalInit proposal"))]
ExternalCommitMustHaveExactlyOneExternalInit,
    #[cfg_attr(feature = "std", error("External commit must have a new leaf"))]
    ExternalCommitMustHaveNewLeaf,
#[cfg_attr(feature = "std", error("External commit contains removal of other identity"))]
ExternalCommitRemovesOtherIdentity,
#[cfg_attr(feature = "std", error("External commit contains more than one Remove proposal"))]
ExternalCommitWithMoreThanOneRemove,
    #[cfg_attr(feature = "std", error("Duplicate PSK IDs"))]
    DuplicatePskIds,
#[cfg_attr(feature = "std", error("Invalid proposal type {0:?} in external commit"))]
InvalidProposalTypeInExternalCommit(ProposalType),
    #[cfg_attr(feature = "std", error("Committer can not remove themselves"))]
    CommitterSelfRemoval,
#[cfg_attr(feature = "std", error("Only members can commit proposals by reference"))]
OnlyMembersCanCommitProposalsByRef,
    #[cfg_attr(feature = "std", error("Other proposal with ReInit"))]
    OtherProposalWithReInit,
    #[cfg_attr(feature = "std", error("Unsupported group extension {0:?}"))]
    UnsupportedGroupExtension(ExtensionType),
    #[cfg_attr(feature = "std", error("Unsupported custom proposal type {0:?}"))]
    UnsupportedCustomProposal(ProposalType),
    #[cfg_attr(feature = "std", error("by-ref proposal not found"))]
    ProposalNotFound,
#[cfg_attr(feature = "std", error("Removing non-existing member (or removing a member twice)"))]
RemovingNonExistingMember,
    #[cfg_attr(feature = "std", error("Updated identity not a valid successor"))]
    InvalidSuccessor,
#[cfg_attr(feature = "std", error("Updating non-existing member (or updating a member twice)"))]
UpdatingNonExistingMember,
    #[cfg_attr(feature = "std", error("Failed generating next path secret"))]
    FailedGeneratingPathSecret,
    #[cfg_attr(feature = "std", error("Invalid group info"))]
    InvalidGroupInfo,
    #[cfg_attr(feature = "std", error("Invalid welcome message"))]
    InvalidWelcomeMessage,
    #[cfg_attr(feature = "std", error("Exporter deleted"))]
    ExporterDeleted,
    #[cfg_attr(feature = "std", error("Self-remove already proposed"))]
    SelfRemoveAlreadyProposed,
    #[cfg_attr(feature = "std", error("Default value listed"))]
    DefaultValueListed,
    #[cfg_attr(feature = "std", error("not a subgroup"))]
    NotASubgroup,
}

impl IntoAnyError for MlsError {
    #[cfg(feature = "std")]
    fn into_dyn_error(self) -> Result<Box<dyn std::error::Error + Send + Sync>, Self> {
        Ok(self.into())
    }
}

impl From<mls_rs_codec::Error> for MlsError {
    #[inline]
    fn from(e: mls_rs_codec::Error) -> Self {
        MlsError::SerializationError(e.into_any_error())
    }
}

impl From<ExtensionError> for MlsError {
    #[inline]
    fn from(e: ExtensionError) -> Self {
        MlsError::ExtensionError(e.into_any_error())
    }
}

/// MLS client used to create key packages and manage groups.
///
/// [`Client::builder`] can be used to instantiate it.
///
/// Clients are able to support multiple protocol versions, ciphersuites
/// and underlying identities used to join groups and generate key packages.
/// Applications may decide to create one or many clients depending on their
/// specific needs.
#[derive(Clone, Debug)]
pub struct Client<C> {
    pub(crate) config: C,
    pub(crate) signing_identity: Option<(SigningIdentity, CipherSuite)>,
    pub(crate) signer: Option<SignatureSecretKey>,
    pub(crate) version: ProtocolVersion,
}

impl Client<()> {
    /// Returns a [`ClientBuilder`]
    /// used to configure client preferences and providers.
    pub fn builder() -> ClientBuilder<BaseConfig> {
        ClientBuilder::new()
    }
}

impl<C> Client<C>
where
    C: ClientConfig + Clone,
{
    pub(crate) fn new(
        config: C,
        signer: Option<SignatureSecretKey>,
        signing_identity: Option<(SigningIdentity, CipherSuite)>,
        version: ProtocolVersion,
    ) -> Self {
        Client {
            config,
            signer,
            signing_identity,
            version,
        }
    }

    pub fn to_builder(&self, timestamp: Option<MlsTime>) -> ClientBuilder<MakeConfig<C>> {
        ClientBuilder::from_config(recreate_config(
            self.config.clone(),
            self.signer.clone(),
            self.signing_identity.clone(),
            self.version,
            timestamp,
        ))
    }

    /// Creates a new key package message that can be used to to add this
    /// client to a [Group](crate::group::Group). Each call to this function
    /// will produce a unique value that is signed by `signing_identity`.
    ///
    /// The secret keys for the resulting key package message will be stored in
    /// the [KeyPackageStorage](crate::KeyPackageStorage)
    /// that was used to configure the client and will
    /// automatically be erased when this key package is used to
    /// [join a group](Client::join_group).
    ///
    /// # Warning
    ///
    /// A key package message may only be used once.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn generate_key_package_message(
        &self,
        key_package_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
        timestamp: Option<MlsTime>,
    ) -> Result<MlsMessage, MlsError> {
        Ok(self
            .generate_key_package(key_package_extensions, leaf_node_extensions, timestamp)
            .await?
            .key_package_message())
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    async fn generate_key_package(
        &self,
        key_package_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
        timestamp: Option<MlsTime>,
    ) -> Result<KeyPackageGeneration, MlsError> {
        let (signing_identity, cipher_suite) = self.signing_identity()?;

        let cipher_suite_provider = self
            .config
            .crypto_provider()
            .cipher_suite_provider(cipher_suite)
            .ok_or(MlsError::UnsupportedCipherSuite(cipher_suite))?;

        let key_package_generator = KeyPackageGenerator {
            protocol_version: self.version,
            cipher_suite_provider: &cipher_suite_provider,
            signing_key: self.signer()?,
            signing_identity,
        };

        let key_pkg_gen = key_package_generator
            .generate(
                self.config.lifetime(timestamp),
                self.config.capabilities(),
                key_package_extensions,
                leaf_node_extensions,
            )
            .await?;

        key_pkg_gen
            .key_package
            .leaf_node
            .validate_no_default_values_listed()?;

        let (id, key_package_data) = key_pkg_gen.to_storage()?;

        self.config
            .key_package_repo()
            .insert(id, key_package_data)
            .await
            .map_err(|e| MlsError::KeyPackageRepoError(e.into_any_error()))?;

        Ok(key_pkg_gen)
    }

    /// Create a group with a specific group_id.
    ///
    /// This function behaves the same way as
    /// [create_group](Client::create_group) except that it
    /// specifies a specific unique group identifier to be used.
    ///
    /// # Warning
    ///
    /// It is recommended to use [create_group](Client::create_group)
    /// instead of this function because it guarantees that group_id values
    /// are globally unique.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn create_group_with_id(
        &self,
        group_id: Vec<u8>,
        group_context_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
        timestamp: Option<MlsTime>,
    ) -> Result<Group<C>, MlsError> {
        let (signing_identity, cipher_suite) = self.signing_identity()?;

        Group::new(
            self.config.clone(),
            Some(group_id),
            cipher_suite,
            self.version,
            signing_identity.clone(),
            group_context_extensions,
            leaf_node_extensions,
            self.signer()?.clone(),
            timestamp,
        )
        .await
    }

    /// Create a MLS group.
    ///
    /// The `cipher_suite` provided must be supported by the
    /// [CipherSuiteProvider](crate::CipherSuiteProvider)
    /// that was used to build the client.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn create_group(
        &self,
        group_context_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
        timestamp: Option<MlsTime>,
    ) -> Result<Group<C>, MlsError> {
        let (signing_identity, cipher_suite) = self.signing_identity()?;

        Group::new(
            self.config.clone(),
            None,
            cipher_suite,
            self.version,
            signing_identity.clone(),
            group_context_extensions,
            leaf_node_extensions,
            self.signer()?.clone(),
            timestamp,
        )
        .await
    }

    /// Join a MLS group via a welcome message created by a
    /// [Commit](crate::group::CommitOutput).
    ///
    /// `tree_data` is required to be provided out of band if the client that
    /// created `welcome_message` did not use the `ratchet_tree_extension`
    /// according to [`MlsRules::commit_options`](`crate::MlsRules::commit_options`).
    /// at the time the welcome message was created. `tree_data` can
    /// be exported from a group using the
    /// [export tree function](crate::group::Group::export_tree).
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn join_group(
        &self,
        tree_data: Option<ExportedTree<'_>>,
        welcome_message: &MlsMessage,
        maybe_time: Option<MlsTime>,
    ) -> Result<(Group<C>, NewMemberInfo), MlsError> {
        Group::join(
            welcome_message,
            tree_data,
            self.config.clone(),
            self.signer()?.clone(),
            maybe_time,
        )
        .await
    }

    /// Decrypt GroupInfo encrypted in the Welcome message without actually joining
    /// the group. The ratchet tree is not needed.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn examine_welcome_message(
        &self,
        welcome_message: &MlsMessage,
    ) -> Result<GroupInfo, MlsError> {
        Group::decrypt_group_info(welcome_message, &self.config).await
    }

    /// Validate GroupInfo message. This does NOT validate the ratchet tree in case
    /// it is provided in the extension. It validates the signature, identity of the
    /// signer, identities of external senders and cipher suite.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn validate_group_info(
        &self,
        group_info_message: &MlsMessage,
        signer: &SigningIdentity,
    ) -> Result<(), MlsError> {
        let MlsMessagePayload::GroupInfo(group_info) = &group_info_message.payload else {
            return Err(MlsError::UnexpectedMessageType);
        };

        let cs = cipher_suite_provider(
            self.config.crypto_provider(),
            group_info.group_context.cipher_suite,
        )?;

        let id = self.config.identity_provider();

        validate_group_info_joiner(group_info_message.version, group_info, signer, &id, &cs)
            .await?;

        let context = MemberValidationContext::ForNewGroup {
            current_context: &group_info.group_context,
        };

        id.validate_member(signer, None, context)
            .await
            .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

        Ok(())
    }

    /// 0-RTT add to an existing [group](crate::group::Group)
    ///
    /// External commits allow for immediate entry into a
    /// [group](crate::group::Group), even if all of the group members
    /// are currently offline and unable to process messages. Sending an
    /// external commit is only allowed for groups that have provided
    /// a public `group_info_message` containing an
    /// [ExternalPubExt](crate::extension::ExternalPubExt), which can be
    /// generated by an existing group member using the
    /// [group_info_message](crate::group::Group::group_info_message)
    /// function.
    ///
    /// `tree_data` may be provided following the same rules as [Client::join_group]
    ///
    /// If PSKs are provided in `external_psks`, the
    /// [PreSharedKeyStorage](crate::PreSharedKeyStorage)
    /// used to configure the client will be searched to resolve their values.
    ///
    /// `to_remove` may be used to remove an existing member provided that the
    /// identity of the existing group member at that [index](crate::group::Member::index)
    /// is a [valid successor](crate::IdentityProvider::valid_successor)
    /// of `signing_identity` as defined by the
    /// [IdentityProvider](crate::IdentityProvider) that this client
    /// was configured with.
    ///
    /// # Warning
    ///
    /// Only one external commit can be performed against a given group info.
    /// There may also be security trade-offs to this approach.
    ///
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn commit_external(
        &self,
        group_info_msg: MlsMessage,
    ) -> Result<(Group<C>, MlsMessage), MlsError> {
        ExternalCommitBuilder::new(
            self.signer()?.clone(),
            self.signing_identity()?.0.clone(),
            self.config.clone(),
        )
        .build(group_info_msg)
        .await
    }

    pub fn external_commit_builder(&self) -> Result<ExternalCommitBuilder<C>, MlsError> {
        Ok(ExternalCommitBuilder::new(
            self.signer()?.clone(),
            self.signing_identity()?.0.clone(),
            self.config.clone(),
        ))
    }

    /// Load an existing group state into this client using the
    /// [GroupStateStorage](crate::GroupStateStorage) that
    /// this client was configured to use.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    #[inline(never)]
    pub async fn load_group(&self, group_id: &[u8]) -> Result<Group<C>, MlsError> {
        let snapshot = self
            .config
            .group_state_storage()
            .state(group_id)
            .await
            .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))?
            .ok_or(MlsError::GroupNotFound)?;

        let snapshot = Snapshot::mls_decode(&mut &**snapshot)?;

        Group::from_snapshot(self.config.clone(), snapshot).await
    }

    /// Load an existing group state into this client using the
    /// [GroupStateStorage](crate::GroupStateStorage) that
    /// this client was configured to use. The tree is taken from
    /// `tree_data` instead of the stored state.
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    #[inline(never)]
    pub async fn load_group_with_ratchet_tree(
        &self,
        group_id: &[u8],
        tree_data: ExportedTree<'_>,
    ) -> Result<Group<C>, MlsError> {
        let snapshot = self
            .config
            .group_state_storage()
            .state(group_id)
            .await
            .map_err(|e| MlsError::GroupStorageError(e.into_any_error()))?
            .ok_or(MlsError::GroupNotFound)?;

        let mut snapshot = Snapshot::mls_decode(&mut &**snapshot)?;
        snapshot.state.public_tree.nodes = tree_data.0.into_owned();

        Group::from_snapshot(self.config.clone(), snapshot).await
    }

    /// Request to join an existing [group](crate::group::Group).
    ///
    /// An existing group member will need to perform a
    /// [commit](crate::Group::commit) to complete the add and the resulting
    /// welcome message can be used by [join_group](Client::join_group).
    #[cfg(feature = "by_ref_proposal")]
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn external_add_proposal(
        &self,
        group_info: &MlsMessage,
        tree_data: Option<crate::group::ExportedTree<'_>>,
        authenticated_data: Vec<u8>,
        key_package_extensions: ExtensionList,
        leaf_node_extensions: ExtensionList,
        timestamp: Option<MlsTime>,
    ) -> Result<MlsMessage, MlsError> {
        let protocol_version = group_info.version;

        let protocol_version_ok =
            self.config.version_supported(protocol_version) && protocol_version == self.version;

        if !protocol_version_ok {
            return Err(MlsError::UnsupportedProtocolVersion(protocol_version));
        }

        let group_info = group_info
            .as_group_info()
            .ok_or(MlsError::UnexpectedMessageType)?;

        let cipher_suite = group_info.group_context.cipher_suite;

        let cipher_suite_provider = self
            .config
            .crypto_provider()
            .cipher_suite_provider(cipher_suite)
            .ok_or(MlsError::UnsupportedCipherSuite(cipher_suite))?;

        crate::group::validate_tree_and_info_joiner(
            protocol_version,
            group_info,
            tree_data,
            &self.config.identity_provider(),
            &cipher_suite_provider,
            timestamp,
        )
        .await?;

        let key_package = self
            .generate_key_package(key_package_extensions, leaf_node_extensions, timestamp)
            .await?
            .key_package;

        (key_package.cipher_suite == cipher_suite)
            .then_some(())
            .ok_or(MlsError::UnsupportedCipherSuite(cipher_suite))?;

        let message = AuthenticatedContent::new_signed(
            &cipher_suite_provider,
            &group_info.group_context,
            Sender::NewMemberProposal,
            Content::Proposal(Box::new(Proposal::Add(Box::new(AddProposal {
                key_package,
            })))),
            self.signer()?,
            WireFormat::PublicMessage,
            authenticated_data,
        )
        .await?;

        let plaintext = PublicMessage {
            content: message.content,
            auth: message.auth,
            membership_tag: None,
        };

        Ok(MlsMessage {
            version: protocol_version,
            payload: MlsMessagePayload::Plain(plaintext),
        })
    }

    fn signer(&self) -> Result<&SignatureSecretKey, MlsError> {
        self.signer.as_ref().ok_or(MlsError::SignerNotFound)
    }

    pub fn signing_identity(&self) -> Result<(&SigningIdentity, CipherSuite), MlsError> {
        self.signing_identity
            .as_ref()
            .map(|(id, cs)| (id, *cs))
            .ok_or(MlsError::SignerNotFound)
    }

    /// The [KeyPackageStorage] that this client was configured to use.
    pub fn key_package_store(&self) -> <C as ClientConfig>::KeyPackageRepository {
        self.config.key_package_repo()
    }

    /// The [PreSharedKeyStorage](crate::PreSharedKeyStorage) that
    /// this client was configured to use.
    pub fn secret_store(&self) -> <C as ClientConfig>::PskStore {
        self.config.secret_store()
    }

    /// The [GroupStateStorage] that this client was configured to use.
    pub fn group_state_storage(&self) -> <C as ClientConfig>::GroupStateStorage {
        self.config.group_state_storage()
    }

    /// The [IdentityProvider](crate::IdentityProvider) that this client was configured to use.
    pub fn identity_provider(&self) -> <C as ClientConfig>::IdentityProvider {
        self.config.identity_provider()
    }
}
