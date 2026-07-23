/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::ctap2::commands::client_pin::PinUvAuthTokenPermission;
use crate::ctap2::commands::get_info::{AuthenticatorInfo, AuthenticatorVersion};
use crate::errors::AuthenticatorError;
use crate::{ctap2::commands::CommandError, transport::errors::HIDError};
use serde::{
    de::{Error as SerdeError, MapAccess, Unexpected, Visitor},
    Deserialize, Deserializer, Serialize, Serializer,
};
use serde_bytes::ByteBuf;
use std::convert::TryFrom;
use std::fmt;

#[cfg(feature = "crypto_nss")]
mod nss;
#[cfg(feature = "crypto_nss")]
use nss as backend;

#[cfg(feature = "crypto_openssl")]
mod openssl;
#[cfg(feature = "crypto_openssl")]
use self::openssl as backend;

#[cfg(feature = "crypto_rust")]
mod rustcrypto;
#[cfg(feature = "crypto_rust")]
use rustcrypto as backend;

#[cfg(feature = "crypto_dummy")]
mod dummy;
#[cfg(feature = "crypto_dummy")]
use dummy as backend;

use backend::{
    decrypt_aes_256_cbc_no_pad, ecdhe_p256_raw, encrypt_aes_256_cbc_no_pad, gen_p256, hmac_sha256,
    random_bytes, sha256,
};

mod der;

pub use backend::ecdsa_p256_sha256_sign_raw;

pub struct PinUvAuthProtocol(Box<dyn PinProtocolImpl + Send + Sync>);
impl PinUvAuthProtocol {
    pub fn from_id(id: u64) -> Option<Self> {
        match id {
            1 => Some(Self(Box::new(PinUvAuth1 {}))),
            2 => Some(Self(Box::new(PinUvAuth2 {}))),
            _ => None,
        }
    }

    pub fn id(&self) -> u64 {
        self.0.protocol_id()
    }

    pub fn encapsulate(&self, peer_cose_key: &COSEKey) -> Result<SharedSecret, CryptoError> {
        self.0.encapsulate(peer_cose_key)
    }
}

/// The output of `PinUvAuthProtocol::encapsulate` is supposed to be used with the same
/// PinProtocolImpl. So we stash a copy of the calling PinUvAuthProtocol in the output SharedSecret.
/// We need a trick here to tell the compiler that every PinProtocolImpl we define will implement
/// Clone.
trait ClonablePinProtocolImpl {
    fn clone_box(&self) -> Box<dyn PinProtocolImpl + Send + Sync>;
}

impl<T> ClonablePinProtocolImpl for T
where
    T: 'static + PinProtocolImpl + Clone + Send + Sync,
{
    fn clone_box(&self) -> Box<dyn PinProtocolImpl + Send + Sync> {
        Box::new(self.clone())
    }
}

impl Clone for PinUvAuthProtocol {
    fn clone(&self) -> Self {
        PinUvAuthProtocol(self.0.as_ref().clone_box())
    }
}

/// CTAP 2.1, Section 6.5.4. PIN/UV Auth Protocol Abstract Definition
trait PinProtocolImpl: ClonablePinProtocolImpl {
    fn protocol_id(&self) -> u64;
    fn encrypt(&self, key: &[u8], plaintext: &[u8]) -> Result<Vec<u8>, CryptoError>;
    fn decrypt(&self, key: &[u8], ciphertext: &[u8]) -> Result<Vec<u8>, CryptoError>;
    fn authenticate(&self, key: &[u8], message: &[u8]) -> Result<Vec<u8>, CryptoError>;
    fn kdf(&self, z: &[u8]) -> Result<Vec<u8>, CryptoError>;
    fn encapsulate(&self, peer_cose_key: &COSEKey) -> Result<SharedSecret, CryptoError> {

        match peer_cose_key.alg {
            COSEAlgorithm::ECDH_ES_HKDF256 => (),
            other => return Err(CryptoError::UnsupportedAlgorithm(other)),
        }

        let peer_cose_ec2_key = match peer_cose_key.key {
            COSEKeyType::EC2(ref key) => key,
            _ => return Err(CryptoError::UnsupportedKeyType),
        };

        let (shared_point, client_public_sec1) = ecdhe_p256_raw(peer_cose_ec2_key)?;

        let client_cose_ec2_key =
            COSEEC2Key::from_sec1_uncompressed(Curve::SECP256R1, &client_public_sec1)?;

        let client_cose_key = COSEKey {
            alg: COSEAlgorithm::ECDH_ES_HKDF256,
            key: COSEKeyType::EC2(client_cose_ec2_key),
        };

        let shared_secret = SharedSecret {
            pin_protocol: PinUvAuthProtocol(self.clone_box()),
            key: self.kdf(&shared_point)?,
            inputs: PublicInputs {
                peer: peer_cose_key.clone(),
                client: client_cose_key,
            },
        };

        Ok(shared_secret)
    }
}

impl TryFrom<&AuthenticatorInfo> for PinUvAuthProtocol {
    type Error = CommandError;

