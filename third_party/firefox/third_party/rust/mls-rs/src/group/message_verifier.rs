// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

#[cfg(feature = "by_ref_proposal")]
use alloc::{vec, vec::Vec};

use crate::{
    client::MlsError,
    crypto::SignaturePublicKey,
    group::{GroupContext, MembershipTag, PublicMessage, Sender},
    signer::Signable,
    tree_kem::{node::LeafIndex, TreeKemPublic},
    CipherSuiteProvider,
};

#[cfg(feature = "by_ref_proposal")]
use crate::{extension::ExternalSendersExt, identity::SigningIdentity};

use super::message_signature::{AuthenticatedContent, MessageSigningContext};

#[cfg(feature = "by_ref_proposal")]
use super::proposal::Proposal;

#[derive(Debug)]
pub(crate) enum SignaturePublicKeysContainer<'a> {
    RatchetTree(&'a TreeKemPublic),
    #[cfg(feature = "private_message")]
    List(&'a [Option<SignaturePublicKey>]),
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn verify_plaintext_authentication<P: CipherSuiteProvider>(
    cipher_suite_provider: &P,
    plaintext: PublicMessage,
    membership_key: Option<&[u8]>,
    context: &GroupContext,
    signature_keys_container: SignaturePublicKeysContainer<'_>,
) -> Result<AuthenticatedContent, MlsError> {
    let tag = plaintext.membership_tag.clone();
    let auth_content = AuthenticatedContent::from(plaintext);

    #[cfg(feature = "by_ref_proposal")]
    let external_signers = external_signers(context);

    match &auth_content.content.sender {
        Sender::Member(_) => {
            if let Some(membership_key) = membership_key {
                let expected_tag = MembershipTag::create(
                    &auth_content,
                    context,
                    membership_key,
                    cipher_suite_provider,
                )
                .await?;

                let plaintext_tag = tag.as_ref().ok_or(MlsError::InvalidMembershipTag)?;

                if &expected_tag != plaintext_tag {
                    return Err(MlsError::InvalidMembershipTag);
                }
            }
        }
        _ => {
            tag.is_none()
                .then_some(())
                .ok_or(MlsError::MembershipTagForNonMember)?;
        }
    }

    verify_auth_content_signature(
        cipher_suite_provider,
        signature_keys_container,
        context,
        &auth_content,
        #[cfg(feature = "by_ref_proposal")]
        &external_signers,
    )
    .await?;

    Ok(auth_content)
}

#[cfg(feature = "by_ref_proposal")]
fn external_signers(context: &GroupContext) -> Vec<SigningIdentity> {
    context
        .extensions
        .get_as::<ExternalSendersExt>()
        .unwrap_or(None)
        .map_or(vec![], |extern_senders_ext| {
            extern_senders_ext.allowed_senders
        })
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
pub(crate) async fn verify_auth_content_signature<P: CipherSuiteProvider>(
    cipher_suite_provider: &P,
    signature_keys_container: SignaturePublicKeysContainer<'_>,
    context: &GroupContext,
    auth_content: &AuthenticatedContent,
    #[cfg(feature = "by_ref_proposal")] external_signers: &[SigningIdentity],
) -> Result<(), MlsError> {
    let sender_public_key = signing_identity_for_sender(
        signature_keys_container,
        &auth_content.content.sender,
        &auth_content.content.content,
        #[cfg(feature = "by_ref_proposal")]
        external_signers,
    )?;

    let context = MessageSigningContext {
        group_context: Some(context),
        protocol_version: context.protocol_version,
    };

    auth_content
        .verify(cipher_suite_provider, &sender_public_key, &context)
        .await?;

    Ok(())
}

fn signing_identity_for_sender(
    signature_keys_container: SignaturePublicKeysContainer,
    sender: &Sender,
    content: &super::framing::Content,
    #[cfg(feature = "by_ref_proposal")] external_signers: &[SigningIdentity],
) -> Result<SignaturePublicKey, MlsError> {
    match sender {
        Sender::Member(leaf_index) => {
            signing_identity_for_member(signature_keys_container, LeafIndex::try_from(*leaf_index)?)
        }
        #[cfg(feature = "by_ref_proposal")]
        Sender::External(external_key_index) => {
            signing_identity_for_external(*external_key_index, external_signers)
        }
        Sender::NewMemberCommit => signing_identity_for_new_member_commit(content),
        #[cfg(feature = "by_ref_proposal")]
        Sender::NewMemberProposal => signing_identity_for_new_member_proposal(content),
    }
}

fn signing_identity_for_member(
    signature_keys_container: SignaturePublicKeysContainer,
    leaf_index: LeafIndex,
) -> Result<SignaturePublicKey, MlsError> {
    match signature_keys_container {
        SignaturePublicKeysContainer::RatchetTree(tree) => Ok(tree
            .get_leaf_node(leaf_index)?
            .signing_identity
            .signature_key
            .clone()), 
        #[cfg(feature = "private_message")]
        SignaturePublicKeysContainer::List(list) => list
            .get(*leaf_index as usize)
            .cloned()
            .flatten()
            .ok_or(MlsError::LeafNotFound(*leaf_index)),
    }
}

#[cfg(feature = "by_ref_proposal")]
fn signing_identity_for_external(
    index: u32,
    external_signers: &[SigningIdentity],
) -> Result<SignaturePublicKey, MlsError> {
    external_signers
        .get(index as usize)
        .map(|spk| spk.signature_key.clone())
        .ok_or(MlsError::UnknownSigningIdentityForExternalSender)
}

fn signing_identity_for_new_member_commit(
    content: &super::framing::Content,
) -> Result<SignaturePublicKey, MlsError> {
    match content {
        super::framing::Content::Commit(commit) => {
            if let Some(path) = &commit.path {
                Ok(path.leaf_node.signing_identity.signature_key.clone())
            } else {
                Err(MlsError::CommitMissingPath)
            }
        }
        #[cfg(any(feature = "private_message", feature = "by_ref_proposal"))]
        _ => Err(MlsError::ExpectedCommitForNewMemberCommit),
    }
}

#[cfg(feature = "by_ref_proposal")]
fn signing_identity_for_new_member_proposal(
    content: &super::framing::Content,
) -> Result<SignaturePublicKey, MlsError> {
    match content {
        super::framing::Content::Proposal(proposal) => {
            if let Proposal::Add(p) = proposal.as_ref() {
                Ok(p.key_package
                    .leaf_node
                    .signing_identity
                    .signature_key
                    .clone())
            } else {
                Err(MlsError::ExpectedAddProposalForNewMemberProposal)
            }
        }
        _ => Err(MlsError::ExpectedAddProposalForNewMemberProposal),
    }
}
