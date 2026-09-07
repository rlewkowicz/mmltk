// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use crate::client::MlsError;
use crate::crypto::CipherSuiteProvider;
use crate::group::message_signature::{AuthenticatedContentTBS, FramedContentAuthData};
use crate::group::GroupContext;
use alloc::vec::Vec;
use core::{
    fmt::{self, Debug},
    ops::Deref,
};
use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};
use mls_rs_core::error::IntoAnyError;
use subtle::ConstantTimeEq;

use super::message_signature::AuthenticatedContent;

#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode)]
struct AuthenticatedContentTBM<'a> {
    content_tbs: AuthenticatedContentTBS<'a>,
    auth: &'a FramedContentAuthData,
}

impl<'a> AuthenticatedContentTBM<'a> {
    pub fn from_authenticated_content(
        auth_content: &'a AuthenticatedContent,
        group_context: &'a GroupContext,
    ) -> AuthenticatedContentTBM<'a> {
        AuthenticatedContentTBM {
            content_tbs: AuthenticatedContentTBS::from_authenticated_content(
                auth_content,
                Some(group_context),
                group_context.protocol_version,
            ),
            auth: &auth_content.auth,
        }
    }
}

#[derive(Clone, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub struct MembershipTag(#[mls_codec(with = "mls_rs_codec::byte_vec")] Vec<u8>);

impl PartialEq for MembershipTag {
    fn eq(&self, other: &Self) -> bool {
        self.0.ct_eq(&other.0).into()
    }
}

impl Debug for MembershipTag {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("MembershipTag")
            .fmt(f)
    }
}

impl Deref for MembershipTag {
    type Target = Vec<u8>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl From<Vec<u8>> for MembershipTag {
    fn from(m: Vec<u8>) -> Self {
        Self(m)
    }
}

impl MembershipTag {
    #[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
    pub(crate) async fn create<P: CipherSuiteProvider>(
        authenticated_content: &AuthenticatedContent,
        group_context: &GroupContext,
        membership_key: &[u8],
        cipher_suite_provider: &P,
    ) -> Result<Self, MlsError> {
        let plaintext_tbm = AuthenticatedContentTBM::from_authenticated_content(
            authenticated_content,
            group_context,
        );

        let serialized_tbm = plaintext_tbm.mls_encode_to_vec()?;

        let tag = cipher_suite_provider
            .mac(membership_key, &serialized_tbm)
            .await
            .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?;

        Ok(MembershipTag(tag))
    }
}
