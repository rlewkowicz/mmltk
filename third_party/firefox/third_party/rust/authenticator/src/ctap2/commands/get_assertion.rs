use super::get_info::AuthenticatorInfo;
use super::large_blobs::LargeBlobArrayElement;
use super::{
    Command, CommandError, CtapResponse, PinUvAuthCommand, PinUvAuthResult, RequestCtap1,
    RequestCtap2, Retryable, StatusCode,
};
use crate::consts::{
    PARAMETER_SIZE, U2F_AUTHENTICATE, U2F_DONT_ENFORCE_USER_PRESENCE_AND_SIGN,
    U2F_REQUEST_USER_PRESENCE,
};
use crate::crypto::{COSEKey, CryptoError, PinUvAuthParam, PinUvAuthToken, SharedSecret};
use crate::ctap2::attestation::{AuthenticatorData, AuthenticatorDataFlags, HmacSecretResponse};
use crate::ctap2::client_data::ClientDataHash;
use crate::ctap2::commands::get_next_assertion::GetNextAssertion;
use crate::ctap2::commands::make_credentials::UserVerification;
use crate::ctap2::server::{
    AuthenticationExtensionsClientInputs, AuthenticationExtensionsClientOutputs,
    AuthenticationExtensionsPRFInputs, AuthenticationExtensionsPRFOutputs, AuthenticatorAttachment,
    AuthenticatorExtensionsCredBlob, PublicKeyCredentialDescriptor, PublicKeyCredentialUserEntity,
    RelyingParty, RpIdHash, UserVerificationRequirement,
};
use crate::ctap2::utils::{read_be_u32, read_byte};
use crate::errors::AuthenticatorError;
use crate::transport::errors::{ApduErrorStatus, HIDError};
use crate::transport::{FidoDevice, VirtualFidoDevice};
use crate::u2ftypes::CTAP1RequestAPDU;
use serde::{
    de::{Error as DesError, MapAccess, Visitor},
    ser::Error as SerError,
    Deserialize, Deserializer, Serialize, Serializer,
};
use serde_bytes::ByteBuf;
use serde_cbor::{de::from_slice, ser, Value};
use std::collections::HashMap;
use std::convert::TryFrom;
use std::fmt;
use std::io::Cursor;

#[derive(Clone, Copy, Debug, Serialize)]
pub struct GetAssertionOptions {
    #[serde(rename = "uv", skip_serializing_if = "Option::is_none")]
    pub user_verification: Option<bool>,
    #[serde(rename = "up", skip_serializing_if = "Option::is_none")]
    pub user_presence: Option<bool>,
}

impl Default for GetAssertionOptions {
    fn default() -> Self {
        Self {
            user_presence: Some(true),
            user_verification: None,
        }
    }
}

impl GetAssertionOptions {
    pub(crate) fn has_some(&self) -> bool {
        self.user_presence.is_some() || self.user_verification.is_some()
    }
}

