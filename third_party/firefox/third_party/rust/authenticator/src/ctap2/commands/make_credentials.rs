use super::get_info::{AuthenticatorInfo, AuthenticatorVersion};
use super::{
    Command, CommandError, CtapResponse, PinUvAuthCommand, RequestCtap1, RequestCtap2, Retryable,
    StatusCode,
};
use crate::consts::{PARAMETER_SIZE, U2F_REGISTER, U2F_REQUEST_USER_PRESENCE};
use crate::crypto::{
    parse_u2f_der_certificate, COSEAlgorithm, COSEEC2Key, COSEKey, COSEKeyType, Curve,
    PinUvAuthParam, PinUvAuthToken,
};
use crate::ctap2::attestation::{
    AAGuid, AttestationObject, AttestationStatement, AttestationStatementFidoU2F,
    AttestedCredentialData, AuthenticatorData, AuthenticatorDataFlags, HmacSecretResponse,
};
use crate::ctap2::client_data::ClientDataHash;
use crate::ctap2::server::{
    AuthenticationExtensionsClientInputs, AuthenticationExtensionsClientOutputs,
    AuthenticationExtensionsPRFOutputs, AuthenticatorAttachment, AuthenticatorExtensionsCredBlob,
    CredentialProtectionPolicy, PublicKeyCredentialDescriptor, PublicKeyCredentialParameters,
    PublicKeyCredentialUserEntity, RelyingParty, RpIdHash, UserVerificationRequirement,
};
use crate::ctap2::utils::{read_byte, serde_parse_err};
use crate::errors::AuthenticatorError;
use crate::transport::errors::{ApduErrorStatus, HIDError};
use crate::transport::{FidoDevice, VirtualFidoDevice};
use crate::u2ftypes::CTAP1RequestAPDU;
use serde::{
    de::{Error as DesError, MapAccess, Unexpected, Visitor},
    Deserialize, Deserializer, Serialize, Serializer,
};
use serde_bytes::ByteBuf;
use serde_cbor::{self, de::from_slice, ser, Value};
use std::collections::HashMap;
use std::fmt;
use std::io::{Cursor, Read};

#[derive(Debug, PartialEq, Eq)]
pub struct MakeCredentialsResult {
    pub att_obj: AttestationObject,
    pub attachment: AuthenticatorAttachment,
    pub extensions: AuthenticationExtensionsClientOutputs,
    pub ep_attestation: Option<bool>,
    pub large_blob_key: Option<Vec<u8>>,
    pub unsigned_extension_outputs: Option<HashMap<String, serde_cbor::Value>>,
}

impl MakeCredentialsResult {
    pub fn from_ctap1(input: &[u8], rp_id_hash: &RpIdHash) -> Result<Self, CommandError> {
        let mut data = Cursor::new(input);
        let magic_num = read_byte(&mut data).map_err(CommandError::Deserializing)?;
        if magic_num != 0x05 {
            error!("error while parsing registration: magic header not 0x05, but {magic_num}");
            return Err(CommandError::Deserializing(DesError::invalid_value(
                serde::de::Unexpected::Unsigned(magic_num as u64),
                &"0x05",
            )));
        }
        let mut public_key = [0u8; 65];
        data.read_exact(&mut public_key)
            .map_err(|_| CommandError::Deserializing(serde_parse_err("PublicKey")))?;

        let credential_id_len = read_byte(&mut data).map_err(CommandError::Deserializing)?;
        let mut credential_id = vec![0u8; credential_id_len as usize];
        data.read_exact(&mut credential_id)
            .map_err(|_| CommandError::Deserializing(serde_parse_err("CredentialId")))?;

        let cert_and_sig = parse_u2f_der_certificate(&data.get_ref()[data.position() as usize..])
            .map_err(|err| {
            CommandError::Deserializing(serde_parse_err(&format!(
                "Certificate and Signature: {err:?}",
            )))
        })?;

        let credential_ec2_key = COSEEC2Key::from_sec1_uncompressed(Curve::SECP256R1, &public_key)
            .map_err(|err| {
                CommandError::Deserializing(serde_parse_err(&format!("EC2 Key: {err:?}",)))
            })?;

        let credential_public_key = COSEKey {
            alg: COSEAlgorithm::ES256,
            key: COSEKeyType::EC2(credential_ec2_key),
        };

        let auth_data = AuthenticatorData {
            rp_id_hash: rp_id_hash.clone(),
            flags: AuthenticatorDataFlags::USER_PRESENT | AuthenticatorDataFlags::ATTESTED,
            counter: 0,
            credential_data: Some(AttestedCredentialData {
                aaguid: AAGuid::default(),
                credential_id,
                credential_public_key,
            }),
            extensions: Default::default(),
        };

        let att_stmt = AttestationStatement::FidoU2F(AttestationStatementFidoU2F::new(
            cert_and_sig.certificate,
            cert_and_sig.signature,
        ));

        let att_obj = AttestationObject {
            auth_data,
            att_stmt,
        };

        Ok(Self {
            att_obj,
            attachment: AuthenticatorAttachment::Unknown,
            extensions: Default::default(),
            ep_attestation: None,
            large_blob_key: None,
            unsigned_extension_outputs: None,
        })
    }
}

