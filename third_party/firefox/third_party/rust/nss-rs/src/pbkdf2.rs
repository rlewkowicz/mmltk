// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{os::raw::c_int, ptr::null_mut};

use crate::{
    Error, SECItemBorrowed,
    hmac::{HmacAlgorithm, hmac_alg_to_prf_oid},
    p11::{
        PK11_CreatePBEV2AlgorithmID, PK11_PBEKeyGen, PRBool, SECOID_DestroyAlgorithmID, SECOidTag,
        Slot, SymKey,
    },
};

/// Derive a key using PBKDF2.
///
/// Returns the derived key bytes as a `Vec<u8>`.
///
/// # Errors
///
/// Returns an error if inputs have invalid lengths, or if NSS functions fail.
pub fn pbkdf2(
    alg: &HmacAlgorithm,
    password: &[u8],
    salt: &[u8],
    iterations: u32,
    key_len: usize,
) -> Result<Vec<u8>, Error> {
    crate::init()?;

    let iterations = c_int::try_from(iterations)?;
    let key_len_int = c_int::try_from(key_len)?;

    let mut salt_item = SECItemBorrowed::wrap(salt)?;

    let slot = Slot::internal()?;
    let mut pw_item = SECItemBorrowed::wrap(password)?;

    let algid = unsafe {
        PK11_CreatePBEV2AlgorithmID(
            SECOidTag::SEC_OID_PKCS5_PBKDF2,
            hmac_alg_to_prf_oid(alg),
            hmac_alg_to_prf_oid(alg),
            key_len_int,
            iterations,
            salt_item.as_mut(),
        )
    };
    if algid.is_null() {
        return Err(Error::last_nss_error());
    }

    let key_ptr = unsafe {
        PK11_PBEKeyGen(
            *slot,
            algid,
            pw_item.as_mut(),
            PRBool::from(false),
            null_mut(),
        )
    };
    unsafe {
        SECOID_DestroyAlgorithmID(algid, PRBool::from(true));
    }

    let key = SymKey::from_ptr(key_ptr)?;
    let data = key.key_data()?;
    Ok(Vec::from(data))
}
