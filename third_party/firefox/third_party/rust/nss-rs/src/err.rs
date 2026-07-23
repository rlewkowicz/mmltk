// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{os::raw::c_char, str::Utf8Error};

use crate::nss_prelude::*;

include!(concat!(env!("OUT_DIR"), "/nspr_error.rs"));
#[expect(non_snake_case, dead_code, reason = "Code is bindgen-generated.")]
mod codes {
    include!(concat!(env!("OUT_DIR"), "/nss_secerr.rs"));
    include!(concat!(env!("OUT_DIR"), "/nss_sslerr.rs"));
}
pub use codes::{SECErrorCodes as sec, SSLErrorCodes as ssl};
use thiserror::Error;

#[expect(dead_code, reason = "Code is bindgen-generated.")]
pub mod nspr {
    include!(concat!(env!("OUT_DIR"), "/nspr_err.rs"));
}

#[expect(dead_code, reason = "Some constants are not used.")]
pub mod mozpkix {

    #[expect(non_camel_case_types, reason = "Code is bindgen-generated.")]
    pub type mozilla_pkix_ErrorCode = ::std::os::raw::c_int;
    pub const MOZILLA_PKIX_ERROR_KEY_PINNING_FAILURE: mozilla_pkix_ErrorCode = -16384;
    pub const MOZILLA_PKIX_ERROR_CA_CERT_USED_AS_END_ENTITY: mozilla_pkix_ErrorCode = -16383;
    pub const MOZILLA_PKIX_ERROR_INADEQUATE_KEY_SIZE: mozilla_pkix_ErrorCode = -16382;
    pub const MOZILLA_PKIX_ERROR_V1_CERT_USED_AS_CA: mozilla_pkix_ErrorCode = -16381;
    pub const MOZILLA_PKIX_ERROR_NO_RFC822NAME_MATCH: mozilla_pkix_ErrorCode = -16380;
    pub const MOZILLA_PKIX_ERROR_NOT_YET_VALID_CERTIFICATE: mozilla_pkix_ErrorCode = -16379;
    pub const MOZILLA_PKIX_ERROR_NOT_YET_VALID_ISSUER_CERTIFICATE: mozilla_pkix_ErrorCode = -16378;
    pub const MOZILLA_PKIX_ERROR_SIGNATURE_ALGORITHM_MISMATCH: mozilla_pkix_ErrorCode = -16377;
    pub const MOZILLA_PKIX_ERROR_OCSP_RESPONSE_FOR_CERT_MISSING: mozilla_pkix_ErrorCode = -16376;
    pub const MOZILLA_PKIX_ERROR_VALIDITY_TOO_LONG: mozilla_pkix_ErrorCode = -16375;
    pub const MOZILLA_PKIX_ERROR_REQUIRED_TLS_FEATURE_MISSING: mozilla_pkix_ErrorCode = -16374;
    pub const MOZILLA_PKIX_ERROR_INVALID_INTEGER_ENCODING: mozilla_pkix_ErrorCode = -16373;
    pub const MOZILLA_PKIX_ERROR_EMPTY_ISSUER_NAME: mozilla_pkix_ErrorCode = -16372;
    pub const MOZILLA_PKIX_ERROR_ADDITIONAL_POLICY_CONSTRAINT_FAILED: mozilla_pkix_ErrorCode =
        -16371;
    pub const MOZILLA_PKIX_ERROR_SELF_SIGNED_CERT: mozilla_pkix_ErrorCode = -16370;
    pub const MOZILLA_PKIX_ERROR_MITM_DETECTED: mozilla_pkix_ErrorCode = -16369;
    pub const END_OF_LIST: mozilla_pkix_ErrorCode = -16368;
}

pub type Res<T> = Result<T, Error>;