    fn try_from(info: &AuthenticatorInfo) -> Result<Self, Self::Error> {
        if let Some(pin_protocols) = &info.pin_protocols {
            pin_protocols
                .iter()
                .copied()
                .find_map(PinUvAuthProtocol::from_id)
                .ok_or(CommandError::UnsupportedPinProtocol)
        } else {
            match info.max_supported_version() {
                AuthenticatorVersion::U2F_V2 | AuthenticatorVersion::Unknown => {
                    Err(CommandError::UnsupportedPinProtocol)
                }
                AuthenticatorVersion::FIDO_2_0 => Ok(PinUvAuthProtocol(Box::new(PinUvAuth1 {}))),
                AuthenticatorVersion::FIDO_2_1_PRE | AuthenticatorVersion::FIDO_2_1 => {
                    Ok(PinUvAuthProtocol(Box::new(PinUvAuth2 {})))
                }
            }
        }
    }
}

impl fmt::Debug for PinUvAuthProtocol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("PinUvAuthProtocol")
            .field("id", &self.id())
            .finish()
    }
}

/// CTAP 2.1, Section 6.5.6.
#[derive(Copy, Clone)]
pub struct PinUvAuth1;

impl PinProtocolImpl for PinUvAuth1 {
    fn protocol_id(&self) -> u64 {
        1
    }

    fn encrypt(&self, key: &[u8], plaintext: &[u8]) -> Result<Vec<u8>, CryptoError> {
        encrypt_aes_256_cbc_no_pad(key, None, plaintext)
    }

    fn decrypt(&self, key: &[u8], ciphertext: &[u8]) -> Result<Vec<u8>, CryptoError> {
        decrypt_aes_256_cbc_no_pad(key, None, ciphertext)
    }

    fn authenticate(&self, key: &[u8], message: &[u8]) -> Result<Vec<u8>, CryptoError> {
        let mut hmac = hmac_sha256(key, message)?;
        hmac.truncate(16);
        Ok(hmac)
    }

    fn kdf(&self, z: &[u8]) -> Result<Vec<u8>, CryptoError> {
        sha256(z)
    }
}

/// CTAP 2.1, Section 6.5.7.
#[derive(Copy, Clone)]
pub struct PinUvAuth2;

impl PinProtocolImpl for PinUvAuth2 {
    fn protocol_id(&self) -> u64 {
        2
    }

    fn encrypt(&self, key: &[u8], plaintext: &[u8]) -> Result<Vec<u8>, CryptoError> {
        if key.len() != 64 {
            return Err(CryptoError::LibraryFailure);
        }
        let key = &key[32..64];

        let iv = random_bytes(16)?;
        let mut ct = encrypt_aes_256_cbc_no_pad(key, Some(&iv), plaintext)?;

        let mut out = iv;
        out.append(&mut ct);
        Ok(out)
    }

    fn decrypt(&self, key: &[u8], ciphertext: &[u8]) -> Result<Vec<u8>, CryptoError> {
        if key.len() < 64 || ciphertext.len() < 16 {
            return Err(CryptoError::LibraryFailure);
        }
        let key = &key[32..64];
        let (iv, ct) = ciphertext.split_at(16);
        decrypt_aes_256_cbc_no_pad(key, Some(iv), ct)
    }

    fn authenticate(&self, key: &[u8], message: &[u8]) -> Result<Vec<u8>, CryptoError> {
        if key.len() < 32 {
            return Err(CryptoError::LibraryFailure);
        }
        let key = &key[0..32];
        hmac_sha256(key, message)
    }

    fn kdf(&self, z: &[u8]) -> Result<Vec<u8>, CryptoError> {

        let prk = hmac_sha256(&[0u8; 32], z)?;
        let mut shared_secret = hmac_sha256(&prk, "CTAP2 HMAC key\x01".as_bytes())?;
        shared_secret.append(&mut hmac_sha256(&prk, "CTAP2 AES key\x01".as_bytes())?);
        Ok(shared_secret)
    }
}

#[derive(Clone, Debug)]
struct PublicInputs {
    client: COSEKey,
    peer: COSEKey,
}

#[derive(Clone, Debug)]
pub struct SharedSecret {
    pub pin_protocol: PinUvAuthProtocol,
    key: Vec<u8>,
    inputs: PublicInputs,
}

impl SharedSecret {
    pub fn encrypt(&self, plaintext: &[u8]) -> Result<Vec<u8>, CryptoError> {
        self.pin_protocol.0.encrypt(&self.key, plaintext)
    }
    pub fn decrypt(&self, ciphertext: &[u8]) -> Result<Vec<u8>, CryptoError> {
        self.pin_protocol.0.decrypt(&self.key, ciphertext)
    }
    pub fn decrypt_pin_token(
        &self,
        permissions: PinUvAuthTokenPermission,
        encrypted_pin_token: &[u8],
    ) -> Result<PinUvAuthToken, CryptoError> {
        let pin_token = self.decrypt(encrypted_pin_token)?;
        Ok(PinUvAuthToken {
            pin_protocol: self.pin_protocol.clone(),
            pin_token,
            permissions,
        })
    }
    pub fn authenticate(&self, message: &[u8]) -> Result<Vec<u8>, CryptoError> {
        self.pin_protocol.0.authenticate(&self.key, message)
    }
    pub fn client_input(&self) -> &COSEKey {
        &self.inputs.client
    }
    pub fn peer_input(&self) -> &COSEKey {
        &self.inputs.peer
    }

}

