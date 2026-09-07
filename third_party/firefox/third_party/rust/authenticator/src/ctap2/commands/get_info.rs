use super::{Command, CommandError, CtapResponse, RequestCtap2, StatusCode};
use crate::ctap2::attestation::AAGuid;
use crate::ctap2::server::PublicKeyCredentialParameters;
use crate::transport::errors::HIDError;
use crate::transport::{FidoDevice, VirtualFidoDevice};
use serde::{
    de::{Error as SError, IgnoredAny, MapAccess, Visitor},
    Deserialize, Deserializer, Serialize,
};
use serde_cbor::{de::from_slice, Value};
use std::collections::BTreeMap;
use std::fmt;

#[derive(Debug, Default)]
pub struct GetInfo {}

impl RequestCtap2 for GetInfo {
    type Output = AuthenticatorInfo;

    fn command(&self) -> Command {
        Command::GetInfo
    }

    fn wire_format(&self) -> Result<Vec<u8>, HIDError> {
        Ok(Vec::new())
    }

    fn handle_response_ctap2<Dev: FidoDevice>(
        &self,
        _dev: &mut Dev,
        input: &[u8],
    ) -> Result<Self::Output, HIDError> {
        if input.is_empty() {
            return Err(CommandError::InputTooSmall.into());
        }

        let status: StatusCode = input[0].into();

        if input.len() > 1 {
            if status.is_ok() {
                trace!("parsing authenticator info data: {:#04X?}", &input);
                let authenticator_info =
                    from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
                Ok(authenticator_info)
            } else {
                let data: Value = from_slice(&input[1..]).map_err(CommandError::Deserializing)?;
                Err(CommandError::StatusCode(status, Some(data)).into())
            }
        } else {
            Err(CommandError::InputTooSmall.into())
        }
    }

    fn send_to_virtual_device<Dev: VirtualFidoDevice>(
        &self,
        dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        dev.get_info()
    }
}

fn true_val() -> bool {
    true
}

#[derive(Debug, Deserialize, Clone, Eq, PartialEq, Serialize)]
pub struct AuthenticatorOptions {
    /// Indicates that the device is attached to the client and therefore can’t
    /// be removed and used on another client.
    #[serde(rename = "plat", default)]
    pub platform_device: bool,
    /// Indicates that the device is capable of storing keys on the device
    /// itself and therefore can satisfy the authenticatorGetAssertion request
    /// with allowList parameter not specified or empty.
    #[serde(rename = "rk", default)]
    pub resident_key: bool,

    /// Client PIN:
    ///  If present and set to true, it indicates that the device is capable of
    ///   accepting a PIN from the client and PIN has been set.
    ///  If present and set to false, it indicates that the device is capable of
    ///   accepting a PIN from the client and PIN has not been set yet.
    ///  If absent, it indicates that the device is not capable of accepting a
    ///   PIN from the client.
    /// Client PIN is one of the ways to do user verification.
    #[serde(rename = "clientPin")]
    pub client_pin: Option<bool>,

    /// Indicates that the device is capable of testing user presence.
    #[serde(rename = "up", default = "true_val")]
    pub user_presence: bool,

    /// Indicates that the device is capable of verifying the user within
    /// itself. For example, devices with UI, biometrics fall into this
    /// category.
    ///  If present and set to true, it indicates that the device is capable of
    ///   user verification within itself and has been configured.
    ///  If present and set to false, it indicates that the device is capable of
    ///   user verification within itself and has not been yet configured. For
    ///   example, a biometric device that has not yet been configured will
    ///   return this parameter set to false.
    ///  If absent, it indicates that the device is not capable of user
    ///   verification within itself.
    /// A device that can only do Client PIN will not return the "uv" parameter.
    /// If a device is capable of verifying the user within itself as well as
    /// able to do Client PIN, it will return both "uv" and the Client PIN
    /// option.
    #[serde(rename = "uv")]
    pub user_verification: Option<bool>,