impl<'de> Deserialize<'de> for MakeCredentialsResult {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct MakeCredentialsResultVisitor;

        impl<'de> Visitor<'de> for MakeCredentialsResultVisitor {
            type Value = MakeCredentialsResult;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a cbor map")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut format: Option<&str> = None;
                let mut auth_data: Option<AuthenticatorData> = None;
                let mut att_stmt: Option<AttestationStatement> = None;
                let mut ep_attestation: Option<bool> = None;
                let mut large_blob_key: Option<Vec<u8>> = None;
                let mut unsigned_extension_outputs: Option<HashMap<String, serde_cbor::Value>> =
                    None;

                while let Some(key) = map.next_key()? {
                    match key {
                        0x01 => {
                            if format.is_some() {
                                return Err(DesError::duplicate_field("fmt (0x01)"));
                            }
                            format = Some(map.next_value()?);
                        }
                        0x02 => {
                            if auth_data.is_some() {
                                return Err(DesError::duplicate_field("authData (0x02)"));
                            }
                            auth_data = Some(map.next_value()?);
                        }
                        0x03 => {
                            let format =
                                format.ok_or_else(|| DesError::missing_field("fmt (0x01)"))?;
                            if att_stmt.is_some() {
                                return Err(DesError::duplicate_field("attStmt (0x03)"));
                            }
                            att_stmt = match format {
                                "none" => {
                                    let map: std::collections::BTreeMap<(), ()> =
                                        map.next_value()?;
                                    if !map.is_empty() {
                                        return Err(DesError::invalid_value(
                                            Unexpected::Map,
                                            &"the empty map",
                                        ));
                                    }
                                    Some(AttestationStatement::None)
                                }
                                "packed" => Some(AttestationStatement::Packed(map.next_value()?)),
                                "fido-u2f" => {
                                    Some(AttestationStatement::FidoU2F(map.next_value()?))
                                }
                                _ => {
                                    return Err(DesError::custom(
                                        "unknown attestation statement format",
                                    ))
                                }
                            }
                        }
                        0x04 => {
                            if ep_attestation.is_some() {
                                return Err(M::Error::duplicate_field("ep_attestation"));
                            }
                            let ep_attestation_val: bool = map.next_value()?;
                            ep_attestation = Some(ep_attestation_val);
                        }
                        0x05 => {
                            if large_blob_key.is_some() {
                                return Err(M::Error::duplicate_field("large_blob_key"));
                            }
                            let large_blob_key_bytes: ByteBuf = map.next_value()?;
                            large_blob_key = Some(large_blob_key_bytes.into_vec());
                        }
                        0x06 => {
                            if unsigned_extension_outputs.is_some() {
                                return Err(M::Error::duplicate_field(
                                    "unsigned_extension_outputs",
                                ));
                            }
                            unsigned_extension_outputs = Some(map.next_value()?);
                        }
                        _ => continue,
                    }
                }

                let auth_data = auth_data
                    .ok_or_else(|| M::Error::custom("found no authData (0x02)".to_string()))?;
                let att_stmt = att_stmt
                    .ok_or_else(|| M::Error::custom("found no attStmt (0x03)".to_string()))?;

                Ok(MakeCredentialsResult {
                    att_obj: AttestationObject {
                        auth_data,
                        att_stmt,
                    },
                    attachment: AuthenticatorAttachment::Unknown,
                    extensions: Default::default(),
                    ep_attestation,
                    large_blob_key,
                    unsigned_extension_outputs,
                })
            }
        }

        deserializer.deserialize_bytes(MakeCredentialsResultVisitor)
    }
}

impl CtapResponse for MakeCredentialsResult {}

#[derive(Copy, Clone, Debug, Default, Serialize)]
pub struct MakeCredentialsOptions {
    #[serde(rename = "rk", skip_serializing_if = "Option::is_none")]
    pub resident_key: Option<bool>,
    #[serde(rename = "uv", skip_serializing_if = "Option::is_none")]
    pub user_verification: Option<bool>,
}

