// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use super::leaf_node::{LeafNode, LeafNodeSigningContext, LeafNodeSource};
use crate::client::MlsError;
use crate::CipherSuiteProvider;
use crate::{signer::Signable, time::MlsTime};
use mls_rs_core::identity::MemberValidationContext;
use mls_rs_core::{error::IntoAnyError, identity::IdentityProvider};

use crate::extension::RequiredCapabilitiesExt;

#[cfg(feature = "by_ref_proposal")]
use crate::extension::ExternalSendersExt;

pub enum ValidationContext<'a> {
    Add(Option<MlsTime>),
    Update((&'a [u8], u32, Option<MlsTime>)),
    Commit((&'a [u8], u32, Option<MlsTime>)),
}

impl ValidationContext<'_> {
    fn signing_context(&self) -> LeafNodeSigningContext<'_> {
        match *self {
            ValidationContext::Add(_) => Default::default(),
            ValidationContext::Update((group_id, leaf_index, _)) => (group_id, leaf_index).into(),
            ValidationContext::Commit((group_id, leaf_index, _)) => (group_id, leaf_index).into(),
        }
    }

    fn generation_time(&self) -> Option<MlsTime> {
        match *self {
            ValidationContext::Add(t) => t,
            ValidationContext::Update((_, _, t)) => t,
            ValidationContext::Commit((_, _, t)) => t,
        }
    }
}

#[derive(Clone, Debug)]
pub struct LeafNodeValidator<'a, C, CP>
where
    C: IdentityProvider,
    CP: CipherSuiteProvider,
{
    cipher_suite_provider: &'a CP,
    identity_provider: &'a C,
    context: MemberValidationContext<'a>,
}

impl<'a, C: IdentityProvider, CP: CipherSuiteProvider> LeafNodeValidator<'a, C, CP> {
    pub fn new(
        cipher_suite_provider: &'a CP,
        identity_provider: &'a C,
        context: MemberValidationContext<'a>,
    ) -> Self {
        Self {
            cipher_suite_provider,
            identity_provider,
            context,
        }
    }

    fn check_context(
        &self,
        leaf_node: &LeafNode,
        context: &ValidationContext,
    ) -> Result<(), MlsError> {
        match context {
            ValidationContext::Add(time) => {
                if let LeafNodeSource::KeyPackage(lifetime) = &leaf_node.leaf_node_source {
                    if let Some(current_time) = *time {
                        if !lifetime.within_lifetime(current_time) {
                            return Err(MlsError::InvalidLifetime {
                                not_before: lifetime.not_before,
                                not_after: lifetime.not_after,
                                timestamp: current_time,
                            });
                        }
                    }
                } else {
                    return Err(MlsError::InvalidLeafNodeSource);
                }
            }
            ValidationContext::Update(_) => {
                if !matches!(leaf_node.leaf_node_source, LeafNodeSource::Update) {
                    return Err(MlsError::InvalidLeafNodeSource);
                }
            }
            ValidationContext::Commit(_) => {
                if !matches!(leaf_node.leaf_node_source, LeafNodeSource::Commit(_)) {
                    return Err(MlsError::InvalidLeafNodeSource);
                }
            }
        }

        Ok(())
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub async fn revalidate(
        &self,
        leaf_node: &LeafNode,
        group_id: &[u8],
        leaf_index: u32,
        maybe_time: Option<MlsTime>,
    ) -> Result<(), MlsError> {
        let context = match leaf_node.leaf_node_source {
            LeafNodeSource::KeyPackage(_) => ValidationContext::Add(maybe_time),
            LeafNodeSource::Update => ValidationContext::Update((group_id, leaf_index, maybe_time)),
            LeafNodeSource::Commit(_) => {
                ValidationContext::Commit((group_id, leaf_index, maybe_time))
            }
        };

        self.check_if_valid(leaf_node, context).await
    }

    pub fn validate_required_capabilities(&self, leaf_node: &LeafNode) -> Result<(), MlsError> {
        let Some(required_capabilities) = self
            .context
            .new_extensions()
            .and_then(|ext| ext.get_as::<RequiredCapabilitiesExt>().transpose())
            .transpose()?
        else {
            return Ok(());
        };

        for extension in &required_capabilities.extensions {
            if !leaf_node.capabilities.extensions.contains(extension) {
                return Err(MlsError::RequiredExtensionNotFound(*extension));
            }
        }

        for proposal in &required_capabilities.proposals {
            if !leaf_node.capabilities.proposals.contains(proposal) {
                return Err(MlsError::RequiredProposalNotFound(*proposal));
            }
        }

        for credential in &required_capabilities.credentials {
            if !leaf_node.capabilities.credentials.contains(credential) {
                return Err(MlsError::RequiredCredentialNotFound(*credential));
            }
        }

        Ok(())
    }

    #[cfg(feature = "by_ref_proposal")]
    pub fn validate_external_senders_ext_credentials(
        &self,
        leaf_node: &LeafNode,
    ) -> Result<(), MlsError> {
        let Some(ext) = self
            .context
            .new_extensions()
            .and_then(|ext| ext.get_as::<ExternalSendersExt>().transpose())
            .transpose()?
        else {
            return Ok(());
        };

        ext.allowed_senders.iter().try_for_each(|sender| {
            let cred_type = sender.credential.credential_type();
            leaf_node
                .capabilities
                .credentials
                .contains(&cred_type)
                .then_some(())
                .ok_or(MlsError::RequiredCredentialNotFound(cred_type))
        })
    }

    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn check_if_valid(
        &self,
        leaf_node: &LeafNode,
        context: ValidationContext<'_>,
    ) -> Result<(), MlsError> {
        self.check_context(leaf_node, &context)?;

        self.identity_provider
            .validate_member(
                &leaf_node.signing_identity,
                context.generation_time(),
                self.context,
            )
            .await
            .map_err(|e| MlsError::IdentityProviderError(e.into_any_error()))?;

        leaf_node
            .verify(
                self.cipher_suite_provider,
                &leaf_node.signing_identity.signature_key,
                &context.signing_context(),
            )
            .await?;

        self.validate_required_capabilities(leaf_node)?;

        for one_ext in &*leaf_node.extensions {
            if !leaf_node
                .capabilities
                .extensions
                .contains(&one_ext.extension_type)
            {
                return Err(MlsError::ExtensionNotInCapabilities(one_ext.extension_type));
            }
        }

        if let Some(extensions) = self.context.new_extensions() {
            extensions
                .iter()
                .map(|ext| ext.extension_type)
                .find(|ext_type| {
                    !ext_type.is_default() && !leaf_node.capabilities.extensions.contains(ext_type)
                })
                .map(MlsError::UnsupportedGroupExtension)
                .map_or(Ok(()), Err)?;
        }

        leaf_node.validate_no_default_values_listed()?;

        #[cfg(feature = "by_ref_proposal")]
        self.validate_external_senders_ext_credentials(leaf_node)?;

        Ok(())
    }
}

impl LeafNode {
    pub fn validate_no_default_values_listed(&self) -> Result<(), MlsError> {
        self.capabilities
            .extensions
            .iter()
            .all(|ext| !ext.is_default())
            .then_some(())
            .ok_or(MlsError::DefaultValueListed)?;

        self.capabilities
            .proposals
            .iter()
            .all(|prop| !prop.is_default())
            .then_some(())
            .ok_or(MlsError::DefaultValueListed)?;

        Ok(())
    }
}
