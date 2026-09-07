// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;
use mls_rs_core::{error::IntoAnyError, identity::CertificateChain};

use crate::{
    DerCertificate, SubjectComponent, X509CertificateReader, X509IdentityError,
    X509IdentityExtractor,
};

#[derive(Debug, Clone)]
/// A utility to determine unique identity for use with MLS by reading
/// the subject of a certificate.
///
/// The default behavior of this struct is to try and produce an identity
/// based on the common name component of the subject. If a common name
/// component is not found, then the byte value of the entire subject
/// is used as a fallback.
pub struct SubjectIdentityExtractor<R: X509CertificateReader> {
    offset: usize,
    reader: R,
}

impl<R> SubjectIdentityExtractor<R>
where
    R: X509CertificateReader,
{
    /// Create a new identity extractor.
    ///
    /// `offset` is used to determine which certificate in a [`CertificateChain`]
    /// should be used to evaluate identity. A value of 0 indicates to use the
    /// leaf (first value) of the chain.
    pub fn new(offset: usize, reader: R) -> Self {
        Self { offset, reader }
    }

    fn extract_common_name(
        &self,
        certificate: &DerCertificate,
    ) -> Result<Option<SubjectComponent>, X509IdentityError> {
        Ok(self
            .reader
            .subject_components(certificate)
            .map_err(|err| X509IdentityError::IdentityExtractorError(err.into_any_error()))?
            .iter()
            .find(|component| matches!(component, SubjectComponent::CommonName(_)))
            .cloned())
    }

    /// Get a unique identifier for a `certificate_chain`.
    pub fn identity(
        &self,
        certificate_chain: &CertificateChain,
    ) -> Result<Vec<u8>, X509IdentityError> {
        let cert = get_certificate(certificate_chain, self.offset)?;

        let common_name_value = self.extract_common_name(cert)?;

        if let Some(SubjectComponent::CommonName(common_name)) = common_name_value {
            return Ok(common_name.as_bytes().to_vec());
        }

        self.subject_bytes(cert)
    }

    fn subject_bytes(&self, certificate: &DerCertificate) -> Result<Vec<u8>, X509IdentityError> {
        self.reader
            .subject_bytes(certificate)
            .map_err(|e| X509IdentityError::X509ReaderError(e.into_any_error()))
    }

    /// Determine if `successor` resolves to the same
    /// identity value as `predecessor`, indicating that
    /// `predecessor` and `successor` are controlled by the same entity.
    pub fn valid_successor(
        &self,
        predecessor: &CertificateChain,
        successor: &CertificateChain,
    ) -> Result<bool, X509IdentityError> {
        let predecessor_cert = get_certificate(predecessor, 0)?;
        let successor_cert = get_certificate(successor, 0)?;

        let predecessor_common_name = self.extract_common_name(predecessor_cert)?;

        let successor_common_name = self.extract_common_name(successor_cert)?;

        if let (Some(pre_common_name), Some(succ_common_name)) =
            (predecessor_common_name, successor_common_name)
        {
            return Ok(pre_common_name == succ_common_name);
        }

        Ok(self.subject_bytes(predecessor_cert)? == self.subject_bytes(successor_cert)?)
    }
}

impl<R> X509IdentityExtractor for SubjectIdentityExtractor<R>
where
    R: X509CertificateReader,
{
    type Error = X509IdentityError;

    fn identity(&self, certificate_chain: &CertificateChain) -> Result<Vec<u8>, Self::Error> {
        self.identity(certificate_chain)
    }

    fn valid_successor(
        &self,
        predecessor: &CertificateChain,
        successor: &CertificateChain,
    ) -> Result<bool, Self::Error> {
        self.valid_successor(predecessor, successor)
    }
}

fn get_certificate(
    certificate_chain: &CertificateChain,
    offset: usize,
) -> Result<&DerCertificate, X509IdentityError> {
    certificate_chain
        .get(offset)
        .ok_or(X509IdentityError::InvalidOffset)
}
