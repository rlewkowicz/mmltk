use crate::{MlsDecode, MlsEncode, MlsSize};
use alloc::{string::String, vec::Vec};

impl MlsSize for str {
    fn mls_encoded_len(&self) -> usize {
        self.as_bytes().mls_encoded_len()
    }
}

impl MlsEncode for str {
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
        self.as_bytes().mls_encode(writer)
    }
}

impl MlsSize for String {
    fn mls_encoded_len(&self) -> usize {
        self.as_str().mls_encoded_len()
    }
}

impl MlsEncode for String {
    fn mls_encode(&self, writer: &mut Vec<u8>) -> Result<(), crate::Error> {
        self.as_str().mls_encode(writer)
    }
}

impl MlsDecode for String {
    fn mls_decode(reader: &mut &[u8]) -> Result<Self, crate::Error> {
        String::from_utf8(Vec::mls_decode(reader)?).map_err(|_| crate::Error::Utf8)
    }
}
