use crate::{
    crypto::{PinUvAuthParam, PinUvAuthToken},
    ctap2::server::UserVerificationRequirement,
    errors::{AuthenticatorError, UnsupportedOption},
    transport::errors::HIDError,
    FidoDevice,
};
use serde::{
    de::{Error as SerdeError, IgnoredAny, MapAccess, Visitor},
    ser::{Error as SerError, SerializeMap},
    Deserialize, Deserializer, Serialize, Serializer,
};
use serde_bytes::ByteBuf;
use serde_cbor::{from_slice, to_vec, Value};
use sha2::{Digest, Sha256};
use std::fmt;

use super::{Command, CommandError, CtapResponse, PinUvAuthCommand, RequestCtap2, StatusCode};

#[derive(Debug)]
pub(crate) struct LargeBlobs {
    get: Option<u64>, 
    set: Option<ByteBuf>, 
    offset: u64, 
    length: Option<u64>, 
    pin_uv_auth_param: Option<PinUvAuthParam>, 
}

impl PinUvAuthCommand for LargeBlobs {
    fn set_pin_uv_auth_param(
        &mut self,
        pin_uv_auth_token: Option<PinUvAuthToken>,
    ) -> Result<(), AuthenticatorError> {
        let mut param = None;
        if let Some(token) = pin_uv_auth_token {
            let mut data = vec![0xff; 32];
            data.extend([0x0c, 0x00]);
            data.extend((self.offset as u32).to_le_bytes());
            if let Some(ref set) = self.set {
                data.extend(Sha256::digest(set.as_slice()));
            }
            param = Some(token.derive(&data).map_err(CommandError::Crypto)?);
        }
        self.pin_uv_auth_param = param;
        Ok(())
    }

    fn can_skip_user_verification(
        &mut self,
        _info: &crate::AuthenticatorInfo,
        _uv: UserVerificationRequirement,
    ) -> bool {
        false
    }

    fn set_uv_option(&mut self, _uv: Option<bool>) {
    }

    fn get_pin_uv_auth_param(&self) -> Option<&PinUvAuthParam> {
        self.pin_uv_auth_param.as_ref()
    }

    fn get_rp_id(&self) -> Option<&String> {
        None
    }

    fn hmac_requested(&self) -> bool {
        false
    }
}

impl Serialize for LargeBlobs {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        if self.set.is_none() && self.get.is_none() {
            return Err(SerError::custom("Either set or get has to be set"));
        }
        let mut map_len = 2; 
        if self.length.is_some() {
            map_len += 1;
        }
        if self.pin_uv_auth_param.is_some() {
            map_len += 2;
        }

        let mut map = serializer.serialize_map(Some(map_len))?;
        if let Some(ref get) = self.get {
            map.serialize_entry(&0x01, get)?;
        }
        if let Some(ref set) = self.set {
            map.serialize_entry(&0x02, set)?;
        }
        map.serialize_entry(&0x03, &self.offset)?;
        if let Some(ref length) = self.length {
            map.serialize_entry(&0x04, length)?;
        }
        if let Some(ref pin_uv_auth_param) = self.pin_uv_auth_param {
            map.serialize_entry(&0x05, pin_uv_auth_param)?;
            map.serialize_entry(&0x06, &pin_uv_auth_param.pin_protocol.id())?;
        }
        map.end()
    }
}

impl RequestCtap2 for LargeBlobs {
    type Output = LargeBlobSegment;

    fn command(&self) -> Command {
        Command::LargeBlobs
    }

    fn wire_format(&self) -> Result<Vec<u8>, HIDError> {
        let output = to_vec(&self).map_err(CommandError::Serializing)?;
        trace!("client subcommmand: {:04X?}", &output);
        Ok(output)
    }

    fn handle_response_ctap2<Dev>(
        &self,
        _dev: &mut Dev,
        input: &[u8],
    ) -> Result<Self::Output, HIDError>
    where
        Dev: FidoDevice,
    {
        if input.is_empty() {
            return Err(CommandError::InputTooSmall.into());
        }

        let status: StatusCode = input[0].into();
        if status.is_ok() {
            if input.len() > 1 {
                let payload = &input[1..];
                Ok(payload.to_vec())
            } else {
                Ok(Vec::new())
            }
        } else {
            let data: Option<Value> = if input.len() > 1 {
                let payload = &input[1..];
                Some(from_slice(payload).map_err(CommandError::Deserializing)?)
            } else {
                None
            };
            Err(CommandError::StatusCode(status, data).into())
        }
    }

