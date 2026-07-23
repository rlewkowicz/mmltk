// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

use alloc::vec::Vec;

#[cfg(feature = "external_client")]
use alloc::vec;

use mls_rs_codec::{MlsDecode, MlsEncode, MlsSize};

#[cfg(feature = "external_client")]
use mls_rs_core::psk::PreSharedKeyStorage;

#[cfg(feature = "external_client")]
use core::convert::Infallible;
use core::fmt::{self, Debug};

#[cfg(feature = "psk")]
use crate::{client::MlsError, CipherSuiteProvider};

#[cfg(feature = "psk")]
use mls_rs_core::error::IntoAnyError;

#[cfg(feature = "psk")]
pub(crate) mod resolver;
pub(crate) mod secret;

pub use mls_rs_core::psk::{ExternalPskId, PreSharedKey};

#[derive(Clone, Debug, Eq, Hash, PartialEq, PartialOrd, Ord, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub(crate) struct PreSharedKeyID {
    pub key_id: JustPreSharedKeyID,
    pub psk_nonce: PskNonce,
}

impl PreSharedKeyID {
    #[cfg(feature = "psk")]
    pub(crate) fn new<P: CipherSuiteProvider>(
        key_id: JustPreSharedKeyID,
        cs: &P,
    ) -> Result<Self, MlsError> {
        Ok(Self {
            key_id,
            psk_nonce: PskNonce::random(cs)
                .map_err(|e| MlsError::CryptoProviderError(e.into_any_error()))?,
        })
    }
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialOrd, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(u8)]
pub(crate) enum JustPreSharedKeyID {
    External(ExternalPskId) = 1u8,
    Resumption(ResumptionPsk) = 2u8,
}

#[derive(Clone, Eq, Hash, Ord, PartialOrd, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub(crate) struct PskGroupId(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    pub Vec<u8>,
);

impl Debug for PskGroupId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("PskGroupId")
            .fmt(f)
    }
}

#[derive(Clone, Eq, Hash, PartialEq, PartialOrd, Ord, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub(crate) struct PskNonce(
    #[mls_codec(with = "mls_rs_codec::byte_vec")]
    #[cfg_attr(feature = "serde", serde(with = "mls_rs_core::vec_serde"))]
    pub Vec<u8>,
);

impl Debug for PskNonce {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("PskNonce")
            .fmt(f)
    }
}

#[cfg(feature = "psk")]
impl PskNonce {
    pub fn random<P: CipherSuiteProvider>(
        cipher_suite_provider: &P,
    ) -> Result<Self, <P as CipherSuiteProvider>::Error> {
        Ok(Self(cipher_suite_provider.random_bytes_vec(
            cipher_suite_provider.kdf_extract_size(),
        )?))
    }
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialOrd, PartialEq, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub(crate) struct ResumptionPsk {
    pub usage: ResumptionPSKUsage,
    pub psk_group_id: PskGroupId,
    pub psk_epoch: u64,
}

#[derive(Clone, Debug, Eq, Hash, PartialEq, Ord, PartialOrd, MlsSize, MlsEncode, MlsDecode)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(u8)]
pub(crate) enum ResumptionPSKUsage {
    Application = 1u8,
    Reinit = 2u8,
    Branch = 3u8,
}

#[cfg(feature = "psk")]
#[derive(Clone, Debug, PartialEq, MlsSize, MlsEncode)]
struct PSKLabel<'a> {
    id: &'a PreSharedKeyID,
    index: u16,
    count: u16,
}

#[cfg(feature = "external_client")]
#[derive(Clone, Copy, Debug)]
pub(crate) struct AlwaysFoundPskStorage;

#[cfg(feature = "external_client")]
#[cfg_attr(not(mls_build_async), maybe_async::must_be_sync)]
#[cfg_attr(mls_build_async, maybe_async::must_be_async)]
impl PreSharedKeyStorage for AlwaysFoundPskStorage {
    type Error = Infallible;

    async fn get(&self, _: &ExternalPskId) -> Result<Option<PreSharedKey>, Self::Error> {
        Ok(Some(vec![].into()))
    }
}