#[derive(Clone, Debug)]
pub struct PinUvAuthToken {
    pub pin_protocol: PinUvAuthProtocol,
    pin_token: Vec<u8>,
    pub permissions: PinUvAuthTokenPermission,
}

impl PinUvAuthToken {
    pub fn derive(self, message: &[u8]) -> Result<PinUvAuthParam, CryptoError> {
        let pin_auth = self.pin_protocol.0.authenticate(&self.pin_token, message)?;
        Ok(PinUvAuthParam {
            pin_auth,
            pin_protocol: self.pin_protocol,
            permissions: self.permissions,
        })
    }
}

#[derive(Clone, Debug)]
pub struct PinUvAuthParam {
    pin_auth: Vec<u8>,
    pub pin_protocol: PinUvAuthProtocol,
    #[allow(dead_code)] 
    permissions: PinUvAuthTokenPermission,
}

impl PinUvAuthParam {
    pub(crate) fn create_empty() -> Self {
        let pin_protocol = PinUvAuthProtocol(Box::new(PinUvAuth1 {}));
        Self {
            pin_auth: vec![],
            pin_protocol,
            permissions: PinUvAuthTokenPermission::empty(),
        }
    }

}

impl Serialize for PinUvAuthParam {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serde_bytes::serialize(&self.pin_auth[..], serializer)
    }
}

/// A Curve identifier. You probably will never need to alter
/// or use this value, as it is set inside the Credential for you.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Curve {
    /// Identifies this curve as SECP256R1 (X9_62_PRIME256V1 in OpenSSL)
    SECP256R1 = 1,
    /// Identifies this curve as SECP384R1
    SECP384R1 = 2,
    /// Identifies this curve as SECP521R1
    SECP521R1 = 3,
    /// Identifieds this as OKP X25519 for use w/ ECDH only
    X25519 = 4,
    /// Identifieds this as OKP X448 for use w/ ECDH only
    X448 = 5,
    /// Identifieds this as OKP Ed25519 for use w/ EdDSA only
    Ed25519 = 6,
    /// Identifieds this as OKP Ed448 for use w/ EdDSA only
    Ed448 = 7,
}

impl Serialize for Curve {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_i64(*self as i64)
    }
}

impl TryFrom<i64> for Curve {
    type Error = CryptoError;
    fn try_from(i: i64) -> Result<Self, Self::Error> {
        match i {
            i if i == Curve::SECP256R1 as i64 => Ok(Curve::SECP256R1),
            i if i == Curve::SECP384R1 as i64 => Ok(Curve::SECP384R1),
            i if i == Curve::SECP521R1 as i64 => Ok(Curve::SECP521R1),
            i if i == Curve::X25519 as i64 => Ok(Curve::X25519),
            i if i == Curve::X448 as i64 => Ok(Curve::X448),
            i if i == Curve::Ed25519 as i64 => Ok(Curve::Ed25519),
            i if i == Curve::Ed448 as i64 => Ok(Curve::Ed448),
            _ => Err(CryptoError::UnknownKeyType),
        }
    }
}
/// A COSE signature algorithm, indicating the type of key and hash type
/// that should be used.
/// see: https://www.iana.org/assignments/cose/cose.xhtml#table-algorithms
#[rustfmt::skip]
#[allow(non_camel_case_types)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum COSEAlgorithm {
    INSECURE_RS1 = -65535,             
    RS512 = -259,                      
    RS384 = -258,                      
    RS256 = -257,                      
    Ed448 = -53,                       
    ESP512 = -52,                      
    ESP384 = -51,                      
    ES256K = -47,                      
    HSS_LMS = -46,                     
    SHAKE256 = -45,                    
    SHA512 = -44,                      
    SHA384 = -43,                      
    RSAES_OAEP_SHA_512 = -42,          
    RSAES_OAEP_SHA_256 = -41,          
    RSAES_OAEP_RFC_8017_default = -40, 
    PS512 = -39,                       
    PS384 = -38,                       
    PS256 = -37,                       
    ES512 = -36,                       
    ES384 = -35,                       
    ECDH_SS_A256KW = -34,              
    ECDH_SS_A192KW = -33,              
    ECDH_SS_A128KW = -32,              
    ECDH_ES_A256KW = -31,              
    ECDH_ES_A192KW = -30,              
    ECDH_ES_A128KW = -29,              
    ECDH_SS_HKDF512 = -28,             
    ECDH_SS_HKDF256 = -27,             
    ECDH_ES_HKDF512 = -26,             
    ECDH_ES_HKDF256 = -25,             
    Ed25519 = -19,                     
    SHAKE128 = -18,                    
    SHA512_256 = -17,                  
    SHA256 = -16,                      
    SHA256_64 = -15,                   
    SHA1 = -14,                        
    Direct_HKDF_AES256 = -13,          
    Direct_HKDF_AES128 = -12,          
    Direct_HKDF_SHA512 = -11,          
    Direct_HKDF_SHA256 = -10,          
    ESP256 = -9,                       
    EDDSA = -8,                        
    ES256 = -7,                        
    Direct = -6,                       
    A256KW = -5,                       
    A192KW = -4,                       
    A128KW = -3,                       
    A128GCM = 1,                       
    A192GCM = 2,                       
    A256GCM = 3,                       
    HMAC256_64 = 4,                    
    HMAC256_256 = 5,                   
    HMAC384_384 = 6,                   
    HMAC512_512 = 7,                   
    AES_CCM_16_64_128 = 10,            
    AES_CCM_16_64_256 = 11,            
    AES_CCM_64_64_128 = 12,            
    AES_CCM_64_64_256 = 13,            
    AES_MAC_128_64 = 14,               
    AES_MAC_256_64 = 15,               
    ChaCha20_Poly1305 = 24,            
    AES_MAC_128_128 = 25,              
    AES_MAC_256_128 = 26,              
    AES_CCM_16_128_128 = 30,           
    AES_CCM_16_128_256 = 31,           
    AES_CCM_64_128_128 = 32,           
    AES_CCM_64_128_256 = 33,           
    IV_GENERATION = 34,                
}

