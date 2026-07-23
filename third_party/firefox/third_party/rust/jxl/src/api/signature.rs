// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::{
    api::ProcessingResult,
    error::{Error, Result},
};

/// The magic bytes for a bare JPEG XL codestream.
pub(crate) const CODESTREAM_SIGNATURE: [u8; 2] = [0xff, 0x0a];
/// The magic bytes for a file using the JPEG XL container format.
pub(crate) const CONTAINER_SIGNATURE: [u8; 12] =
    [0, 0, 0, 0xc, b'J', b'X', b'L', b' ', 0xd, 0xa, 0x87, 0xa];

#[derive(Debug, PartialEq)]
pub enum JxlSignatureType {
    Codestream,
    Container,
}

impl JxlSignatureType {
    pub(crate) fn signature(&self) -> &[u8] {
        match self {
            JxlSignatureType::Container => &CONTAINER_SIGNATURE,
            JxlSignatureType::Codestream => &CODESTREAM_SIGNATURE,
        }
    }
}

pub(crate) fn check_signature_internal(file_prefix: &[u8]) -> Result<Option<JxlSignatureType>> {
    let prefix_len = file_prefix.len();

    for st in [JxlSignatureType::Codestream, JxlSignatureType::Container] {
        let len = st.signature().len();
        let len_to_check = prefix_len.min(len);

        if file_prefix[..len_to_check] == st.signature()[..len_to_check] {
            return if prefix_len >= len {
                Ok(Some(st))
            } else {
                Err(Error::OutOfBounds(len - prefix_len))
            };
        }
    }
    Ok(None)
}

/// Checks if the given buffer starts with a valid JPEG XL signature.
///
/// # Returns
///
/// A `ProcessingResult` which is:
/// - `Complete(Some(_))` if a full container or codestream signature is found.
/// - `Complete(None)` if the prefix is definitively not a JXL signature.
/// - `NeedsMoreInput` if the prefix matches a signature but is too short.
pub fn check_signature(file_prefix: &[u8]) -> ProcessingResult<Option<JxlSignatureType>, ()> {
    ProcessingResult::new(check_signature_internal(file_prefix)).unwrap()
}
