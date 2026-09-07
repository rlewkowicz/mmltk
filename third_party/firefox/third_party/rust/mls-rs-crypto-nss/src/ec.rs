// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// Copyright by contributors to this project.
// SPDX-License-Identifier: (Apache-2.0 OR MIT)

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

use nss_rs::{PrivateKey, PublicKey};

use alloc::vec::Vec;
use mls_rs_crypto_traits::Curve;

#[cfg(feature = "std")]
use std::array::TryFromSliceError;

#[cfg(not(feature = "std"))]
use core::array::TryFromSliceError;
use core::fmt::{self, Debug};

use crate::Hash;

#[derive(Debug, Clone)]
pub enum EcPublicKey {
    X25519(nss_rs::PublicKey),
    Ed25519(nss_rs::PublicKey),
    P256(nss_rs::PublicKey),
}

#[derive(Clone)]
pub enum EcPrivateKey {
    X25519(nss_rs::PrivateKey),
    Ed25519(nss_rs::PrivateKey),
    P256(nss_rs::PrivateKey),
}

#[derive(Debug)]
#[cfg_attr(feature = "std", derive(thiserror::Error))]
pub enum EcError {
    #[cfg_attr(feature = "std", error("unsupported curve type"))]
    UnsupportedCurve,
    #[cfg_attr(feature = "std", error("invalid public key data"))]
    EcKeyInvalidKeyData,
    #[cfg_attr(feature = "std", error("ec key is not a signature key"))]
    EcKeyNotSignature,
    #[cfg_attr(feature = "std", error(transparent))]
    TryFromSliceError(TryFromSliceError),
    #[cfg_attr(feature = "std", error("rand error: {0:?}"))]
    RandCoreError(rand_core::Error),
    #[cfg_attr(feature = "std", error("ecdh key type mismatch"))]
    EcdhKeyTypeMismatch,
    #[cfg_attr(feature = "std", error("ec key is not an ecdh key"))]
    EcKeyNotEcdh,
    #[cfg_attr(feature = "std", error("general nss failure"))]
    GeneralFailure,
}

pub const DER_SEQUENCE: u8 = 0x30;
pub const DER_INTEGER: u8 = 0x02;
pub const DER_BITSTRING: u8 = 0x03;
pub const DER_OCTETSTRING: u8 = 0x04;

impl From<rand_core::Error> for EcError {
    fn from(value: rand_core::Error) -> Self {
        EcError::RandCoreError(value)
    }
}

impl From<TryFromSliceError> for EcError {
    fn from(e: TryFromSliceError) -> Self {
        EcError::TryFromSliceError(e)
    }
}

impl core::fmt::Debug for EcPrivateKey {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::X25519(_) => f.write_str("X25519 Secret Key"),
            Self::Ed25519(_) => f.write_str("Ed25519 Secret Key"),
            Self::P256(_) => f.write_str("P256 Secret Key"),
        }
    }
}

fn private_key_len(curve: Curve) -> usize {
    match curve {
        Curve::P256 => 0x20 as usize,
        Curve::Ed25519 | Curve::X25519 => 0x20 as usize,
        _ => 0 as usize,
    }
}

fn public_key_len(curve: Curve) -> usize {
    match curve {
        Curve::P256 => 0x41 as usize,
        Curve::Ed25519 | Curve::X25519 => 32 as usize,
        _ => 0 as usize,
    }
}

fn max_size_ecdsa_part(curve: Curve) -> Result<usize, EcError> {
    match curve {
        Curve::P256 => return Ok(0x20),
        _ => return Err(EcError::EcKeyInvalidKeyData),
    }
}