    fn send_to_virtual_device<Dev: crate::VirtualFidoDevice>(
        &self,
        _dev: &mut Dev,
    ) -> Result<Self::Output, HIDError> {
        unimplemented!()
    }
}

#[derive(Default, Debug, Clone, PartialEq, Eq)]
pub struct LargeBlobArrayElement {
    /// AEAD_AES_256_GCM ciphertext, implicitly including the AEAD “authentication tag” at the end.
    pub ciphertext: Vec<u8>,
    /// AEAD_AES_256_GCM nonce. MUST be exactly 12 bytes long.
    pub nonce: [u8; 12],
    /// Contains the length, in bytes, of the uncompressed data.
    pub orig_size: u64,
}

impl Serialize for LargeBlobArrayElement {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        let map_len = 3; 
        let mut map = serializer.serialize_map(Some(map_len))?;
        map.serialize_entry(&0x01, &self.ciphertext)?;
        map.serialize_entry(&0x02, &self.nonce)?;
        map.serialize_entry(&0x03, &self.orig_size)?;
        map.end()
    }
}

impl<'de> Deserialize<'de> for LargeBlobArrayElement {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct LargeBlobArrayElementVisitor;

        impl<'de> Visitor<'de> for LargeBlobArrayElementVisitor {
            type Value = LargeBlobArrayElement;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a map")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut ciphertext = None; 
                let mut nonce = None; 
                let mut orig_size = None; 

                while let Some(key) = map.next_key()? {
                    match key {
                        0x01 => {
                            if ciphertext.is_some() {
                                return Err(SerdeError::duplicate_field("ciphertext"));
                            }
                            ciphertext = Some(map.next_value()?);
                        }
                        0x02 => {
                            if nonce.is_some() {
                                return Err(SerdeError::duplicate_field("nonce"));
                            }
                            nonce = Some(map.next_value()?);
                        }
                        0x03 => {
                            if orig_size.is_some() {
                                return Err(SerdeError::duplicate_field("orig_size"));
                            }
                            orig_size = Some(map.next_value()?);
                        }
                        k => {
                            warn!("LargeBlobArray: unexpected key: {:?}", k);
                            let _ = map.next_value::<IgnoredAny>()?;
                            continue;
                        }
                    }
                }

                let ciphertext = ciphertext.ok_or_else(|| M::Error::missing_field("ciphertext"))?;
                let nonce = nonce.ok_or_else(|| M::Error::missing_field("nonce"))?;
                let orig_size = orig_size.ok_or_else(|| M::Error::missing_field("orig_size"))?;

                Ok(LargeBlobArrayElement {
                    ciphertext,
                    nonce,
                    orig_size,
                })
            }
        }
        deserializer.deserialize_bytes(LargeBlobArrayElementVisitor)
    }
}

#[derive(Default, Debug, PartialEq, Eq)]
pub struct LargeBlobsResponse {
    pub(crate) large_blob_array: Vec<LargeBlobArrayElement>,
    /// Truncated SHA-256 hash of the preceding bytes
    pub(crate) hash: [u8; 16],
    pub(crate) byte_len: u64,
}

impl<'de> Deserialize<'de> for LargeBlobsResponse {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct LargeBlobsResponseVisitor;

        impl<'de> Visitor<'de> for LargeBlobsResponseVisitor {
            type Value = LargeBlobsResponse;

            fn expecting(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
                formatter.write_str("a map")
            }

            fn visit_map<M>(self, mut map: M) -> Result<Self::Value, M::Error>
            where
                M: MapAccess<'de>,
            {
                let mut response = None; 

                while let Some(key) = map.next_key()? {
                    match key {
                        0x01 => {
                            if response.is_some() {
                                return Err(SerdeError::duplicate_field("response"));
                            }
                            let payload: ByteBuf = map.next_value()?;
                            if payload.len() < 17 {
                                return Err(SerdeError::invalid_length(
                                    payload.len(),
                                    &">= 17 bytes",
                                ));
                            }
                            let (mut large_blob, mut hash_slice) =
                                payload.split_at(payload.len() - 16);

                            let expected_hash = Sha256::digest(large_blob);
                            let default_large_blob = [0x80];
                            let default_hash = [
                                0x76, 0xbe, 0x8b, 0x52, 0x8d, 0x00, 0x75, 0xf7, 0xaa, 0xe9, 0x8d,
                                0x6f, 0xa5, 0x7a, 0x6d, 0x3c,
                            ];
                            if &expected_hash[0..16] != hash_slice {
                                warn!("Large blob array hash doesn't match with the expected value! Assuming an empty array.");
                                large_blob = &default_large_blob;
                                hash_slice = &default_hash;
                            }

                            let byte_len = large_blob.len() as u64;
                            let large_blob_array: Vec<LargeBlobArrayElement> =
                                from_slice(large_blob).map_err(M::Error::custom)?;
                            let mut hash = [0u8; 16];
                            hash.copy_from_slice(hash_slice);
                            response = Some(LargeBlobsResponse {
                                large_blob_array,
                                hash,
                                byte_len,
                            });
                        }
                        k => {
                            warn!("LargeBlobsResponse: unexpected key: {:?}", k);
                            let _ = map.next_value::<IgnoredAny>()?;
                            continue;
                        }
                    }
                }
                let response =
                    response.ok_or_else(|| M::Error::missing_field("large_blob_bytes"))?;

                Ok(response)
            }
        }
        deserializer.deserialize_bytes(LargeBlobsResponseVisitor)
    }
}

