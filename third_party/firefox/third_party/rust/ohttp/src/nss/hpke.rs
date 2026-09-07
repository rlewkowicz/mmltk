use std::{
    convert::TryFrom,
    ops::Deref,
    os::raw::c_uint,
    ptr::{addr_of_mut, null, null_mut},
};

use log::{log_enabled, trace};
pub use sys::{HpkeAeadId as AeadId, HpkeKdfId as KdfId, HpkeKemId as KemId};

use super::{
    super::hpke::{Aead, Kdf, Kem},
    err::{sec::SEC_ERROR_INVALID_ARGS, secstatus_to_res, Error},
    p11::{sys, Item, PrivateKey, PublicKey, Slot, SymKey},
};
use crate::{
    crypto::{Decrypt, Encrypt},
    err::Res,
};

/// Configuration for `Hpke`.
#[derive(Clone, Copy)]
pub struct Config {
    kem: Kem,
    kdf: Kdf,
    aead: Aead,
}

impl Config {
    pub fn new(kem: Kem, kdf: Kdf, aead: Aead) -> Self {
        Self { kem, kdf, aead }
    }

    pub fn kem(self) -> Kem {
        self.kem
    }

    pub fn kdf(self) -> Kdf {
        self.kdf
    }

    pub fn aead(self) -> Aead {
        self.aead
    }

    pub fn supported(self) -> bool {
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_ValidateParameters(
                KemId::Type::from(u16::from(self.kem)),
                KdfId::Type::from(u16::from(self.kdf)),
                AeadId::Type::from(u16::from(self.aead)),
            )
        })
        .is_ok()
    }
}

impl Default for Config {
    fn default() -> Self {
        Self {
            kem: Kem::X25519Sha256,
            kdf: Kdf::HkdfSha256,
            aead: Aead::Aes128Gcm,
        }
    }
}

pub trait Exporter {
    fn export(&self, info: &[u8], len: usize) -> Res<SymKey>;
}

unsafe fn destroy_hpke_context(cx: *mut sys::HpkeContext) {
    sys::PK11_HPKE_DestroyContext(cx, sys::PRBool::from(true));
}

scoped_ptr!(HpkeContext, sys::HpkeContext, destroy_hpke_context);

impl HpkeContext {
    fn new(config: Config) -> Res<Self> {
        let ptr = unsafe {
            sys::PK11_HPKE_NewContext(
                KemId::Type::from(u16::from(config.kem)),
                KdfId::Type::from(u16::from(config.kdf)),
                AeadId::Type::from(u16::from(config.aead)),
                null_mut(),
                null(),
            )
        };
        Self::from_ptr(ptr)
    }
}

unsafe impl Send for HpkeContext {}

impl Exporter for HpkeContext {
    fn export(&self, info: &[u8], len: usize) -> Res<SymKey> {
        let mut out: *mut sys::PK11SymKey = null_mut();
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_ExportSecret(
                self.ptr,
                &Item::wrap(info),
                c_uint::try_from(len).unwrap(),
                &raw mut out,
            )
        })?;
        SymKey::from_ptr(out)
    }
}

#[allow(clippy::module_name_repetitions)]
pub struct HpkeS {
    context: HpkeContext,
    config: Config,
}

impl HpkeS {
    /// Create a new context that uses the KEM mode for sending.
    #[allow(clippy::similar_names)]
    pub fn new(config: Config, pk_r: &PublicKey, info: &[u8]) -> Res<Self> {
        let (sk_e, pk_e) = generate_key_pair(config.kem)?;
        let context = HpkeContext::new(config)?;
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_SetupS(
                context.ptr(),
                pk_e.ptr(),
                sk_e.ptr(),
                pk_r.ptr(),
                &Item::wrap(info),
            )
        })?;
        Ok(Self { context, config })
    }

    pub fn config(&self) -> Config {
        self.config
    }

    /// Get the encapsulated KEM secret.
    pub fn enc(&self) -> Res<Vec<u8>> {
        let v = unsafe { sys::PK11_HPKE_GetEncapPubKey(self.context.ptr()) };
        let r = unsafe { v.as_ref() }.ok_or_else(|| Error::from(SEC_ERROR_INVALID_ARGS))?;
        let len = usize::try_from(r.len).unwrap();
        let slc = unsafe { std::slice::from_raw_parts(r.data, len) };
        Ok(Vec::from(slc))
    }
}

impl Encrypt for HpkeS {
    fn seal(&mut self, aad: &[u8], pt: &[u8]) -> Res<Vec<u8>> {
        let mut out: *mut sys::SECItem = null_mut();
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_Seal(
                self.context.ptr(),
                &Item::wrap(aad),
                &Item::wrap(pt),
                &raw mut out,
            )
        })?;
        let v = Item::from_ptr(out)?;
        Ok(unsafe { v.into_vec() })
    }

    fn alg(&self) -> Aead {
        self.config.aead()
    }
}

impl Exporter for HpkeS {
    fn export(&self, info: &[u8], len: usize) -> Res<SymKey> {
        self.context.export(info, len)
    }
}

impl Deref for HpkeS {
    type Target = Config;
    fn deref(&self) -> &Self::Target {
        &self.config
    }
}

