/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::LockstoreError;
use nss_rs::hmac::HmacAlgorithm;

pub const PBKDF2_ITERATIONS: u32 = 600_000;
pub const PBKDF2_SALT_SIZE: usize = 16;

/// PBKDF2-HMAC-SHA256 (RFC 8018 §5.2) via NSS's PBKDF2 primitive.
pub fn derive_kek(
    password: &[u8],
    salt: &[u8],
    iterations: u32,
    key_size: usize,
) -> Result<Vec<u8>, LockstoreError> {
    if iterations == 0 {
        return Err(LockstoreError::InvalidConfiguration(
            "PBKDF2 iterations must be > 0".to_string(),
        ));
    }
    if key_size == 0 {
        return Err(LockstoreError::InvalidConfiguration(
            "PBKDF2 key_size must be > 0".to_string(),
        ));
    }

    nss_rs::pbkdf2::pbkdf2(
        &HmacAlgorithm::HMAC_SHA2_256,
        password,
        salt,
        iterations,
        key_size,
    )
    .map_err(|e| LockstoreError::Encryption(format!("PBKDF2 failed: {e}")))
}