    /// If pinUvAuthToken is:
    /// present and set to true
    ///  if the clientPin option id is present and set to true, then the
    ///  authenticator supports authenticatorClientPIN's getPinUvAuthTokenUsingPinWithPermissions
    ///  subcommand. If the uv option id is present and set to true, then
    ///  the authenticator supports authenticatorClientPIN's getPinUvAuthTokenUsingUvWithPermissions
    ///  subcommand.
    /// present and set to false, or absent.
    ///  the authenticator does not support authenticatorClientPIN's
    ///  getPinUvAuthTokenUsingPinWithPermissions and getPinUvAuthTokenUsingUvWithPermissions
    ///  subcommands.
    #[serde(rename = "pinUvAuthToken")]
    pub pin_uv_auth_token: Option<bool>,

    /// If this noMcGaPermissionsWithClientPin is:
    /// present and set to true
    ///  A pinUvAuthToken obtained via getPinUvAuthTokenUsingPinWithPermissions
    ///  (or getPinToken) cannot be used for authenticatorMakeCredential or
    ///  authenticatorGetAssertion commands, because it will lack the necessary
    ///  mc and ga permissions. In this situation, platforms SHOULD NOT attempt
    ///  to use getPinUvAuthTokenUsingPinWithPermissions if using
    ///  getPinUvAuthTokenUsingUvWithPermissions fails.
    /// present and set to false, or absent.
    ///  A pinUvAuthToken obtained via getPinUvAuthTokenUsingPinWithPermissions
    ///  (or getPinToken) can be used for authenticatorMakeCredential or
    ///  authenticatorGetAssertion commands.
    /// Note: noMcGaPermissionsWithClientPin MUST only be present if the
    ///       clientPin option ID is present.
    #[serde(rename = "noMcGaPermissionsWithClientPin")]
    pub no_mc_ga_permissions_with_client_pin: Option<bool>,

    /// If largeBlobs is:
    /// present and set to true
    ///  the authenticator supports the authenticatorLargeBlobs command.
    /// present and set to false, or absent.
    ///  The authenticatorLargeBlobs command is NOT supported.
    #[serde(rename = "largeBlobs")]
    pub large_blobs: Option<bool>,

    /// Enterprise Attestation feature support:
    /// If ep is:
    /// Present and set to true
    ///  The authenticator is enterprise attestation capable, and enterprise
    ///  attestation is enabled.
    /// Present and set to false
    ///  The authenticator is enterprise attestation capable, and enterprise
    ///  attestation is disabled.
    /// Absent
    ///  The Enterprise Attestation feature is NOT supported.
    #[serde(rename = "ep")]
    pub ep: Option<bool>,

    /// If bioEnroll is:
    /// present and set to true
    ///  the authenticator supports the authenticatorBioEnrollment commands,
    ///  and has at least one bio enrollment presently provisioned.
    /// present and set to false
    ///  the authenticator supports the authenticatorBioEnrollment commands,
    ///  and does not yet have any bio enrollments provisioned.
    /// absent
    ///  the authenticatorBioEnrollment commands are NOT supported.
    #[serde(rename = "bioEnroll")]
    pub bio_enroll: Option<bool>,

    /// "FIDO_2_1_PRE" Prototype Credential management support:
    /// If userVerificationMgmtPreview is:
    /// present and set to true
    ///  the authenticator supports the Prototype authenticatorBioEnrollment (0x41)
    ///  commands, and has at least one bio enrollment presently provisioned.
    /// present and set to false
    ///  the authenticator supports the Prototype authenticatorBioEnrollment (0x41)
    ///  commands, and does not yet have any bio enrollments provisioned.
    /// absent
    ///  the Prototype authenticatorBioEnrollment (0x41) commands are not supported.
    #[serde(rename = "userVerificationMgmtPreview")]
    pub user_verification_mgmt_preview: Option<bool>,

    /// getPinUvAuthTokenUsingUvWithPermissions support for requesting the be permission:
    /// This option ID MUST only be present if bioEnroll is also present.
    /// If uvBioEnroll is:
    /// present and set to true
    ///  requesting the be permission when invoking getPinUvAuthTokenUsingUvWithPermissions
    ///  is supported.
    /// present and set to false, or absent.
    ///  requesting the be permission when invoking getPinUvAuthTokenUsingUvWithPermissions
    ///  is NOT supported.
    #[serde(rename = "uvBioEnroll")]
    pub uv_bio_enroll: Option<bool>,

