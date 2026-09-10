use std::fmt;

use crate::application_codec::Value;
use crate::generated::{
    MAX_ERROR_DETAIL_BYTES, MAX_INTENT_VALUE_BYTES, ReflectedRecordPath, ServerRecordKind, MAX_INTENT_VALUE_DEPTH, MAX_INTENT_VALUE_ITEMS,
    MAX_OUTPUT_VALUE_BYTES, MAX_OUTPUT_VALUE_ITEMS, MAX_RECORD_WIRE_BYTES, MAX_SNAPSHOT_COUNT,
    SYSTEM_EVENT_FIELD_COUNT,
};

#[derive(Clone, Copy)]
struct CompleteItemBudget {
    max_items: usize,
    max_depth: usize,
}

const PROTOCOL_ITEM_BUDGET: CompleteItemBudget = CompleteItemBudget {
    max_items: MAX_RECORD_WIRE_BYTES,
    max_depth: MAX_INTENT_VALUE_DEPTH,
};

#[derive(Debug, Clone, PartialEq)]
pub struct Envelope {
    pub kind: String,
    pub payload: Value,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProtocolError(pub String);

impl fmt::Display for ProtocolError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for ProtocolError {}

pub fn object<K: Into<String>>(fields: impl IntoIterator<Item = (K, Value)>) -> Value {
    Value::Object(
        fields
            .into_iter()
            .map(|(key, value)| (key.into(), value))
            .collect(),
    )
}

pub fn reject_unknown_fields<E>(
    fields: &[(String, Value)],
    mut known: impl FnMut(&str) -> bool,
    error: impl FnOnce(&str) -> E,
) -> Result<(), E> {
    fields
        .iter()
        .find_map(|(name, _)| (!known(name)).then_some(name.as_str()))
        .map_or(Ok(()), |name| Err(error(name)))
}

pub fn encode_envelope(kind: &str, payload: &Value) -> Result<Vec<u8>, ProtocolError> {
    let size = checked_size(1, checked_size(5, head_len(kind.len() as u64))?)?;
    let size = checked_size(size, kind.len())?;
    let size = checked_size(size, 8)?;
    let size = checked_size(size, encoded_len(payload)?)?;
    if size > MAX_RECORD_WIRE_BYTES {
        return Err(ProtocolError(
            "CBOR envelope exceeds protocol byte limit".into(),
        ));
    }
    let mut output = Vec::with_capacity(size);
    head(5, 2, &mut output);
    encode_text_item("kind", &mut output);
    encode_text_item(kind, &mut output);
    encode_text_item("payload", &mut output);
    encode_value(payload, &mut output)?;
    Ok(output)
}

pub fn decode_envelope(bytes: &[u8]) -> Result<Envelope, ProtocolError> {
    if bytes.len() > MAX_RECORD_WIRE_BYTES {
        return Err(ProtocolError(
            "CBOR envelope exceeds protocol byte limit".into(),
        ));
    }
    preflight_protocol_record(bytes)?;
    let mut cursor = 0;
    let value = decode_value(bytes, &mut cursor, MAX_INTENT_VALUE_DEPTH, 0)?;
    if cursor != bytes.len() {
        return Err(ProtocolError("trailing CBOR bytes".into()));
    }
    let Value::Object(mut fields) = value else {
        return Err(ProtocolError("envelope must be a map".into()));
    };
    if fields.len() != 2 || fields[0].0 != "kind" || fields[1].0 != "payload" {
        return Err(ProtocolError("envelope fields are invalid".into()));
    }
    let payload = fields.pop().expect("validated payload").1;
    let Value::Text(kind) = fields.pop().expect("validated kind").1 else {
        return Err(ProtocolError("envelope kind must be text".into()));
    };
    Ok(Envelope { kind, payload })
}

fn preflight_protocol_record(bytes: &[u8]) -> Result<(), ProtocolError> {
    const KIND_PREFIX: &[u8] = &[0xa2, 0x64, b'k', b'i', b'n', b'd'];
    if !bytes.starts_with(KIND_PREFIX) {
        return Err(ProtocolError(
            "browser protocol envelope prefix is invalid".into(),
        ));
    }
    let kind_head = *bytes
        .get(KIND_PREFIX.len())
        .ok_or_else(|| ProtocolError("truncated browser protocol record kind".into()))?;
    let kind_length = usize::from(kind_head & 0x1f);
    if kind_head >> 5 != 3 || kind_length > 23 {
        return Err(ProtocolError(
            "browser protocol record kind is invalid".into(),
        ));
    }
    let kind = bytes
        .get(KIND_PREFIX.len() + 1..KIND_PREFIX.len() + 1 + kind_length)
        .ok_or_else(|| ProtocolError("truncated browser protocol record kind".into()))?;
    let record = match ServerRecordKind::parse(kind) {
        Some(kind) => match kind {
            ServerRecordKind::Bootstrap => RecordKind::Bootstrap,
            ServerRecordKind::IntentReply => RecordKind::IntentReply,
            ServerRecordKind::SystemEvent => RecordKind::SystemEvent,
            ServerRecordKind::InputProgress | ServerRecordKind::InteractionRejected => {
                RecordKind::Reflected(kind.reflected_preflight().ok_or_else(|| ProtocolError("missing reflected server preflight".into()))?)
            }
        },
        None => RecordKind::Unknown,
    };
    ciborium_walk_complete_item(bytes, PROTOCOL_ITEM_BUDGET, PreflightPath::Envelope(record))
}

#[derive(Clone, Copy)]
enum RecordKind {
    Bootstrap,
    IntentReply,
    SystemEvent,
    Reflected(ReflectedRecordPath),
    Unknown,
}

#[derive(Clone, Copy)]
enum PreflightPath {
    Envelope(RecordKind),
    Payload(RecordKind),
    Reflected(ReflectedRecordPath),
    BootstrapSnapshots,
    BootstrapSnapshot,
    BootstrapSnapshotValue,
    ReplyError,
    ReplyErrorDetail,
    OrdinaryDynamic,
    Other,
}

impl PreflightPath {
    fn max_collection_items(self) -> usize {
        match self {
            Self::Envelope(_) => 2,
            Self::Payload(RecordKind::Bootstrap) => 4,
            Self::Payload(RecordKind::IntentReply) => 4,
            Self::Payload(RecordKind::Reflected(path)) | Self::Reflected(path) => path.max_collection_items(),
            Self::Payload(RecordKind::SystemEvent) => SYSTEM_EVENT_FIELD_COUNT,
            Self::BootstrapSnapshots => MAX_SNAPSHOT_COUNT,
            Self::BootstrapSnapshot | Self::ReplyError => 2,
            Self::BootstrapSnapshotValue | Self::OrdinaryDynamic => MAX_OUTPUT_VALUE_ITEMS,
            Self::Payload(RecordKind::Unknown) | Self::ReplyErrorDetail | Self::Other => {
                MAX_INTENT_VALUE_ITEMS
            }
        }
    }

