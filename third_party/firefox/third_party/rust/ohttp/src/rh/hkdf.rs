#![allow(dead_code)] 

use hkdf::Hkdf as HkdfImpl;
use log::trace;
use sha2::{Sha256, Sha384, Sha512};

use super::SymKey;
use crate::{
    err::{Error, Res},
    hpke::{Aead, Kdf},
};

#[derive(Clone, Copy)]
pub enum KeyMechanism {
    Aead(Aead),
    #[allow(dead_code)] 
    Hkdf,
}

impl KeyMechanism {
    fn len(self) -> usize {
        match self {
            Self::Aead(a) => a.n_k(),
            Self::Hkdf => 0, 
        }
    }
}

pub enum Hkdf {
    Sha256,
    Sha384,
    Sha512,
}

impl Hkdf {
    pub fn new(kdf: Kdf) -> Self {
        match kdf {
            Kdf::HkdfSha256 => Self::Sha256,
            Kdf::HkdfSha384 => Self::Sha384,
            Kdf::HkdfSha512 => Self::Sha512,
        }
    }

#[cfg(any())]









    #[allow(clippy::unnecessary_wraps)]
    pub fn import_ikm(ikm: &[u8]) -> Res<SymKey> {
        Ok(SymKey::from(ikm))
    }

    #[allow(clippy::unnecessary_wraps)]
    pub fn extract(&self, salt: &[u8], ikm: &SymKey) -> Res<SymKey> {
        let prk = match self {
            Self::Sha256 => {
                SymKey::from(HkdfImpl::<Sha256>::extract(Some(salt), &ikm.0).0.as_slice())
            }
            Self::Sha384 => {
                SymKey::from(HkdfImpl::<Sha384>::extract(Some(salt), &ikm.0).0.as_slice())
            }
            Self::Sha512 => {
                SymKey::from(HkdfImpl::<Sha512>::extract(Some(salt), &ikm.0).0.as_slice())
            }
        };
        trace!(
            "HKDF extract: salt={} ikm={:?} prk={:?}",
            hex::encode(salt),
            ikm,
            prk
        );
        Ok(prk)
    }

    pub fn expand_key(&self, prk: &SymKey, info: &[u8], key_mech: KeyMechanism) -> Res<SymKey> {
        let okm = SymKey::from(self.expand_data(prk, info, key_mech.len())?);
        trace!(
            "HKDF expand_key: prk={:?} info={} okm={:?}",
            prk,
            hex::encode(info),
            okm,
        );
        Ok(okm)
    }

    pub fn expand_data(&self, prk: &SymKey, info: &[u8], len: usize) -> Res<Vec<u8>> {
        let mut okm = vec![0; len];
        match self {
            Self::Sha256 => {
                let h = HkdfImpl::<Sha256>::from_prk(&prk.0).map_err(|_| Error::Internal)?;
                h.expand(info, &mut okm).map_err(|_| Error::Internal)?;
            }
            Self::Sha384 => {
                let h = HkdfImpl::<Sha384>::from_prk(&prk.0).map_err(|_| Error::Internal)?;
                h.expand(info, &mut okm).map_err(|_| Error::Internal)?;
            }
            Self::Sha512 => {
                let h = HkdfImpl::<Sha512>::from_prk(&prk.0).map_err(|_| Error::Internal)?;
                h.expand(info, &mut okm).map_err(|_| Error::Internal)?;
            }
        }
        trace!(
            "HKDF expand_data: prk={:?} info={} len={} okm={:?}",
            prk,
            hex::encode(info),
            len,
            hex::encode(&okm),
        );
        Ok(okm)
    }
}