    /// authenticatorConfig command support:
    /// If authnrCfg is:
    /// present and set to true
    ///  the authenticatorConfig command is supported.
    /// present and set to false, or absent.
    ///  the authenticatorConfig command is NOT supported.
    #[serde(rename = "authnrCfg")]
    pub authnr_cfg: Option<bool>,

    /// getPinUvAuthTokenUsingUvWithPermissions support for requesting the acfg permission:
    /// This option ID MUST only be present if authnrCfg is also present.
    /// If uvAcfg is:
    /// present and set to true
    ///  requesting the acfg permission when invoking getPinUvAuthTokenUsingUvWithPermissions
    ///  is supported.
    /// present and set to false, or absent.
    ///  requesting the acfg permission when invoking getPinUvAuthTokenUsingUvWithPermissions
    ///  is NOT supported.
    #[serde(rename = "uvAcfg")]
    pub uv_acfg: Option<bool>,

    /// Credential management support:
    /// If credMgmt is:
    /// present and set to true
    ///  the authenticatorCredentialManagement command is supported.
    /// present and set to false, or absent.
    ///  the authenticatorCredentialManagement command is NOT supported.
    #[serde(rename = "credMgmt")]
    pub cred_mgmt: Option<bool>,

    /// "FIDO_2_1_PRE" Prototype Credential management support:
    /// If credentialMgmtPreview is:
    /// present and set to true
    ///  the Prototype authenticatorCredentialManagement (0x41) command is supported.
    /// present and set to false, or absent.
    ///  the Prototype authenticatorCredentialManagement (0x41) command is NOT supported.
    #[serde(rename = "credentialMgmtPreview")]
    pub credential_mgmt_preview: Option<bool>,

    /// Support for the Set Minimum PIN Length feature.
    /// If setMinPINLength is:
    /// present and set to true
    ///  the setMinPINLength subcommand is supported.
    /// present and set to false, or absent.
    ///  the setMinPINLength subcommand is NOT supported.
    /// Note: setMinPINLength MUST only be present if the clientPin option ID is present.
    #[serde(rename = "setMinPINLength")]
    pub set_min_pin_length: Option<bool>,

    /// Support for making non-discoverable credentials without requiring User Verification.
    /// If makeCredUvNotRqd is:
    /// present and set to true
    ///  the authenticator allows creation of non-discoverable credentials without
    ///  requiring any form of user verification, if the platform requests this behaviour.
    /// present and set to false, or absent.
    ///  the authenticator requires some form of user verification for creating
    ///  non-discoverable credentials, regardless of the parameters the platform supplies
    ///  for the authenticatorMakeCredential command.
    /// Authenticators SHOULD include this option with the value true.
    #[serde(rename = "makeCredUvNotRqd")]
    pub make_cred_uv_not_rqd: Option<bool>,

    /// Support for the Always Require User Verification feature:
    /// If alwaysUv is
    /// present and set to true
    ///  the authenticator supports the Always Require User Verification feature and it is enabled.
    /// present and set to false
    ///  the authenticator supports the Always Require User Verification feature but it is disabled.
    /// absent
    ///  the authenticator does not support the Always Require User Verification feature.
    /// Note: If the alwaysUv option ID is present and true the authenticator MUST set the value
    ///       of makeCredUvNotRqd to false.
    #[serde(rename = "alwaysUv")]
    pub always_uv: Option<bool>,
}

impl Default for AuthenticatorOptions {
    fn default() -> Self {
        AuthenticatorOptions {
            platform_device: false,
            resident_key: false,
            client_pin: None,
            user_presence: true,
            user_verification: None,
            pin_uv_auth_token: None,
            no_mc_ga_permissions_with_client_pin: None,
            large_blobs: None,
            ep: None,
            bio_enroll: None,
            user_verification_mgmt_preview: None,
            uv_bio_enroll: None,
            authnr_cfg: None,
            uv_acfg: None,
            cred_mgmt: None,
            credential_mgmt_preview: None,
            set_min_pin_length: None,
            make_cred_uv_not_rqd: None,
            always_uv: None,
        }
    }
}

#[allow(non_camel_case_types)]
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum AuthenticatorVersion {
    U2F_V2,
    FIDO_2_0,
    FIDO_2_1_PRE,
    FIDO_2_1,
    #[serde(other)]
    Unknown,
}

