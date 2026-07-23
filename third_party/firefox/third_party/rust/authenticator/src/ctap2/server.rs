use super::commands::get_assertion::HmacSecretExtension;
use crate::crypto::{COSEAlgorithm, CryptoError, PinUvAuthToken, SharedSecret};
use crate::{errors::AuthenticatorError, AuthenticatorTransports, KeyHandle};
use base64::Engine;
use serde::de::MapAccess;
use serde::{
    de::{Error as SerdeError, Unexpected, Visitor},
    Deserialize, Deserializer, Serialize, Serializer,
};
use serde_bytes::{ByteBuf, Bytes};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::convert::{Into, TryFrom};
use std::fmt;

#[derive(Serialize, Deserialize, PartialEq, Eq, Clone)]
pub struct RpIdHash(pub [u8; 32]);

impl fmt::Debug for RpIdHash {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let value = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(self.0);
        write!(f, "RpIdHash({value})")
    }
}

impl AsRef<[u8]> for RpIdHash {
    fn as_ref(&self) -> &[u8] {
        self.0.as_ref()
    }
}

impl RpIdHash {
    pub fn from(src: &[u8]) -> Result<RpIdHash, AuthenticatorError> {
        let mut payload = [0u8; 32];
        if src.len() != payload.len() {
            Err(AuthenticatorError::InvalidRelyingPartyInput)
        } else {
            payload.copy_from_slice(src);
            Ok(RpIdHash(payload))
        }
    }
}

#[derive(Debug, Serialize, Clone, Default, Deserialize, PartialEq, Eq)]
pub struct RelyingParty {
    pub id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
}

impl RelyingParty {
    pub fn from<S>(id: S) -> Self
    where
        S: Into<String>,
    {
        Self {
            id: id.into(),
            name: None,
        }
    }

    pub fn hash(&self) -> RpIdHash {
        RpIdHash(Sha256::digest(&self.id).into())
    }
}

#[derive(Debug, Serialize, Clone, Eq, PartialEq, Deserialize, Default)]
pub struct PublicKeyCredentialUserEntity {
    #[serde(with = "serde_bytes")]
    pub id: Vec<u8>,
    pub name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none", rename = "displayName")]
    pub display_name: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PublicKeyCredentialParameters {
    pub alg: COSEAlgorithm,
}

impl TryFrom<i32> for PublicKeyCredentialParameters {
    type Error = AuthenticatorError;
    fn try_from(arg: i32) -> Result<Self, Self::Error> {
        let alg = COSEAlgorithm::try_from(arg as i64)?;
        Ok(PublicKeyCredentialParameters { alg })
    }
}

impl Serialize for PublicKeyCredentialParameters {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serialize_map!(
            serializer,
            "alg" => &self.alg,
            "type" => "public-key",
        )
    }
}

impl<'de> Deserialize<'de> for PublicKeyCredentialParameters {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct PublicKeyCredentialParametersVisitor;

        impl<'de> Visitor<'de> for PublicKeyCredentialParametersVisitor {
            type Value = PublicKeyCredentialParameters;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a map")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut found_type = false;
                let mut alg = None;
                while let Some(key) = map.next_key()? {
                    match key {
                        "alg" => {
                            if alg.is_some() {
                                return Err(SerdeError::duplicate_field("alg"));
                            }
                            alg = Some(map.next_value()?);
                        }
                        "type" => {
                            if found_type {
                                return Err(SerdeError::duplicate_field("type"));
                            }

                            let v: &str = map.next_value()?;
                            if v != "public-key" {
                                return Err(SerdeError::custom(format!("invalid value: {v}")));
                            }
                            found_type = true;
                        }
                        v => {
                            return Err(SerdeError::unknown_field(v, &[]));
                        }
                    }
                }

                if !found_type {
                    return Err(SerdeError::missing_field("type"));
                }

                let alg = alg.ok_or_else(|| SerdeError::missing_field("alg"))?;

                Ok(PublicKeyCredentialParameters { alg })
            }
        }

        deserializer.deserialize_bytes(PublicKeyCredentialParametersVisitor)
    }
}

#[derive(Debug, PartialEq, Serialize, Deserialize, Eq, Clone)]
#[serde(rename_all = "lowercase")]
pub enum Transport {
    USB,
    NFC,
    BLE,
    Internal,
}

impl From<AuthenticatorTransports> for Vec<Transport> {
    fn from(t: AuthenticatorTransports) -> Self {
        let mut transports = Vec::new();
        if t.contains(AuthenticatorTransports::USB) {
            transports.push(Transport::USB);
        }
        if t.contains(AuthenticatorTransports::NFC) {
            transports.push(Transport::NFC);
        }
        if t.contains(AuthenticatorTransports::BLE) {
            transports.push(Transport::BLE);
        }

        transports
    }
}

pub type PublicKeyCredentialId = Vec<u8>;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PublicKeyCredentialDescriptor {
    pub id: PublicKeyCredentialId,
    pub transports: Vec<Transport>,
}

impl Serialize for PublicKeyCredentialDescriptor {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serialize_map!(
            serializer,
            "id" => Bytes::new(&self.id),
            "type" => "public-key",

        )
    }
}