impl Serialize for COSEAlgorithm {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_i64(*self as i64)
    }
}

impl<'de> Deserialize<'de> for COSEAlgorithm {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct COSEAlgorithmVisitor;

        impl<'de> Visitor<'de> for COSEAlgorithmVisitor {
            type Value = COSEAlgorithm;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a signed integer")
            }

            fn visit_i64<E>(self, v: i64) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                COSEAlgorithm::try_from(v).map_err(|_| {
                    SerdeError::invalid_value(Unexpected::Signed(v), &"valid COSEAlgorithm")
                })
            }
        }

        deserializer.deserialize_any(COSEAlgorithmVisitor)
    }
}

impl TryFrom<i64> for COSEAlgorithm {
    type Error = CryptoError;
    fn try_from(i: i64) -> Result<Self, Self::Error> {
        match i {
            i if i == COSEAlgorithm::RS512 as i64 => Ok(COSEAlgorithm::RS512),
            i if i == COSEAlgorithm::RS384 as i64 => Ok(COSEAlgorithm::RS384),
            i if i == COSEAlgorithm::RS256 as i64 => Ok(COSEAlgorithm::RS256),
            i if i == COSEAlgorithm::Ed448 as i64 => Ok(COSEAlgorithm::Ed448),
            i if i == COSEAlgorithm::ESP512 as i64 => Ok(COSEAlgorithm::ESP512),
            i if i == COSEAlgorithm::ESP384 as i64 => Ok(COSEAlgorithm::ESP384),
            i if i == COSEAlgorithm::ES256K as i64 => Ok(COSEAlgorithm::ES256K),
            i if i == COSEAlgorithm::HSS_LMS as i64 => Ok(COSEAlgorithm::HSS_LMS),
            i if i == COSEAlgorithm::SHAKE256 as i64 => Ok(COSEAlgorithm::SHAKE256),
            i if i == COSEAlgorithm::SHA512 as i64 => Ok(COSEAlgorithm::SHA512),
            i if i == COSEAlgorithm::SHA384 as i64 => Ok(COSEAlgorithm::SHA384),
            i if i == COSEAlgorithm::RSAES_OAEP_SHA_512 as i64 => {
                Ok(COSEAlgorithm::RSAES_OAEP_SHA_512)
            }
            i if i == COSEAlgorithm::RSAES_OAEP_SHA_256 as i64 => {
                Ok(COSEAlgorithm::RSAES_OAEP_SHA_256)
            }
            i if i == COSEAlgorithm::RSAES_OAEP_RFC_8017_default as i64 => {
                Ok(COSEAlgorithm::RSAES_OAEP_RFC_8017_default)
            }
            i if i == COSEAlgorithm::PS512 as i64 => Ok(COSEAlgorithm::PS512),
            i if i == COSEAlgorithm::PS384 as i64 => Ok(COSEAlgorithm::PS384),
            i if i == COSEAlgorithm::PS256 as i64 => Ok(COSEAlgorithm::PS256),
            i if i == COSEAlgorithm::ES512 as i64 => Ok(COSEAlgorithm::ES512),
            i if i == COSEAlgorithm::ES384 as i64 => Ok(COSEAlgorithm::ES384),
            i if i == COSEAlgorithm::ECDH_SS_A256KW as i64 => Ok(COSEAlgorithm::ECDH_SS_A256KW),
            i if i == COSEAlgorithm::ECDH_SS_A192KW as i64 => Ok(COSEAlgorithm::ECDH_SS_A192KW),
            i if i == COSEAlgorithm::ECDH_SS_A128KW as i64 => Ok(COSEAlgorithm::ECDH_SS_A128KW),
            i if i == COSEAlgorithm::ECDH_ES_A256KW as i64 => Ok(COSEAlgorithm::ECDH_ES_A256KW),
            i if i == COSEAlgorithm::ECDH_ES_A192KW as i64 => Ok(COSEAlgorithm::ECDH_ES_A192KW),
            i if i == COSEAlgorithm::ECDH_ES_A128KW as i64 => Ok(COSEAlgorithm::ECDH_ES_A128KW),
            i if i == COSEAlgorithm::ECDH_SS_HKDF512 as i64 => Ok(COSEAlgorithm::ECDH_SS_HKDF512),
            i if i == COSEAlgorithm::ECDH_SS_HKDF256 as i64 => Ok(COSEAlgorithm::ECDH_SS_HKDF256),
            i if i == COSEAlgorithm::ECDH_ES_HKDF512 as i64 => Ok(COSEAlgorithm::ECDH_ES_HKDF512),
            i if i == COSEAlgorithm::ECDH_ES_HKDF256 as i64 => Ok(COSEAlgorithm::ECDH_ES_HKDF256),
            i if i == COSEAlgorithm::Ed25519 as i64 => Ok(COSEAlgorithm::Ed25519),
            i if i == COSEAlgorithm::SHAKE128 as i64 => Ok(COSEAlgorithm::SHAKE128),
            i if i == COSEAlgorithm::SHA512_256 as i64 => Ok(COSEAlgorithm::SHA512_256),
            i if i == COSEAlgorithm::SHA256 as i64 => Ok(COSEAlgorithm::SHA256),
            i if i == COSEAlgorithm::SHA256_64 as i64 => Ok(COSEAlgorithm::SHA256_64),
            i if i == COSEAlgorithm::SHA1 as i64 => Ok(COSEAlgorithm::SHA1),
            i if i == COSEAlgorithm::Direct_HKDF_AES256 as i64 => {
                Ok(COSEAlgorithm::Direct_HKDF_AES256)
            }
            i if i == COSEAlgorithm::Direct_HKDF_AES128 as i64 => {
                Ok(COSEAlgorithm::Direct_HKDF_AES128)
            }
            i if i == COSEAlgorithm::Direct_HKDF_SHA512 as i64 => {
                Ok(COSEAlgorithm::Direct_HKDF_SHA512)
            }
            i if i == COSEAlgorithm::Direct_HKDF_SHA256 as i64 => {
                Ok(COSEAlgorithm::Direct_HKDF_SHA256)
            }
            i if i == COSEAlgorithm::ESP256 as i64 => Ok(COSEAlgorithm::ESP256),
            i if i == COSEAlgorithm::EDDSA as i64 => Ok(COSEAlgorithm::EDDSA),
            i if i == COSEAlgorithm::ES256 as i64 => Ok(COSEAlgorithm::ES256),
            i if i == COSEAlgorithm::Direct as i64 => Ok(COSEAlgorithm::Direct),
            i if i == COSEAlgorithm::A256KW as i64 => Ok(COSEAlgorithm::A256KW),
            i if i == COSEAlgorithm::A192KW as i64 => Ok(COSEAlgorithm::A192KW),
            i if i == COSEAlgorithm::A128KW as i64 => Ok(COSEAlgorithm::A128KW),
            i if i == COSEAlgorithm::A128GCM as i64 => Ok(COSEAlgorithm::A128GCM),
            i if i == COSEAlgorithm::A192GCM as i64 => Ok(COSEAlgorithm::A192GCM),
            i if i == COSEAlgorithm::A256GCM as i64 => Ok(COSEAlgorithm::A256GCM),
            i if i == COSEAlgorithm::HMAC256_64 as i64 => Ok(COSEAlgorithm::HMAC256_64),
            i if i == COSEAlgorithm::HMAC256_256 as i64 => Ok(COSEAlgorithm::HMAC256_256),
            i if i == COSEAlgorithm::HMAC384_384 as i64 => Ok(COSEAlgorithm::HMAC384_384),
            i if i == COSEAlgorithm::HMAC512_512 as i64 => Ok(COSEAlgorithm::HMAC512_512),
            i if i == COSEAlgorithm::AES_CCM_16_64_128 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_16_64_128)
            }
            i if i == COSEAlgorithm::AES_CCM_16_64_256 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_16_64_256)
            }
            i if i == COSEAlgorithm::AES_CCM_64_64_128 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_64_64_128)
            }
            i if i == COSEAlgorithm::AES_CCM_64_64_256 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_64_64_256)
            }
            i if i == COSEAlgorithm::AES_MAC_128_64 as i64 => Ok(COSEAlgorithm::AES_MAC_128_64),
            i if i == COSEAlgorithm::AES_MAC_256_64 as i64 => Ok(COSEAlgorithm::AES_MAC_256_64),
            i if i == COSEAlgorithm::ChaCha20_Poly1305 as i64 => {
                Ok(COSEAlgorithm::ChaCha20_Poly1305)
            }
            i if i == COSEAlgorithm::AES_MAC_128_128 as i64 => Ok(COSEAlgorithm::AES_MAC_128_128),
            i if i == COSEAlgorithm::AES_MAC_256_128 as i64 => Ok(COSEAlgorithm::AES_MAC_256_128),
            i if i == COSEAlgorithm::AES_CCM_16_128_128 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_16_128_128)
            }
            i if i == COSEAlgorithm::AES_CCM_16_128_256 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_16_128_256)
            }
            i if i == COSEAlgorithm::AES_CCM_64_128_128 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_64_128_128)
            }
            i if i == COSEAlgorithm::AES_CCM_64_128_256 as i64 => {
                Ok(COSEAlgorithm::AES_CCM_64_128_256)
            }
            i if i == COSEAlgorithm::IV_GENERATION as i64 => Ok(COSEAlgorithm::IV_GENERATION),
            i if i == COSEAlgorithm::INSECURE_RS1 as i64 => Ok(COSEAlgorithm::INSECURE_RS1),
            _ => Err(CryptoError::UnknownAlgorithm),
        }
    }
}