impl MakeCredentialsOptions {
    pub(crate) fn has_some(&self) -> bool {
        self.resident_key.is_some() || self.user_verification.is_some()
    }
}

pub(crate) trait UserVerification {
    fn ask_user_verification(&self) -> bool;
}

impl UserVerification for MakeCredentialsOptions {
    fn ask_user_verification(&self) -> bool {
        self.user_verification.unwrap_or(false)
    }
}

#[derive(Debug, Default, Clone, Serialize)]
pub struct MakeCredentialsExtensions {
    #[serde(skip_serializing)]
    pub cred_props: Option<bool>,
    #[serde(rename = "credProtect", skip_serializing_if = "Option::is_none")]
    pub cred_protect: Option<CredentialProtectionPolicy>,
    #[serde(rename = "hmac-secret", skip_serializing_if = "Option::is_none")]
    pub hmac_secret: Option<HmacCreateSecretOrPrf>,
    #[serde(rename = "minPinLength", skip_serializing_if = "Option::is_none")]
    pub min_pin_length: Option<bool>,
    #[serde(rename = "credBlob", skip_serializing_if = "Option::is_none")]
    pub cred_blob: Option<AuthenticatorExtensionsCredBlob>,
    #[serde(rename = "largeBlobKey", skip_serializing_if = "Option::is_none")]
    pub large_blob_key: Option<bool>,
    #[serde(rename = "thirdPartyPayment", skip_serializing_if = "Option::is_none")]
    pub third_party_payment: Option<bool>,
}

#[derive(Debug, Clone)]
pub enum HmacCreateSecretOrPrf {
    HmacCreateSecret(bool),
    Prf,
}

impl Serialize for HmacCreateSecretOrPrf {
    fn serialize<S>(&self, s: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            Self::HmacCreateSecret(hmac_secret) => s.serialize_bool(*hmac_secret),
            Self::Prf => s.serialize_bool(true),
        }
    }
}

impl MakeCredentialsExtensions {
    fn has_content(&self) -> bool {
        self.cred_protect.is_some()
            || self.hmac_secret.is_some()
            || self.min_pin_length.is_some()
            || self.cred_blob.is_some()
            || self.large_blob_key.is_some()
            || self.third_party_payment.is_some()
    }
}

impl From<AuthenticationExtensionsClientInputs> for MakeCredentialsExtensions {
    fn from(input: AuthenticationExtensionsClientInputs) -> Self {
        Self {
            cred_props: input.cred_props,
            cred_protect: input.credential_protection_policy,
            hmac_secret: match (input.hmac_create_secret, input.prf) {
                (None, None) => None,
                (_, Some(_)) => Some(HmacCreateSecretOrPrf::Prf),
                (Some(hmac_secret), _) => {
                    Some(HmacCreateSecretOrPrf::HmacCreateSecret(hmac_secret))
                }
            },
            min_pin_length: input.min_pin_length,
            cred_blob: input.cred_blob,
            large_blob_key: input.large_blob_key,
            third_party_payment: input.third_party_payment,
        }
    }
}

#[derive(Debug, Clone)]
pub struct MakeCredentials {
    pub client_data_hash: ClientDataHash,
    pub rp: RelyingParty,
    pub user: Option<PublicKeyCredentialUserEntity>,
    pub pub_cred_params: Vec<PublicKeyCredentialParameters>,
    pub exclude_list: Vec<PublicKeyCredentialDescriptor>,

    pub extensions: MakeCredentialsExtensions,
    pub options: MakeCredentialsOptions,
    pub pin_uv_auth_param: Option<PinUvAuthParam>,
    pub enterprise_attestation: Option<u64>,

    pub attestation_formats_preference: Option<Vec<String>>,
}

