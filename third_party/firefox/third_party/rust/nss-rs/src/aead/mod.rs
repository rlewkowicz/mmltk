// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#[cfg(not(feature = "disable-encryption"))]
use std::os::raw::c_char;
#[cfg(not(feature = "disable-encryption"))]
use std::ptr::null;
use std::{
    os::raw::{c_int, c_uint},
    ptr::null_mut,
};

#[cfg(feature = "disable-encryption")]
pub use recprot::AEAD_NULL_TAG;
pub use recprot::RecordProtection;

use crate::{
    Cipher, SECItemBorrowed, SymKey,
    constants::{TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256},
    err::{Error, Res, sec::SEC_ERROR_BAD_DATA},
    p11::{
        self, CK_ATTRIBUTE_TYPE, CK_GENERATOR_FUNCTION, CK_MECHANISM_TYPE, CKA_DECRYPT,
        CKA_ENCRYPT, CKA_NSS_MESSAGE, CKG_GENERATE_COUNTER_XOR, CKG_NO_GENERATE, CKM_AES_GCM,
        CKM_CHACHA20_POLY1305, Context, PK11_AEADOp, PK11_CreateContextBySymKey,
    },
    secstatus_to_res,
};
#[cfg(not(feature = "disable-encryption"))]
use crate::{
    Version,
    hp::SSL_HkdfExpandLabelWithMech,
    p11::{CKM_HKDF_DATA, PK11SymKey},
};

#[cfg(all(feature = "blapi", feature = "disable-encryption"))]
compile_error!("`blapi` and `disable-encryption` are mutually exclusive features");

/// Shared API contract for all `RecordProtection` backends.
///
/// Implemented by each cfg-selected `recprot*.rs` backend so that a
/// signature change in one backend is caught at compile time across all.
/// Import this trait to call AEAD methods on `RecordProtection`.
pub trait RecordProtectionOps {
    /// Get the expansion size (authentication tag length) for this AEAD.
    #[must_use]
    fn expansion(&self) -> usize;

    /// Encrypt plaintext with associated data.
    ///
    /// # Errors
    ///
    /// Returns `Error` when encryption fails.
    fn encrypt<'a>(
        &self,
        count: u64,
        aad: &[u8],
        input: &[u8],
        output: &'a mut [u8],
    ) -> Res<&'a [u8]>;

    /// Encrypt plaintext in place with associated data.
    ///
    /// # Errors
    ///
    /// Returns `Error` when encryption fails.
    fn encrypt_in_place(&self, count: u64, aad: &[u8], data: &mut [u8]) -> Res<usize>;

    /// Decrypt ciphertext with associated data.
    ///
    /// # Errors
    ///
    /// Returns `Error` when decryption or authentication fails.
    fn decrypt<'a>(
        &self,
        count: u64,
        aad: &[u8],
        input: &[u8],
        output: &'a mut [u8],
    ) -> Res<&'a [u8]>;

    /// Decrypt ciphertext in place with associated data.
    ///
    /// # Errors
    ///
    /// Returns `Error` when decryption or authentication fails.
    fn decrypt_in_place(&self, count: u64, aad: &[u8], data: &mut [u8]) -> Res<usize>;
}

#[cfg_attr(feature = "disable-encryption", path = "recprot_null.rs")]
#[cfg_attr(feature = "blapi", path = "recprot_blapi.rs")]
mod recprot;

#[cfg(not(feature = "disable-encryption"))]
fn expand_label(
    version: Version,
    cipher: Cipher,
    secret: &SymKey,
    label: &str,
    mech: CK_MECHANISM_TYPE,
    key_len: c_uint,
) -> Res<SymKey> {
    let mut ptr: *mut PK11SymKey = null_mut();
    unsafe {
        SSL_HkdfExpandLabelWithMech(
            version,
            cipher,
            **secret,
            null(),
            0,
            label.as_ptr().cast::<c_char>(),
            c_uint::try_from(label.len())?,
            mech,
            key_len,
            &raw mut ptr,
        )
    }?;
    SymKey::from_ptr(ptr)
}

#[cfg(not(feature = "disable-encryption"))]
fn expand_hkdf_label(
    version: Version,
    cipher: Cipher,
    secret: &SymKey,
    label: &str,
    key_len: c_uint,
) -> Res<SymKey> {
    expand_label(
        version,
        cipher,
        secret,
        label,
        CK_MECHANISM_TYPE::from(CKM_HKDF_DATA),
        key_len,
    )
}

/// Derive a fixed-size raw key buffer using HKDF-Data.  The const generic `N`
/// selects the output length, so callers get a `[u8; N]` directly with no
/// further `try_into` boilerplate.
#[cfg(not(feature = "disable-encryption"))]
pub(crate) fn expand_label_buf<const N: usize>(
    version: Version,
    cipher: Cipher,
    secret: &SymKey,
    label: &str,
) -> Res<[u8; N]> {
    let k = expand_hkdf_label(version, cipher, secret, label, c_uint::try_from(N)?)?;
    k.key_data()?.try_into().map_err(|_| Error::Internal)
}