#[derive(Clone, Debug, PartialEq, PartialOrd, Ord, Eq, Error)]
pub enum Error {
    #[error("AEAD error")]
    Aead,
    #[error("Aead truncated")]
    AeadTruncated,
    #[error("Certificate decoding error")]
    CertificateDecoding,
    #[error("Certificate encoding error")]
    CertificateEncoding,
    #[error("Certificate loading error")]
    CertificateLoading,
    #[error("Cipher initialization error")]
    CipherInit,
    #[error("Failed to create SSL socket")]
    CreateSslSocket,
    #[error("ECH error, retry needed")]
    EchRetry(Vec<u8>),
    #[error("HKDF error")]
    Hkdf,
    #[error("Internal error")]
    Internal,
    #[error("Integer overflow")]
    IntegerOverflow,
    #[error("Invalid ALPN")]
    InvalidAlpn,
    #[error("Invalid epoch")]
    InvalidEpoch,
    #[error("Invalid certificate compression ID")]
    InvalidCertificateCompressionID,
    #[error("Invalid input")]
    InvalidInput,
    #[error("Invalid state for this operation")]
    InvalidState,
    #[error("Mixed handshake method")]
    MixedHandshakeMethod,
    #[error("No data available")]
    NoDataAvailable,
    #[error("NSS error: {name} ({code}): {desc}")]
    Nss {
        name: String,
        code: PRErrorCode,
        desc: String,
    },
    #[error("Self encryption error")]
    SelfEncrypt,
    #[error("String conversion error")]
    String,
    #[error("Time travel detected")]
    TimeTravel,
    #[error("Unsupported cipher")]
    UnsupportedCipher,
    #[error("Unsupported curve")]
    UnsupportedCurve,
    #[error("Unsupported hash")]
    UnsupportedHash,
    #[error("Unsupported version")]
    UnsupportedVersion,
}

impl Error {
    pub(crate) fn last_nss_error() -> Self {
        Self::from(unsafe { PR_GetError() })
    }
}

impl From<std::num::TryFromIntError> for Error {
    fn from(_: std::num::TryFromIntError) -> Self {
        Self::IntegerOverflow
    }
}
impl From<std::ffi::NulError> for Error {
    fn from(_: std::ffi::NulError) -> Self {
        Self::Internal
    }
}
impl From<Utf8Error> for Error {
    fn from(_: Utf8Error) -> Self {
        Self::String
    }
}
impl From<PRErrorCode> for Error {
    fn from(code: PRErrorCode) -> Self {
        let name = wrap_str_fn(|| unsafe { PR_ErrorToName(code) }, "UNKNOWN_ERROR");
        let desc = wrap_str_fn(
            || unsafe { PR_ErrorToString(code, PR_LANGUAGE_I_DEFAULT) },
            "...",
        );
        Self::Nss { name, code, desc }
    }
}

use std::ffi::CStr;

fn wrap_str_fn<F>(f: F, dflt: &str) -> String
where
    F: FnOnce() -> *const c_char,
{
    unsafe {
        let p = f();
        if p.is_null() {
            return dflt.to_string();
        }
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

pub const fn is_blocked(result: &Res<()>) -> bool {
    match result {
        Err(Error::Nss { code, .. }) => *code == nspr::PR_WOULD_BLOCK_ERROR,
        _ => false,
    }
}

pub trait IntoResult {
    /// The `Ok` type for the result.
    type Ok;

    /// Unsafe in our implementors because they take a pointer and have no way
    /// to ensure that the pointer is valid. An invalid pointer could cause UB
    /// in `impl Drop for Scoped`.
    fn into_result(self) -> Result<Self::Ok, Error>;
}

pub fn into_result<P>(ptr: *mut P) -> Result<*mut P, Error> {
    if ptr.is_null() {
        Err(Error::last_nss_error())
    } else {
        Ok(ptr)
    }
}

macro_rules! impl_into_result {
    ($pointer:ty) => {
        impl $crate::err::IntoResult for *mut $pointer {
            type Ok = *mut $pointer;

            fn into_result(self) -> Result<Self::Ok, $crate::err::Error> {
                $crate::err::into_result(self)
            }
        }
    };
}

impl IntoResult for SECStatus {
    type Ok = ();

    fn into_result(self) -> Result<(), Error> {
        if self == SECSuccess {
            Ok(())
        } else {
            Err(Error::last_nss_error())
        }
    }
}

pub fn secstatus_to_res(code: SECStatus) -> Res<()> {
    SECStatus::into_result(code)
}
