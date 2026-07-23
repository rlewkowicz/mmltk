use super::server::AuthenticatorExtensionsCredBlob;
use super::utils::{from_slice_stream, read_be_u16, read_be_u32, read_byte};
use crate::crypto::{COSEAlgorithm, CryptoError, SharedSecret};
use crate::ctap2::server::{CredentialProtectionPolicy, HMACGetSecretOutput, RpIdHash};
use crate::ctap2::utils::serde_parse_err;
use crate::{crypto::COSEKey, errors::AuthenticatorError};
use base64::Engine;
use serde::ser::{Error as SerError, SerializeMap, Serializer};
use serde::{
    de::{Error as SerdeError, Unexpected, Visitor},
    Deserialize, Deserializer, Serialize,
};
use serde_cbor;
use std::convert::TryInto;
use std::fmt;
use std::io::{Cursor, Read};

#[derive(Debug, PartialEq, Eq)]
pub enum HmacSecretResponse {
    /// This is returned by MakeCredential calls to display if CredRandom was
    /// successfully generated
    Confirmed(bool),
    /// This is returned by GetAssertion:
    /// AES256-CBC(shared_secret, HMAC-SHA265(CredRandom, salt1) || HMAC-SHA265(CredRandom, salt2))
    Secret(Vec<u8>),
}

impl HmacSecretResponse {
    /// Return the decrypted HMAC outputs, if this is an instance of [HmacSecretResponse::Secret].
    pub fn decrypt_secrets(
        &self,
        shared_secret: &SharedSecret,
    ) -> Option<Result<HMACGetSecretOutput, CryptoError>> {
        if let HmacSecretResponse::Secret(hmac_outputs) = self {
            Some(Self::decrypt_secrets_internal(shared_secret, hmac_outputs))
        } else {
            None
        }
    }

    fn decrypt_secrets_internal(
        shared_secret: &SharedSecret,
        hmac_outputs: &[u8],
    ) -> Result<HMACGetSecretOutput, CryptoError> {
        let output_secrets = shared_secret.decrypt(hmac_outputs)?;
        match if output_secrets.len() < 32 {
            Err(CryptoError::WrongSaltLength)
        } else {
            let (output1, output2) = output_secrets.split_at(32);
            Ok(HMACGetSecretOutput {
                output1: output1
                    .try_into()
                    .map_err(|_| CryptoError::WrongSaltLength)?,
                output2: (!output2.is_empty())
                    .then(|| output2.try_into().map_err(|_| CryptoError::WrongSaltLength))
                    .transpose()?,
            })
        } {
            err @ Err(CryptoError::WrongSaltLength) => {
                debug!(
                    "Bad hmac-secret output length: {} bytes (expected exactly 32 or 64)",
                    output_secrets.len()
                );
                err
            }
            other => other,
        }
    }
}

impl Serialize for HmacSecretResponse {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            HmacSecretResponse::Confirmed(x) => serializer.serialize_bool(*x),
            HmacSecretResponse::Secret(x) => serializer.serialize_bytes(x),
        }
    }
}
impl<'de> Deserialize<'de> for HmacSecretResponse {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct HmacSecretResponseVisitor;

        impl<'de> Visitor<'de> for HmacSecretResponseVisitor {
            type Value = HmacSecretResponse;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array or a boolean")
            }

            fn visit_bytes<E>(self, v: &[u8]) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                Ok(HmacSecretResponse::Secret(v.to_vec()))
            }

            fn visit_bool<E>(self, v: bool) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                Ok(HmacSecretResponse::Confirmed(v))
            }
        }
        deserializer.deserialize_any(HmacSecretResponseVisitor)
    }
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct Extension {
    #[serde(rename = "credProtect", skip_serializing_if = "Option::is_none")]
    pub cred_protect: Option<CredentialProtectionPolicy>,
    #[serde(rename = "hmac-secret", skip_serializing_if = "Option::is_none")]
    pub hmac_secret: Option<HmacSecretResponse>,
    #[serde(rename = "minPinLength", skip_serializing_if = "Option::is_none")]
    pub min_pin_length: Option<u64>,
    #[serde(rename = "credBlob", skip_serializing_if = "Option::is_none")]
    pub cred_blob: Option<AuthenticatorExtensionsCredBlob>,
}

impl Extension {
    pub fn has_some(&self) -> bool {
        self.min_pin_length.is_some()
            || self.hmac_secret.is_some()
            || self.cred_protect.is_some()
            || self.cred_blob.is_some()
    }
}

#[derive(Serialize, PartialEq, Default, Eq, Clone)]
pub struct AAGuid(pub [u8; 16]);

impl AAGuid {
    pub fn from(src: &[u8]) -> Result<AAGuid, AuthenticatorError> {
        let mut payload = [0u8; 16];
        if src.len() != payload.len() {
            Err(AuthenticatorError::InternalError(String::from(
                "Failed to parse AAGuid",
            )))
        } else {
            payload.copy_from_slice(src);
            Ok(AAGuid(payload))
        }
    }
}