impl<'de> Deserialize<'de> for PublicKeyCredentialDescriptor {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct PublicKeyCredentialDescriptorVisitor;

        impl<'de> Visitor<'de> for PublicKeyCredentialDescriptorVisitor {
            type Value = PublicKeyCredentialDescriptor;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a map")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut found_type = false;
                let mut id = None;
                let mut transports = None;
                while let Some(key) = map.next_key()? {
                    match key {
                        "id" => {
                            if id.is_some() {
                                return Err(SerdeError::duplicate_field("id"));
                            }
                            let id_bytes: ByteBuf = map.next_value()?;
                            id = Some(id_bytes.into_vec());
                        }
                        "transports" => {
                            if transports.is_some() {
                                return Err(SerdeError::duplicate_field("transports"));
                            }
                            transports = Some(map.next_value()?);
                        }
                        "type" => {
                            if found_type {
                                return Err(SerdeError::duplicate_field("type"));
                            }
                            let v: &str = map.next_value()?;
                            if v != "public-key" {
                                return Err(SerdeError::custom(format!("invalid value: {v}")));
                            }
                            found_type = true;
                        }
                        v => {
                            return Err(SerdeError::unknown_field(v, &[]));
                        }
                    }
                }

                if !found_type {
                    return Err(SerdeError::missing_field("type"));
                }

                let id = id.ok_or_else(|| SerdeError::missing_field("id"))?;
                let transports = transports.unwrap_or_default();

                Ok(PublicKeyCredentialDescriptor { id, transports })
            }
        }

        deserializer.deserialize_any(PublicKeyCredentialDescriptorVisitor)
    }
}