impl UserVerification for GetAssertionOptions {
    fn ask_user_verification(&self) -> bool {
        self.user_verification.unwrap_or(false)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct CalculatedHmacSecretExtension {
    pub public_key: COSEKey,
    pub salt_enc: Vec<u8>,
    pub salt_auth: Vec<u8>,
}

/// Wrapper type recording whether the hmac-secret input originally came from the hmacGetSecret or the prf client extension input.
#[derive(Debug, Clone, PartialEq)]
pub enum HmacGetSecretOrPrf {
    /// hmac-secret inputs set by the hmacGetSecret client extension input.
    HmacGetSecret(HmacSecretExtension),
    /// hmac-secret input is to be calculated from PRF inputs, but we haven't yet identified which eval or evalByCredential entry to use.
    PrfUninitialized(AuthenticationExtensionsPRFInputs),
    /// prf client input with no eval or matching evalByCredential entry.
    PrfUnmatched,
    /// hmac-secret inputs set by the prf client extension input.
    Prf(HmacSecretExtension),
}

impl HmacGetSecretOrPrf {
    fn skip_serializing(value: &Option<Self>) -> bool {
        matches!(value, None | Some(Self::PrfUnmatched))
    }

    /// Calculate the appropriate hmac-secret or PRF salt inputs from the given inputs.
    ///
    /// - If this is a `HmacGetSecret` instance,
    ///   this returns a new `HmacGetSecret` instance with `calculated_hmac` set, paired with [None].
    /// - If this is a `PrfUninitialized` instance,
    ///   this attempts to select a PRF input to calculate salts from.
    ///   If an input is found, this returns a `Prf` instance with `calculated_hmac` set.
    ///   If the selected input came from `eval_by_credential`,
    ///   then this is paired with a [Some] referencing the matching element of `allow_credentials`.
    ///   If the selected input was `eval`, then this is paired with [None].
    ///   If no input is found, this returns `PrfUnmatched` and [None].
    /// - If this is a `Prf` or `PrfUnmatched` instance, this panics.
    ///
    /// If the [Option] return value is [Some], the caller SHOULD set `allowCredentials`
    /// to contain only that [PublicKeyCredentialDescriptor] value.
    ///
    /// # Panics
    /// If this is a `Prf` or `PrfUnmatched` instance.
    pub fn calculate<'allow_cred>(
        self,
        secret: &SharedSecret,
        allow_credentials: &'allow_cred [PublicKeyCredentialDescriptor],
        puat: Option<&PinUvAuthToken>,
    ) -> Result<(Self, Option<&'allow_cred PublicKeyCredentialDescriptor>), CryptoError> {
        Ok(match self {
            Self::HmacGetSecret(mut extension) => {
                extension.calculate(secret, puat)?;
                (Self::HmacGetSecret(extension), None)
            }

            Self::PrfUninitialized(prf) => match prf.calculate(secret, allow_credentials, puat)? {
                Some((hmac_secret, selected_credential)) => {
                    (Self::Prf(hmac_secret), selected_credential)
                }
                None => (Self::PrfUnmatched, None),
            },

            Self::Prf(_) | Self::PrfUnmatched => {
                unreachable!("hmac-secret inputs from PRF already initialized")
            }
        })
    }
}

impl Serialize for HmacGetSecretOrPrf {
    fn serialize<S>(&self, s: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        match self {
            Self::HmacGetSecret(ext) => ext.serialize(s),
            Self::PrfUninitialized(_) => Err(serde::ser::Error::custom(
                "PrfUninitialized must be replaced with Prf or PrfUnmatched before serializing",
            )),
            Self::PrfUnmatched => unreachable!("PrfUnmatched serialization should be skipped"),
            Self::Prf(ext) => ext.serialize(s),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct HmacSecretExtension {
    pub salt1: Vec<u8>,
    pub salt2: Option<Vec<u8>>,
    calculated_hmac: Option<CalculatedHmacSecretExtension>,
    pin_protocol: Option<u64>,
}

impl HmacSecretExtension {
    pub fn new(salt1: Vec<u8>, salt2: Option<Vec<u8>>) -> Self {
        HmacSecretExtension {
            salt1,
            salt2,
            calculated_hmac: None,
            pin_protocol: None,
        }
    }


    /// Calculate inputs for the `hmac-secret` extension.
    /// See "authenticatorGetAssertion additional behaviors"
    /// in https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-20210615.html#sctn-hmac-secret-extension
    pub fn calculate(
        &mut self,
        secret: &SharedSecret,
        puat: Option<&PinUvAuthToken>,
    ) -> Result<(), CryptoError> {
        let salt_enc = match (
            <[u8; 32]>::try_from(self.salt1.as_slice()),
            self.salt2.as_deref().map(<[u8; 32]>::try_from),
        ) {
            (Ok(salt1), None) => secret.encrypt(&salt1),
            (Ok(salt1), Some(Ok(salt2))) => secret.encrypt(&[salt1, salt2].concat()),
            (Err(_), _) | (_, Some(Err(_))) => {
                debug!("Invalid hmac-secret salt length(s): salt1: {}, salt2: {:?} (expected 32 and 32|None)",
                       self.salt1.len(), self.salt2.as_ref().map(Vec::len));
                Err(CryptoError::WrongSaltLength)
            }
        }?;
        let salt_auth = secret.authenticate(&salt_enc)?;
        let public_key = secret.client_input().clone();
        self.calculated_hmac = Some(CalculatedHmacSecretExtension {
            public_key,
            salt_enc,
            salt_auth,
        });

        self.pin_protocol = puat
            .map(|puat| puat.pin_protocol.id())
            .filter(|id| *id != 1);

        Ok(())
    }
}

impl Serialize for HmacSecretExtension {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        if let Some(calc) = &self.calculated_hmac {
            serialize_map_optional! {
                serializer,
                &1 => Some(&calc.public_key),
                &2 => Some(serde_bytes::Bytes::new(&calc.salt_enc)),
                &3 => Some(serde_bytes::Bytes::new(&calc.salt_auth)),
                &4 => &self.pin_protocol,
            }
        } else {
            Err(SerError::custom(
                "hmac secret has not been calculated before being serialized",
            ))
        }
    }
}

#[derive(Debug, Default, Clone, Serialize)]
pub struct GetAssertionExtensions {
    #[serde(skip_serializing)]
    pub app_id: Option<String>,
    #[serde(
        rename = "hmac-secret",
        skip_serializing_if = "HmacGetSecretOrPrf::skip_serializing"
    )]
    pub hmac_secret: Option<HmacGetSecretOrPrf>,
    #[serde(rename = "credBlob", skip_serializing_if = "Option::is_none")]
    pub cred_blob: Option<bool>,
    #[serde(rename = "largeBlobKey", skip_serializing_if = "Option::is_none")]
    pub large_blob_key: Option<bool>,
    #[serde(rename = "thirdPartyPayment", skip_serializing_if = "Option::is_none")]
    pub third_party_payment: Option<bool>,
}

impl From<AuthenticationExtensionsClientInputs> for GetAssertionExtensions {
    fn from(input: AuthenticationExtensionsClientInputs) -> Self {
        let prf = input.prf;
        Self {
            app_id: input.app_id,
            hmac_secret: input
                .hmac_get_secret
                .map(|hmac_secret| {
                    HmacGetSecretOrPrf::HmacGetSecret(HmacSecretExtension::new(
                        hmac_secret.salt1.into(),
                        hmac_secret.salt2.map(|salt2| salt2.into()),
                    ))
                })
                .or_else(
                    || prf.map(HmacGetSecretOrPrf::PrfUninitialized), 
                ),
            cred_blob: match input.cred_blob {
                Some(AuthenticatorExtensionsCredBlob::AsBool(x)) => Some(x),
                _ => None,
            },
            large_blob_key: input.large_blob_key,
            third_party_payment: input.third_party_payment,
        }
    }
}

impl GetAssertionExtensions {
    fn has_content(&self) -> bool {
        self.hmac_secret.is_some()
            || self.cred_blob.is_some()
            || self.large_blob_key.is_some()
            || self.third_party_payment.is_some()
    }
}

#[derive(Debug, Clone)]
pub struct GetAssertion {
    pub client_data_hash: ClientDataHash,
    pub rp: RelyingParty,
    pub allow_list: Vec<PublicKeyCredentialDescriptor>,