/// A COSE Elliptic Curve Public Key. This is generally the provided credential
/// that an authenticator registers, and is used to authenticate the user.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct COSEEC2Key {
    /// The curve that this key references.
    pub curve: Curve,
    /// The key's public X coordinate.
    pub x: Vec<u8>,
    /// The key's public Y coordinate.
    pub y: Vec<u8>,
}

impl COSEEC2Key {
    pub fn from_sec1_uncompressed(curve: Curve, key: &[u8]) -> Result<Self, CryptoError> {
        if !(curve == Curve::SECP256R1 && key.len() == 65) {
            return Err(CryptoError::UnsupportedCurve(curve));
        }
        if key[0] != 0x04 {
            return Err(CryptoError::MalformedInput);
        }
        let key = &key[1..];
        let (x, y) = key.split_at(key.len() / 2);
        Ok(COSEEC2Key {
            curve,
            x: x.to_vec(),
            y: y.to_vec(),
        })
    }

    pub fn der_spki(&self) -> Result<Vec<u8>, CryptoError> {
        if self.curve != Curve::SECP256R1 {
            return Err(CryptoError::UnsupportedCurve(self.curve));
        }

        der::sequence(&[
            &der::sequence(&[
                &der::object_id(der::OID_EC_PUBLIC_KEY_BYTES)?,
                &der::object_id(der::OID_SECP256R1_BYTES)?,
            ])?,
            &der::bit_string(
                &[&[0x04], self.x.as_slice(), self.y.as_slice()].concat(),
            )?,
        ])
    }
}