impl From<&KeyHandle> for PublicKeyCredentialDescriptor {
    fn from(kh: &KeyHandle) -> Self {
        Self {
            id: kh.credential.clone(),
            transports: kh.transports.into(),
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum ResidentKeyRequirement {
    Discouraged,
    Preferred,
    Required,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum UserVerificationRequirement {
    Discouraged,
    Preferred,
    Required,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum CredentialProtectionPolicy {
    UserVerificationOptional = 1,
    UserVerificationOptionalWithCredentialIDList = 2,
    UserVerificationRequired = 3,
}

impl Serialize for CredentialProtectionPolicy {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_u64(*self as u64)
    }
}

impl<'de> Deserialize<'de> for CredentialProtectionPolicy {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct CredentialProtectionPolicyVisitor;

        impl<'de> Visitor<'de> for CredentialProtectionPolicyVisitor {
            type Value = CredentialProtectionPolicy;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("an integer")
            }

            fn visit_u64<E>(self, v: u64) -> Result<Self::Value, E>
            where
                E: SerdeError,
            {
                match v {
                    1 => Ok(CredentialProtectionPolicy::UserVerificationOptional),
                    2 => Ok(
                        CredentialProtectionPolicy::UserVerificationOptionalWithCredentialIDList,
                    ),
                    3 => Ok(CredentialProtectionPolicy::UserVerificationRequired),
                    _ => Err(SerdeError::invalid_value(
                        Unexpected::Unsigned(v),
                        &"valid CredentialProtectionPolicy",
                    )),
                }
            }
        }

        deserializer.deserialize_any(CredentialProtectionPolicyVisitor)
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum AuthenticatorExtensionsCredBlob {
    /// Used in GetAssertion-requests to request the stored blob,
    /// and in MakeCredential-responses to signify if the
    /// storing worked.
    AsBool(bool),
    /// Used in MakeCredential-requests to store a new credBlob,
    /// and in GetAssertion-responses when retrieving the
    /// stored blob.
    #[serde(serialize_with = "vec_to_bytebuf", deserialize_with = "bytebuf_to_vec")]
    AsBytes(Vec<u8>),
}

fn vec_to_bytebuf<S>(data: &[u8], s: S) -> Result<S::Ok, S::Error>
where
    S: Serializer,
{
    ByteBuf::from(data).serialize(s)
}

fn bytebuf_to_vec<'de, D>(deserializer: D) -> Result<Vec<u8>, D::Error>
where
    D: Deserializer<'de>,
{
    let bytes = <ByteBuf>::deserialize(deserializer)?;
    Ok(bytes.to_vec())
}

#[derive(Clone, Debug, Default)]
pub struct AuthenticationExtensionsClientInputs {
    pub app_id: Option<String>,
    pub cred_props: Option<bool>,
    pub credential_protection_policy: Option<CredentialProtectionPolicy>,
    pub enforce_credential_protection_policy: Option<bool>,
    pub hmac_create_secret: Option<bool>,
    pub hmac_get_secret: Option<HMACGetSecretInput>,
    pub min_pin_length: Option<bool>,
    pub prf: Option<AuthenticationExtensionsPRFInputs>,
    /// MakeCredential-requests use AsBytes
    /// GetAssertion-requests use AsBool
    pub cred_blob: Option<AuthenticatorExtensionsCredBlob>,
    pub large_blob_key: Option<bool>,
    pub third_party_payment: Option<bool>,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct CredentialProperties {
    pub rk: bool,
}

/// Salt inputs for the `hmac-secret` extension.
/// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-20210615.html#dictdef-hmacgetsecretinput
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct HMACGetSecretInput {
    pub salt1: [u8; 32],
    pub salt2: Option<[u8; 32]>,
}

/// Decrypted HMAC outputs from the `hmac-secret` extension.
/// https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-20210615.html#dictdef-hmacgetsecretoutput
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct HMACGetSecretOutput {
    pub output1: [u8; 32],
    pub output2: Option<[u8; 32]>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct AuthenticationExtensionsPRFInputs {
    pub eval: Option<AuthenticationExtensionsPRFValues>,
    pub eval_by_credential: Option<HashMap<Vec<u8>, AuthenticationExtensionsPRFValues>>,
}

impl AuthenticationExtensionsPRFInputs {
    /// Select an `eval` or `evalByCredential` entry and calculate hmac-secret salt inputs from those inputs.
    ///
    /// Returns [None] if the `eval` input was not given and no credential in `allow_credentials` matched any `evalByCredential` entry.
    /// Otherwise returns the initialized [HmacSecretExtension] and, if an `evalByCredential` entry was used to compute the salt inputs,
    /// the [PublicKeyCredentialDescriptor] matching that `evalByCredential` entry.
    /// If present, `allowCredentials` SHOULD be set to contain only that [PublicKeyCredentialDescriptor] value.
    pub fn calculate<'allow_cred>(
        &self,
        secret: &SharedSecret,
        allow_credentials: &'allow_cred [PublicKeyCredentialDescriptor],
        puat: Option<&PinUvAuthToken>,
    ) -> Result<
        Option<(
            HmacSecretExtension,
            Option<&'allow_cred PublicKeyCredentialDescriptor>,
        )>,
        CryptoError,
    > {
        if let Some((selected_credential, ev)) = self.select_eval(allow_credentials) {
            let mut hmac_secret = HmacSecretExtension::new(
                Self::eval_to_salt(&ev.first).to_vec(),
                ev.second
                    .as_ref()
                    .map(|second| Self::eval_to_salt(second).to_vec()),
            );
            hmac_secret.calculate(secret, puat)?;
            Ok(Some((hmac_secret, selected_credential)))
        } else {
            Ok(None)
        }
    }

    /// Select an `evalByCredential` entry matching any element of `allow_credentials`,
    /// or otherwise fall back to `eval`, if present, if no match is found.
    fn select_eval<'allow_cred>(
        &self,
        allow_credentials: &'allow_cred [PublicKeyCredentialDescriptor],
    ) -> Option<(
        Option<&'allow_cred PublicKeyCredentialDescriptor>,
        &AuthenticationExtensionsPRFValues,
    )> {
        self.select_credential(allow_credentials)
            .map(|(cred, ev)| (Some(cred), ev))
            .or(self.eval.as_ref().map(|eval| (None, eval)))
    }

    /// Select an `evalByCredential` entry matching any element of `allow_credentials`.
    fn select_credential<'allow_cred>(
        &self,
        allow_credentials: &'allow_cred [PublicKeyCredentialDescriptor],
    ) -> Option<(
        &'allow_cred PublicKeyCredentialDescriptor,
        &AuthenticationExtensionsPRFValues,
    )> {
        self.eval_by_credential
            .as_ref()
            .and_then(|eval_by_credential| {
                allow_credentials
                    .iter()
                    .find_map(|pkcd| eval_by_credential.get(&pkcd.id).map(|eval| (pkcd, eval)))
            })
    }

    /// Convert a PRF eval input to an hmac-secret salt input.
    fn eval_to_salt(eval: &[u8]) -> [u8; 32] {
        Sha256::new_with_prefix(b"WebAuthn PRF")
            .chain_update([0x00].iter())
            .chain_update(eval.iter())
            .finalize()
            .into()
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct AuthenticationExtensionsPRFValues {
    pub first: Vec<u8>,
    pub second: Option<Vec<u8>>,
}

impl From<HMACGetSecretOutput> for AuthenticationExtensionsPRFValues {
    fn from(hmac_output: HMACGetSecretOutput) -> Self {
        Self {
            first: hmac_output.output1.to_vec(),
            second: hmac_output.output2.map(|o2| o2.to_vec()),
        }
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct AuthenticationExtensionsPRFOutputs {
    pub enabled: Option<bool>,
    pub results: Option<AuthenticationExtensionsPRFValues>,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct AuthenticationExtensionsClientOutputs {
    pub app_id: Option<bool>,
    pub cred_props: Option<CredentialProperties>,
    pub hmac_create_secret: Option<bool>,
    pub hmac_get_secret: Option<HMACGetSecretOutput>,
    pub prf: Option<AuthenticationExtensionsPRFOutputs>,
    /// MakeCredential-responses use AsBool
    /// GetAssertion-responses use AsBytes
    pub cred_blob: Option<AuthenticatorExtensionsCredBlob>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AuthenticatorAttachment {
    CrossPlatform,
    Platform,
    Unknown,
}
