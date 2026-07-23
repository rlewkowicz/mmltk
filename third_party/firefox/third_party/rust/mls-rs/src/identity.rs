// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

/// Basic credential identity provider.
pub mod basic;

/// X.509 certificate identity provider.
#[cfg(feature = "x509")]
pub mod x509 {
    pub use mls_rs_identity_x509::*;
}

pub use mls_rs_core::identity::{
    Credential, CredentialType, CustomCredential, MlsCredential, SigningIdentity,
};