    pub extensions: GetAssertionExtensions,
    pub options: GetAssertionOptions,
    pub pin_uv_auth_param: Option<PinUvAuthParam>,

    pub enterprise_attestation: Option<u64>,
    pub attestation_formats_preference: Option<Vec<String>>,
}

impl GetAssertion {
    pub fn new(
        client_data_hash: ClientDataHash,
        rp: RelyingParty,
        allow_list: Vec<PublicKeyCredentialDescriptor>,
        options: GetAssertionOptions,
        extensions: GetAssertionExtensions,
    ) -> Self {
        Self {
            client_data_hash,
            rp,
            allow_list,
            extensions,
            options,
            pin_uv_auth_param: None,
            enterprise_attestation: None,
            attestation_formats_preference: None,
        }
    }

    pub fn process_hmac_secret_and_prf_extension(
        mut self,
        shared_secret: Option<(&SharedSecret, &PinUvAuthResult)>,
    ) -> Result<Self, AuthenticatorError> {
        let (new_hmac_secret, new_allow_list) = self
            .extensions
            .hmac_secret
            .take()
            .and_then(|hmac_get_secret_or_prf| {
                if let Some((secret, pin_uv_auth_result)) = shared_secret {
                    Some(hmac_get_secret_or_prf.calculate(
                        secret,
                        &self.allow_list,
                        pin_uv_auth_result.get_pin_uv_auth_token().as_ref(),
                    ))
                } else {
                    debug!(
                        "Shared secret not available - will not send hmac-secret extension input: {:?}",
                        hmac_get_secret_or_prf
                    );
                    match hmac_get_secret_or_prf {
                        HmacGetSecretOrPrf::HmacGetSecret(_) => None,
                        HmacGetSecretOrPrf::PrfUninitialized(_)
                        | HmacGetSecretOrPrf::PrfUnmatched
                        | HmacGetSecretOrPrf::Prf(_) => {
                            Some(Ok((HmacGetSecretOrPrf::PrfUnmatched, None)))
                        }
                    }
                }
            })
            .transpose()
            .map_err(|err| match err {
                CryptoError::WrongSaltLength => AuthenticatorError::InvalidRelyingPartyInput,
                e => e.into(),
            })?
            .map(|(nhs, nal)| (Some(nhs), nal))
            .unwrap_or((None, None));

        (self.extensions.hmac_secret, self.allow_list) = (
            new_hmac_secret,
            new_allow_list
                .map(|selected_credential| vec![selected_credential.clone()])
                .unwrap_or(self.allow_list),
        );

        Ok(self)
    }