#[allow(clippy::module_name_repetitions)]
pub struct HpkeR {
    context: HpkeContext,
    config: Config,
}

impl HpkeR {
    /// Create a new context that uses the KEM mode for sending.
    #[allow(clippy::similar_names)]
    pub fn new(
        config: Config,
        pk_r: &PublicKey,
        sk_r: &PrivateKey,
        enc: &[u8],
        info: &[u8],
    ) -> Res<Self> {
        let context = HpkeContext::new(config)?;
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_SetupR(
                context.ptr(),
                pk_r.ptr(),
                sk_r.ptr(),
                &Item::wrap(enc),
                &Item::wrap(info),
            )
        })?;
        Ok(Self { context, config })
    }

    pub fn config(&self) -> Config {
        self.config
    }

    pub fn decode_public_key(kem: Kem, k: &[u8]) -> Res<PublicKey> {
        let context = HpkeContext::new(Config {
            kem,
            ..Config::default()
        })?;
        let mut ptr: *mut sys::SECKEYPublicKey = null_mut();
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_Deserialize(
                context.ptr(),
                k.as_ptr(),
                c_uint::try_from(k.len()).unwrap(),
                &raw mut ptr,
            )
        })?;
        PublicKey::from_ptr(ptr)
    }
}

impl Decrypt for HpkeR {
    fn open(&mut self, aad: &[u8], ct: &[u8]) -> Res<Vec<u8>> {
        let mut out: *mut sys::SECItem = null_mut();
        secstatus_to_res(unsafe {
            sys::PK11_HPKE_Open(
                self.context.ptr(),
                &Item::wrap(aad),
                &Item::wrap(ct),
                &raw mut out,
            )
        })?;
        let v = Item::from_ptr(out)?;
        Ok(unsafe { v.into_vec() })
    }

    fn alg(&self) -> Aead {
        self.config.aead()
    }
}

impl Exporter for HpkeR {
    fn export(&self, info: &[u8], len: usize) -> Res<SymKey> {
        self.context.export(info, len)
    }
}

impl Deref for HpkeR {
    type Target = Config;
    fn deref(&self) -> &Self::Target {
        &self.config
    }
}

/// Generate a key pair for the identified KEM.
pub fn generate_key_pair(kem: Kem) -> Res<(PrivateKey, PublicKey)> {
    assert_eq!(kem, Kem::X25519Sha256);
    let slot = Slot::internal()?;

    let oid_data = unsafe { sys::SECOID_FindOIDByTag(sys::SECOidTag::SEC_OID_CURVE25519) };
    let oid = unsafe { oid_data.as_ref() }.ok_or_else(Error::internal)?;
    let oid_slc =
        unsafe { std::slice::from_raw_parts(oid.oid.data, usize::try_from(oid.oid.len).unwrap()) };
    let mut params: Vec<u8> = Vec::with_capacity(oid_slc.len() + 2);
    params.push(u8::try_from(sys::SEC_ASN1_OBJECT_ID).unwrap());
    params.push(u8::try_from(oid.oid.len).unwrap());
    params.extend_from_slice(oid_slc);

    let mut public_ptr: *mut sys::SECKEYPublicKey = null_mut();
    let mut wrapped = Item::wrap(&params);

    let insensitive_secret_ptr = if log_enabled!(log::Level::Trace) {
        unsafe {
            sys::PK11_GenerateKeyPairWithOpFlags(
                slot.ptr(),
                sys::CK_MECHANISM_TYPE::from(sys::CKM_EC_KEY_PAIR_GEN),
                addr_of_mut!(wrapped).cast(),
                &raw mut public_ptr,
                sys::PK11_ATTR_SESSION | sys::PK11_ATTR_INSENSITIVE | sys::PK11_ATTR_PUBLIC,
                sys::CK_FLAGS::from(sys::CKF_DERIVE),
                sys::CK_FLAGS::from(sys::CKF_DERIVE),
                null_mut(),
            )
        }
    } else {
        null_mut()
    };
    assert_eq!(insensitive_secret_ptr.is_null(), public_ptr.is_null());
    let secret_ptr = if insensitive_secret_ptr.is_null() {
        unsafe {
            sys::PK11_GenerateKeyPairWithOpFlags(
                slot.ptr(),
                sys::CK_MECHANISM_TYPE::from(sys::CKM_EC_KEY_PAIR_GEN),
                addr_of_mut!(wrapped).cast(),
                &raw mut public_ptr,
                sys::PK11_ATTR_SESSION | sys::PK11_ATTR_SENSITIVE | sys::PK11_ATTR_PRIVATE,
                sys::CK_FLAGS::from(sys::CKF_DERIVE),
                sys::CK_FLAGS::from(sys::CKF_DERIVE),
                null_mut(),
            )
        }
    } else {
        insensitive_secret_ptr
    };
    assert_eq!(secret_ptr.is_null(), public_ptr.is_null());
    let sk = PrivateKey::from_ptr(secret_ptr)?;
    let pk = PublicKey::from_ptr(public_ptr)?;
    trace!("Generated key pair: sk={sk:?} pk={pk:?}");
    Ok((sk, pk))
}