    fn max_leaf_bytes(self) -> usize {
        match self {
            Self::ReplyErrorDetail => MAX_ERROR_DETAIL_BYTES,
            Self::Payload(RecordKind::Reflected(path)) | Self::Reflected(path) => path.max_leaf_bytes(),
            Self::BootstrapSnapshotValue | Self::OrdinaryDynamic => MAX_OUTPUT_VALUE_BYTES,
            _ => MAX_INTENT_VALUE_BYTES,
        }
    }

    fn max_key_bytes(self) -> usize {
        match self {
            Self::Payload(RecordKind::Reflected(path)) | Self::Reflected(path) => path.max_key_bytes(),
            _ => self.max_leaf_bytes(),
        }
    }

    fn array_child(self) -> Self {
        match self {
            Self::BootstrapSnapshots => Self::BootstrapSnapshot,
            Self::BootstrapSnapshotValue | Self::OrdinaryDynamic => Self::OrdinaryDynamic,
            _ => Self::Other,
        }
    }

    fn map_child(self, key: &[u8]) -> Self {
        match (self, key) {
            (Self::Envelope(record), b"payload") => Self::Payload(record),
            (Self::Payload(RecordKind::Bootstrap), b"snapshots") => Self::BootstrapSnapshots,
            (Self::BootstrapSnapshot, b"value") => Self::BootstrapSnapshotValue,
            (Self::Payload(RecordKind::IntentReply), b"result") => Self::OrdinaryDynamic,
            (Self::Payload(RecordKind::IntentReply), b"error") => Self::ReplyError,
            (Self::ReplyError, b"detail") => Self::ReplyErrorDetail,
            (Self::Payload(RecordKind::Reflected(path)) | Self::Reflected(path), key) => Self::Reflected(path.map_child(key)),
            (Self::Payload(RecordKind::SystemEvent), b"value") => Self::OrdinaryDynamic,
            (Self::BootstrapSnapshotValue | Self::OrdinaryDynamic, _) => Self::OrdinaryDynamic,
            _ => Self::Other,
        }
    }
}

fn ciborium_walk_complete_item(
    bytes: &[u8],
    budget: CompleteItemBudget,
    path: PreflightPath,
) -> Result<(), ProtocolError> {
    let mut decoder = ciborium_ll::Decoder::from(bytes);
    let mut items = 0_usize;
    ciborium_walk_item(bytes, &mut decoder, budget, &mut items, 0, path)?;
    if decoder.offset() != bytes.len() {
        return Err(ProtocolError("trailing CBOR bytes".into()));
    }
    Ok(())
}

fn ciborium_walk_item(
    bytes: &[u8],
    decoder: &mut ciborium_ll::Decoder<&[u8]>,
    budget: CompleteItemBudget,
    items: &mut usize,
    depth: usize,
    path: PreflightPath,
) -> Result<(), ProtocolError> {
    use ciborium_ll::{Header, simple};

    if depth > budget.max_depth {
        return Err(ProtocolError("CBOR nesting limit".into()));
    }
    if *items >= budget.max_items {
        return Err(ProtocolError("CBOR item limit".into()));
    }
    *items += 1;
    let header = decoder
        .pull()
        .map_err(|error| ProtocolError(format!("invalid CBOR item: {error:?}")))?;
    match header {
        Header::Positive(_) | Header::Negative(_) => Ok(()),
        Header::Float(value) if value.is_finite() => Ok(()),
        Header::Simple(simple::FALSE | simple::TRUE | simple::NULL) => Ok(()),
        Header::Bytes(Some(length)) => {
            if length > path.max_leaf_bytes() {
                return Err(ProtocolError(
                    "browser protocol value exceeds byte limit".into(),
                ));
            }
            let mut segments = decoder.bytes(Some(length));
            while let Some(mut segment) = segments
                .pull()
                .map_err(|error| ProtocolError(format!("invalid CBOR bytes: {error:?}")))?
            {
                let mut scratch = [0_u8; 256];
                while segment
                    .pull(&mut scratch)
                    .map_err(|error| ProtocolError(format!("truncated CBOR bytes: {error:?}")))?
                    .is_some()
                {}
            }
            Ok(())
        }
        Header::Text(Some(length)) => {
            if length > path.max_leaf_bytes() {
                return Err(ProtocolError(
                    "browser protocol value exceeds byte limit".into(),
                ));
            }
            consume_ciborium_text(decoder, length, "text")?;
            Ok(())
        }
        Header::Array(Some(count)) => {
            if count > path.max_collection_items() {
                return Err(ProtocolError(
                    "browser protocol array exceeds item limit".into(),
                ));
            }
            let child_path = path.array_child();
            for _ in 0..count {
                ciborium_walk_item(bytes, decoder, budget, items, depth + 1, child_path)?;
            }
            Ok(())
        }
        Header::Map(Some(count)) => {
            if count > path.max_collection_items() {
                return Err(ProtocolError(
                    "browser protocol map exceeds item limit".into(),
                ));
            }
            for _ in 0..count {
                if *items >= budget.max_items {
                    return Err(ProtocolError("CBOR item limit".into()));
                }
                *items += 1;
                let key_header = decoder
                    .pull()
                    .map_err(|error| ProtocolError(format!("invalid CBOR map key: {error:?}")))?;
                let Header::Text(Some(length)) = key_header else {
                    return Err(ProtocolError("CBOR map key is not definite text".into()));
                };
                if length > path.max_key_bytes() {
                    return Err(ProtocolError(
                        "browser protocol map key exceeds byte limit".into(),
                    ));
                }
                let key_offset = decoder.offset();
                consume_ciborium_text(decoder, length, "map key")?;
                let key_end = key_offset
                    .checked_add(length)
                    .ok_or_else(|| ProtocolError("CBOR map key length overflow".into()))?;
                let key = bytes
                    .get(key_offset..key_end)
                    .ok_or_else(|| ProtocolError("truncated CBOR map key".into()))?;
                ciborium_walk_item(
                    bytes,
                    decoder,
                    budget,
                    items,
                    depth + 1,
                    path.map_child(key),
                )?;
            }
            Ok(())
        }
        Header::Float(_)
        | Header::Simple(_)
        | Header::Bytes(None)
        | Header::Text(None)
        | Header::Array(None)
        | Header::Map(None)
        | Header::Tag(_)
        | Header::Break => Err(ProtocolError("unsupported CBOR item".into())),
    }
}

/// Consume a definite CBOR text item through ciborium-ll's segmented API.
/// `Decoder::text` validates UTF-8 as it yields `&str`; this path intentionally
/// retains none of the text because generic structural admission only owns
/// framing and resource budgets.
fn consume_ciborium_text(
    decoder: &mut ciborium_ll::Decoder<&[u8]>,
    length: usize,
    context: &str,
) -> Result<(), ProtocolError> {
    let mut segments = decoder.text(Some(length));
    while let Some(mut segment) = segments
        .pull()
        .map_err(|error| ProtocolError(format!("invalid CBOR {context}: {error:?}")))?
    {
        let mut scratch = [0_u8; 256];
        while segment
            .pull(&mut scratch)
            .map_err(|error| ProtocolError(format!("truncated CBOR {context}: {error:?}")))?
            .is_some()
        {}
    }
    Ok(())
}

pub fn encode_value(value: &Value, output: &mut Vec<u8>) -> Result<(), ProtocolError> {
    match value {
        Value::Null => output.push(0xf6),
        Value::Bool(false) => output.push(0xf4),
        Value::Bool(true) => output.push(0xf5),
        Value::Signed(value) => {
            if *value >= 0 {
                head(0, *value as u64, output);
            } else {
                head(1, value.unsigned_abs() - 1, output);
            }
        }
        Value::Unsigned(value) => head(0, *value, output),
        Value::Float(value) => write_float(*value, output)?,
        Value::Text(text) => {
            head(3, text.len() as u64, output);
            output.extend(text.as_bytes());
        }
        Value::Bytes(bytes) => {
            head(2, bytes.len() as u64, output);
            output.extend(bytes);
        }
        Value::Array(values) => {
            encode_value_array(values, output)?;
        }
        Value::Object(fields) => {
            encode_value_object(fields, output)?;
        }
    }
    Ok(())
}

/// Enforces the recursive bounds attached to native `wire::Value` client
/// members. This owns only the dynamic CBOR vocabulary; individual records
/// retain their reflected scalar and object-shape requirements.
pub(crate) fn validate_client_dynamic_value(value: &Value) -> Result<(), ProtocolError> {
    validate_dynamic_value(
        value,
        MAX_INTENT_VALUE_BYTES,
        MAX_INTENT_VALUE_ITEMS,
        MAX_INTENT_VALUE_DEPTH,
    )
}

pub(crate) fn validate_server_dynamic_value(value: &Value) -> Result<(), ProtocolError> {
    validate_dynamic_value(
        value,
        MAX_OUTPUT_VALUE_BYTES,
        MAX_OUTPUT_VALUE_ITEMS,
        MAX_INTENT_VALUE_DEPTH,
    )
}

fn validate_dynamic_value(
    value: &Value,
    max_bytes: usize,
    max_items: usize,
    max_depth: usize,
) -> Result<(), ProtocolError> {
    validate_dynamic_value_at(value, max_bytes, max_items, max_depth, 0)
}

fn validate_dynamic_value_at(
    value: &Value,
    max_bytes: usize,
    max_items: usize,
    max_depth: usize,
    depth: usize,
) -> Result<(), ProtocolError> {
    if depth > max_depth {
        return Err(ProtocolError("dynamic value exceeds nesting limit".into()));
    }

    if let Some(text) = value.text() {
        return (text.len() <= max_bytes)
            .then_some(())
            .ok_or_else(|| ProtocolError("dynamic text exceeds byte limit".into()));
    }
    if let Some(bytes) = value.bytes() {
        return (bytes.len() <= max_bytes)
            .then_some(())
            .ok_or_else(|| ProtocolError("dynamic bytes exceed byte limit".into()));
    }
    if let Some(values) = value.array() {
        if values.len() > max_items {
            return Err(ProtocolError("dynamic array exceeds item limit".into()));
        }
        return values.iter().try_for_each(|child| {
            validate_dynamic_value_at(child, max_bytes, max_items, max_depth, depth + 1)
        });
    }
    if let Some(fields) = value.object() {
        if fields.len() > max_items {
            return Err(ProtocolError("dynamic object exceeds item limit".into()));
        }
        for (index, (name, child)) in fields.iter().enumerate() {
            if name.len() > max_bytes {
                return Err(ProtocolError(
                    "dynamic object key exceeds byte limit".into(),
                ));
            }
            if fields[..index].iter().any(|(prior, _)| prior == name) {
                return Err(ProtocolError("dynamic object has duplicate key".into()));
            }
            validate_dynamic_value_at(child, max_bytes, max_items, max_depth, depth + 1)?;
        }
    }
    if matches!(value, Value::Float(number) if !number.is_finite()) {
        return Err(ProtocolError("dynamic value has non-finite float".into()));
    }
    Ok(())
}

fn encode_value_array(values: &[Value], output: &mut Vec<u8>) -> Result<(), ProtocolError> {
    head(4, values.len() as u64, output);
    for value in values {
        encode_value(value, output)?;
    }
    Ok(())
}

fn encode_value_object(
    fields: &[(String, Value)],
    output: &mut Vec<u8>,
) -> Result<(), ProtocolError> {
    head(5, fields.len() as u64, output);
    for (key, value) in fields {
        head(3, key.len() as u64, output);
        output.extend(key.as_bytes());
        encode_value(value, output)?;
    }
    Ok(())
}

/// Measure the protocol's canonical representation without allocating an
/// intermediate byte buffer.  Domain staging uses this to uphold the native
/// per-domain budgets before the new document becomes visible.
pub fn encoded_len(value: &Value) -> Result<usize, ProtocolError> {
    measure_value(value, MAX_INTENT_VALUE_DEPTH, 0)
}

fn measure_value(value: &Value, max_depth: usize, depth: usize) -> Result<usize, ProtocolError> {
    if depth > max_depth {
        return Err(ProtocolError("CBOR nesting limit".into()));
    }
    match value {
        Value::Null | Value::Bool(_) => Ok(1),
        Value::Signed(value) if *value >= 0 => Ok(head_len(*value as u64)),
        Value::Signed(value) => Ok(head_len(value.unsigned_abs() - 1)),
        Value::Unsigned(value) => Ok(head_len(*value)),
        Value::Float(value) => float_len(*value),
        Value::Text(value) => checked_size(head_len(value.len() as u64), value.len()),
        Value::Bytes(value) => checked_size(head_len(value.len() as u64), value.len()),
        Value::Array(values) => values
            .iter()
            .try_fold(head_len(values.len() as u64), |size, value| {
                checked_size(size, measure_value(value, max_depth, depth + 1)?)
            }),
        Value::Object(fields) => {
            fields
                .iter()
                .try_fold(head_len(fields.len() as u64), |size, (key, value)| {
                    let size = checked_size(size, head_len(key.len() as u64))?;
                    let size = checked_size(size, key.len())?;
                    checked_size(size, measure_value(value, max_depth, depth + 1)?)
                })
        }
    }
}

fn float_len(value: f64) -> Result<usize, ProtocolError> {
    if !value.is_finite() {
        return Err(ProtocolError("non-finite float".into()));
    }
    let as_f32 = value as f32;
    if f64::from(as_f32) != value {
        return Ok(9);
    }
    Ok(if half_to_f32(f32_to_half(as_f32)) == as_f32 {
        3
    } else {
        5
    })
}

fn checked_size(left: usize, right: usize) -> Result<usize, ProtocolError> {
    left.checked_add(right)
        .ok_or_else(|| ProtocolError("CBOR size overflow".into()))
}

fn head_len(value: u64) -> usize {
    match value {
        0..=23 => 1,
        24..=0xff => 2,
        0x100..=0xffff => 3,
        0x1_0000..=0xffff_ffff => 5,
        _ => 9,
    }
}

pub(crate) fn encode_text_item(value: &str, output: &mut Vec<u8>) {
    head(3, value.len() as u64, output);
    output.extend_from_slice(value.as_bytes());
}

pub(crate) fn head(major: u8, value: u64, output: &mut Vec<u8>) {
    match value {
        0..=23 => output.push((major << 5) | value as u8),
        24..=0xff => output.extend([(major << 5) | 24, value as u8]),
        0x100..=0xffff => {
            output.push((major << 5) | 25);
            output.extend((value as u16).to_be_bytes());
        }
        0x1_0000..=0xffff_ffff => {
            output.push((major << 5) | 26);
            output.extend((value as u32).to_be_bytes());
        }
        _ => {
            output.push((major << 5) | 27);
            output.extend(value.to_be_bytes());
        }
    }
}

fn decode_value(
    bytes: &[u8],
    cursor: &mut usize,
    max_depth: usize,
    depth: usize,
) -> Result<Value, ProtocolError> {
    if depth > max_depth {
        return Err(ProtocolError("CBOR nesting limit".into()));
    }
    let first = *bytes
        .get(*cursor)
        .ok_or_else(|| ProtocolError("truncated CBOR".into()))?;
    *cursor += 1;
    let major = first >> 5;
    if major == 7 {
        return match first & 31 {
            20 => Ok(Value::Bool(false)),
            21 => Ok(Value::Bool(true)),
            22 => Ok(Value::Null),
            25 => float(f64::from(half_to_f32(u16::from_be_bytes(
                take(bytes, cursor, 2)?.try_into().expect("fixed half"),
            )))),
            26 => {
                let value = f32::from_bits(u32::from_be_bytes(
                    take(bytes, cursor, 4)?.try_into().expect("fixed float"),
                ));
                if half_to_f32(f32_to_half(value)) == value {
                    return Err(ProtocolError("nonminimal CBOR float".into()));
                }
                float(f64::from(value))
            }
            27 => {
                let value = f64::from_bits(u64::from_be_bytes(
                    take(bytes, cursor, 8)?.try_into().expect("fixed float"),
                ));
                if f64::from(value as f32) == value {
                    return Err(ProtocolError("nonminimal CBOR float".into()));
                }
                float(value)
            }
            _ => Err(ProtocolError("unsupported CBOR simple value".into())),
        };
    }
    let value = length(first & 31, bytes, cursor)?;
    match major {
        0 => Ok(Value::Unsigned(value)),
        1 if value <= i64::MAX as u64 => Ok(Value::Signed(-1_i64 - value as i64)),
        1 => Err(ProtocolError("negative integer overflow".into())),
        2 => Ok(Value::Bytes(take(bytes, cursor, value)?.to_vec())),
        3 => Ok(Value::Text(
            std::str::from_utf8(take(bytes, cursor, value)?)
                .map_err(|_| ProtocolError("invalid UTF-8 text".into()))?
                .into(),
        )),
        4 => {
            let count = usize::try_from(value)
                .map_err(|_| ProtocolError("CBOR item count overflow".into()))?;
            if count > bytes.len().saturating_sub(*cursor) {
                return Err(ProtocolError("truncated CBOR array".into()));
            }
            let mut values = Vec::with_capacity(count);
            for _ in 0..count {
                values.push(decode_value(bytes, cursor, max_depth, depth + 1)?);
            }
            Ok(Value::Array(values))
        }
        5 => {
            let count = usize::try_from(value)
                .map_err(|_| ProtocolError("CBOR item count overflow".into()))?;
            if count > bytes.len().saturating_sub(*cursor) / 2 {
                return Err(ProtocolError("truncated CBOR map".into()));
            }
            let mut fields = Vec::with_capacity(count);
            for _ in 0..count {
                let key = decode_value(bytes, cursor, max_depth, depth + 1)?
                    .text()
                    .ok_or_else(|| ProtocolError("CBOR map key is not text".into()))?
                    .to_owned();
                if fields.iter().any(|(known, _)| known == &key) {
                    return Err(ProtocolError("duplicate CBOR map key".into()));
                }
                fields.push((key, decode_value(bytes, cursor, max_depth, depth + 1)?));
            }
            Ok(Value::Object(fields))
        }
        _ => Err(ProtocolError("unsupported CBOR major type".into())),
    }
}

fn float(value: f64) -> Result<Value, ProtocolError> {
    value
        .is_finite()
        .then_some(Value::Float(value))
        .ok_or_else(|| ProtocolError("non-finite float".into()))
}

fn write_float(value: f64, output: &mut Vec<u8>) -> Result<(), ProtocolError> {
    if !value.is_finite() {
        return Err(ProtocolError("non-finite float".into()));
    }
    let as_f32 = value as f32;
    if f64::from(as_f32) == value {
        let half = f32_to_half(as_f32);
        if half_to_f32(half) == as_f32 {
            output.push(0xf9);
            output.extend(half.to_be_bytes());
        } else {
            output.push(0xfa);
            output.extend(as_f32.to_bits().to_be_bytes());
        }
    } else {
        output.push(0xfb);
        output.extend(value.to_bits().to_be_bytes());
    }
    Ok(())
}

fn half_to_f32(bits: u16) -> f32 {
    let sign = u32::from(bits & 0x8000) << 16;
    let exponent = u32::from((bits >> 10) & 0x1f);
    let fraction = u32::from(bits & 0x03ff);
    let raw = match exponent {
        0 if fraction == 0 => sign,
        0 => {
            let mut fraction = fraction;
            let mut exponent = -14_i32;
            while fraction & 0x400 == 0 {
                fraction <<= 1;
                exponent -= 1;
            }
            sign | (u32::try_from(exponent + 127).expect("half exponent") << 23)
                | ((fraction & 0x3ff) << 13)
        }
        31 => sign | 0x7f80_0000 | (fraction << 13),
        _ => sign | ((exponent + 112) << 23) | (fraction << 13),
    };
    f32::from_bits(raw)
}

fn f32_to_half(value: f32) -> u16 {
    let bits = value.to_bits();
    let sign = u16::try_from((bits >> 16) & 0x8000).expect("half sign");
    let exponent = i32::try_from((bits >> 23) & 0xff).expect("f32 exponent") - 127 + 15;
    let fraction = bits & 0x7f_ffff;
    if exponent <= 0 {
        if exponent < -10 {
            return sign;
        }
        let mantissa = (fraction | 0x80_0000) >> u32::try_from(14 - exponent).expect("half shift");
        return sign | u16::try_from((mantissa + 1) >> 1).expect("half fraction");
    }
    if exponent >= 31 {
        return sign | 0x7c00;
    }
    sign | (u16::try_from(exponent).expect("half exponent") << 10)
        | u16::try_from((fraction + 0x1000) >> 13).expect("half fraction")
}

fn length(additional: u8, bytes: &[u8], cursor: &mut usize) -> Result<u64, ProtocolError> {
    let width = match additional {
        0..=23 => return Ok(additional as u64),
        24 => 1,
        25 => 2,
        26 => 4,
        27 => 8,
        _ => return Err(ProtocolError("indefinite CBOR is forbidden".into())),
    };
    let raw = take(bytes, cursor, width)?;
    let mut value = 0;
    for byte in raw {
        value = (value << 8) | u64::from(*byte);
    }
    if (width == 1 && value < 24)
        || (width == 2 && value <= 0xff)
        || (width == 4 && value <= 0xffff)
        || (width == 8 && value <= 0xffff_ffff)
    {
        return Err(ProtocolError("nonminimal CBOR integer".into()));
    }
    Ok(value)
}

fn take<'a>(bytes: &'a [u8], cursor: &mut usize, length: u64) -> Result<&'a [u8], ProtocolError> {
    let length =
        usize::try_from(length).map_err(|_| ProtocolError("CBOR length overflow".into()))?;
    let end = cursor
        .checked_add(length)
        .ok_or_else(|| ProtocolError("CBOR length overflow".into()))?;
    let result = bytes
        .get(*cursor..end)
        .ok_or_else(|| ProtocolError("truncated CBOR".into()))?;
    *cursor = end;
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn accepts_ciborium_reader<R: ciborium_io::Read>(_reader: R) {}

    #[test]
    fn envelope_round_trip_preserves_large_bytes() {
        let payload = object([
            (
                "bytes",
                Value::Bytes((0..4096).map(|value| value as u8).collect()),
            ),
            ("value", Value::Signed(-7)),
        ]);
        let encoded = encode_envelope("Bootstrap", &payload).expect("encode");
        assert_eq!(
            decode_envelope(&encoded).expect("decode"),
            Envelope {
                kind: "Bootstrap".into(),
                payload
            }
        );
    }

    #[test]
    fn rejects_duplicate_and_malformed_fields() {
        accepts_ciborium_reader(&[0xa0][..]);
        assert!(
            decode_envelope(&[
                0xa2, 0x64, b'k', b'i', b'n', b'd', 0x61, b'x', 0x64, b'k', b'i', b'n', b'd', 0x61,
                b'y'
            ])
            .is_err()
        );
        assert!(decode_envelope(&[0xbf]).is_err());
        assert!(
            decode_envelope(&[
                0xa2, 0x67, b'p', b'a', b'y', b'l', b'o', b'a', b'd', 0xf6, 0x64, b'k', b'i', b'n',
                b'd', 0x61, b'x'
            ])
            .is_err()
        );
    }

    #[test]
    fn rejects_nonminimal_float_width() {
        // 1.0 is exactly representable as half precision, so a float32
        // payload is outside the protocol's shortest-lossless dialect.
        let mut cursor = 0;
        assert!(
            decode_value(
                &[0xfa, 0x3f, 0x80, 0x00, 0x00],
                &mut cursor,
                MAX_INTENT_VALUE_DEPTH,
                0,
            )
            .is_err()
        );
    }

    #[test]
    fn integer_destination_interpretation_is_checked() {
        assert_eq!(Value::Unsigned(5).integer_i64(), Some(5));
        assert_eq!(Value::Signed(5).integer_u64(), Some(5));
        assert_eq!(Value::Signed(-1).integer_u64(), None);
        assert_eq!(Value::Unsigned(u64::MAX).integer_i64(), None);
    }

    #[test]
    fn protocol_fourteen_preflight_routes_reflected_field_limits() {
        let oversized_snapshot = object([
            ("system_id", Value::Unsigned(1)),
            ("value", Value::Text("x".repeat(MAX_OUTPUT_VALUE_BYTES + 1))),
        ]);
        let payload = object([
            (
                "protocol_version",
                Value::Unsigned(crate::generated::BROWSER_PROTOCOL_VERSION),
            ),
            (
                "schema_fingerprint",
                Value::Array(vec![Value::Unsigned(1), Value::Unsigned(2)]),
            ),
            ("snapshots", Value::Array(vec![oversized_snapshot])),
        ]);
        let encoded = encode_envelope("Bootstrap", &payload).expect("trusted snapshot fixture");
        assert!(preflight_protocol_record(&encoded).is_err());

        let snapshot = object([("system_id", Value::Unsigned(1)), ("value", Value::Null)]);
        let payload = object([
            (
                "protocol_version",
                Value::Unsigned(crate::generated::BROWSER_PROTOCOL_VERSION),
            ),
            (
                "schema_fingerprint",
                Value::Array(vec![Value::Unsigned(1), Value::Unsigned(2)]),
            ),
            (
                "snapshots",
                Value::Array(vec![snapshot; MAX_SNAPSHOT_COUNT + 1]),
            ),
        ]);
        let encoded = encode_envelope("Bootstrap", &payload).expect("trusted snapshot fixture");
        assert!(preflight_protocol_record(&encoded).is_err());

        let error = object([
            ("category", Value::Text("Failed".into())),
            (
                "detail",
                Value::Text("x".repeat(MAX_ERROR_DETAIL_BYTES + 1)),
            ),
        ]);
        let payload = object([
            (
                "protocol_version",
                Value::Unsigned(crate::generated::BROWSER_PROTOCOL_VERSION),
            ),
            ("correlation", Value::Unsigned(1)),
            ("result", Value::Null),
            ("error", error),
        ]);
        let encoded = encode_envelope("IntentReply", &payload).expect("trusted reply fixture");
        assert!(preflight_protocol_record(&encoded).is_err());
    }

    #[test]
    fn output_scene_collections_use_output_limits_without_relaxing_intents() {
        let scene = Value::Array(vec![Value::Unsigned(1); 4096]);
        assert!(validate_client_dynamic_value(&scene).is_err());
        let payload = object([
            (
                "protocol_version",
                Value::Unsigned(crate::generated::BROWSER_PROTOCOL_VERSION),
            ),
            ("system_id", Value::Unsigned(1)),
            ("event_id", Value::Unsigned(2)),
            ("delivery", Value::Text("LatestState".into())),
            ("state_revision", Value::Unsigned(7)),
            ("value", scene),
        ]);
        let encoded = encode_envelope("SystemEvent", &payload).unwrap();
        assert!(decode_envelope(&encoded).is_ok());
    }
}