/// All the nonces are the same length.  Exploit that.
pub const NONCE_LEN: usize = 12;

/// The portion of the nonce that is a counter.
const COUNTER_LEN: usize = size_of::<SequenceNumber>();

fn xor_nonce(base: &[u8; NONCE_LEN], count: SequenceNumber) -> [u8; NONCE_LEN] {
    let mut nonce = *base;
    for (n, &s) in nonce[NONCE_LEN - COUNTER_LEN..]
        .iter_mut()
        .zip(&count.to_be_bytes())
    {
        *n ^= s;
    }
    nonce
}

/// The NSS API insists on us identifying the tag separately, which is awful.
/// All of the AEAD functions here have a tag of this length, so use a fixed offset.
const TAG_LEN: usize = 16;

/// Split `data` into `(ct_len, tag)`, returning `SEC_ERROR_BAD_DATA` if it is
/// too short to contain a tag.
fn split_tag(data: &[u8]) -> Res<(usize, [u8; TAG_LEN])> {
    let ct_len = data
        .len()
        .checked_sub(TAG_LEN)
        .ok_or_else(|| Error::from(SEC_ERROR_BAD_DATA))?;
    let mut tag = [0u8; TAG_LEN];
    tag.copy_from_slice(&data[ct_len..]);
    Ok((ct_len, tag))
}

pub type SequenceNumber = u64;

/// All the lengths used by `PK11_AEADOp` are signed.  This converts to that.
fn c_int_len<T>(l: T) -> Res<c_int>
where
    T: TryInto<c_int>,
    T::Error: std::error::Error,
{
    l.try_into().map_err(|_| Error::IntegerOverflow)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Encrypt,
    Decrypt,
}