/// A Octet Key Pair (OKP).
/// The other version uses only the x-coordinate as the y-coordinate is
/// either to be recomputed or not needed for the key agreement operation ('OKP').
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct COSEOKPKey {
    /// The curve that this key references.
    pub curve: Curve,
    /// The key's public X coordinate.
    pub x: Vec<u8>,
}

impl COSEOKPKey {
    pub fn der_spki(&self) -> Result<Vec<u8>, CryptoError> {
        if self.curve != Curve::Ed25519 {
            return Err(CryptoError::UnsupportedCurve(self.curve));
        }

        der::sequence(&[
            &der::sequence(&[
                &der::object_id(der::OID_ED25519_BYTES)?,
            ])?,
            &der::bit_string(
                self.x.as_slice(),
            )?,
        ])
    }
}

/// A COSE RSA PublicKey. This is a provided credential from a registered authenticator.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct COSERSAKey {
    /// An RSA modulus
    pub n: Vec<u8>,
    /// An RSA exponent
    pub e: Vec<u8>,
}

impl COSERSAKey {
    pub fn der_spki(&self) -> Result<Vec<u8>, CryptoError> {
        der::sequence(&[
            &der::sequence(&[
                &der::object_id(der::OID_RSA_ENCRYPTION_BYTES)?,
                &der::null()?,
            ])?,
            &der::bit_string(
                &der::sequence(&[&der::integer(&self.n)?, &der::integer(&self.e)?])?,
            )?,
        ])
    }
}

#[allow(non_camel_case_types)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum COSEKeyTypeId {
    /// Octet Key Pair
    OKP = 1,
    /// Elliptic Curve Keys w/ x- and y-coordinate
    EC2 = 2,
    /// RSA
    RSA = 3,
}

impl Serialize for COSEKeyTypeId {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_i64(*self as i64)
    }
}

