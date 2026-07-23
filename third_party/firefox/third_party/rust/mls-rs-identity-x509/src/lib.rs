// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

#![cfg_attr(not(feature = "std"), no_std)]
extern crate alloc;

mod error;
mod identity_extractor;
mod provider;
mod traits;
mod util;

use alloc::vec::Vec;
use core::fmt::{self, Debug};

pub use error::*;
pub use identity_extractor::*;
pub use provider::*;
pub use traits::*;

pub use mls_rs_core::identity::{CertificateChain, DerCertificate};

#[cfg(any())]









wasm_bindgen_test::wasm_bindgen_test_configure!(run_in_browser);

#[derive(Clone, PartialEq, Eq)]
/// X.509 certificate request in DER format.
pub struct DerCertificateRequest(Vec<u8>);

impl Debug for DerCertificateRequest {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        mls_rs_core::debug::pretty_bytes(&self.0)
            .named("DerCertificateRequest")
            .fmt(f)
    }
}

impl DerCertificateRequest {
    /// Create a DER certificate request from raw bytes.
    pub fn new(data: Vec<u8>) -> DerCertificateRequest {
        DerCertificateRequest(data)
    }

    /// Convert this certificate request into raw bytes.
    pub fn into_vec(self) -> Vec<u8> {
        self.0
    }
}

impl From<Vec<u8>> for DerCertificateRequest {
    fn from(data: Vec<u8>) -> Self {
        DerCertificateRequest(data)
    }
}

impl AsRef<[u8]> for DerCertificateRequest {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}
