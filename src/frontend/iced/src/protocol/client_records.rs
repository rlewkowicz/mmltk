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
pub(crate) fn reserve(bytes: &mut Vec<u8>, additional: usize, limit: usize) -> Result<(), super::ProtocolError> {
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
impl<T: Compact> Compact for [T] {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> {
        compact_head(4, self.len() as u64, bytes)?;
        for item in self { item.compact(bytes)?; }
        Ok(())
    }
}
impl<T: Compact> Compact for Vec<T> {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> { self.as_slice().compact(bytes) }
}
impl<T: Compact, const N: usize> Compact for [T; N] {
    fn compact(&self, bytes: &mut Vec<u8>) -> Result<(), super::ProtocolError> { self.as_slice().compact(bytes) }
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
    crate::generated::encode_interaction_record(crate::generated::BROWSER_PROTOCOL_VERSION, endpoint, payload, output)
}