impl TryFrom<i64> for COSEKeyTypeId {
    type Error = CryptoError;
    fn try_from(i: i64) -> Result<Self, Self::Error> {
        match i {
            i if i == COSEKeyTypeId::OKP as i64 => Ok(COSEKeyTypeId::OKP),
            i if i == COSEKeyTypeId::EC2 as i64 => Ok(COSEKeyTypeId::EC2),
            i if i == COSEKeyTypeId::RSA as i64 => Ok(COSEKeyTypeId::RSA),
            _ => Err(CryptoError::UnknownKeyType),
        }
    }
}

/// The type of Key contained within a COSE value. You should never need
/// to alter or change this type.
#[allow(non_camel_case_types)]
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum COSEKeyType {
    /// Identifies this as an Elliptic Curve EC2 key
    EC2(COSEEC2Key),
    /// Identifies this as an Elliptic Curve octet key pair
    OKP(COSEOKPKey),
    /// Identifies this as an RSA key
    RSA(COSERSAKey),
}

/// A COSE Key as provided by the Authenticator. You should never need
/// to alter or change these values.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct COSEKey {
    /// COSE signature algorithm, indicating the type of key and hash type
    /// that should be used.
    pub alg: COSEAlgorithm,
    /// The public key
    pub key: COSEKeyType,
}

impl COSEKey {
    /// Generates a new key pair for the specified algorithm.
    /// Returns an PKCS#8 encoding of the private key, and the public key as a COSEKey.
    pub fn generate(alg: COSEAlgorithm) -> Result<(Vec<u8>, Self), CryptoError> {
        if alg != COSEAlgorithm::ES256 && alg != COSEAlgorithm::ECDH_ES_HKDF256 {
            return Err(CryptoError::UnsupportedAlgorithm(alg));
        }
        let (private, public) = gen_p256()?;
        let cose_ec2_key = COSEEC2Key::from_sec1_uncompressed(Curve::SECP256R1, &public)?;
        let public = COSEKey {
            alg,
            key: COSEKeyType::EC2(cose_ec2_key),
        };
        Ok((private, public))
    }

    pub fn der_spki(&self) -> Result<Vec<u8>, CryptoError> {
        match &self.key {
            COSEKeyType::EC2(ec2_key) => ec2_key.der_spki(),
            COSEKeyType::OKP(okp_key) => okp_key.der_spki(),
            COSEKeyType::RSA(rsa_key) => rsa_key.der_spki(),
        }
    }
}

impl<'de> Deserialize<'de> for COSEKey {
    fn deserialize<D>(deserializer: D) -> std::result::Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct COSEKeyVisitor;

        impl<'de> Visitor<'de> for COSEKeyVisitor {
            type Value = COSEKey;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a map")
            }

            fn visit_map<M>(self, mut map: M) -> std::result::Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut key_type: Option<COSEKeyTypeId> = None;
                let mut alg: Option<COSEAlgorithm> = None;
                let mut curve: Option<Curve> = None;
                let mut x: Option<Vec<u8>> = None;
                let mut y: Option<Vec<u8>> = None;

                let mut n: Option<Vec<u8>> = None;
                let mut e: Option<Vec<u8>> = None;

                while let Some(key) = map.next_key()? {
                    match key {
                        1 => {
                            if key_type.is_some() {
                                return Err(SerdeError::duplicate_field("key_type"));
                            }
                            let value: i64 = map.next_value()?;
                            let val = COSEKeyTypeId::try_from(value).map_err(|_| {
                                SerdeError::custom(format!("unsupported key_type {value}"))
                            })?;
                            key_type = Some(val);
                        }
                        3 => {
                            if alg.is_some() {
                                return Err(SerdeError::duplicate_field("alg"));
                            }
                            let value: i64 = map.next_value()?;
                            let val = COSEAlgorithm::try_from(value).map_err(|_| {
                                SerdeError::custom(format!("unsupported algorithm {value}"))
                            })?;
                            alg = Some(val);
                        }
                        -1 => match key_type {
                            None => return Err(SerdeError::missing_field("key_type")),
                            Some(COSEKeyTypeId::OKP) | Some(COSEKeyTypeId::EC2) => {
                                if curve.is_some() {
                                    return Err(SerdeError::duplicate_field("curve"));
                                }
                                let value: i64 = map.next_value()?;
                                let val = Curve::try_from(value).map_err(|_| {
                                    SerdeError::custom(format!("unsupported curve {value}"))
                                })?;
                                curve = Some(val);
                            }
                            Some(COSEKeyTypeId::RSA) => {
                                if n.is_some() {
                                    return Err(SerdeError::duplicate_field("n"));
                                }
                                let value: ByteBuf = map.next_value()?;
                                n = Some(value.to_vec());
                            }
                        },
                        -2 => match key_type {
                            None => return Err(SerdeError::missing_field("key_type")),
                            Some(COSEKeyTypeId::OKP) | Some(COSEKeyTypeId::EC2) => {
                                if x.is_some() {
                                    return Err(SerdeError::duplicate_field("x"));
                                }
                                let value: ByteBuf = map.next_value()?;
                                x = Some(value.to_vec());
                            }
                            Some(COSEKeyTypeId::RSA) => {
                                if e.is_some() {
                                    return Err(SerdeError::duplicate_field("e"));
                                }
                                let value: ByteBuf = map.next_value()?;
                                e = Some(value.to_vec());
                            }
                        },
                        -3 if key_type == Some(COSEKeyTypeId::EC2) => {
                            if y.is_some() {
                                return Err(SerdeError::duplicate_field("y"));
                            }
                            let value: ByteBuf = map.next_value()?;
                            y = Some(value.to_vec());
                        }
                        other => {
                            return Err(SerdeError::custom(format!("unexpected field: {other}")));
                        }
                    };
                }