impl MakeCredentials {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        client_data_hash: ClientDataHash,
        rp: RelyingParty,
        user: Option<PublicKeyCredentialUserEntity>,
        pub_cred_params: Vec<PublicKeyCredentialParameters>,
        exclude_list: Vec<PublicKeyCredentialDescriptor>,
        options: MakeCredentialsOptions,
        extensions: MakeCredentialsExtensions,
    ) -> Self {
        Self {
            client_data_hash,
            rp,
            user,
            pub_cred_params,
            exclude_list,
            extensions,
            options,
            pin_uv_auth_param: None,
            enterprise_attestation: None,
            attestation_formats_preference: None,
        }
    }

    pub fn finalize_result<Dev: FidoDevice>(&self, dev: &Dev, result: &mut MakeCredentialsResult) {
        let maybe_info = dev.get_authenticator_info();

        result.attachment = match maybe_info {
            Some(info) if info.options.platform_device => AuthenticatorAttachment::Platform,
            Some(_) => AuthenticatorAttachment::CrossPlatform,
            None => AuthenticatorAttachment::Unknown,
        };

        let dev_supports_rk = maybe_info.is_some_and(|info| info.options.resident_key);
        let requested_rk = self.options.resident_key.unwrap_or(false);
        let max_supported_version = maybe_info.map_or(AuthenticatorVersion::U2F_V2, |info| {
            info.max_supported_version()
        });
        let rk_uncertain = max_supported_version == AuthenticatorVersion::FIDO_2_0
            && dev_supports_rk
            && !requested_rk;
        if self.extensions.cred_props == Some(true) && !rk_uncertain {
            result
                .extensions
                .cred_props
                .get_or_insert(Default::default())
                .rk = requested_rk;
        }

        match self.extensions.hmac_secret {
            Some(HmacCreateSecretOrPrf::HmacCreateSecret(true)) => {
                result.extensions.hmac_create_secret =
                    Some(match result.att_obj.auth_data.extensions.hmac_secret {
                        Some(HmacSecretResponse::Confirmed(flag)) => flag,
                        Some(HmacSecretResponse::Secret(_)) => true,
                        None => false,
                    });
            }
            Some(HmacCreateSecretOrPrf::Prf) => {
                result.extensions.prf =
                    Some(match &result.att_obj.auth_data.extensions.hmac_secret {
                        None => AuthenticationExtensionsPRFOutputs {
                            enabled: Some(false),
                            results: None,
                        },
                        Some(HmacSecretResponse::Confirmed(flag)) => {
                            AuthenticationExtensionsPRFOutputs {
                                enabled: Some(*flag),
                                results: None,
                            }
                        }
                        Some(hmac_response @ HmacSecretResponse::Secret(_)) => {
                            AuthenticationExtensionsPRFOutputs {
                                enabled: Some(true),
                                results: dev
                                    .get_shared_secret()
                                    .and_then(|shared_secret| {
                                        hmac_response.decrypt_secrets(shared_secret)
                                    })
                                    .and_then(Result::ok)
                                    .map(|outputs| outputs.into()),
                            }
                        }
                    })
            }
            None | Some(HmacCreateSecretOrPrf::HmacCreateSecret(false)) => {}
        }

        result.extensions.cred_blob = result.att_obj.auth_data.extensions.cred_blob.clone();
    }
}

impl PinUvAuthCommand for MakeCredentials {
    fn set_pin_uv_auth_param(
        &mut self,
        pin_uv_auth_token: Option<PinUvAuthToken>,
    ) -> Result<(), AuthenticatorError> {
        let mut param = None;
        if let Some(token) = pin_uv_auth_token {
            param = Some(
                token
                    .derive(self.client_data_hash.as_ref())
                    .map_err(CommandError::Crypto)?,
            );
        }
        self.pin_uv_auth_param = param;
        Ok(())
    }

    fn set_uv_option(&mut self, uv: Option<bool>) {
        self.options.user_verification = uv;
    }

    fn get_rp_id(&self) -> Option<&String> {
        Some(&self.rp.id)
    }

    fn can_skip_user_verification(
        &mut self,
        info: &AuthenticatorInfo,
        uv_req: UserVerificationRequirement,
    ) -> bool {

        let supports_uv = info.options.user_verification == Some(true);
        let pin_configured = info.options.client_pin == Some(true);

        let device_protected = supports_uv || pin_configured;

        let make_cred_uv_not_required = info.options.make_cred_uv_not_rqd == Some(true)
            && self.options.resident_key != Some(true)
            && uv_req == UserVerificationRequirement::Discouraged;

        let always_uv = info.options.always_uv == Some(true);

        !always_uv && (!device_protected || make_cred_uv_not_required)
    }

    fn get_pin_uv_auth_param(&self) -> Option<&PinUvAuthParam> {
        self.pin_uv_auth_param.as_ref()
    }

    fn hmac_requested(&self) -> bool {
        !(self.extensions.hmac_secret.is_none()
            || matches!(
                self.extensions.hmac_secret,
                Some(HmacCreateSecretOrPrf::HmacCreateSecret(false))
            ))
    }
}

