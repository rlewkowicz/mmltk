// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::{util::credential_to_chain, CertificateChain, X509IdentityError};
use alloc::vec;
use alloc::vec::Vec;
use mls_rs_core::{
    crypto::SignaturePublicKey,
    error::IntoAnyError,
    extension::ExtensionList,
    identity::{CredentialType, IdentityProvider, MemberValidationContext, SigningIdentity},
    time::MlsTime,
};

#[cfg(not(feature = "std"))]
use alloc::boxed::Box;


/// X.509 certificate unique identity trait.
pub trait X509IdentityExtractor {
    type Error: IntoAnyError;

    /// Produce a unique identity value to represent the entity controlling a
    /// certificate credential within an MLS group.
    fn identity(&self, certificate_chain: &CertificateChain) -> Result<Vec<u8>, Self::Error>;

    /// Determine if `successor` is controlled by the same entity as
    /// `predecessor`.
    fn valid_successor(
        &self,
        predecessor: &CertificateChain,
        successor: &CertificateChain,
    ) -> Result<bool, Self::Error>;
}

/// X.509 certificate validation trait.
pub trait X509CredentialValidator {
    type Error: IntoAnyError;

    /// Validate a certificate chain.
    ///
    /// If `timestamp` is set to `None` then expiration checks should be skipped.
    fn validate_chain(
        &self,
        chain: &CertificateChain,
        timestamp: Option<MlsTime>,
    ) -> Result<SignaturePublicKey, Self::Error>;
}

#[derive(Clone, Debug)]
#[non_exhaustive]
/// A customizable generic X.509 certificate identity provider.
///
/// This provider forwards its individual [`IdentityProvider`]
/// behavior to its generic sub-components.
///
/// Only X509 credentials are supported by this provider.
pub struct X509IdentityProvider<IE, V> {
    pub identity_extractor: IE,
    pub validator: V,
}

impl<IE, V> X509IdentityProvider<IE, V>
where
    IE: X509IdentityExtractor,
    V: X509CredentialValidator,
{
    /// Create a new identity provider.
    pub fn new(identity_extractor: IE, validator: V) -> Self {
        Self {
            identity_extractor,
            validator,
        }
    }

    /// Determine if a certificate is valid based on the behavior of the
    /// underlying validator provided.
    fn validate(
        &self,
        signing_identity: &SigningIdentity,
        timestamp: Option<MlsTime>,
    ) -> Result<(), X509IdentityError> {
        let chain = credential_to_chain(&signing_identity.credential)?;

        let leaf_public_key = self
            .validator
            .validate_chain(&chain, timestamp)
            .map_err(|e| X509IdentityError::X509ValidationError(e.into_any_error()))?;

        if leaf_public_key != signing_identity.signature_key {
            return Err(X509IdentityError::SignatureKeyMismatch);
        }

        Ok(())
    }
}

#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl<IE, V> IdentityProvider for X509IdentityProvider<IE, V>
where
    IE: X509IdentityExtractor + Send + Sync,
    V: X509CredentialValidator + Send + Sync,
{
    type Error = X509IdentityError;

    /// Determine if a certificate is valid based on the behavior of the
    /// underlying validator provided.
    async fn validate_member(
        &self,
        signing_identity: &SigningIdentity,
        timestamp: Option<MlsTime>,
        _context: MemberValidationContext<'_>,
    ) -> Result<(), X509IdentityError> {
        self.validate(signing_identity, timestamp)
    }

    /// Produce a unique identity value to represent the entity controlling a
    /// certificate credential within an MLS group.
    async fn identity(
        &self,
        signing_id: &SigningIdentity,
        _extensions: &ExtensionList,
    ) -> Result<Vec<u8>, X509IdentityError> {
        self.identity_extractor
            .identity(&credential_to_chain(&signing_id.credential)?)
            .map_err(|e| X509IdentityError::IdentityExtractorError(e.into_any_error()))
    }

    /// Determine if `successor` is controlled by the same entity as
    /// `predecessor` based on the behavior of the underlying identity
    /// extractor provided.
    async fn valid_successor(
        &self,
        predecessor: &SigningIdentity,
        successor: &SigningIdentity,
        _extensions: &ExtensionList,
    ) -> Result<bool, X509IdentityError> {
        self.identity_extractor
            .valid_successor(
                &credential_to_chain(&predecessor.credential)?,
                &credential_to_chain(&successor.credential)?,
            )
            .map_err(|e| X509IdentityError::IdentityExtractorError(e.into_any_error()))
    }

    async fn validate_external_sender(
        &self,
        signing_identity: &SigningIdentity,
        timestamp: Option<MlsTime>,
        _extensions: Option<&ExtensionList>,
    ) -> Result<(), Self::Error> {
        self.validate(signing_identity, timestamp)
    }

    /// Supported credential types.
    ///
    /// Only [`CredentialType::X509`] is supported.
    fn supported_types(&self) -> Vec<CredentialType> {
        vec![CredentialType::X509]
    }
}