    pub fn finalize_result<Dev: FidoDevice>(&self, dev: &Dev, result: &mut GetAssertionResult) {
        result.attachment = match dev.get_authenticator_info() {
            Some(info) if info.options.platform_device => AuthenticatorAttachment::Platform,
            Some(_) => AuthenticatorAttachment::CrossPlatform,
            None => AuthenticatorAttachment::Unknown,
        };

        if let Some(app_id) = &self.extensions.app_id {
            result.extensions.app_id =
                Some(result.assertion.auth_data.rp_id_hash == RelyingParty::from(app_id).hash());
        }

        match self.extensions.hmac_secret {
            Some(HmacGetSecretOrPrf::HmacGetSecret(_)) => {
                result.extensions.hmac_get_secret =
                    if let Some(hmac_response @ HmacSecretResponse::Secret(_)) =
                        &result.assertion.auth_data.extensions.hmac_secret
                    {
                        dev.get_shared_secret()
                            .and_then(|shared_secret| hmac_response.decrypt_secrets(shared_secret))
                            .and_then(|result| match result {
                                Ok(ok) => Some(ok),
                                Err(err) => {
                                    debug!("Failed to decrypt hmac-secret response: {:?}", err);
                                    None
                                }
                            })
                    } else {
                        None
                    };
            }
            Some(HmacGetSecretOrPrf::PrfUninitialized(_)) => {
                unreachable!("Reached GetAssertion.finalize_result without replacing PrfUninitialized instance with Prf")
            }
            Some(HmacGetSecretOrPrf::PrfUnmatched) => {
                result.extensions.prf = Some(AuthenticationExtensionsPRFOutputs {
                    enabled: None,
                    results: None,
                });
            }
            Some(HmacGetSecretOrPrf::Prf(_)) => {
                result.extensions.prf = Some(AuthenticationExtensionsPRFOutputs {
                    enabled: None,
                    results: if let Some(hmac_response @ HmacSecretResponse::Secret(_)) =
                        &result.assertion.auth_data.extensions.hmac_secret
                    {
                        dev.get_shared_secret()
                            .and_then(|shared_secret| hmac_response.decrypt_secrets(shared_secret))
                            .and_then(|result| match result {
                                Ok(ok) => Some(ok),
                                Err(err) => {
                                    debug!("Failed to decrypt hmac-secret response: {:?}", err);
                                    None
                                }
                            })
                            .map(|outputs| outputs.into())
                    } else {
                        None
                    },
                });
            }
            None => {}
        }

        result.extensions.cred_blob = result.assertion.auth_data.extensions.cred_blob.clone();
    }
}

impl PinUvAuthCommand for GetAssertion {
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
        let uv_discouraged = uv_req == UserVerificationRequirement::Discouraged;
        let always_uv = info.options.always_uv == Some(true);