impl Serialize for MakeCredentials {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        debug!("Serialize MakeCredentials");
        serialize_map_optional!(
            serializer,
            &0x01 => Some(&self.client_data_hash),
            &0x02 => Some(&self.rp),
            &0x03 => Some(&self.user),
            &0x04 => Some(&self.pub_cred_params),
            &0x05 => (!self.exclude_list.is_empty()).then_some(&self.exclude_list),
            &0x06 => self.extensions.has_content().then_some(&self.extensions),
            &0x07 => self.options.has_some().then_some(&self.options),
            &0x08 => &self.pin_uv_auth_param,
            &0x09 => self.pin_uv_auth_param.as_ref().map(|p| p.pin_protocol.id()),
            &0x0a => &self.enterprise_attestation,
            &0x0b => &self.attestation_formats_preference,
        )
    }
}

impl RequestCtap1 for MakeCredentials {
    type Output = MakeCredentialsResult;
    type AdditionalInfo = ();

    fn ctap1_format(&self) -> Result<(Vec<u8>, ()), HIDError> {
        let flags = U2F_REQUEST_USER_PRESENCE;

        let mut register_data = Vec::with_capacity(2 * PARAMETER_SIZE);
        register_data.extend_from_slice(self.client_data_hash.as_ref());
        register_data.extend_from_slice(self.rp.hash().as_ref());
        let cmd = U2F_REGISTER;
        let apdu = CTAP1RequestAPDU::serialize(cmd, flags, &register_data)?;

        Ok((apdu, ()))
    }

    fn handle_response_ctap1<Dev: FidoDevice>(
        &self,
        dev: &mut Dev,
        status: Result<(), ApduErrorStatus>,
        input: &[u8],
        _add_info: &(),
    ) -> Result<Self::Output, Retryable<HIDError>> {
        if Err(ApduErrorStatus::ConditionsNotSatisfied) == status {
            return Err(Retryable::Retry);
        }
        if let Err(err) = status {
            return Err(Retryable::Error(HIDError::ApduStatus(err)));
        }

        let mut output = MakeCredentialsResult::from_ctap1(input, &self.rp.hash())
            .map_err(|e| Retryable::Error(HIDError::Command(e)))?;
        self.finalize_result(dev, &mut output);
        Ok(output)
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        let mut output = dev.make_credentials(self)?;
        self.finalize_result(dev, &mut output);
        Ok(output)
    }
}

impl RequestCtap2 for MakeCredentials {
    type Output = MakeCredentialsResult;

    fn command(&self) -> Command {
        Command::MakeCredentials
    }

    fn wire_format(&self) -> Result<Vec<u8>, HIDError> {
        Ok(ser::to_vec(&self).map_err(CommandError::Serializing)?)
    }

    fn handle_response_ctap2<Dev: FidoDevice>(
        &self,
        dev: &mut Dev,
        input: &[u8],
    ) -> Result<Self::Output, HIDError> {
        if input.is_empty() {
            return Err(HIDError::Command(CommandError::InputTooSmall));
        }

        let status: StatusCode = input[0].into();
        debug!("response status code: {:?}", status);
        if input.len() == 1 {
            if status.is_ok() {
                return Err(HIDError::Command(CommandError::InputTooSmall));
            }
            return Err(HIDError::Command(CommandError::StatusCode(status, None)));
        }

        if status.is_ok() {
            let mut output: MakeCredentialsResult =
                from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
            self.finalize_result(dev, &mut output);
            Ok(output)
        } else {
            let data: Value = from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
            Err(HIDError::Command(CommandError::StatusCode(
                status,
                Some(data),
            )))
        }
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        let mut output = dev.make_credentials(self)?;
        self.finalize_result(dev, &mut output);
        Ok(output)
    }
}

pub(crate) fn dummy_make_credentials_cmd() -> MakeCredentials {
    let mut req = MakeCredentials::new(
        ClientDataHash([
            208, 206, 230, 252, 125, 191, 89, 154, 145, 157, 184, 251, 149, 19, 17, 38, 159, 14,
            183, 129, 247, 132, 28, 108, 192, 84, 74, 217, 218, 52, 21, 75,
        ]),
        RelyingParty::from("make.me.blink"),
        Some(PublicKeyCredentialUserEntity {
            id: vec![0],
            name: Some(String::from("make.me.blink")),
            ..Default::default()
        }),
        vec![PublicKeyCredentialParameters {
            alg: COSEAlgorithm::ES256,
        }],
        vec![],
        MakeCredentialsOptions::default(),
        MakeCredentialsExtensions::default(),
    );
    req.pin_uv_auth_param = Some(PinUvAuthParam::create_empty());
    req
}