impl fmt::Debug for AAGuid {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "AAGuid({:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x})",
            self.0[0],
            self.0[1],
            self.0[2],
            self.0[3],
            self.0[4],
            self.0[5],
            self.0[6],
            self.0[7],
            self.0[8],
            self.0[9],
            self.0[10],
            self.0[11],
            self.0[12],
            self.0[13],
            self.0[14],
            self.0[15]
        )
    }
}

impl<'de> Deserialize<'de> for AAGuid {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AAGuidVisitor;

        impl<'de> Visitor<'de> for AAGuidVisitor {
            type Value = AAGuid;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array")
            }

            fn visit_bytes<E>(self, v: &[u8]) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                let mut buf = [0u8; 16];
                if v.len() != buf.len() {
                    return Err(E::invalid_length(v.len(), &"16"));
                }

                buf.copy_from_slice(v);

                Ok(AAGuid(buf))
            }
        }

        deserializer.deserialize_bytes(AAGuidVisitor)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct AttestedCredentialData {
    pub aaguid: AAGuid,
    pub credential_id: Vec<u8>,
    pub credential_public_key: COSEKey,
}

fn parse_attested_cred_data<R: Read, E: SerdeError>(
    data: &mut R,
) -> Result<AttestedCredentialData, E> {
    let mut aaguid_raw = [0u8; 16];
    data.read_exact(&mut aaguid_raw)
        .map_err(|_| serde_parse_err("AAGuid"))?;
    let aaguid = AAGuid(aaguid_raw);
    let cred_len = read_be_u16(data)?;
    let mut credential_id = vec![0u8; cred_len as usize];
    data.read_exact(&mut credential_id)
        .map_err(|_| serde_parse_err("CredentialId"))?;
    let credential_public_key = from_slice_stream(data)?;
    Ok(AttestedCredentialData {
        aaguid,
        credential_id,
        credential_public_key,
    })
}

bitflags! {
    pub struct AuthenticatorDataFlags: u8 {
        const USER_PRESENT = 0x01;
        const RESERVED_1 = 0x02;
        const USER_VERIFIED = 0x04;
        const RESERVED_3 = 0x08;
        const RESERVED_4 = 0x10;
        const RESERVED_5 = 0x20;
        const ATTESTED = 0x40;
        const EXTENSION_DATA = 0x80;
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct AuthenticatorData {
    pub rp_id_hash: RpIdHash,
    pub flags: AuthenticatorDataFlags,
    pub counter: u32,
    pub credential_data: Option<AttestedCredentialData>,
    pub extensions: Extension,
}

impl AuthenticatorData {
    pub fn to_vec(&self) -> Vec<u8> {
        match serde_cbor::value::to_value(self) {
            Ok(serde_cbor::value::Value::Bytes(out)) => out,
            _ => unreachable!(), 
        }
    }
}

impl<'de> Deserialize<'de> for AuthenticatorData {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AuthenticatorDataVisitor;

        impl<'de> Visitor<'de> for AuthenticatorDataVisitor {
            type Value = AuthenticatorData;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array")
            }

            fn visit_bytes<E>(self, input: &[u8]) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                let mut cursor = Cursor::new(input);
                let mut rp_id_hash_raw = [0u8; 32];
                cursor
                    .read_exact(&mut rp_id_hash_raw)
                    .map_err(|_| serde_parse_err("32 bytes"))?;
                let rp_id_hash = RpIdHash(rp_id_hash_raw);

                let flags = AuthenticatorDataFlags::from_bits_truncate(read_byte(&mut cursor)?);
                let counter = read_be_u32(&mut cursor)?;
                let mut credential_data = None;
                if flags.contains(AuthenticatorDataFlags::ATTESTED) {
                    credential_data = Some(parse_attested_cred_data(&mut cursor)?);
                }

                let extensions = if flags.contains(AuthenticatorDataFlags::EXTENSION_DATA) {
                    from_slice_stream(&mut cursor)?
                } else {
                    Default::default()
                };

                Ok(AuthenticatorData {
                    rp_id_hash,
                    flags,
                    counter,
                    credential_data,
                    extensions,
                })
            }
        }

        deserializer.deserialize_bytes(AuthenticatorDataVisitor)
    }
}

impl Serialize for AuthenticatorData {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        let mut data = Vec::new();
        data.extend(self.rp_id_hash.0); 
        data.extend([self.flags.bits()]); 
        data.extend(self.counter.to_be_bytes()); 

        if let Some(cred) = &self.credential_data {
            data.extend(cred.aaguid.0); 
            data.extend((cred.credential_id.len() as u16).to_be_bytes()); 
            data.extend(&cred.credential_id); 
            data.extend(
                &serde_cbor::to_vec(&cred.credential_public_key)
                    .map_err(|_| SerError::custom("Failed to serialize auth_data"))?,
            );
        }
        if self.extensions.has_some() || self.flags.contains(AuthenticatorDataFlags::EXTENSION_DATA)
        {
            data.extend(
                &serde_cbor::to_vec(&self.extensions)
                    .map_err(|_| SerError::custom("Failed to serialize auth_data"))?,
            );
        }