pub type LargeBlobSegment = Vec<u8>;

impl CtapResponse for LargeBlobSegment {}

pub fn read_large_blob_array<Dev>(
    dev: &mut Dev,
    keep_alive: &dyn Fn() -> bool,
) -> Result<LargeBlobsResponse, AuthenticatorError>
where
    Dev: FidoDevice,
{
    let max_msg_size = dev
        .get_authenticator_info()
        .and_then(|i| i.max_msg_size)
        .unwrap_or(1024);
    if max_msg_size <= 64 {
        return Err(AuthenticatorError::UnsupportedOption(
            UnsupportedOption::LargeBlobs,
        ));
    }
    let max_fragment_length = max_msg_size - 64;
    let mut bytes = vec![];
    let mut offset = 0;
    loop {
        let cmd = LargeBlobs {
            get: Some(max_fragment_length as u64),
            set: None,
            offset,
            length: None,
            pin_uv_auth_param: None,
        };
        let mut segment = dev.send_cbor_cancellable(&cmd, keep_alive)?;
        let segment_len = segment.len();
        bytes.append(&mut segment);
        if segment_len < max_fragment_length {
            break;
        } else {
            offset += segment_len as u64;
            continue;
        }
    }
    let response: LargeBlobsResponse = from_slice(&bytes).map_err(CommandError::Deserializing)?;
    Ok(response)
}

pub fn write_large_blob_segment<Dev>(
    dev: &mut Dev,
    keep_alive: &dyn Fn() -> bool,
    bytes: &[u8],
    initial_offset: u64,
    pin_uv_auth_token: Option<PinUvAuthToken>,
) -> Result<(), AuthenticatorError>
where
    Dev: FidoDevice,
{
    let max_msg_size = dev
        .get_authenticator_info()
        .and_then(|i| i.max_msg_size)
        .unwrap_or(1024);
    if max_msg_size <= 64 {
        return Err(AuthenticatorError::UnsupportedOption(
            UnsupportedOption::LargeBlobs,
        ));
    }
    let max_fragment_length = max_msg_size - 64;
    let total_length = bytes.len();
    let mut offset = initial_offset;
    for chunk in bytes.chunks(max_fragment_length) {
        let chunk_len = chunk.len();
        let mut cmd = LargeBlobs {
            get: None,
            set: Some(ByteBuf::from(chunk)),
            offset,
            length: if offset == 0 {
                Some(total_length as u64)
            } else {
                None
            },
            pin_uv_auth_param: None,
        };
        cmd.set_pin_uv_auth_param(pin_uv_auth_token.clone())?;
        dev.send_cbor_cancellable(&cmd, keep_alive)?;
        offset += chunk_len as u64;
    }
    Ok(())
}

pub fn add_large_blob<Dev>(
    dev: &mut Dev,
    keep_alive: &dyn Fn() -> bool,
    blob: LargeBlobArrayElement,
    pin_uv_auth_token: Option<PinUvAuthToken>,
) -> Result<(), AuthenticatorError>
where
    Dev: FidoDevice,
{
    let mut array = read_large_blob_array(dev, keep_alive)?;
    array.large_blob_array.push(blob);
    let mut bytes = to_vec(&array.large_blob_array).map_err(CommandError::Serializing)?;

    let mut hasher = Sha256::new();
    hasher.update(&bytes);
    let hash = hasher.finalize();
    bytes.extend_from_slice(&hash[..16]);
    write_large_blob_segment(dev, keep_alive, &bytes, 0, pin_uv_auth_token)
}