        !always_uv && (!device_protected || uv_discouraged)
    }

    fn get_pin_uv_auth_param(&self) -> Option<&PinUvAuthParam> {
        self.pin_uv_auth_param.as_ref()
    }

    fn hmac_requested(&self) -> bool {
        self.extensions.hmac_secret.is_some()
    }
}

impl Serialize for GetAssertion {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serialize_map_optional! {
            serializer,
            &1 => Some(&self.rp.id),
            &2 => Some(&self.client_data_hash),
            &3 => (!&self.allow_list.is_empty()).then_some(&self.allow_list),
            &4 => self.extensions.has_content().then_some(&self.extensions),
            &5 => self.options.has_some().then_some(&self.options),
            &6 => &self.pin_uv_auth_param,
            &7 => self.pin_uv_auth_param.as_ref().map(|p| p.pin_protocol.id()),
            &8 => &self.enterprise_attestation,
            &9 => &self.attestation_formats_preference,
        }
    }
}

type GetAssertionOutput = Vec<GetAssertionResult>;
impl CtapResponse for GetAssertionOutput {}

impl RequestCtap1 for GetAssertion {
    type Output = Vec<GetAssertionResult>;
    type AdditionalInfo = PublicKeyCredentialDescriptor;

    fn ctap1_format(&self) -> Result<(Vec<u8>, Self::AdditionalInfo), HIDError> {
        let key_handle = match &self.allow_list[..] {
            [key_handle] => key_handle,
            [] => {
                return Err(HIDError::Command(CommandError::StatusCode(
                    StatusCode::NoCredentials,
                    None,
                )));
            }
            _ => {
                return Err(HIDError::UnsupportedCommand);
            }
        };

        debug!("sending key_handle = {:?}", key_handle);

        let flags = if self.options.user_presence.unwrap_or(true) {
            U2F_REQUEST_USER_PRESENCE
        } else {
            U2F_DONT_ENFORCE_USER_PRESENCE_AND_SIGN
        };
        let mut auth_data =
            Vec::with_capacity(2 * PARAMETER_SIZE + 1  + key_handle.id.len());

        auth_data.extend_from_slice(self.client_data_hash.as_ref());
        auth_data.extend_from_slice(self.rp.hash().as_ref());
        auth_data.extend_from_slice(&[key_handle.id.len() as u8]);
        auth_data.extend_from_slice(key_handle.id.as_ref());

        let cmd = U2F_AUTHENTICATE;
        let apdu = CTAP1RequestAPDU::serialize(cmd, flags, &auth_data)?;
        Ok((apdu, key_handle.clone()))
    }

    fn handle_response_ctap1<Dev: FidoDevice>(
        &self,
        dev: &mut Dev,
        status: Result<(), ApduErrorStatus>,
        input: &[u8],
        add_info: &PublicKeyCredentialDescriptor,
    ) -> Result<Self::Output, Retryable<HIDError>> {
        if Err(ApduErrorStatus::ConditionsNotSatisfied) == status {
            return Err(Retryable::Retry);
        }
        if let Err(err) = status {
            return Err(Retryable::Error(HIDError::ApduStatus(err)));
        }

        let mut result = GetAssertionResult::from_ctap1(input, &self.rp.hash(), add_info)
            .map_err(|e| Retryable::Error(HIDError::Command(e)))?;
        self.finalize_result(dev, &mut result);
        Ok(vec![result])
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        let mut results = dev.get_assertion(self)?;
        for result in results.iter_mut() {
            self.finalize_result(dev, result);
        }
        Ok(results)
    }
}

impl RequestCtap2 for GetAssertion {
    type Output = Vec<GetAssertionResult>;