                let key_type = key_type.ok_or_else(|| SerdeError::missing_field("key_type (1)"))?;
                let alg = alg.ok_or_else(|| SerdeError::missing_field("alg (3)"))?;

                let res = match key_type {
                    COSEKeyTypeId::OKP => {
                        let curve = curve.ok_or_else(|| SerdeError::missing_field("curve (-1)"))?;
                        let x = x.ok_or_else(|| SerdeError::missing_field("x (-2)"))?;
                        COSEKeyType::OKP(COSEOKPKey { curve, x })
                    }
                    COSEKeyTypeId::EC2 => {
                        let curve = curve.ok_or_else(|| SerdeError::missing_field("curve (-1)"))?;
                        let x = x.ok_or_else(|| SerdeError::missing_field("x (-2)"))?;
                        let y = y.ok_or_else(|| SerdeError::missing_field("y (-3)"))?;
                        COSEKeyType::EC2(COSEEC2Key { curve, x, y })
                    }
                    COSEKeyTypeId::RSA => {
                        let n = n.ok_or_else(|| SerdeError::missing_field("n (-1)"))?;
                        let e = e.ok_or_else(|| SerdeError::missing_field("e (-2)"))?;
                        COSEKeyType::RSA(COSERSAKey { e, n })
                    }
                };
                Ok(COSEKey { alg, key: res })
            }
        }

        deserializer.deserialize_bytes(COSEKeyVisitor)
    }
}

impl Serialize for COSEKey {
    fn serialize<S>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match &self.key {
            COSEKeyType::OKP(key) => {
                serialize_map!(
                    serializer,
                    &1 => &COSEKeyTypeId::OKP,
                    &3 => &self.alg,
                    &-1 => &key.curve,
                    &-2 => &serde_bytes::Bytes::new(&key.x),
                )
            }
            COSEKeyType::EC2(key) => {
                serialize_map!(
                    serializer,
                    &1 => &COSEKeyTypeId::EC2,
                    &3 => &self.alg,
                    &-1 => &key.curve,
                    &-2 => &serde_bytes::Bytes::new(&key.x),
                    &-3 => &serde_bytes::Bytes::new(&key.y),
                )
            }
            COSEKeyType::RSA(key) => {
                serialize_map!(
                    serializer,
                    &1 => &COSEKeyTypeId::RSA,
                    &3 => &self.alg,
                    &-1 => &serde_bytes::Bytes::new(&key.n),
                    &-2 => &serde_bytes::Bytes::new(&key.e),
                )
            }
        }
    }
}

/// Errors that can be returned from COSE functions.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub enum CryptoError {
    LibraryFailure,
    MalformedInput,
    UnknownKeyType,
    UnknownSignatureScheme,
    UnknownAlgorithm,
    WrongSaltLength,
    UnsupportedAlgorithm(COSEAlgorithm),
    UnsupportedCurve(Curve),
    UnsupportedKeyType,
    Backend(String),
}

impl From<CryptoError> for CommandError {
    fn from(e: CryptoError) -> Self {
        CommandError::Crypto(e)
    }
}

impl From<CryptoError> for AuthenticatorError {
    fn from(e: CryptoError) -> Self {
        AuthenticatorError::HIDError(HIDError::Command(CommandError::Crypto(e)))
    }
}

pub struct U2FRegisterAnswer<'a> {
    pub certificate: &'a [u8],
    pub signature: &'a [u8],
}

pub fn parse_u2f_der_certificate(data: &[u8]) -> Result<U2FRegisterAnswer<'_>, CryptoError> {
    if data.len() < 4 {
        return Err(CryptoError::MalformedInput);
    }
    if data[0] != 0x30 {
        return Err(CryptoError::MalformedInput);
    }

    let end = if (data[1] & 0x80) == 0 {
        2 + data[1] as usize
    } else if data[1] == 0x81 {

        if data[2] < 128 {
            return Err(CryptoError::MalformedInput);
        }
        3 + data[2] as usize
    } else if data[1] == 0x82 {
        let l = u16::from_be_bytes([data[2], data[3]]);
        if l < 256 {
            return Err(CryptoError::MalformedInput);
        }
        4 + l as usize
    } else {
        return Err(CryptoError::MalformedInput);
    };

    if data.len() < end {
        return Err(CryptoError::MalformedInput);
    }

    Ok(U2FRegisterAnswer {
        certificate: &data[0..end],
        signature: &data[end..],
    })
}
