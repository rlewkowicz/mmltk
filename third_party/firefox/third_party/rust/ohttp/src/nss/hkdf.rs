use std::{convert::TryFrom, os::raw::c_int, ptr::null_mut};

use log::trace;

use super::{
    super::hpke::{Aead, Kdf},
    p11::{
        sys::{
            self, CKA_DERIVE, CKF_HKDF_SALT_DATA, CKF_HKDF_SALT_NULL, CKM_AES_GCM,
            CKM_CHACHA20_POLY1305, CKM_HKDF_DATA, CKM_HKDF_DERIVE, CKM_SHA256, CK_BBOOL,
            CK_HKDF_PARAMS, CK_INVALID_HANDLE, CK_MECHANISM_TYPE, CK_OBJECT_HANDLE, CK_ULONG,
        },
        ParamItem, SymKey,
    },
};
use crate::err::Res;

#[derive(Clone, Copy)]
pub enum KeyMechanism {
    Aead(Aead),
    #[allow(dead_code)] 
    Hkdf,
}

impl KeyMechanism {
    fn mech(self) -> CK_MECHANISM_TYPE {
        CK_MECHANISM_TYPE::from(match self {
            Self::Aead(Aead::Aes128Gcm | Aead::Aes256Gcm) => CKM_AES_GCM,
            Self::Aead(Aead::ChaCha20Poly1305) => CKM_CHACHA20_POLY1305,
            Self::Hkdf => CKM_HKDF_DERIVE,
        })
    }

    fn len(self) -> usize {
        match self {
            Self::Aead(a) => a.n_k(),
            Self::Hkdf => 0, 
        }
    }
}

pub struct Hkdf {
    kdf: Kdf,
}

impl Hkdf {
    pub fn new(kdf: Kdf) -> Self {
        Self { kdf }
    }


    fn mech(&self) -> CK_MECHANISM_TYPE {
        CK_MECHANISM_TYPE::from(match self.kdf {
            Kdf::HkdfSha256 => CKM_SHA256,
            _ => unimplemented!(),
        })
    }

    pub fn extract(&self, salt: &[u8], ikm: &SymKey) -> Res<SymKey> {
        let salt_type = if salt.is_empty() {
            CKF_HKDF_SALT_NULL
        } else {
            CKF_HKDF_SALT_DATA
        };
        let mut params = CK_HKDF_PARAMS {
            bExtract: CK_BBOOL::from(true),
            bExpand: CK_BBOOL::from(false),
            prfHashMechanism: self.mech(),
            ulSaltType: CK_ULONG::from(salt_type),
            pSalt: salt.as_ptr().cast_mut(), 
            ulSaltLen: CK_ULONG::try_from(salt.len()).unwrap(),
            hSaltKey: CK_OBJECT_HANDLE::from(CK_INVALID_HANDLE),
            pInfo: null_mut(),
            ulInfoLen: 0,
        };
        let mut params_item = ParamItem::new(&mut params);
        let ptr = unsafe {
            sys::PK11_Derive(
                ikm.ptr(),
                CK_MECHANISM_TYPE::from(CKM_HKDF_DERIVE),
                params_item.ptr(),
                CK_MECHANISM_TYPE::from(CKM_HKDF_DERIVE),
                CK_MECHANISM_TYPE::from(CKA_DERIVE),
                0,
            )
        };

        let prk = SymKey::from_ptr(ptr)?;
        trace!(
            "HKDF extract: salt={} ikm={} prk={}",
            hex::encode(salt),
            hex::encode(ikm.key_data()?),
            hex::encode(prk.key_data()?),
        );
        Ok(prk)
    }

    fn expand_params(&self, info: &[u8]) -> CK_HKDF_PARAMS {
        CK_HKDF_PARAMS {
            bExtract: CK_BBOOL::from(false),
            bExpand: CK_BBOOL::from(true),
            prfHashMechanism: self.mech(),
            ulSaltType: CK_ULONG::from(CKF_HKDF_SALT_NULL),
            pSalt: null_mut(),
            ulSaltLen: 0,
            hSaltKey: CK_OBJECT_HANDLE::from(CK_INVALID_HANDLE),
            pInfo: info.as_ptr().cast_mut(), 
            ulInfoLen: CK_ULONG::try_from(info.len()).unwrap(),
        }
    }

    pub fn expand_key(&self, prk: &SymKey, info: &[u8], key_mech: KeyMechanism) -> Res<SymKey> {
        let mut params = self.expand_params(info);
        let mut params_item = ParamItem::new(&mut params);
        let ptr = unsafe {
            sys::PK11_Derive(
                prk.ptr(),
                CK_MECHANISM_TYPE::from(CKM_HKDF_DERIVE),
                params_item.ptr(),
                key_mech.mech(),
                CK_MECHANISM_TYPE::from(CKA_DERIVE),
                c_int::try_from(key_mech.len()).unwrap(),
            )
        };
        let okm = SymKey::from_ptr(ptr)?;
        trace!(
            "HKDF expand_key: prk={} info={} okm={}",
            hex::encode(prk.key_data()?),
            hex::encode(info),
            hex::encode(okm.key_data()?),
        );
        Ok(okm)
    }

    pub fn expand_data(&self, prk: &SymKey, info: &[u8], len: usize) -> Res<Vec<u8>> {
        let mut params = self.expand_params(info);
        let mut params_item = ParamItem::new(&mut params);
        let ptr = unsafe {
            sys::PK11_Derive(
                prk.ptr(),
                CK_MECHANISM_TYPE::from(CKM_HKDF_DATA),
                params_item.ptr(),
                CK_MECHANISM_TYPE::from(CKM_HKDF_DERIVE),
                CK_MECHANISM_TYPE::from(CKA_DERIVE),
                c_int::try_from(len).unwrap(),
            )
        };
        let k = SymKey::from_ptr(ptr)?;
        let r = Vec::from(k.key_data()?);
        trace!(
            "HKDF expand_data: prk={} info={} okm={}",
            hex::encode(prk.key_data()?),
            hex::encode(info),
            hex::encode(&r),
        );
        Ok(r)
    }
}
