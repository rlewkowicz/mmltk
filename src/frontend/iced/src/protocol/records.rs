use super::client_records::{Intent, IntentField, Interaction};
use super::{
    ProtocolError, decode_envelope, encode_envelope, object, reject_unknown_fields,
    validate_client_dynamic_value, validate_server_dynamic_value,
};
use crate::application_codec::{FromApplicationValue, Value};
use crate::generated::{
    EventDelivery, ServerRecordKind, MAX_INTENT_FIELDS,
    MAX_SNAPSHOT_COUNT,
};

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum RendererObservation {
    Ready,
    Surface { width: u32, height: u32, scale: f64 },
    Presented { sample_revision: u64 },
}

#[derive(Debug, Clone, PartialEq)]
pub struct Bootstrap {
    pub schema_fingerprint: [u64; 2],
    pub input_epoch: u64,
    pub snapshots: Vec<crate::generated::ApplicationSnapshot>,
}

pub type ApplicationError = crate::generated::ApplicationErrorRecord;

#[derive(Debug, Clone, PartialEq)]
pub struct IntentReply {
    pub correlation: u64,
    pub result: Result<Value, ApplicationError>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SystemEvent {
    pub delivery: EventDelivery,
    pub state_revision: u64,
    pub event: crate::generated::ApplicationEvent,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ServerRecord {
    Bootstrap(Bootstrap),
    IntentReply(IntentReply),
    SystemEvent(SystemEvent),
    InteractionRejected(crate::generated::InteractionRejected),
    InputProgress(crate::generated::InputProgress),
}

fn protocol_payload(values: impl IntoIterator<Item = (&'static str, Value)>) -> Value {
    let mut fields = vec![(
        "protocol_version".into(),
        Value::Unsigned(crate::generated::BROWSER_PROTOCOL_VERSION),
    )];
    fields.extend(
        values
            .into_iter()
            .map(|(name, value)| (name.to_owned(), value)),
    );
    Value::Object(fields)
}

fn encode_fields(fields: &[IntentField]) -> Result<Value, ProtocolError> {
    if fields.len() > MAX_INTENT_FIELDS
        || fields.iter().any(|field| field.field_id == 0)
        || fields.iter().enumerate().any(|(index, field)| {
            fields[..index]
                .iter()
                .any(|prior| prior.field_id == field.field_id)
        })
    {
        return Err(ProtocolError("intent fields are invalid".into()));
    }
    fields
        .iter()
        .map(|field| {
            validate_client_dynamic_value(&field.value)?;
            Ok(object([
                ("field_id", Value::Unsigned(field.field_id)),
                ("value", field.value.clone()),
            ]))
        })
        .collect::<Result<Vec<_>, _>>()
        .map(Value::Array)
}

impl Intent {
    pub fn encode(&self) -> Result<Vec<u8>, ProtocolError> {
        if self.correlation == 0 || self.endpoint_id == 0 {
            return Err(ProtocolError("intent identity is invalid".into()));
        }
        encode_envelope(
            "Intent",
            &protocol_payload([
                ("correlation", Value::Unsigned(self.correlation)),
                ("endpoint_id", Value::Unsigned(self.endpoint_id)),
                ("fields", encode_fields(&self.fields)?),
            ]),
        )
    }
}

impl Interaction {
    pub fn encode(&self) -> Result<Vec<u8>, ProtocolError> {
        if self.endpoint_id == 0 {
            return Err(ProtocolError("interaction endpoint is invalid".into()));
        }
        if self.value.len() > crate::generated::MAX_INTENT_VALUE_BYTES {
            return Err(ProtocolError("interaction byte capacity exceeded".into()));
        }
        let mut bytes = Vec::new();
        super::client_records::encode_interaction_bytes(self.endpoint_id, &self.value, &mut bytes)?;
        Ok(bytes)
    }
}

impl RendererObservation {
    pub fn encode(&self) -> Result<Vec<u8>, ProtocolError> {
        let (kind, width, height, scale, revision) = match *self {
            Self::Ready => ("Ready", 0, 0, 1.0, 0),
            Self::Surface {
                width,
                height,
                scale,
            } if width != 0 && height != 0 && scale.is_finite() && scale > 0.0 => {
                ("Surface", width, height, scale, 0)
            }
            Self::Presented { sample_revision } if sample_revision != 0 => {
                ("Presented", 0, 0, 1.0, sample_revision)
            }
            _ => return Err(ProtocolError("renderer observation is invalid".into())),
        };
        encode_envelope(
            "RendererObservation",
            &protocol_payload([
                ("kind", Value::Text(kind.into())),
                ("width", Value::Unsigned(u64::from(width))),
                ("height", Value::Unsigned(u64::from(height))),
                ("scale", Value::Float(scale)),
                ("sample_revision", Value::Unsigned(revision)),
            ]),
        )
    }
}

fn required_object<'a>(
    payload: &'a Value,
    names: &[&str],
) -> Result<&'a [(String, Value)], ProtocolError> {
    let fields = payload
        .object()
        .ok_or_else(|| ProtocolError("record payload must be an object".into()))?;
    reject_unknown_fields(
        fields,
        |name| names.contains(&name),
        |name| ProtocolError(format!("unknown record field {name}")),
    )?;
    if fields.len() != names.len() || names.iter().any(|name| payload.field(name).is_none()) {
        return Err(ProtocolError("record payload has missing fields".into()));
    }
    Ok(fields)
}

fn into_required_object(
    payload: Value,
    names: &[&str],
) -> Result<Vec<(String, Value)>, ProtocolError> {
    required_object(&payload, names)?;
    let Value::Object(fields) = payload else {
        unreachable!("required_object accepted a non-object");
    };
    Ok(fields)
}

fn take_required(fields: &mut Vec<(String, Value)>, name: &str) -> Value {
    let index = fields
        .iter()
        .position(|(candidate, _)| candidate == name)
        .expect("required field");
    fields.swap_remove(index).1
}

fn protocol(payload: &Value) -> Result<(), ProtocolError> {
    if payload
        .field("protocol_version")
        .and_then(Value::integer_u64)
        != Some(crate::generated::BROWSER_PROTOCOL_VERSION)
    {
        return Err(ProtocolError("protocol version mismatch".into()));
    }
    Ok(())
}

fn decode_snapshot(
    value: Value,
) -> Result<(u64, crate::generated::ApplicationSnapshot), ProtocolError> {
    let mut fields = into_required_object(value, &["system_id", "value"])?;
    let system_id = take_required(&mut fields, "system_id")
        .integer_u64()
        .filter(|identity| *identity != 0)
        .ok_or_else(|| ProtocolError("snapshot system identity is invalid".into()))?;
    let value = take_required(&mut fields, "value");
    validate_server_dynamic_value(&value)?;
    crate::generated::decode_application_snapshot(system_id, value)
        .map(|snapshot| (system_id, snapshot))
        .map_err(ProtocolError)
}

fn decode_error(value: Value) -> Result<ApplicationError, ProtocolError> {
    ApplicationError::from_application_value(value).map_err(ProtocolError)
}

pub fn decode_server(bytes: &[u8]) -> Result<ServerRecord, ProtocolError> {
    let envelope = decode_envelope(bytes)?;
    protocol(&envelope.payload)?;
    let kind = ServerRecordKind::parse(envelope.kind.as_bytes())
        .ok_or_else(|| ProtocolError("unknown server record".into()))?;
    match kind {
        ServerRecordKind::Bootstrap => {
            let mut fields = into_required_object(
                envelope.payload,
                &["protocol_version", "schema_fingerprint", "input_epoch", "snapshots"],
            )?;
            let Value::Array(fingerprint) = take_required(&mut fields, "schema_fingerprint") else {
                return Err(ProtocolError("schema fingerprint must be an array".into()));
            };
            let schema_fingerprint: [u64; 2] = fingerprint
                .into_iter()
                .map(|word| {
                    word.integer_u64()
                        .ok_or_else(|| ProtocolError("schema fingerprint word is invalid".into()))
                })
                .collect::<Result<Vec<_>, _>>()?
                .try_into()
                .map_err(|_| ProtocolError("schema fingerprint must contain two words".into()))?;
            if schema_fingerprint != crate::generated::SCHEMA_FINGERPRINT {
                return Err(ProtocolError(
                    "application schema fingerprint mismatch".into(),
                ));
            }
            let Value::Array(snapshots) = take_required(&mut fields, "snapshots") else {
                return Err(ProtocolError("bootstrap snapshots must be an array".into()));
            };
            if snapshots.len() > MAX_SNAPSHOT_COUNT {
                return Err(ProtocolError("bootstrap snapshot capacity exceeded".into()));
            }
            let snapshots = snapshots
                .into_iter()
                .map(decode_snapshot)
                .collect::<Result<Vec<_>, _>>()?;
            if snapshots.iter().enumerate().any(|(index, snapshot)| {
                snapshots[..index].iter().any(|prior| prior.0 == snapshot.0)
            }) {
                return Err(ProtocolError(
                    "bootstrap has duplicate system snapshots".into(),
                ));
            }
            Ok(ServerRecord::Bootstrap(Bootstrap {
                schema_fingerprint,
                input_epoch: take_required(&mut fields, "input_epoch").integer_u64().filter(|epoch| *epoch != 0).ok_or_else(|| ProtocolError("invalid input epoch".into()))?,
                snapshots: snapshots
                    .into_iter()
                    .map(|(_, snapshot)| snapshot)
                    .collect(),
            }))
        }
        ServerRecordKind::InteractionRejected => {
            let record = crate::generated::InteractionRejected::from_application_value(envelope.payload).map_err(ProtocolError)?;
            if record.endpointid == 0 { return Err(ProtocolError("invalid rejected endpoint".into())); }
            Ok(ServerRecord::InteractionRejected(record))
        }
        ServerRecordKind::InputProgress => {
            let record = crate::generated::InputProgress::from_application_value(envelope.payload).map_err(ProtocolError)?;
            if record.progress.epoch == 0 { return Err(ProtocolError("invalid progress epoch".into())); }
            Ok(ServerRecord::InputProgress(record))
        }

        ServerRecordKind::IntentReply => {
            let Value::Object(mut fields) = envelope.payload else {
                return Err(ProtocolError("record payload must be an object".into()));
            };
            reject_unknown_fields(
                &fields,
                |name| ["protocol_version", "correlation", "result", "error"].contains(&name),
                |name| ProtocolError(format!("unknown record field {name}")),
            )?;
            if fields.len() != 3
                || fields
                    .iter()
                    .filter(|(name, _)| name == "result" || name == "error")
                    .count()
                    != 1
            {
                return Err(ProtocolError(
                    "reply must carry exactly one result or error".into(),
                ));
            }
            let correlation = take_required(&mut fields, "correlation")
                .integer_u64()
                .filter(|identity| *identity != 0)
                .ok_or_else(|| ProtocolError("reply correlation is invalid".into()))?;
            let result = if let Some(index) = fields.iter().position(|(name, _)| name == "error") {
                let error = fields.swap_remove(index).1;
                Err(decode_error(error)?)
            } else {
                let value = take_required(&mut fields, "result");
                if matches!(value, Value::Null) {
                    return Err(ProtocolError("reply result is invalid".into()));
                }
                validate_server_dynamic_value(&value)?;
                Ok(value)
            };
            Ok(ServerRecord::IntentReply(IntentReply {
                correlation,
                result,
            }))
        }
        ServerRecordKind::SystemEvent => {
            let mut fields = into_required_object(
                envelope.payload,
                &[
                    "protocol_version",
                    "system_id",
                    "event_id",
                    "delivery",
                    "state_revision",
                    "value",
                ],
            )?;
            let system_id = take_required(&mut fields, "system_id")
                .integer_u64()
                .filter(|identity| *identity != 0)
                .ok_or_else(|| ProtocolError("event system identity is invalid".into()))?;
            let event_id = take_required(&mut fields, "event_id")
                .integer_u64()
                .filter(|identity| *identity != 0)
                .ok_or_else(|| ProtocolError("event identity is invalid".into()))?;
            let delivery_value = take_required(&mut fields, "delivery");
            let state_revision = take_required(&mut fields, "state_revision")
                .integer_u64()
                .ok_or_else(|| ProtocolError("event state revision is invalid".into()))?;
            let delivery = EventDelivery::from_application_value(delivery_value)
                .map_err(|_| ProtocolError("event delivery is invalid".into()))?;
            let expected_delivery =
                crate::generated::application_event_delivery(system_id, event_id)
                    .ok_or_else(|| ProtocolError("event identity is invalid".into()))?;
            if expected_delivery != delivery {
                return Err(ProtocolError("event delivery does not match schema".into()));
            }
            let value = take_required(&mut fields, "value");
            validate_server_dynamic_value(&value)?;
            let event = crate::generated::decode_application_event(system_id, event_id, value)
                .map_err(ProtocolError)?;
            Ok(ServerRecord::SystemEvent(SystemEvent { delivery, state_revision, event }))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn full_scene_output_reaches_typed_server_decode_without_relaxing_input() {
        use crate::application_codec::IntoApplicationValue as _;
        let mut snapshot = crate::view_model::test_support::explore_snapshot();
        snapshot.ready = true;
        snapshot.mode = crate::generated::ExploreMode::Detail;
        snapshot.selectedimage = Some(0);
        snapshot.frame = crate::view_model::test_support::visual_frame(
            crate::generated::PresentationSourceKind::Explore,
            1,
        );
        snapshot.scene.framewidth = 640;
        snapshot.scene.frameheight = 480;
        snapshot.scene.objects = vec![crate::view_model::test_support::annotation_object(0); 4096];
        snapshot.scene.categories = vec![crate::generated::ArtifactClassName {
            value: "person".into(),
        }];
        snapshot.scene.palette = vec![crate::generated::AnnotationColor {
            hue: 0.0,
            saturation: 1.0,
            value: 1.0,
        }];
        let value = snapshot.clone().into_application_value();
        let bootstrap = protocol_payload([
            ("input_epoch", Value::Unsigned(1)),
            (
                "schema_fingerprint",
                Value::Array(
                    crate::generated::SCHEMA_FINGERPRINT
                        .into_iter()
                        .map(Value::Unsigned)
                        .collect(),
                ),
            ),
            (
                "snapshots",
                Value::Array(vec![object([
                    (
                        "system_id",
                        Value::Unsigned(crate::generated::SYSTEM_Explore),
                    ),
                    ("value", value.clone()),
                ])]),
            ),
        ]);
        assert!(matches!(
            decode_server(&encode_envelope("Bootstrap", &bootstrap).unwrap()),
            Ok(ServerRecord::Bootstrap(_))
        ));
        let event = protocol_payload([
            (
                "system_id",
                Value::Unsigned(crate::generated::SYSTEM_Explore),
            ),
            (
                "event_id",
                Value::Unsigned(crate::generated::EVENT_Explore_ExploreChanged),
            ),
            ("delivery", Value::Text("LatestState".into())),
            ("state_revision", Value::Unsigned(snapshot.revision)),
            (
                "value",
                crate::generated::ExploreChanged { snapshot }.into_application_value(),
            ),
        ]);
        assert!(matches!(
            decode_server(&encode_envelope("SystemEvent", &event).unwrap()),
            Ok(ServerRecord::SystemEvent(decoded)) if Some(decoded.state_revision) == event.field("state_revision").and_then(Value::integer_u64)
        ));
        let reply = protocol_payload([
            ("correlation", Value::Unsigned(1)),
            ("result", value.clone()),
        ]);
        assert!(matches!(
            decode_server(&encode_envelope("IntentReply", &reply).unwrap()),
            Ok(ServerRecord::IntentReply(_))
        ));
        assert!(
            Interaction {
                replaceable: false,
                endpoint_id: 1,
                value: vec![0; crate::generated::MAX_INTENT_VALUE_BYTES + 1],
            }
            .encode()
            .is_err()
        );
        assert!(
            Intent {
                correlation: 1,
                endpoint_id: 1,
                fields: vec![IntentField { field_id: 1, value }]
            }
            .encode()
            .is_err()
        );
        let oversized = protocol_payload([
            ("correlation", Value::Unsigned(1)),
            (
                "result",
                Value::Text("x".repeat(crate::generated::MAX_OUTPUT_VALUE_BYTES + 1)),
            ),
        ]);
        assert!(decode_server(&encode_envelope("IntentReply", &oversized).unwrap()).is_err());
    }

    #[test]
    fn bootstrap_rejects_mismatched_or_invalid_fingerprints() {
        let valid = encode_envelope(
            "Bootstrap",
            &protocol_payload([
                ("input_epoch", Value::Unsigned(1)),
                (
                    "schema_fingerprint",
                    Value::Array(
                        crate::generated::SCHEMA_FINGERPRINT
                            .into_iter()
                            .map(Value::Unsigned)
                            .collect(),
                    ),
                ),
                ("snapshots", Value::Array(Vec::new())),
            ]),
        )
        .expect("valid Bootstrap");
        assert!(matches!(
            decode_server(&valid),
            Ok(ServerRecord::Bootstrap(_))
        ));
        let mismatched = encode_envelope(
            "Bootstrap",
            &protocol_payload([
                ("input_epoch", Value::Unsigned(1)),
                (
                    "schema_fingerprint",
                    Value::Array(vec![Value::Unsigned(1), Value::Unsigned(2)]),
                ),
                ("snapshots", Value::Array(Vec::new())),
            ]),
        )
        .expect("mismatched Bootstrap");
        assert!(decode_server(&mismatched).is_err());
        let mut trailing = valid;
        trailing.push(0xf6);
        assert!(decode_server(&trailing).is_err());
        assert!(decode_server(&[0xbf]).is_err());
    }

    #[test]
    fn phase_nine_fingerprint_rejects_before_opaque_train_mask_semantics() {
        use crate::application_codec::IntoApplicationValue as _;
        const OLD_FINGERPRINT: [u64; 2] = [5665704887670380944, 6499206666294531679];
        assert_ne!(OLD_FINGERPRINT, crate::generated::SCHEMA_FINGERPRINT);
        let default = crate::generated::application_snapshot_defaults()
            .unwrap()
            .into_iter()
            .find(|default| default.system_id == crate::generated::SYSTEM_Settings)
            .unwrap();
        let crate::generated::ApplicationSnapshot::Settings(settings) = default.value else {
            panic!("settings default has wrong generated variant");
        };
        let mut invalid_settings = settings.into_application_value();
        *invalid_settings
            .field_mut("settings_state")
            .and_then(|value| value.field_mut("workflows"))
            .and_then(|value| value.field_mut("train"))
            .and_then(|value| value.field_mut("request"))
            .and_then(|value| value.field_mut("recipe_overrides"))
            .and_then(|value| value.field_mut("mask"))
            .unwrap() = Value::Unsigned(0x0800);
        let encoded = |fingerprint: [u64; 2]| {
            encode_envelope(
                "Bootstrap",
                &protocol_payload([
                    ("input_epoch", Value::Unsigned(1)),
                    (
                        "schema_fingerprint",
                        Value::Array(fingerprint.into_iter().map(Value::Unsigned).collect()),
                    ),
                    (
                        "snapshots",
                        Value::Array(vec![Value::Object(vec![
                            (
                                "system_id".into(),
                                Value::Unsigned(crate::generated::SYSTEM_Settings),
                            ),
                            ("value".into(), invalid_settings.clone()),
                        ])]),
                    ),
                ]),
            )
            .unwrap()
        };
        let old_error = decode_server(&encoded(OLD_FINGERPRINT)).unwrap_err();
        assert!(old_error.0.contains("fingerprint mismatch"));
        let current_error =
            decode_server(&encoded(crate::generated::SCHEMA_FINGERPRINT)).unwrap_err();
        assert!(current_error.0.contains("above maximum"));
    }

    fn decode_hex(hex: &str) -> Vec<u8> {
        assert_eq!(hex.len() % 2, 0);
        hex.as_bytes()
            .chunks_exact(2)
            .map(|pair| {
                let digit = |value: u8| match value {
                    b'0'..=b'9' => value - b'0',
                    b'a'..=b'f' => value - b'a' + 10,
                    _ => panic!("invalid protocol fixture hex"),
                };
                digit(pair[0]) << 4 | digit(pair[1])
            })
            .collect()
    }

    fn client_fixtures() -> Vec<(&'static str, Vec<u8>)> {
        include_str!(env!("MMLTK_PROTOCOL_V14_CLIENT_FIXTURE_PATH"))
            .lines()
            .map(|line| {
                let (kind, hex) = line.split_once(' ').expect("client fixture");
                (kind, decode_hex(hex))
            })
            .collect()
    }

    fn native_server_fixtures() -> Vec<&'static [u8]> {
        let bytes = include_bytes!(env!("MMLTK_PROTOCOL_V14_SERVER_FIXTURE_PATH"));
        let mut records = Vec::new();
        let mut cursor = 0;
        while cursor < bytes.len() {
            let header: [u8; 4] = bytes[cursor..cursor + 4]
                .try_into()
                .expect("native fixture length");
            cursor += header.len();
            let size = u32::from_be_bytes(header) as usize;
            records.push(
                bytes
                    .get(cursor..cursor + size)
                    .expect("complete native fixture"),
            );
            cursor += size;
        }
        records
    }

    #[test]
    fn client_records_match_native_protocol_fourteen_fixtures() {
        let fixtures = client_fixtures();
        // Native interop checks exhaustive coverage against the reflected variant.
        for (kind, bytes) in &fixtures {
            assert!(decode_envelope(bytes).is_ok(), "invalid fixture {kind}");
        }
        let observation = RendererObservation::Surface {
            width: 640,
            height: 480,
            scale: 1.5,
        }
        .encode()
        .expect("encode renderer observation");
        assert!(
            [
                "Intent:settings.Update",
                "Intent:file_dialog.Open",
                "Intent:file_dialog.Open.model_artifact",
                "Interaction:explore.UpdateViewport",
            ]
            .into_iter()
            .all(|kind| fixtures.iter().any(|record| record.0 == kind))
        );
        assert!(fixtures.contains(&("RendererObservation", observation)));

        let dialog = fixtures
            .iter()
            .find(|(kind, _)| *kind == "Intent:file_dialog.Open")
            .expect("generated file-dialog intent");
        let envelope = decode_envelope(&dialog.1).expect("decode intent envelope");
        assert_eq!(envelope.kind, "Intent");
        assert_eq!(
            envelope
                .payload
                .field("correlation")
                .and_then(Value::integer_u64),
            Some(17)
        );
        let mut trailing = dialog.1.clone();
        trailing.push(0xf6);
        assert!(decode_envelope(&trailing).is_err());
    }

    #[test]
    fn bootstrap_reply_and_event_decode_without_session_state() {
        let native = native_server_fixtures();
        assert_eq!(native.len(), 7);
        let Ok(ServerRecord::Bootstrap(bootstrap)) = decode_server(native[0]) else {
            panic!("native Bootstrap");
        };
        assert_eq!(
            bootstrap.schema_fingerprint,
            crate::generated::SCHEMA_FINGERPRINT
        );
        assert_eq!(bootstrap.input_epoch, 1);
        assert!(matches!(decode_server(native[4]).unwrap(), ServerRecord::InputProgress(crate::generated::InputProgress { progress, error: None , .. })
            if progress.epoch == 1 && progress.consumedsequence == 2 && progress.rejection.is_none()));
        assert!(matches!(decode_server(native[5]).unwrap(), ServerRecord::InteractionRejected(record)
            if record.endpointid == crate::generated::ENDPOINT_Explore_UpdateViewport && record.error.category == crate::generated::ApplicationErrorCategory::Unavailable && record.error.detail == "fixture unavailable"));
        let limit = crate::generated::ReflectedRecordPath::AnnotationInputProgress.map_child(b"rejection").max_leaf_bytes();
        let ServerRecord::InputProgress(present) = decode_server(native[6]).unwrap() else { panic!("native optional progress") };
        assert_eq!(present.progress.rejection.as_ref().unwrap().len(), limit);
        assert_eq!(present.error.as_ref().unwrap().category, crate::generated::ApplicationErrorCategory::Busy);
        let mut excessive = decode_envelope(native[6]).unwrap().payload;
        let Value::Object(fields) = &mut excessive else { panic!("progress envelope") };
        let Value::Object(progress) = &mut fields.iter_mut().find(|(name, _)| name == "progress").unwrap().1 else { panic!("progress") };
        progress.iter_mut().find(|(name, _)| name == "rejection").unwrap().1 = Value::Text("r".repeat(limit + 1));
        assert!(crate::generated::InputProgress::from_application_value(excessive.clone()).is_err());
        assert!(decode_server(&encode_envelope("InputProgress", &excessive).unwrap()).is_err());
        for kind in ["InputProgress", "InteractionRejected"] {
            let index = if kind == "InputProgress" { 4 } else { 5 };
            let original = decode_envelope(native[index]).unwrap().payload;
            let Value::Object(original_fields) = original else { panic!("reflected record") };
            for mutation in 0..3 {
                let mut fields = original_fields.clone();
                match mutation {
                    0 => { fields.pop(); }
                    1 => { fields[0].0 = "unknown".into(); }
                    _ => { fields[0].0 = fields[1].0.clone(); }
                }
                let payload = Value::Object(fields);
                let decoded = if kind == "InputProgress" {
                    crate::generated::InputProgress::from_application_value(payload).map(|_| ())
                } else { crate::generated::InteractionRejected::from_application_value(payload).map(|_| ()) };
                assert!(decoded.is_err());
            }
        }
        let mut zero_epoch = decode_envelope(native[0]).unwrap().payload;
        if let Value::Object(fields) = &mut zero_epoch { fields.iter_mut().find(|(name, _)| name == "input_epoch").unwrap().1 = Value::Unsigned(0); }
        assert!(decode_server(&encode_envelope("Bootstrap", &zero_epoch).unwrap()).is_err());
        for (index, selected) in [(1, false), (2, true)] {
            let ServerRecord::IntentReply(reply) =
                decode_server(native[index]).expect("decode native IntentReply")
            else {
                panic!("native IntentReply");
            };
            let typed_reply = crate::generated::decode_application_reply(
                crate::generated::ENDPOINT_FileDialog_Open,
                reply.result.expect("successful native reply"),
            )
            .expect("typed native reply");
            let crate::generated::ApplicationReply::FileDialogOpen(snapshot) = typed_reply else {
                panic!("typed file-dialog reply");
            };
            let selection = snapshot.selection.expect("dialog result");
            assert!(crate::generated::FILE_DIALOGS.iter().any(|dialog| {
                matches!(
                    &selection.target,
                    crate::generated::FileDialogTarget::SettingsFieldTarget(target)
                        if dialog.stable_field_id == target.stableid
                )
            }));
            match selection.result {
                crate::generated::FileDialogCancelledOrFileDialogSelectedVariant::
                    FileDialogSelected(value) => {
                        assert!(selected);
                        assert_eq!(value.path, "/tmp/protocol-v14-fixture");
                    }
                crate::generated::FileDialogCancelledOrFileDialogSelectedVariant::
                    FileDialogCancelled(_) => assert!(!selected),
            }
        }
        let ServerRecord::SystemEvent(event) =
            decode_server(native[3]).expect("decode native SystemEvent")
        else {
            panic!("native SystemEvent");
        };
        assert!(matches!(
            event.event,
            crate::generated::ApplicationEvent::SettingsSettingsChanged(_)
        ));

        let bootstrap = encode_envelope(
            "Bootstrap",
            &protocol_payload([
                ("input_epoch", Value::Unsigned(1)),
                (
                    "schema_fingerprint",
                    Value::Array(
                        crate::generated::SCHEMA_FINGERPRINT
                            .into_iter()
                            .map(Value::Unsigned)
                            .collect(),
                    ),
                ),
                (
                    "snapshots",
                    Value::Array(vec![object([
                        ("system_id", Value::Unsigned(u64::MAX)),
                        ("value", Value::Object(Vec::new())),
                    ])]),
                ),
            ]),
        )
        .expect("encode bootstrap");
        assert!(decode_server(&bootstrap).is_err());

        let reply = encode_envelope(
            "IntentReply",
            &protocol_payload([
                ("correlation", Value::Unsigned(17)),
                (
                    "error",
                    object([
                        ("category", Value::Text("Busy".into())),
                        ("detail", Value::Text("already active".into())),
                    ]),
                ),
            ]),
        )
        .expect("encode reply");
        let Ok(ServerRecord::IntentReply(reply)) = decode_server(&reply) else {
            panic!("intent reply");
        };
        assert_eq!(
            reply.result.expect_err("busy reply").category,
            crate::generated::ApplicationErrorCategory::Busy
        );

        let event = encode_envelope(
            "SystemEvent",
            &protocol_payload([
                ("system_id", Value::Unsigned(19)),
                ("event_id", Value::Unsigned(23)),
                ("delivery", Value::Text("Transient".into())),
                ("value", Value::Object(Vec::new())),
            ]),
        )
        .expect("encode event");
        assert!(decode_server(&event).is_err());
    }

    #[test]
    fn generated_selected_dialog_path_decoder_enforces_reflected_byte_bounds() {
        use crate::application_codec::FromApplicationValue as _;

        let path = crate::generated::REFLECTED_FIELD_FACTS
            .iter()
            .find(|fact| {
                fact.owner == "FileDialogSelected" && fact.min_bytes != 0 && fact.max_bytes != 0
            })
            .expect("reflected selected path fact");
        let decode = |value| crate::generated::FileDialogSelected::from_application_value(value);
        assert!(decode(Value::Object(Vec::new())).is_err());
        assert!(decode(Value::Object(vec![(path.name.into(), Value::Unsigned(1),)])).is_err());
        assert!(
            decode(Value::Object(vec![(
                path.name.into(),
                Value::Text(String::new()),
            )]))
            .is_err()
        );
        assert!(
            decode(Value::Object(vec![(
                path.name.into(),
                Value::Text("x".repeat(path.max_bytes + 1)),
            )]))
            .is_err()
        );
    }

    #[test]
    fn generated_schema_surfaces_are_complete_and_self_consistent() {
        use std::collections::BTreeSet;

        let request_ids: BTreeSet<_> = crate::generated::APPLICATION_REQUEST_FIELDS
            .iter()
            .map(|field| {
                assert_ne!(field.endpoint_id, 0);
                assert_ne!(field.field_id, 0);
                field.field_id
            })
            .collect();
        assert_eq!(
            request_ids.len(),
            crate::generated::APPLICATION_REQUEST_FIELDS.len()
        );

        let settings_ids: BTreeSet<_> = crate::generated::SETTINGS_LEAVES
            .iter()
            .map(|field| {
                assert_ne!(field.stable_field_id, 0);
                assert!(!field.path.is_empty());
                if field.finite {
                    assert!(
                        field
                            .minimum
                            .into_iter()
                            .chain(field.maximum)
                            .all(f64::is_finite)
                    );
                }
                if let Some((minimum, maximum)) = field.minimum.zip(field.maximum) {
                    assert!(minimum <= maximum);
                }
                assert!(field.maximum_bytes == 0 || field.minimum_bytes <= field.maximum_bytes);
                assert!(
                    crate::generated::REFLECTED_FIELD_FACTS
                        .iter()
                        .any(|reflected| {
                            field.path.rsplit('.').next() == Some(reflected.name)
                                && field.finite == reflected.finite
                                && field.minimum == reflected.minimum
                                && field.maximum == reflected.maximum
                                && field.minimum_bytes == reflected.min_bytes
                                && field.maximum_bytes == reflected.max_bytes
                                && field.maximum_items == reflected.max_items
                        })
                );
                field.stable_field_id
            })
            .collect();
        assert_eq!(settings_ids.len(), crate::generated::SETTINGS_LEAVES.len());

        for dialog in crate::generated::FILE_DIALOGS {
            assert!(settings_ids.contains(&dialog.stable_field_id));
            let leaf = crate::generated::SETTINGS_LEAVES
                .iter()
                .find(|leaf| leaf.stable_field_id == dialog.stable_field_id)
                .expect("dialog settings leaf");
            assert!(leaf.has_file_dialog);
            assert_eq!(leaf.path, dialog.field_path);
            assert_eq!(leaf.workflows, dialog.workflows);
            assert!(!dialog.title.is_empty());
            assert!(!dialog.filter.is_empty());
            assert!(!dialog.pattern.is_empty());
        }
        assert_eq!(crate::generated::MODEL_ARTIFACT_DIALOGS.len(), 9);
        for dialog in crate::generated::MODEL_ARTIFACT_DIALOGS {
            assert_eq!(dialog.target.stableid, dialog.stable_field_id);
            let artifact = crate::generated::SETTINGS_LEAVES
                .iter()
                .filter(|leaf| {
                    leaf.stable_field_id == dialog.stable_field_id && leaf.path == dialog.field_path
                })
                .count();
            assert_eq!(artifact, 1);
            for related in [
                dialog.source_field_id,
                dialog.input_field_id,
                dialog.preset_field_id,
                dialog.resolution_field_id,
            ] {
                assert_eq!(
                    crate::generated::SETTINGS_LEAVES
                        .iter()
                        .filter(|leaf| leaf.stable_field_id == related)
                        .count(),
                    1
                );
            }
            if let Some(predicate) = dialog.predicate_field_id {
                assert_eq!(
                    crate::generated::SETTINGS_LEAVES
                        .iter()
                        .filter(|leaf| leaf.stable_field_id == predicate)
                        .count(),
                    1
                );
            }
            assert!(
                crate::generated::MODEL_SELECTION_COMPATIBILITY_CATALOG
                    .iter()
                    .any(|row| {
                        row.workflow == dialog.target.workflow
                            && row.input == dialog.target.input
                            && row.artifactfieldpath == dialog.field_path
                    })
            );
        }

        let provider_ids: BTreeSet<_> = crate::generated::CATALOG_PROVIDERS
            .iter()
            .map(|provider| {
                assert_ne!(provider.stable_id, 0);
                assert!(!provider.identity.is_empty());
                assert!(!provider.row_type.is_empty());
                assert_eq!(
                    provider.row_count,
                    crate::generated::CATALOG_ROWS
                        .iter()
                        .filter(|row| row.provider_id == provider.stable_id)
                        .count()
                );
                provider.stable_id
            })
            .collect();
        assert_eq!(
            provider_ids.len(),
            crate::generated::CATALOG_PROVIDERS.len()
        );
        let row_ids: BTreeSet<_> = crate::generated::CATALOG_ROWS
            .iter()
            .map(|row| {
                assert_ne!(row.stable_id, 0);
                assert!(!row.key.is_empty());
                row.stable_id
            })
            .collect();
        assert_eq!(row_ids.len(), crate::generated::CATALOG_ROWS.len());
        let typed_rows = crate::generated::application_catalog_rows();
        assert_eq!(typed_rows.len(), crate::generated::CATALOG_ROWS.len());
        for row in typed_rows {
            use crate::application_codec::IntoApplicationValue as _;
            let decoded = crate::generated::decode_application_catalog_row(
                row.provider_id,
                row.value.clone().into_application_value(),
            )
            .expect("typed catalog round trip");
            assert_eq!(decoded, row.value);
            assert!(crate::generated::CATALOG_ROWS.iter().any(|fact| {
                fact.provider_id == row.provider_id
                    && fact.stable_id == row.stable_id
                    && fact.key == row.key
            }));
        }
        let defaults = crate::generated::application_settings_defaults().expect("typed defaults");
        assert_eq!(defaults.len(), crate::generated::SETTINGS_LEAVES.len());
        assert!(defaults.iter().all(|default| {
            crate::generated::SETTINGS_LEAVES.iter().any(|leaf| {
                leaf.stable_field_id == default.stable_field_id && leaf.path == default.path
            })
        }));
        let request_defaults =
            crate::generated::application_request_defaults().expect("typed request defaults");
        assert_eq!(
            request_defaults.len(),
            crate::generated::APPLICATION_REQUEST_FIELDS.len()
        );
        assert!(request_defaults.iter().all(|default| {
            crate::generated::APPLICATION_REQUEST_FIELDS
                .iter()
                .any(|field| {
                    field.endpoint_id == default.endpoint_id && field.field_id == default.field_id
                })
        }));
        let snapshot_defaults =
            crate::generated::application_snapshot_defaults().expect("typed snapshot defaults");
        let snapshot_ids: BTreeSet<_> = snapshot_defaults
            .iter()
            .map(|default| {
                assert_ne!(default.system_id, 0);
                default.system_id
            })
            .collect();
        assert_eq!(snapshot_ids.len(), snapshot_defaults.len());
        let snapshots: Vec<_> = snapshot_defaults
            .iter()
            .map(|default| default.value.clone())
            .collect();
        assert!(crate::generated::application_bootstrap_complete(&snapshots));
        let mut missing = snapshots.clone();
        missing.pop();
        assert!(!crate::generated::application_bootstrap_complete(&missing));
        let mut duplicate = snapshots;
        duplicate[0] = duplicate[1].clone();
        assert!(!crate::generated::application_bootstrap_complete(
            &duplicate
        ));
        assert!(
            crate::generated::REFLECTED_FIELD_FACTS
                .iter()
                .any(|field| field.catalog_provider == Some("RfdetrPresetCatalog"))
        );
        assert!(
            crate::generated::REFLECTED_FIELD_FACTS
                .iter()
                .any(|field| field.progress.is_some())
        );
    }

    #[test]
    fn legacy_session_records_are_not_part_of_protocol_fourteen() {
        let legacy = encode_envelope(
            "HostReset",
            &protocol_payload([("legacy_payload", Value::Object(Vec::new()))]),
        )
        .expect("encode legacy record");
        assert!(decode_server(&legacy).is_err());
    }
}