        serializer.serialize_bytes(&data)
    }
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
/// x509 encoded attestation certificate
pub struct AttestationCertificate(#[serde(with = "serde_bytes")] pub Vec<u8>);

impl AsRef<[u8]> for AttestationCertificate {
    fn as_ref(&self) -> &[u8] {
        self.0.as_ref()
    }
}

#[derive(Serialize, Deserialize, PartialEq, Eq)]
pub struct Signature(#[serde(with = "serde_bytes")] pub Vec<u8>);

impl fmt::Debug for Signature {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let value = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(&self.0);
        write!(f, "Signature({value})")
    }
}

impl AsRef<[u8]> for Signature {
    fn as_ref(&self) -> &[u8] {
        self.0.as_ref()
    }
}

impl From<&[u8]> for Signature {
    fn from(sig: &[u8]) -> Signature {
        Signature(sig.to_vec())
    }
}

#[derive(Debug, PartialEq, Eq, Deserialize)]
#[serde(tag = "fmt", content = "attStmt", rename_all = "lowercase")]
pub enum AttestationStatement {
    #[serde(deserialize_with = "deserialize_none_att_stmt")]
    None,
    Packed(AttestationStatementPacked),
    #[serde(rename = "fido-u2f")]
    FidoU2F(AttestationStatementFidoU2F),
    #[serde(rename = "android-key")]
    AndroidKey(serde_cbor::Value),
    #[serde(rename = "android-safetynet")]
    AndroidSafetyNet(serde_cbor::Value),
    Apple(serde_cbor::Value),
    Tpm(serde_cbor::Value),
}

impl AttestationStatement {
    /// The [attestation statement format identifier][att-fmt-id].
    ///
    /// [att-fmt-id]: https://w3c.github.io/webauthn/#attestation-statement-format-identifier
    pub fn id(&self) -> &str {
        match self {
            Self::None => "none",
            Self::Packed(..) => "packed",
            Self::FidoU2F(..) => "fido-u2f",
            Self::AndroidKey(..) => "android-key",
            Self::AndroidSafetyNet(..) => "android-safetynet",
            Self::Apple(..) => "apple",
            Self::Tpm(..) => "tpm",
        }
    }
}

impl Serialize for AttestationStatement {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            Self::None => serializer.serialize_map(Some(0))?.end(),
            Self::Packed(ref v) => serializer.serialize_some(v),
            Self::FidoU2F(ref v) => serializer.serialize_some(v),
            Self::AndroidKey(ref v) => serializer.serialize_some(v),
            Self::AndroidSafetyNet(ref v) => serializer.serialize_some(v),
            Self::Apple(ref v) => serializer.serialize_some(v),
            Self::Tpm(ref v) => serializer.serialize_some(v),
        }
    }
}

fn deserialize_none_att_stmt<'de, D>(deserializer: D) -> Result<(), D::Error>
where
    D: Deserializer<'de>,
{
    let map = <std::collections::BTreeMap<(), ()>>::deserialize(deserializer)?;

    if !map.is_empty() {
        return Err(D::Error::invalid_value(Unexpected::Map, &"the empty map"));
    }

    Ok(())
}


#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AttestationStatementFidoU2F {
    pub sig: Signature, 
    /// Certificate chain in x509 format
    #[serde(rename = "x5c")]
    pub attestation_cert: Vec<AttestationCertificate>, 
}

impl AttestationStatementFidoU2F {
    pub fn new(cert: &[u8], signature: &[u8]) -> Self {
        AttestationStatementFidoU2F {
            attestation_cert: vec![AttestationCertificate(Vec::from(cert))],
            sig: Signature::from(signature),
        }
    }
}

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AttestationStatementPacked {
    pub alg: COSEAlgorithm, 
    pub sig: Signature,     
    /// Certificate chain in x509 format
    #[serde(rename = "x5c", skip_serializing_if = "Vec::is_empty", default)]
    pub attestation_cert: Vec<AttestationCertificate>, 
}

#[derive(Debug, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AttestationObject {
    pub auth_data: AuthenticatorData,
    #[serde(flatten)]
    pub att_stmt: AttestationStatement,
}

impl AttestationObject {
    pub fn anonymize(&mut self) {
        if let AttestationStatement::Packed(ref packed) = self.att_stmt {
            if packed.attestation_cert.is_empty()
                && self
                    .auth_data
                    .credential_data
                    .as_ref()
                    .is_some_and(|d| d.aaguid == AAGuid::default())
            {
                return;
            }
        }
        self.att_stmt = AttestationStatement::None;
    }
}

impl Serialize for AttestationObject {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serialize_map!(
            serializer,
            &"fmt" => self.att_stmt.id(),
            &"attStmt" => &self.att_stmt,
            &"authData" => &self.auth_data,
        )
    }
}
