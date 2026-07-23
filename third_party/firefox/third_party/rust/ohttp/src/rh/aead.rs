use std::convert::TryFrom;

use aead::{AeadMut, Key, KeyInit, Nonce, Payload};
use aes_gcm::{Aes128Gcm, Aes256Gcm};
use chacha20poly1305::ChaCha20Poly1305;

use super::SymKey;
use crate::{
    crypto::{Decrypt, Encrypt},
    err::Res,
    hpke::Aead as AeadId,
};

/// All the nonces are the same length.  Exploit that.
pub const NONCE_LEN: usize = 12;
const COUNTER_LEN: usize = 8;

type SequenceNumber = u64;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Encrypt,
    Decrypt,
}

enum AeadEngine {
    Aes128Gcm(Box<Aes128Gcm>),
    Aes256Gcm(Box<Aes256Gcm>),
    ChaCha20Poly1305(Box<ChaCha20Poly1305>),
}

impl AeadEngine {
    fn encrypt(&mut self, nonce: &[u8], pt: Payload) -> Res<Vec<u8>> {
        let tag = match self {
            Self::Aes128Gcm(e) => e.encrypt(Nonce::<Aes128Gcm>::from_slice(nonce), pt)?,
            Self::Aes256Gcm(e) => e.encrypt(Nonce::<Aes256Gcm>::from_slice(nonce), pt)?,
            Self::ChaCha20Poly1305(e) => {
                e.encrypt(Nonce::<ChaCha20Poly1305>::from_slice(nonce), pt)?
            }
        };
        Ok(tag)
    }
    fn decrypt(&mut self, nonce: &[u8], pt: Payload) -> Res<Vec<u8>> {
        let tag = match self {
            Self::Aes128Gcm(e) => e.decrypt(Nonce::<Aes128Gcm>::from_slice(nonce), pt)?,
            Self::Aes256Gcm(e) => e.decrypt(Nonce::<Aes256Gcm>::from_slice(nonce), pt)?,
            Self::ChaCha20Poly1305(e) => {
                e.decrypt(Nonce::<ChaCha20Poly1305>::from_slice(nonce), pt)?
            }
        };
        Ok(tag)
    }
}

/// A switch-hitting AEAD that uses a selected primitive.
pub struct Aead {
    mode: Mode,
    #[allow(dead_code, reason = "Used by stream feature")]
    algorithm: AeadId,
    engine: AeadEngine,
    nonce_base: [u8; NONCE_LEN],
    seq: SequenceNumber,
}

impl Aead {
    #[allow(clippy::unnecessary_wraps)]
    pub fn new(
        mode: Mode,
        algorithm: AeadId,
        key: &SymKey,
        nonce_base: [u8; NONCE_LEN],
    ) -> Res<Self> {
        let aead = match algorithm {
            AeadId::Aes128Gcm => AeadEngine::Aes128Gcm(Box::new(Aes128Gcm::new(
                Key::<Aes128Gcm>::from_slice(key.as_ref()),
            ))),
            AeadId::Aes256Gcm => AeadEngine::Aes256Gcm(Box::new(Aes256Gcm::new(
                Key::<Aes256Gcm>::from_slice(key.as_ref()),
            ))),
            AeadId::ChaCha20Poly1305 => AeadEngine::ChaCha20Poly1305(Box::new(
                ChaCha20Poly1305::new(Key::<ChaCha20Poly1305>::from_slice(key.as_ref())),
            )),
        };
        Ok(Self {
            mode,
            algorithm,
            engine: aead,
            nonce_base,
            seq: 0,
        })
    }

#[cfg(any())]









    #[allow(clippy::unnecessary_wraps)]
    fn import_key(_alg: AeadId, k: &[u8]) -> Res<SymKey> {
        Ok(SymKey::from(k))
    }

    fn nonce(&self, seq: SequenceNumber) -> Vec<u8> {
        let mut nonce = Vec::from(self.nonce_base);
        for (i, n) in nonce.iter_mut().rev().take(COUNTER_LEN).enumerate() {
            *n ^= u8::try_from((seq >> (8 * i)) & 0xff).unwrap();
        }
        nonce
    }

    pub fn open_seq(&mut self, aad: &[u8], seq: SequenceNumber, ct: &[u8]) -> Res<Vec<u8>> {
        assert_eq!(self.mode, Mode::Decrypt);
        let nonce = self.nonce(seq);
        let pt = self.engine.decrypt(&nonce, Payload { msg: ct, aad })?;
        Ok(pt)
    }
}

impl Decrypt for Aead {
    fn open(&mut self, aad: &[u8], ct: &[u8]) -> Res<Vec<u8>> {
        let res = self.open_seq(aad, self.seq, ct);
        self.seq += 1;
        res
    }

    fn alg(&self) -> AeadId {
        self.algorithm
    }
}

impl Encrypt for Aead {
    fn seal(&mut self, aad: &[u8], pt: &[u8]) -> Res<Vec<u8>> {
        assert_eq!(self.mode, Mode::Encrypt);
        let nonce = self.nonce(self.seq);
        self.seq += 1;
        let ct = self.engine.encrypt(&nonce, Payload { msg: pt, aad })?;
        Ok(ct)
    }

    fn alg(&self) -> AeadId {
        self.algorithm
    }
}
