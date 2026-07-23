// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use core::ops::Deref;

use super::*;
use crate::hash_reference::HashReference;

#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
/// Unique identifier for a proposal message.
pub struct ProposalRef(HashReference);

impl Deref for ProposalRef {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl ProposalRef {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn from_content<CS: CipherSuiteProvider>(
        cipher_suite_provider: &CS,
        content: &AuthenticatedContent,
    ) -> Result<Self, MlsError> {
        let bytes = &content.mls_encode_to_vec()?;

        Ok(ProposalRef(
            HashReference::compute(bytes, b"MLS 1.0 Proposal Reference", cipher_suite_provider)
                .await?,
        ))
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.0
    }
}
