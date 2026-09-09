use crate::application_codec::Value;

#[derive(Debug, Clone, PartialEq)]
pub struct IntentField {
    pub field_id: u64,
    pub value: Value,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Intent {
    pub correlation: u64,
    pub endpoint_id: u64,
    pub fields: Vec<IntentField>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Interaction {
    pub replaceable: bool,
    pub endpoint_id: u64,
    pub value: Vec<u8>,
}

// Structural implementations are generated from the same native declarations
// that the native compact decoder validates. No field names enter this path.
pub trait Compact {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError>;
}
fn reserve(bytes: &mut Vec<u8>, additional: usize, limit: usize) -> Result<(), super::ProtocolError> {
    if bytes.len().checked_add(additional).is_none_or(|length| length > limit) {
        return Err(super::ProtocolError("compact encoding exceeds byte capacity".into()));
    }
    bytes.try_reserve(additional).map_err(|_| super::ProtocolError("compact encoding allocation failed".into()))
}
pub fn compact_head(major: u8, value: u64, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
    reserve(bytes, 9, crate::generated::MAX_INTENT_VALUE_BYTES)?;
    super::cbor::head(major, value, bytes);
    Ok(())
}
macro_rules! unsigned {
    ($($ty:ty),*) => { $(impl Compact for $ty {
        fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> { compact_head(0, *self as u64, bytes) }
    })* };
}
unsigned!(u8, u16, u32, u64, usize);
macro_rules! signed {
    ($($ty:ty),*) => { $(impl Compact for $ty {
        fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
            let value = *self as i64;
            if value < 0 { compact_head(1, value.unsigned_abs() - 1, bytes) }
            else { compact_head(0, value as u64, bytes) }
        }
    })* };
}
signed!(i8, i16, i32, i64, isize);
impl Compact for bool {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
        reserve(bytes, 1, crate::generated::MAX_INTENT_VALUE_BYTES)?;
        bytes.push(if *self { 0xf5 } else { 0xf4 });
        Ok(())
    }
}
impl Compact for f32 {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> { (*self as f64).compact(bytes) }
}
impl Compact for f64 {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
        reserve(bytes, 9, crate::generated::MAX_INTENT_VALUE_BYTES)?;
        super::cbor::encode_value(&Value::Float(*self), bytes)
    }
}
impl<T: Compact> Compact for Option<T> {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
        match self { Some(value) => value.compact(bytes), None => {
            reserve(bytes, 1, crate::generated::MAX_INTENT_VALUE_BYTES)?;
            bytes.push(0xf6);
            Ok(())
        } }
    }
}
impl<T: Compact> Compact for Vec<T> {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
        compact_head(4, self.len() as u64, bytes)?;
        for item in self { item.compact(bytes)?; }
        Ok(())
    }
}
pub fn compact_bytes(value: &impl Compact) -> Result<Vec<u8>, super::ProtocolError> {
    let mut bytes = Vec::new();
    value.compact(&mut bytes)?;
    Ok(bytes)
}
pub fn encode_compact_interaction(endpoint: u64, value: &impl Compact, scratch: &mut Vec<u8>, output: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
    scratch.clear();
    value.compact(scratch)?;
    encode_interaction_bytes(endpoint, scratch, output)
}
pub fn encode_interaction_bytes(endpoint: u64, payload: &[u8], output: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
    use super::cbor::{head, encode_text_item};
    if endpoint == 0 || payload.len() > crate::generated::MAX_INTENT_VALUE_BYTES {
        return Err(super::ProtocolError("invalid compact interaction endpoint or byte capacity".into()));
    }
    output.clear();
    reserve(output, payload.len().checked_add(96).ok_or_else(|| super::ProtocolError("compact envelope size overflow".into()))?, crate::generated::MAX_RECORD_WIRE_BYTES)?;
    head(5, 2, output);
    encode_text_item("kind", output);
    encode_text_item("Interaction", output);
    encode_text_item("payload", output);
    head(5, 3, output);
    encode_text_item("protocol_version", output);
    head(0, crate::generated::BROWSER_PROTOCOL_VERSION, output);
    encode_text_item("endpoint_id", output);
    head(0, endpoint, output);
    encode_text_item("value", output);
    head(2, payload.len() as u64, output);
    output.extend_from_slice(payload);
    Ok(())
}
