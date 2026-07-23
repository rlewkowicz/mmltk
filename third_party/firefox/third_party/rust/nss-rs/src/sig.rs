/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// the ISC license, reproduced below:

// Copyright 2015-2017 Brian Smith.

// copyright notice and this permission notice appear in all copies.

// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHORS DISCLAIM ALL WARRANTIES
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY

use nss_rs::ec::{Curve, PublicKey};
use nss_rs::HashAlgorithm;

/// A signature verification algorithm.
pub struct SignatureAlgorithm {
    curve_alg: Curve,
    digest_alg: HashAlgorithm,
}

pub static ECDSA_P256_SHA256: VerificationAlgorithm = VerificationAlgorithm {
    curve_alg: Curve::P256,
    digest_alg: HashAlgorithm::SHA256,
};

pub static ECDSA_P384_SHA384: VerificationAlgorithm = VerificationAlgorithm {
    curve_alg: Curve::P384,
    digest_alg: HashAlgorithm::SHA384,
};

/// An unparsed public key for signature operations.
pub struct Signature<'a> {
    alg: &'static VerificationAlgorithm,
    bytes: &'a [u8],
}

impl<'a> Signature<'a> {
    pub fn sign(algorithm: &'static VerificationAlgorithm, bytes: &'a [u8]) -> Self {
        Self {
            alg: algorithm,
            bytes,
        }
    }

    pub fn verify(&self, message: &[u8], signature: &[u8]) -> Result<()> {
        let pub_key = PublicKey::from_bytes(self.alg.curve, self.bytes)?;
        Ok(pub_key.verify(message, signature, self.alg.digest_alg)?)
    }

    pub fn algorithm(&self) -> &'static VerificationAlgorithm {
        self.alg
    }

    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }
}