impl Mode {
    fn p11mode(self) -> CK_ATTRIBUTE_TYPE {
        CK_ATTRIBUTE_TYPE::from(
            CKA_NSS_MESSAGE
                | match self {
                    Self::Encrypt => CKA_ENCRYPT,
                    Self::Decrypt => CKA_DECRYPT,
                },
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AeadAlgorithms {
    Aes128Gcm,
    Aes256Gcm,
    ChaCha20Poly1305,
}

impl AeadAlgorithms {
    #[must_use]
    pub const fn key_len(self) -> c_uint {
        match self {
            Self::Aes128Gcm => 16,
            Self::Aes256Gcm | Self::ChaCha20Poly1305 => 32,
        }
    }

    #[must_use]
    pub fn p11_mech(self) -> CK_MECHANISM_TYPE {
        CK_MECHANISM_TYPE::from(match self {
            Self::Aes128Gcm | Self::Aes256Gcm => CKM_AES_GCM,
            Self::ChaCha20Poly1305 => CKM_CHACHA20_POLY1305,
        })
    }
}

impl TryFrom<Cipher> for AeadAlgorithms {
    type Error = Error;
    fn try_from(cipher: Cipher) -> Res<Self> {
        match cipher {
            TLS_AES_128_GCM_SHA256 => Ok(Self::Aes128Gcm),
            TLS_AES_256_GCM_SHA384 => Ok(Self::Aes256Gcm),
            TLS_CHACHA20_POLY1305_SHA256 => Ok(Self::ChaCha20Poly1305),
            _ => Err(Error::UnsupportedCipher),
        }
    }
}

pub struct Aead {
    mode: Mode,
    ctx: Context,
    nonce_base: [u8; NONCE_LEN],
}

impl Aead {
    pub fn import_key(algorithm: AeadAlgorithms, key: &[u8]) -> Result<SymKey, Error> {
        let slot = p11::Slot::internal().map_err(|_| Error::Internal)?;

        let key_item = SECItemBorrowed::wrap(key)?;
        let key_item_ptr = std::ptr::from_ref(key_item.as_ref()).cast_mut();

        let ptr = unsafe {
            p11::PK11_ImportSymKey(
                *slot,
                algorithm.p11_mech(),
                p11::PK11Origin::PK11_OriginUnwrap,
                CK_ATTRIBUTE_TYPE::from(CKA_ENCRYPT | CKA_DECRYPT),
                key_item_ptr,
                null_mut(),
            )
        };
        SymKey::from_ptr(ptr)
    }

    pub fn new(
        mode: Mode,
        algorithm: AeadAlgorithms,
        key: &SymKey,
        nonce_base: [u8; NONCE_LEN],
    ) -> Result<Self, Error> {
        crate::init()?;

        let ptr = unsafe {
            PK11_CreateContextBySymKey(
                algorithm.p11_mech(),
                mode.p11mode(),
                **key,
                SECItemBorrowed::wrap(&nonce_base[..])?.as_ref(),
            )
        };
        Ok(Self {
            mode,
            ctx: Context::from_ptr(ptr)?,
            nonce_base,
        })
    }

    pub fn encrypt(&mut self, aad: &[u8], pt: &[u8]) -> Result<Vec<u8>, Error> {
        crate::init()?;

        assert_eq!(self.mode, Mode::Encrypt);
        let mut nonce = self.nonce_base;
        let mut ct = vec![0; pt.len() + TAG_LEN];
        let mut ct_len: c_int = 0;
        let mut tag = vec![0; TAG_LEN];
        secstatus_to_res(unsafe {
            PK11_AEADOp(
                *self.ctx,
                CK_GENERATOR_FUNCTION::from(CKG_GENERATE_COUNTER_XOR),
                c_int_len(NONCE_LEN - COUNTER_LEN)?, 
                nonce.as_mut_ptr(),
                c_int_len(nonce.len())?,
                aad.as_ptr(),
                c_int_len(aad.len())?,
                ct.as_mut_ptr(),
                &raw mut ct_len,
                c_int_len(ct.len())?, 
                tag.as_mut_ptr(),
                c_int_len(tag.len())?,
                pt.as_ptr(),
                c_int_len(pt.len())?,
            )
        })?;
        ct.truncate(usize::try_from(ct_len).map_err(|_| Error::IntegerOverflow)?);
        debug_assert_eq!(ct.len(), pt.len());
        ct.append(&mut tag);
        Ok(ct)
    }

    /// Encrypt with an explicit sequence number. Mirrors `decrypt`'s nonce
    /// construction: the final nonce is `nonce_base XOR encode_be(seq)` over
    /// the trailing 8 bytes. The NSS PKCS#11 context's internal counter is
    /// not used (`CKG_NO_GENERATE`). The caller must never reuse
    /// `(nonce_base, seq)` with the same key.
    pub fn encrypt_with_seq(
        &mut self,
        aad: &[u8],
        seq: SequenceNumber,
        pt: &[u8],
    ) -> Result<Vec<u8>, Error> {
        crate::init()?;

        assert_eq!(self.mode, Mode::Encrypt);
        let mut nonce = xor_nonce(&self.nonce_base, seq);
        let mut ct = vec![0; pt.len() + TAG_LEN];
        let mut ct_len: c_int = 0;
        let mut tag = vec![0; TAG_LEN];
        secstatus_to_res(unsafe {
            PK11_AEADOp(
                *self.ctx,
                CK_GENERATOR_FUNCTION::from(CKG_NO_GENERATE),
                c_int_len(NONCE_LEN - COUNTER_LEN)?,
                nonce.as_mut_ptr(),
                c_int_len(nonce.len())?,
                aad.as_ptr(),
                c_int_len(aad.len())?,
                ct.as_mut_ptr(),
                &raw mut ct_len,
                c_int_len(ct.len())?,
                tag.as_mut_ptr(),
                c_int_len(tag.len())?,
                pt.as_ptr(),
                c_int_len(pt.len())?,
            )
        })?;
        ct.truncate(usize::try_from(ct_len).map_err(|_| Error::IntegerOverflow)?);
        debug_assert_eq!(ct.len(), pt.len());
        ct.append(&mut tag);
        Ok(ct)
    }

    pub fn decrypt(
        &mut self,
        aad: &[u8],
        seq: SequenceNumber,
        ct: &[u8],
    ) -> Result<Vec<u8>, Error> {
        crate::init()?;

        assert_eq!(self.mode, Mode::Decrypt);
        let mut nonce = xor_nonce(&self.nonce_base, seq);
        let mut pt = vec![0; ct.len()]; 
        let mut pt_len: c_int = 0;
        let pt_expected = ct.len().checked_sub(TAG_LEN).ok_or(Error::AeadTruncated)?;
        secstatus_to_res(unsafe {
            PK11_AEADOp(
                *self.ctx,
                CK_GENERATOR_FUNCTION::from(CKG_NO_GENERATE),
                c_int_len(NONCE_LEN - COUNTER_LEN)?, 
                nonce.as_mut_ptr(),
                c_int_len(nonce.len())?,
                aad.as_ptr(),
                c_int_len(aad.len())?,
                pt.as_mut_ptr(),
                &raw mut pt_len,
                c_int_len(pt.len())?,
                ct.as_ptr().add(pt_expected).cast_mut(),
                c_int_len(TAG_LEN)?,
                ct.as_ptr(),
                c_int_len(pt_expected)?,
            )
        })?;
        let len = usize::try_from(pt_len).map_err(|_| Error::IntegerOverflow)?;
        debug_assert_eq!(len, pt_expected);
        pt.truncate(len);
        Ok(pt)
    }
}