    fn command(&self) -> Command {
        Command::GetAssertion
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
            return Err(CommandError::InputTooSmall.into());
        }

        let status: StatusCode = input[0].into();
        debug!(
            "response status code: {:?}, rest: {:?}",
            status,
            &input[1..]
        );
        if input.len() == 1 {
            if status.is_ok() {
                return Err(CommandError::InputTooSmall.into());
            }
            return Err(CommandError::StatusCode(status, None).into());
        }

        if status.is_ok() {
            let assertion: GetAssertionResponse =
                from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
            let number_of_credentials = assertion.number_of_credentials.unwrap_or(1);
            let user_selected = assertion.user_selected;
            let large_blob_key = assertion.large_blob_key.clone();
            let mut results = Vec::with_capacity(number_of_credentials);
            results.push(GetAssertionResult {
                assertion: assertion.into(),
                attachment: AuthenticatorAttachment::Unknown,
                extensions: Default::default(),
                user_selected,
                large_blob_key,
                large_blob_array: None,
            });

            let msg = GetNextAssertion;
            for _ in 1..number_of_credentials {
                let assertion = dev.send_cbor(&msg)?;
                let user_selected = assertion.user_selected;
                let large_blob_key = assertion.large_blob_key.clone();
                results.push(GetAssertionResult {
                    assertion: assertion.into(),
                    attachment: AuthenticatorAttachment::Unknown,
                    extensions: Default::default(),
                    user_selected,
                    large_blob_key,
                    large_blob_array: None,
                });
            }

            for result in results.iter_mut() {
                self.finalize_result(dev, result);
            }
            Ok(results)
        } else {
            let data: Value = from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
            Err(CommandError::StatusCode(status, Some(data)).into())
        }
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        let mut results = dev.get_assertion(self)?;
        for result in results.iter_mut() {
            self.finalize_result(dev, result);
        }
        Ok(results)
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct Assertion {
    pub credentials: Option<PublicKeyCredentialDescriptor>, 
    pub auth_data: AuthenticatorData,
    pub signature: Vec<u8>,
    pub user: Option<PublicKeyCredentialUserEntity>,
}

impl From<GetAssertionResponse> for Assertion {
    fn from(r: GetAssertionResponse) -> Self {
        Assertion {
            credentials: r.credentials,
            auth_data: r.auth_data,
            signature: r.signature,
            user: r.user,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct GetAssertionResult {
    pub assertion: Assertion,
    pub attachment: AuthenticatorAttachment,
    pub extensions: AuthenticationExtensionsClientOutputs,
    pub user_selected: Option<bool>,
    pub large_blob_key: Option<Vec<u8>>,
    pub large_blob_array: Option<Vec<LargeBlobArrayElement>>,
}

impl GetAssertionResult {
    pub fn from_ctap1(
        input: &[u8],
        rp_id_hash: &RpIdHash,
        key_handle: &PublicKeyCredentialDescriptor,
    ) -> Result<GetAssertionResult, CommandError> {
        let mut data = Cursor::new(input);
        let user_presence = read_byte(&mut data).map_err(CommandError::Deserializing)?;
        let counter = read_be_u32(&mut data).map_err(CommandError::Deserializing)?;
        let signature = Vec::from(&data.get_ref()[data.position() as usize..]);

        let flag_mask = AuthenticatorDataFlags::USER_PRESENT | AuthenticatorDataFlags::RESERVED_1;
        let flags = flag_mask & AuthenticatorDataFlags::from_bits_truncate(user_presence);
        let auth_data = AuthenticatorData {
            rp_id_hash: rp_id_hash.clone(),
            flags,
            counter,
            credential_data: None,
            extensions: Default::default(),
        };
        let assertion = Assertion {
            credentials: Some(key_handle.clone()),
            signature,
            user: None,
            auth_data,
        };

        Ok(GetAssertionResult {
            assertion,
            attachment: AuthenticatorAttachment::Unknown,
            extensions: Default::default(),
            user_selected: None,
            large_blob_key: None,
            large_blob_array: None,
        })
    }
}

#[derive(Debug, PartialEq)]
pub struct GetAssertionResponse {
    pub credentials: Option<PublicKeyCredentialDescriptor>,
    pub auth_data: AuthenticatorData,
    pub signature: Vec<u8>,
    pub user: Option<PublicKeyCredentialUserEntity>,
    pub number_of_credentials: Option<usize>,
    pub user_selected: Option<bool>,
    pub large_blob_key: Option<Vec<u8>>,
    pub unsigned_extension_outputs: Option<HashMap<String, serde_cbor::Value>>,
    pub ep_attestation: Option<bool>,
    pub att_stmt: Option<HashMap<String, serde_cbor::Value>>,
}

impl CtapResponse for GetAssertionResponse {}

impl<'de> Deserialize<'de> for GetAssertionResponse {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct GetAssertionResponseVisitor;

        impl<'de> Visitor<'de> for GetAssertionResponseVisitor {
            type Value = GetAssertionResponse;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut credentials = None;
                let mut auth_data = None;
                let mut signature = None;
                let mut user = None;
                let mut number_of_credentials = None;
                let mut user_selected = None;
                let mut large_blob_key = None;
                let mut unsigned_extension_outputs = None;
                let mut ep_attestation = None;
                let mut att_stmt = None;

                while let Some(key) = map.next_key()? {
                    match key {
                        0x01 => {
                            if credentials.is_some() {
                                return Err(M::Error::duplicate_field("credentials"));
                            }
                            credentials = Some(map.next_value()?);
                        }
                        0x02 => {
                            if auth_data.is_some() {
                                return Err(M::Error::duplicate_field("auth_data"));
                            }
                            auth_data = Some(map.next_value()?);
                        }
                        0x03 => {
                            if signature.is_some() {
                                return Err(M::Error::duplicate_field("signature"));
                            }
                            let signature_bytes: ByteBuf = map.next_value()?;
                            signature = Some(signature_bytes.into_vec());
                        }
                        0x04 => {
                            if user.is_some() {
                                return Err(M::Error::duplicate_field("user"));
                            }
                            user = map.next_value()?;
                        }
                        0x05 => {
                            if number_of_credentials.is_some() {
                                return Err(M::Error::duplicate_field("number_of_credentials"));
                            }
                            number_of_credentials = Some(map.next_value()?);
                        }
                        0x06 => {
                            if user_selected.is_some() {
                                return Err(M::Error::duplicate_field("user_selected"));
                            }
                            user_selected = Some(map.next_value()?);
                        }
                        0x07 => {
                            if large_blob_key.is_some() {
                                return Err(M::Error::duplicate_field("large_blob_key"));
                            }
                            let large_blob_key_bytes: ByteBuf = map.next_value()?;
                            large_blob_key = Some(large_blob_key_bytes.into_vec());
                        }
                        0x08 => {
                            if unsigned_extension_outputs.is_some() {
                                return Err(M::Error::duplicate_field(
                                    "unsigned_extension_outputs",
                                ));
                            }
                            unsigned_extension_outputs = Some(map.next_value()?);
                        }
                        0x09 => {
                            if ep_attestation.is_some() {
                                return Err(M::Error::duplicate_field("ep_attestation"));
                            }
                            ep_attestation = Some(map.next_value()?);
                        }
                        0x0A => {
                            if att_stmt.is_some() {
                                return Err(M::Error::duplicate_field("att_stmt"));
                            }
                            att_stmt = Some(map.next_value()?);
                        }
                        k => return Err(M::Error::custom(format!("unexpected key: {k:?}"))),
                    }
                }

                let auth_data = auth_data.ok_or_else(|| M::Error::missing_field("auth_data"))?;
                let signature = signature.ok_or_else(|| M::Error::missing_field("signature"))?;

                Ok(GetAssertionResponse {
                    credentials,
                    auth_data,
                    signature,
                    user,
                    number_of_credentials,
                    user_selected,
                    large_blob_key,
                    unsigned_extension_outputs,
                    ep_attestation,
                    att_stmt,
                })
            }
        }

        deserializer.deserialize_bytes(GetAssertionResponseVisitor)
    }
}