#[derive(Clone, Debug, Default, Eq, PartialEq, Serialize)]
pub struct AuthenticatorInfo {
    pub versions: Vec<AuthenticatorVersion>,
    pub extensions: Vec<String>,
    pub aaguid: AAGuid,
    pub options: AuthenticatorOptions,
    pub max_msg_size: Option<usize>,
    pub pin_protocols: Option<Vec<u64>>,
    pub max_credential_count_in_list: Option<usize>,
    pub max_credential_id_length: Option<usize>,
    pub transports: Option<Vec<String>>,
    pub algorithms: Option<Vec<PublicKeyCredentialParameters>>,
    pub max_ser_large_blob_array: Option<u64>,
    pub force_pin_change: Option<bool>,
    pub min_pin_length: Option<u64>,
    pub firmware_version: Option<u64>,
    pub max_cred_blob_length: Option<u64>,
    pub max_rpids_for_set_min_pin_length: Option<u64>,
    pub preferred_platform_uv_attempts: Option<u64>,
    pub uv_modality: Option<u64>,
    pub certifications: Option<BTreeMap<String, u64>>,
    pub remaining_discoverable_credentials: Option<u64>,
    pub vendor_prototype_config_commands: Option<Vec<u64>>,
    pub attestation_formats: Option<Vec<String>>,
    pub uv_count_since_last_pin_entry: Option<u64>,
    pub long_touch_for_reset: Option<bool>,
}

impl AuthenticatorInfo {
    pub fn supports_cred_protect(&self) -> bool {
        self.extensions.contains(&"credProtect".to_string())
    }

    pub fn supports_hmac_secret(&self) -> bool {
        self.extensions.contains(&"hmac-secret".to_string())
    }

    pub fn max_supported_version(&self) -> AuthenticatorVersion {
        let versions = vec![
            AuthenticatorVersion::FIDO_2_1,
            AuthenticatorVersion::FIDO_2_1_PRE,
            AuthenticatorVersion::FIDO_2_0,
            AuthenticatorVersion::U2F_V2,
        ];
        for ver in versions {
            if self.versions.contains(&ver) {
                return ver;
            }
        }
        AuthenticatorVersion::U2F_V2
    }

    pub fn device_is_protected(&self) -> bool {
        self.options.client_pin == Some(true) || self.options.user_verification == Some(true)
    }
}

impl CtapResponse for AuthenticatorInfo {}

macro_rules! parse_next_optional_value {
    ($name:expr, $map:expr) => {
        if $name.is_some() {
            return Err(serde::de::Error::duplicate_field("$name"));
        }
        $name = Some($map.next_value()?);
    };
}

impl<'de> Deserialize<'de> for AuthenticatorInfo {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct AuthenticatorInfoVisitor;

        impl<'de> Visitor<'de> for AuthenticatorInfoVisitor {
            type Value = AuthenticatorInfo;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a byte array")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut versions = Vec::new();
                let mut extensions = Vec::new();
                let mut aaguid = None;
                let mut options = AuthenticatorOptions::default();
                let mut max_msg_size = None;
                let mut pin_protocols: Option<Vec<_>> = None;
                let mut max_credential_count_in_list = None;
                let mut max_credential_id_length = None;
                let mut transports = None;
                let mut algorithms = None;
                let mut max_ser_large_blob_array = None;
                let mut force_pin_change = None;
                let mut min_pin_length = None;
                let mut firmware_version = None;
                let mut max_cred_blob_length = None;
                let mut max_rpids_for_set_min_pin_length = None;
                let mut preferred_platform_uv_attempts = None;
                let mut uv_modality = None;
                let mut certifications = None;
                let mut remaining_discoverable_credentials = None;
                let mut vendor_prototype_config_commands = None;
                let mut attestation_formats = None;
                let mut uv_count_since_last_pin_entry = None;
                let mut long_touch_for_reset = None;
                while let Some(key) = map.next_key()? {
                    match key {
                        0x01 => {
                            if !versions.is_empty() {
                                return Err(serde::de::Error::duplicate_field("versions"));
                            }
                            versions = map.next_value()?;
                        }
                        0x02 => {
                            if !extensions.is_empty() {
                                return Err(serde::de::Error::duplicate_field("extensions"));
                            }
                            extensions = map.next_value()?;
                        }
                        0x03 => {
                            parse_next_optional_value!(aaguid, map);
                        }
                        0x04 => {
                            options = map.next_value()?;
                        }
                        0x05 => {
                            parse_next_optional_value!(max_msg_size, map);
                        }
                        0x06 => {
                            parse_next_optional_value!(pin_protocols, map);
                        }
                        0x07 => {
                            parse_next_optional_value!(max_credential_count_in_list, map);
                        }
                        0x08 => {
                            parse_next_optional_value!(max_credential_id_length, map);
                        }
                        0x09 => {
                            parse_next_optional_value!(transports, map);
                        }
                        0x0a => {
                            if algorithms.is_some() {
                                return Err(serde::de::Error::duplicate_field("algorithms"));
                            }
                            let raw: Vec<Value> = map.next_value()?;
                            let parsed = raw
                                .into_iter()
                                .filter_map(|v| {
                                    match serde_cbor::value::from_value::<
                                        PublicKeyCredentialParameters,
                                    >(v)
                                    {
                                        Ok(p) => Some(p),
                                        Err(e) => {
                                            warn!(
                                                "GetInfo: ignoring unsupported algorithm: {:?}",
                                                e
                                            );
                                            None
                                        }
                                    }
                                })
                                .collect();
                            algorithms = Some(parsed);
                        }
                        0x0b => {
                            parse_next_optional_value!(max_ser_large_blob_array, map);
                        }
                        0x0c => {
                            parse_next_optional_value!(force_pin_change, map);
                        }
                        0x0d => {
                            parse_next_optional_value!(min_pin_length, map);
                        }
                        0x0e => {
                            parse_next_optional_value!(firmware_version, map);
                        }
                        0x0f => {
                            parse_next_optional_value!(max_cred_blob_length, map);
                        }
                        0x10 => {
                            parse_next_optional_value!(max_rpids_for_set_min_pin_length, map);
                        }
                        0x11 => {
                            parse_next_optional_value!(preferred_platform_uv_attempts, map);
                        }
                        0x12 => {
                            parse_next_optional_value!(uv_modality, map);
                        }
                        0x13 => {
                            parse_next_optional_value!(certifications, map);
                        }
                        0x14 => {
                            parse_next_optional_value!(remaining_discoverable_credentials, map);
                        }
                        0x15 => {
                            parse_next_optional_value!(vendor_prototype_config_commands, map);
                        }
                        0x16 => {
                            parse_next_optional_value!(attestation_formats, map);
                        }
                        0x17 => {
                            parse_next_optional_value!(uv_count_since_last_pin_entry, map);
                        }
                        0x18 => {
                            parse_next_optional_value!(long_touch_for_reset, map);
                        }
                        k => {
                            warn!("GetInfo: unexpected key: {:?}", k);
                            let _ = map.next_value::<IgnoredAny>()?;
                            continue;
                        }
                    }
                }

                if versions.is_empty() {
                    return Err(M::Error::custom(
                        "expected at least one version, got none".to_string(),
                    ));
                }

                if let Some(protocols) = &pin_protocols {
                    if protocols.is_empty() {
                        return Err(M::Error::custom(
                            "Token returned empty PIN protocol list, which is not allowed"
                                .to_string(),
                        ));
                    }
                }

                if let Some(aaguid) = aaguid {
                    Ok(AuthenticatorInfo {
                        versions,
                        extensions,
                        aaguid,
                        options,
                        max_msg_size,
                        pin_protocols,
                        max_credential_count_in_list,
                        max_credential_id_length,
                        transports,
                        algorithms,
                        max_ser_large_blob_array,
                        force_pin_change,
                        min_pin_length,
                        firmware_version,
                        max_cred_blob_length,
                        max_rpids_for_set_min_pin_length,
                        preferred_platform_uv_attempts,
                        uv_modality,
                        certifications,
                        remaining_discoverable_credentials,
                        vendor_prototype_config_commands,
                        attestation_formats,
                        uv_count_since_last_pin_entry,
                        long_touch_for_reset,
                    })
                } else {
                    Err(M::Error::custom("No AAGuid specified".to_string()))
                }
            }
        }

        deserializer.deserialize_bytes(AuthenticatorInfoVisitor)
    }
}