fn build_spki_from_raw_public_key(key: Vec<u8>, curve: Curve) -> Result<Vec<u8>, EcError> {
    let mut lh = {
        match curve {
            Curve::P256 => vec![
                DER_SEQUENCE,
                0x59, 
                DER_SEQUENCE,
                0x13, 
                0x06,
                0x07,
                0x2a,
                0x86,
                0x48,
                0xce,
                0x3d,
                0x02,
                0x01,
                0x06, 
                0x08,
                0x2a,
                0x86,
                0x48,
                0xce,
                0x3d,
                0x03,
                0x01,
                0x07,
                DER_BITSTRING,
                0x42, 
                0x00,
            ],
            Curve::Ed25519 => vec![
                DER_SEQUENCE,
                0x2a, 
                DER_SEQUENCE,
                0x5, 
                0x6,
                0x3,
                0x2b,
                0x65,
                0x70, 
                DER_BITSTRING,
                0x21,
                0x0,
            ],
            Curve::X25519 => vec![
                DER_SEQUENCE,
                0x2a, 
                DER_SEQUENCE,
                0x5, 
                0x6,
                0x3,
                0x2b,
                0x65,
                0x6e, 
                DER_BITSTRING,
                0x21, 
                0x0,
            ],
            _ => return Err(EcError::UnsupportedCurve),
        }
    };

    let mut key = key.clone();

    if public_key_len(curve) == 0 {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    if key.len() > public_key_len(curve) {
        if key[0] != DER_OCTETSTRING
            && key[1] as usize != public_key_len(curve)
            && key.len() != public_key_len(curve) + 2
        {
            return Err(EcError::EcKeyInvalidKeyData);
        } else {
            let (_, key) = key.split_at(2);
            return build_spki_from_raw_public_key(key.to_vec(), curve);
        }
    }

    if key.len() < public_key_len(curve) {
        let mut zeros = vec![0 as u8; public_key_len(curve) - key.len()];
        zeros.append(&mut key);
        lh.append(&mut zeros);
    } else {
        lh.append(&mut key);
    }

    Ok(lh)
}

pub fn pub_key_from_uncompressed(bytes: Vec<u8>, curve: Curve) -> Result<EcPublicKey, EcError> {
    let z = build_spki_from_raw_public_key(bytes, curve).unwrap();
    match curve {
        Curve::P256 => match nss_rs::ec::import_ec_public_key_from_spki(&z) {
            Ok(key) => return Ok(EcPublicKey::P256(key)),
            Err(_) => return Err(EcError::EcKeyInvalidKeyData),
        },
        Curve::Ed25519 => match nss_rs::ec::import_ec_public_key_from_spki(&z) {
            Ok(key) => return Ok(EcPublicKey::Ed25519(key)),
            Err(_) => return Err(EcError::EcKeyInvalidKeyData),
        },
        Curve::X25519 => match nss_rs::ec::import_ec_public_key_from_spki(&z) {
            Ok(key) => return Ok(EcPublicKey::X25519(key)),
            Err(_) => return Err(EcError::EcKeyInvalidKeyData),
        },
        _ => Err(EcError::UnsupportedCurve),
    }
}

pub fn pub_key_to_uncompressed(key: EcPublicKey) -> Result<Vec<u8>, EcError> {
    match key {
        EcPublicKey::Ed25519(key) | EcPublicKey::X25519(key) => {
            let k0 = key.key_data_alt().unwrap();
            Ok(k0.to_vec())
        }

        EcPublicKey::P256(key) => {
            let k0 = key.key_data_alt().unwrap();
            if k0.len() == public_key_len(Curve::P256) {
                return Ok(k0.to_vec());
            };


            if k0[0] != DER_OCTETSTRING {
                return Err(EcError::EcKeyInvalidKeyData);
            }
            if k0[1] as usize != public_key_len(Curve::P256) {
                return Err(EcError::EcKeyInvalidKeyData);
            }

            let (_, key) = k0.split_at(2);
            Ok(key.to_vec())
        }
    }
}

pub fn generate_private_key(curve: Curve) -> Result<EcPrivateKey, EcError> {
    let key_pair = nss_rs::ec::ecdh_keygen(&nss_rs::ec::EcCurve::P256).unwrap();
    match curve {
        Curve::P256 => return Ok(EcPrivateKey::P256(key_pair.private)),
        Curve::X25519 => return Ok(EcPrivateKey::X25519(key_pair.private)),
        Curve::Ed25519 => return Ok(EcPrivateKey::Ed25519(key_pair.private)),
        _ => Err(EcError::UnsupportedCurve),
    }
}

#[allow(dead_code)]
pub fn private_key_from_pkcs8(bytes: &[u8], curve: Curve) -> Result<EcPrivateKey, EcError> {
    let private_key = nss_rs::ec::import_ec_private_key_pkcs8(bytes).unwrap();
    match curve {
        Curve::P256 => return Ok(EcPrivateKey::P256(private_key)),
        Curve::Ed25519 => return Ok(EcPrivateKey::Ed25519(private_key)),
        Curve::X25519 => return Ok(EcPrivateKey::X25519(private_key)),
        _ => Err(EcError::UnsupportedCurve),
    }
}

#[allow(dead_code)]
pub fn private_key_to_pkcs8(key: &EcPrivateKey) -> Result<Vec<u8>, EcError> {
    match key {
        EcPrivateKey::P256(key) | EcPrivateKey::Ed25519(key) | EcPrivateKey::X25519(key) => {
            match nss_rs::ec::export_ec_private_key_pkcs8(key) {
                Ok(key) => return Ok(key),
                Err(_) => return Err(EcError::EcKeyInvalidKeyData),
            }
        }
    }
}

fn build_pkcs8_from_raw_private_key(key: Vec<u8>, curve: Curve) -> Result<Vec<u8>, EcError> {
    let mut lh = {
        match curve {
            Curve::P256 => vec![
                DER_SEQUENCE,
                0x41, 
                DER_INTEGER,
                0x1,
                0x0, 
                DER_SEQUENCE,
                0x13,
                0x6,
                0x7,
                0x2a,
                0x86,
                0x48,
                0xce,
                0x3d,
                0x2,
                0x1,
                0x6,
                0x8,
                0x2a,
                0x86,
                0x48,
                0xce,
                0x3d,
                0x3,
                0x1,
                0x7, 
                0x4,
                0x27,
                0x30,
                0x25,
                0x2,
                0x1,
                0x1,
                0x4,
                0x20,
            ],
            Curve::Ed25519 => vec![
                DER_SEQUENCE,
                0x2e,
                DER_INTEGER,
                0x01,
                0x00, 
                DER_SEQUENCE,
                0x05,
                0x06,
                0x03,
                0x2b,
                0x65,
                0x70, 
                DER_OCTETSTRING,
                0x22,
                DER_OCTETSTRING,
                0x20,
            ],
            Curve::X25519 => vec![
                DER_SEQUENCE,
                0x2e,
                DER_INTEGER,
                0x01,
                0x00, 
                DER_SEQUENCE,
                0x05,
                0x06,
                0x03,
                0x2b,
                0x65,
                0x6e, 
                DER_OCTETSTRING,
                0x22,
                DER_OCTETSTRING,
                0x20,
            ],
            _ => return Err(EcError::UnsupportedCurve),
        }
    };

    let mut key = key.clone();

    if private_key_len(curve) == 0 {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    if key.len() > private_key_len(curve) {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    if key.len() < private_key_len(curve) {
        let mut zeros = vec![0 as u8; private_key_len(curve) - key.len()];
        zeros.append(&mut key);
        lh.append(&mut zeros);
    } else {
        lh.append(&mut key);
    }

    Ok(lh)
}

fn private_key_from_bytes_helper(bytes: Vec<u8>, curve: Curve) -> Vec<u8> {
    if is_secret_key_contains_public_key(bytes.clone(), curve) {
        let (test, _) = bytes.split_at(private_key_len(curve));
        return test.to_vec();
    }
    return bytes;
}

pub fn private_key_from_bytes(bytes: Vec<u8>, curve: Curve) -> Result<EcPrivateKey, EcError> {
    let private_key = private_key_from_bytes_helper(bytes, curve);
    let private_key_pkcs8 = build_pkcs8_from_raw_private_key(private_key, curve).unwrap();
    let private_key_imported =
        nss_rs::ec::import_ec_private_key_pkcs8(&private_key_pkcs8).unwrap();
    match curve {
        Curve::P256 => Ok(EcPrivateKey::P256(private_key_imported)),
        Curve::Ed25519 => Ok(EcPrivateKey::Ed25519(private_key_imported)),
        Curve::X25519 => Ok(EcPrivateKey::X25519(private_key_imported)),
        _ => Err(EcError::UnsupportedCurve),
    }
}

pub fn private_key_to_bytes(key: EcPrivateKey) -> Result<Vec<u8>, EcError> {
    match key {
        EcPrivateKey::Ed25519(key) | EcPrivateKey::P256(key) | EcPrivateKey::X25519(key) => {
            Ok(key.key_data().unwrap())
        }
    }
}

pub fn private_key_to_public(private_key: &EcPrivateKey) -> Result<EcPublicKey, EcError> {
    match private_key {
        EcPrivateKey::X25519(key) => Ok(EcPublicKey::X25519(
            nss_rs::ec::convert_to_public(key).unwrap(),
        )),
        EcPrivateKey::Ed25519(key) => Ok(EcPublicKey::Ed25519(
            nss_rs::ec::convert_to_public(key).unwrap(),
        )),
        EcPrivateKey::P256(key) => Ok(EcPublicKey::P256(
            nss_rs::ec::convert_to_public(key).unwrap(),
        )),
    }
}

pub fn private_key_ecdh(
    private_key: &EcPrivateKey,
    remote_public: &EcPublicKey,
) -> Result<Vec<u8>, EcError> {
    let shared_secret = match private_key {
        EcPrivateKey::X25519(private_key) => match remote_public {
            EcPublicKey::X25519(public) => {
                let r = nss_rs::ec::ecdh(private_key, public).unwrap();
                Ok(r)
            }
            _ => Err(EcError::EcdhKeyTypeMismatch),
        },
        EcPrivateKey::Ed25519(_) => Err(EcError::EcKeyNotEcdh),
        EcPrivateKey::P256(private_key) => match remote_public {
            EcPublicKey::P256(public) => {
                let r = nss_rs::ec::ecdh(private_key, public).unwrap();
                Ok(r)
            }
            _ => Err(EcError::EcdhKeyTypeMismatch),
        },
    }?;

    Ok(shared_secret)
}

pub fn sign_p256(private_key: &PrivateKey, data: &[u8]) -> Result<Vec<u8>, EcError> {
    let mut hashed_data = Hash::hash(&Hash::Sha256, data);
    let signature = nss_rs::ec::sign_ecdsa(private_key, hashed_data.as_mut()).unwrap();
    Ok(signature)
}

fn format_ecdsa_p256(buffer: &[u8]) -> Result<Vec<u8>, EcError> {
    if buffer.len() > max_size_ecdsa_part(Curve::P256).unwrap() + 1 {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    if buffer.len() == max_size_ecdsa_part(Curve::P256).unwrap() {
        return Ok(buffer.to_vec());
    }

    if buffer.len() == max_size_ecdsa_part(Curve::P256).unwrap() + 1 {
        if buffer[0] != 0x00 {
            return Err(EcError::EcKeyInvalidKeyData);
        }

        if buffer[1] < 0b1000000 {
            return Err(EcError::EcKeyInvalidKeyData);
        }

        let (_, rest) = buffer.split_at(1);
        return Ok(rest.to_vec());
    }

    let mut buffer = buffer.to_vec();
    let mut zeros = vec![0 as u8; max_size_ecdsa_part(Curve::P256).unwrap() - buffer.len()];
    zeros.append(&mut buffer);
    Ok(zeros)
}

fn parse_ecdsa_p256(signature: &[u8]) -> Result<Vec<u8>, EcError> {
    let signature_vec = signature.to_vec();
    if signature[0] != DER_SEQUENCE {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    let len_buffer = signature[1];
    if (len_buffer + 2) as usize != signature.len() {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    if signature[2] != DER_INTEGER {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    let len_r = signature[3];
    if len_r as usize > max_size_ecdsa_part(Curve::P256).unwrap() + 1 {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    let (_, rs) = signature_vec.split_at(4);

    let skip_until_r = 3;
    let (r, intro_s) = rs.split_at(len_r as usize);

    if signature[(skip_until_r + len_r + 1) as usize] != DER_INTEGER {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    let len_s = signature_vec[(skip_until_r + len_r + 2) as usize];

    if len_s as usize > max_size_ecdsa_part(Curve::P256).unwrap() + 1 {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    let (_, s) = intro_s.split_at(2);

    let mut r = format_ecdsa_p256(r).unwrap();
    let s = format_ecdsa_p256(s).unwrap();

    r.extend(s);

    Ok(r)
}

pub fn verify_p256_(
    public_key: &PublicKey,
    signature: Vec<u8>,
    data: &[u8],
) -> Result<bool, EcError> {
    let mut hashed_data = Hash::hash(&Hash::Sha256, data);
    let result =
        nss_rs::ec::verify_ecdsa(public_key, hashed_data.as_mut(), &signature).unwrap();
    Ok(result)
}

pub fn verify_p256(public_key: &PublicKey, signature: &[u8], data: &[u8]) -> Result<bool, EcError> {
    if signature.len() != max_size_ecdsa_part(Curve::P256).unwrap() * 2 {
        let signature = parse_ecdsa_p256(signature).unwrap();
        return verify_p256_(public_key, signature, data);
    }

    return verify_p256_(public_key, signature.to_vec(), data);
}

pub fn sign_ed25519(private_key: &PrivateKey, data: &[u8]) -> Result<Vec<u8>, EcError> {
    let signature = nss_rs::ec::sign_eddsa(private_key, &data).unwrap();
    Ok(signature)
}

#[allow(dead_code)]
fn encode_ecdsa_p256(signature: Vec<u8>) -> Result<Vec<u8>, EcError> {
    if signature.len() != max_size_ecdsa_part(Curve::P256).unwrap() * 2 {
        return Err(EcError::EcKeyInvalidKeyData);
    }

    let (r, s) = signature.split_at(max_size_ecdsa_part(Curve::P256).unwrap());
    let mut signature = vec![DER_SEQUENCE];

    let r_len = max_size_ecdsa_part(Curve::P256).unwrap() + {
        if r[0] < 0b10000000 {
            0
        } else {
            1
        }
    };
    let s_len = max_size_ecdsa_part(Curve::P256).unwrap() + {
        if s[0] < 0b10000000 {
            0
        } else {
            1
        }
    };

    signature.push((4 + r_len + s_len) as u8);
    signature.push(DER_INTEGER);
    signature.push(r_len as u8);
    if r[0] >= 0b10000000 {
        signature.push(0);
    }

    signature.append(&mut r.to_vec());

    signature.push(DER_INTEGER);
    signature.push(s_len as u8);
    if s[0] >= 0b10000000 {
        signature.push(0);
    }

    signature.append(&mut s.to_vec());
    Ok(signature)
}

pub fn verify_ed25519(
    public_key: &PublicKey,
    signature: &[u8],
    data: &[u8],
) -> Result<bool, EcError> {
    let result = nss_rs::ec::verify_eddsa(public_key, &data, signature).unwrap();
    Ok(result)
}

pub fn generate_keypair(curve: Curve) -> Result<KeyPair, EcError> {
    match curve {
        Curve::P256 => {
            let key = nss_rs::ec::ecdh_keygen(&nss_rs::ec::EcCurve::P256).unwrap();
            let secret: Vec<u8> = private_key_to_bytes(EcPrivateKey::P256(key.private))?;
            let public: Vec<u8> = pub_key_to_uncompressed(EcPublicKey::P256(key.public))?;
            return Ok(KeyPair { public, secret });
        }
        Curve::Ed25519 => {
            let key = nss_rs::ec::ecdh_keygen(&nss_rs::ec::EcCurve::Ed25519).unwrap();
            let secret: Vec<u8> = private_key_to_bytes(EcPrivateKey::Ed25519(key.private))?;
            let public: Vec<u8> = pub_key_to_uncompressed(EcPublicKey::Ed25519(key.public))?;
            return Ok(KeyPair { public, secret });
        }
        Curve::X25519 => {
            let key = nss_rs::ec::ecdh_keygen(&nss_rs::ec::EcCurve::X25519).unwrap();
            let secret: Vec<u8> = private_key_to_bytes(EcPrivateKey::X25519(key.private))?;
            let public: Vec<u8> = pub_key_to_uncompressed(EcPublicKey::X25519(key.public))?;
            return Ok(KeyPair { public, secret });
        }
        _ => {
            let secret = generate_private_key(curve)?;
            let public = private_key_to_public(&secret)?;
            let secret = private_key_to_bytes(secret)?;
            let public = pub_key_to_uncompressed(public)?;
            Ok(KeyPair { public, secret })
        }
    }
}

#[derive(Clone, Default)]
pub struct KeyPair {
    pub public: Vec<u8>,
    pub secret: Vec<u8>,
}

impl Debug for KeyPair {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("KeyPair")
            .field("public", &mls_rs_core::debug::pretty_bytes(&self.public))
            .field("secret", &mls_rs_core::debug::pretty_bytes(&self.secret))
            .finish()
    }
}

fn is_secret_key_contains_public_key(secret_key: Vec<u8>, curve: Curve) -> bool {
    let private_key_len = private_key_len(curve);
    let public_key_len = public_key_len(curve);
    if secret_key.len() == private_key_len + public_key_len {
        return true;
    }
    return false;
}

pub fn private_key_bytes_to_public(secret_key: Vec<u8>, curve: Curve) -> Result<Vec<u8>, EcError> {
    if !is_secret_key_contains_public_key(secret_key.clone(), curve) {
        let secret_key = private_key_from_bytes(secret_key.clone(), curve)?;
        let public_key = private_key_to_public(&secret_key)?;
        pub_key_to_uncompressed(public_key)
    } else {
        let (_, public_key) = secret_key.split_at(private_key_len(curve));
        Ok(public_key.to_vec())
    }
}
