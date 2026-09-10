use crate::protocol::client_records::{Intent, Interaction};
use crate::protocol::{ProtocolError, RendererObservation};
use futures_channel::mpsc;
use std::collections::VecDeque;
use std::fmt;
use std::sync::{Arc, Mutex};

const OUTBOUND_CAPACITY: usize = 64;

#[derive(Debug)]
struct Outbound {
    records: VecDeque<OutboundRecord>,
    input: crate::annotation_input::AnnotationInput,
    writable: Option<std::task::Waker>,
    adjacent_record: bool,
    pressure_observation: Option<(u64, bool)>,
}
impl Default for Outbound {
    fn default() -> Self {
        Self {
            records: VecDeque::with_capacity(OUTBOUND_CAPACITY),
            input: Default::default(),
            writable: None,
            adjacent_record: false,
            pressure_observation: None,
        }
    }
}
impl Outbound {
    fn record_count(&self) -> usize {
        self.records.len() + self.input.command_count()
    }
}

#[derive(Debug)]
pub(crate) enum OutboundRecord {
    Wake,
    Intent(Intent),
    Interaction(Interaction),
    RendererObservation(RendererObservation),
    IntegrationControl(crate::generated::IntegrationControl),
}

impl OutboundRecord {
    pub(crate) fn encode(self) -> Result<Vec<u8>, ProtocolError> {
        match self {
            Self::Wake => Err(ProtocolError("input wake is not a wire record".into())),
            Self::Intent(value) => value.encode(),
            Self::Interaction(value) => value.encode(),
            Self::RendererObservation(value) => value.encode(),
            Self::IntegrationControl(value) => value.encode(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SendDisposition {
    Queued,
    Dropped,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutboundSendError {
    Closed,
    Capacity,
    Allocation,
}

impl fmt::Display for OutboundSendError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Closed => "browser connection is closed",
            Self::Capacity => "outbound capacity is exhausted",
            Self::Allocation => "retained input allocation failed",
        })
    }
}

impl std::error::Error for OutboundSendError {}

#[derive(Debug, Clone)]
pub struct Connection {
    outbound: mpsc::Sender<OutboundRecord>,
    retained: Arc<Mutex<Outbound>>,
    integration: bool,
}

impl Connection {
    pub(crate) fn observe_integration_pressure(&self, sequence: u64) {
        self.retained
            .lock()
            .expect("connection output")
            .pressure_observation = Some((sequence, false));
    }
    pub(crate) fn integration_pressure_settled(&self) -> bool {
        let retained = self.retained.lock().expect("connection output");
        retained
            .pressure_observation
            .is_some_and(|(_, entered)| entered)
            && retained.input.settled()
    }
    pub(crate) fn new(outbound: mpsc::Sender<OutboundRecord>) -> Self {
        Self {
            outbound,
            integration: false,
            retained: Arc::new(Mutex::new(Outbound::default())),
        }
    }

    #[cfg(any(target_arch = "wasm32", test))]
    pub(crate) fn enable_integration(&mut self, enabled: bool) {
        self.integration = enabled && crate::integration_control::reporting_enabled();
    }
    pub(crate) fn is_closed(&self) -> bool {
        self.outbound.is_closed()
            || self
                .retained
                .lock()
                .expect("connection output")
                .input
                .is_closed()
    }
    fn require_open(&self) -> Result<(), OutboundSendError> {
        if self.is_closed() {
            Err(OutboundSendError::Closed)
        } else {
            Ok(())
        }
    }
    pub fn send_intent(&mut self, intent: Intent) -> Result<SendDisposition, OutboundSendError> {
        self.send(OutboundRecord::Intent(intent), false)
    }

    pub fn send_interaction(
        &mut self,
        interaction: Interaction,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.send(OutboundRecord::Interaction(interaction), true)
    }

    pub fn send_annotation_pointer(
        &mut self,
        pointer: crate::generated::AnnotationPointer,
        document_epoch: u64,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.require_open()?;
        if self.integration {
            crate::integration_control::report_annotation_gesture(
                "sending",
                [
                    pointer.interactionid as f64,
                    pointer.sequence as f64,
                    match pointer.phase {
                        crate::generated::AnnotationPointerPhase::Begin => 1.0,
                        crate::generated::AnnotationPointerPhase::Update => 2.0,
                        crate::generated::AnnotationPointerPhase::End => 3.0,
                        crate::generated::AnnotationPointerPhase::Cancel => 4.0,
                    },
                    0.0,
                ],
            );
        }
        let accepted = {
            let mut retained = self.retained.lock().expect("connection output");
            let accepted = retained.input.pointer(pointer, document_epoch);
            if accepted.is_ok() {
                retained.adjacent_record = false;
            }
            accepted
        };
        if accepted.is_err() {
            self.close();
            return Err(OutboundSendError::Allocation);
        }
        self.wake()?;
        Ok(SendDisposition::Queued)
    }

    pub fn send_renderer_observation(
        &mut self,
        observation: RendererObservation,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.send(OutboundRecord::RendererObservation(observation), true)
    }

    pub(crate) fn send_integration_control(
        &mut self,
        receipt: crate::generated::IntegrationControlReceipt,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.send(
            OutboundRecord::IntegrationControl(crate::generated::IntegrationControl {
                protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                receipt,
            }),
            false,
        )
    }

    pub async fn writable(self) -> Result<(), OutboundSendError> {
        self.require_open()?;
        iced::futures::future::poll_fn(|context| {
            let mut retained = self.retained.lock().expect("connection output");
            if self.outbound.is_closed() || retained.input.is_closed() {
                return std::task::Poll::Ready(Err(OutboundSendError::Closed));
            }
            if retained.record_count() < OUTBOUND_CAPACITY {
                return std::task::Poll::Ready(Ok(()));
            }
            retained.writable = Some(context.waker().clone());
            std::task::Poll::Pending
        })
        .await
    }
    pub(crate) fn observe(&self, record: &crate::protocol::ServerRecord) -> Result<(), String> {
        self.retained
            .lock()
            .expect("connection output")
            .input
            .observe(record)
    }
    pub(crate) fn flush(
        &self,
        mut send: impl FnMut(&[u8]) -> Result<(), String>,
    ) -> Result<(), String> {
        let mut retained = self.retained.lock().expect("connection output");
        let before = retained.record_count();
        let result = (|| {
            while let Some(record) = retained.records.pop_front() {
                send(&record.encode().map_err(|error| error.to_string())?)?;
            }
            retained.input.flush(&mut send)?;
            if let Some((sequence, false)) = retained.pressure_observation
                && retained.input.pressure_entered()
            {
                let record = crate::generated::IntegrationControl {
                    protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                    receipt: crate::generated::IntegrationControlReceipt {
                        kind: crate::generated::IntegrationControlKind::PressureEntered,
                        sequence,
                        progress: 0,
                    },
                };
                send(&record.encode().map_err(|error| error.to_string())?)?;
                retained.pressure_observation = Some((sequence, true));
            }
            Ok(())
        })();
        if retained.record_count() < before {
            if let Some(waker) = retained.writable.take() {
                waker.wake();
            }
        }
        result
    }
    pub(crate) fn close(&self) {
        {
            let mut retained = self.retained.lock().expect("connection output");
            retained.input.close();
            retained.records.clear();
            if let Some(waker) = retained.writable.take() {
                waker.wake();
            }
        }
        let _ = self.outbound.clone().try_send(OutboundRecord::Wake);
    }
    fn wake(&mut self) -> Result<(), OutboundSendError> {
        match self.outbound.try_send(OutboundRecord::Wake) {
            Ok(()) => Ok(()),
            Err(error) if error.is_full() => Ok(()),
            Err(_) => Err(OutboundSendError::Closed),
        }
    }
    fn send(
        &mut self,
        record: OutboundRecord,
        transient: bool,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.require_open()?;
        {
            let mut retained = self.retained.lock().expect("connection output");
            let replace = retained.adjacent_record
                && matches!((&record, retained.records.back()),
                (OutboundRecord::Interaction(next), Some(OutboundRecord::Interaction(prior)))
                    if next.replaceable && prior.endpoint_id == next.endpoint_id);
            if replace {
                *retained.records.back_mut().expect("adjacent record") = record;
            } else {
                if retained.record_count() == OUTBOUND_CAPACITY {
                    return if transient {
                        Ok(SendDisposition::Dropped)
                    } else {
                        Err(OutboundSendError::Capacity)
                    };
                }
                match record {
                    OutboundRecord::Intent(intent)
                        if crate::annotation_input::AnnotationInput::owns_command(
                            intent.endpoint_id,
                        ) =>
                    {
                        retained.input.command(intent);
                        retained.adjacent_record = false;
                    }
                    record => {
                        retained.records.push_back(record);
                        retained.adjacent_record = true;
                    }
                }
            }
        }
        self.wake()?;
        Ok(SendDisposition::Queued)
    }
}

#[cfg(test)]
#[derive(Debug)]
pub(crate) enum CapturedRecord {
    Intent(Intent),
    Other(crate::protocol::Envelope),
}
#[cfg(test)]
pub(crate) struct Capture {
    connection: Connection,
    _wake: mpsc::Receiver<OutboundRecord>,
    records: std::collections::VecDeque<crate::protocol::Envelope>,
}
#[cfg(test)]
impl Connection {
    pub(crate) fn test_channel() -> (Self, Capture) {
        let (sender, receiver) = mpsc::channel(1);
        let connection = Self::new(sender);
        connection
            .observe(&crate::protocol::ServerRecord::Bootstrap(
                crate::protocol::Bootstrap {
                    schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
                    input_epoch: 1,
                    snapshots: Vec::new(),
                },
            ))
            .unwrap();
        connection
            .observe(&crate::protocol::ServerRecord::InputProgress(
                crate::generated::InputProgress {
                    protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                    progress: crate::generated::AnnotationInputProgress {
                        epoch: 1,
                        consumedsequence: 0,
                        rejection: None,
                    },
                    error: None,
                },
            ))
            .unwrap();
        let capture = Capture {
            connection: connection.clone(),
            _wake: receiver,
            records: Default::default(),
        };
        (connection, capture)
    }
}
#[cfg(test)]
impl Capture {
    pub(crate) fn try_recv(&mut self) -> Result<CapturedRecord, String> {
        self.connection.flush(|bytes| {
            self.records.push_back(
                crate::protocol::decode_envelope(bytes).map_err(|error| error.to_string())?,
            );
            Ok(())
        })?;
        let envelope = self.records.pop_front().ok_or("no retained output")?;
        if envelope.kind != "Intent" {
            return Ok(CapturedRecord::Other(envelope));
        }
        let payload = envelope.payload;
        let fields = payload
            .field("fields")
            .unwrap()
            .array()
            .unwrap()
            .iter()
            .map(|field| crate::protocol::client_records::IntentField {
                field_id: field.field("field_id").unwrap().integer_u64().unwrap(),
                value: field.field("value").unwrap().clone(),
            })
            .collect();
        Ok(CapturedRecord::Intent(Intent {
            correlation: payload.field("correlation").unwrap().integer_u64().unwrap(),
            endpoint_id: payload.field("endpoint_id").unwrap().integer_u64().unwrap(),
            fields,
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct PointerTransport {
        connection: Connection,
        _capture: Capture,
        wire: Vec<Vec<u8>>,
    }

    impl PointerTransport {
        fn new() -> Self {
            let (connection, capture) = Connection::test_channel();
            Self {
                connection,
                _capture: capture,
                wire: Vec::new(),
            }
        }

        fn credit(&self, sequence: u64) {
            self.connection.observe(&Self::progress(sequence)).unwrap();
        }

        fn progress(sequence: u64) -> crate::protocol::ServerRecord {
            crate::protocol::ServerRecord::InputProgress(crate::generated::InputProgress {
                protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                progress: crate::generated::AnnotationInputProgress {
                    epoch: 1,
                    consumedsequence: sequence,
                    rejection: None,
                },
                error: None,
            })
        }

        fn flush(&mut self) {
            self.connection
                .flush(|bytes| {
                    self.wire.push(bytes.to_vec());
                    Ok(())
                })
                .unwrap();
        }

        fn decoded(&self) -> Vec<crate::protocol::Envelope> {
            self.wire
                .iter()
                .map(|bytes| crate::protocol::decode_envelope(bytes).unwrap())
                .collect()
        }

        fn assert_samples(
            &self,
            samples: &[crate::generated::AnnotationPointer],
            first_sequence: u64,
        ) {
            let input: Vec<_> = self
                .wire
                .iter()
                .filter(|bytes| {
                    let record = crate::protocol::decode_envelope(bytes).unwrap();
                    record.kind == "Interaction"
                        && record.payload.field("endpoint_id").unwrap().integer_u64()
                            == Some(crate::generated::ENDPOINT_Annotation_Input)
                })
                .collect();
            assert_eq!(
                input.len(),
                samples
                    .len()
                    .div_ceil(crate::generated::ANNOTATION_INPUT_BATCH_CAPACITY)
            );
            for (index, batch) in samples
                .chunks(crate::generated::ANNOTATION_INPUT_BATCH_CAPACITY)
                .enumerate()
            {
                let expected = crate::generated::encode_annotation_Input(
                    crate::generated::AnnotationInputBatch {
                        epoch: 1,
                        documentepoch: 1,
                        sequence: first_sequence + index as u64,
                        samples: batch.to_vec(),
                    },
                )
                .unwrap()
                .encode()
                .unwrap();
                assert_eq!(
                    *input[index], expected,
                    "every accepted sample remains ordered"
                );
            }
        }

        fn annotation_snapshot() -> crate::generated::AnnotationSnapshot {
            crate::generated::application_snapshot_defaults()
                .unwrap()
                .into_iter()
                .find_map(|fact| {
                    if let crate::generated::ApplicationSnapshot::Annotation(value) = fact.value {
                        Some(value)
                    } else {
                        None
                    }
                })
                .unwrap()
        }
    }

    #[test]
    fn every_outbound_record_uses_the_canonical_encoder() {
        let records = [
            OutboundRecord::Intent(Intent {
                correlation: 1,
                endpoint_id: 2,
                fields: Vec::new(),
            }),
            OutboundRecord::Interaction(Interaction {
                replaceable: false,
                endpoint_id: 3,
                value: Vec::new(),
            }),
            OutboundRecord::RendererObservation(RendererObservation::Ready),
        ];
        for record in records {
            assert!(!record.encode().expect("outbound record").is_empty());
        }
    }

    #[test]
    fn direct_connection_keeps_transient_pressure_local() {
        let (sender, _receiver) = mpsc::channel(0);
        let mut connection = Connection::new(sender);
        assert_eq!(
            connection
                .send_renderer_observation(RendererObservation::Ready)
                .expect("first observation"),
            SendDisposition::Queued
        );
        assert_eq!(
            connection
                .send_interaction(Interaction {
                    replaceable: false,
                    endpoint_id: 1,
                    value: Vec::new(),
                })
                .expect("transient pressure"),
            SendDisposition::Queued
        );
        for correlation in 1..=62 {
            assert_eq!(
                connection.send_intent(Intent {
                    correlation,
                    endpoint_id: 1,
                    fields: Vec::new()
                }),
                Ok(SendDisposition::Queued)
            );
        }
        assert_eq!(
            connection.send_renderer_observation(RendererObservation::Ready),
            Ok(SendDisposition::Dropped)
        );
        assert_eq!(
            connection.send_intent(Intent {
                correlation: 63,
                endpoint_id: 1,
                fields: Vec::new()
            }),
            Err(OutboundSendError::Capacity)
        );
        let (mut viewport, _capture) = Connection::test_channel();
        for value in 0..128_u8 {
            assert_eq!(
                viewport.send_interaction(Interaction {
                    replaceable: true,
                    endpoint_id: 1,
                    value: vec![value]
                }),
                Ok(SendDisposition::Queued)
            );
        }
        let mut sent = Vec::new();
        viewport
            .flush(|bytes| {
                sent.push(crate::protocol::decode_envelope(bytes).unwrap());
                Ok(())
            })
            .unwrap();
        assert_eq!(sent.len(), 1);
        assert_eq!(
            sent[0].payload.field("value"),
            Some(&crate::application_codec::Value::Bytes(vec![127]))
        );
    }

    #[test]
    fn annotation_pointer_boundaries_are_not_droppable() {
        crate::integration_control::initialize_reporting(false, false);
        let pointer = |phase| crate::generated::AnnotationPointer {
            phase,
            interactionid: 1,
            sequence: 1,
            target: crate::generated::AnnotationPointerTarget {
                object: None,
                element: None,
                role: None,
            },
            point: crate::generated::AnnotationPoint { x: 1.0, y: 1.0 },
            brushradius: crate::generated::default_uiannotationbrushradius().unwrap() as u16,
        };
        for phase in [
            crate::generated::AnnotationPointerPhase::Begin,
            crate::generated::AnnotationPointerPhase::End,
            crate::generated::AnnotationPointerPhase::Cancel,
        ] {
            let (sender, _receiver) = mpsc::channel(0);
            let mut connection = Connection::new(sender);
            connection
                .send_annotation_pointer(pointer(phase), 1)
                .unwrap();
            assert_eq!(
                connection.send_annotation_pointer(pointer(phase), 1),
                Ok(SendDisposition::Queued)
            );
        }
        let (sender, _receiver) = mpsc::channel(0);
        let connection = Connection::new(sender);
        assert!(
            connection
                .observe(&crate::protocol::ServerRecord::InputProgress(
                    crate::generated::InputProgress {
                        protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                        progress: crate::generated::AnnotationInputProgress {
                            epoch: 1,
                            consumedsequence: 0,
                            rejection: None
                        },
                        error: None,
                    }
                ))
                .is_err(),
            "Bootstrap must precede all progress"
        );
        let consumed = PointerTransport::progress;
        let samples: Vec<_> = (0..130)
            .map(|index| {
                let mut sample = pointer(if index == 0 {
                    crate::generated::AnnotationPointerPhase::Begin
                } else if index == 129 {
                    crate::generated::AnnotationPointerPhase::End
                } else {
                    crate::generated::AnnotationPointerPhase::Update
                });
                sample.sequence = index + 1;
                sample.point.x = index as f32;
                sample
            })
            .collect();
        for terminal in [
            crate::generated::AnnotationPointerPhase::End,
            crate::generated::AnnotationPointerPhase::Cancel,
        ] {
            let mut fixture = PointerTransport::new();
            fixture.connection.enable_integration(true);
            assert!(
                !fixture.connection.integration,
                "quiet driver constructs no gesture diagnostics"
            );
            let mut accepted = samples.clone();
            accepted.last_mut().unwrap().phase = terminal;
            // Exhaust both credits before accepting the terminal edge and tail.
            for sample in &accepted[..64] {
                fixture
                    .connection
                    .send_annotation_pointer(sample.clone(), 1)
                    .unwrap();
            }
            fixture.flush();
            assert_eq!(fixture.wire.len(), 2);
            assert!(
                !fixture
                    .connection
                    .retained
                    .lock()
                    .unwrap()
                    .input
                    .pressure_entered()
            );
            for sample in &accepted[64..] {
                fixture
                    .connection
                    .send_annotation_pointer(sample.clone(), 1)
                    .unwrap();
            }
            fixture.flush();
            assert_eq!(
                fixture.wire.len(),
                2,
                "terminal input remains retained under exhausted credits"
            );
            assert!(
                fixture
                    .connection
                    .retained
                    .lock()
                    .unwrap()
                    .input
                    .pressure_entered()
            );
            assert!(!fixture.connection.retained.lock().unwrap().input.settled());
            fixture
                .connection
                .send_renderer_observation(RendererObservation::Ready)
                .unwrap();
            fixture
                .connection
                .send_intent(Intent {
                    correlation: 90,
                    endpoint_id: crate::generated::ENDPOINT_Settings_Update,
                    fields: Vec::new(),
                })
                .unwrap();
            fixture
                .connection
                .send_interaction(Interaction {
                    replaceable: false,
                    endpoint_id: crate::generated::ENDPOINT_Explore_UpdateFilter,
                    value: vec![7],
                })
                .unwrap();
            fixture.flush();
            let ordinary: Vec<_> = fixture
                .decoded()
                .into_iter()
                .skip(2)
                .map(|record| record.kind)
                .collect();
            assert_eq!(ordinary, ["RendererObservation", "Intent", "Interaction"]);
            fixture.credit(1);
            fixture.flush();
            assert_eq!(fixture.wire.len(), 6);
            fixture.credit(3);
            fixture.flush();
            assert_eq!(
                fixture.wire.len(),
                8,
                "partial final batch sends immediately"
            );
            fixture.assert_samples(&accepted, 1);
            fixture.credit(5);
            fixture.flush();
            assert!(fixture.connection.retained.lock().unwrap().input.settled());
            assert_eq!(fixture.wire.len(), 8);
            assert!(fixture.connection.observe(&consumed(6)).is_err());
            fixture.credit(5); // Duplicate cumulative progress consumes no second prefix.
            let stale =
                crate::protocol::ServerRecord::InputProgress(crate::generated::InputProgress {
                    protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                    progress: crate::generated::AnnotationInputProgress {
                        epoch: 99,
                        consumedsequence: u64::MAX,
                        rejection: Some("old peer failure".into()),
                    },
                    error: Some(crate::protocol::ApplicationError {
                        category: crate::generated::ApplicationErrorCategory::Busy,
                        detail: "old rejection".into(),
                    }),
                });
            fixture.connection.observe(&stale).unwrap();
            let rejected =
                crate::protocol::ServerRecord::InputProgress(crate::generated::InputProgress {
                    protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                    progress: crate::generated::AnnotationInputProgress {
                        epoch: 1,
                        consumedsequence: 5,
                        rejection: None,
                    },
                    error: Some(crate::protocol::ApplicationError {
                        category: crate::generated::ApplicationErrorCategory::Busy,
                        detail: "invalid admission".into(),
                    }),
                });
            assert!(
                fixture.connection.observe(&rejected).is_err(),
                "unidentified rejection is terminal, never a rewind"
            );
            fixture.connection.close();
            assert_eq!(
                fixture
                    .connection
                    .send_annotation_pointer(accepted[0].clone(), 1),
                Err(OutboundSendError::Closed)
            );
        }
        let annotation_snapshot = PointerTransport::annotation_snapshot;
        for endpoint in [
            crate::generated::ENDPOINT_Annotation_Open,
            crate::generated::ENDPOINT_Annotation_Edit,
            crate::generated::ENDPOINT_Annotation_Save,
            crate::generated::ENDPOINT_Annotation_Stop,
        ] {
            for event_first in [false, true] {
                for failed in [false, true] {
                    use crate::application_codec::IntoApplicationValue;
                    let mut fixture = PointerTransport::new();
                    fixture
                        .connection
                        .send_annotation_pointer(samples[0].clone(), 1)
                        .unwrap();
                    fixture
                        .connection
                        .send_intent(Intent {
                            correlation: 17,
                            endpoint_id: endpoint,
                            fields: Vec::new(),
                        })
                        .unwrap();
                    fixture
                        .connection
                        .send_annotation_pointer(samples[0].clone(), 1)
                        .unwrap();
                    fixture.flush();
                    assert_eq!(fixture.wire.len(), 1);
                    assert_eq!(
                        crate::protocol::decode_envelope(&fixture.wire[0])
                            .unwrap()
                            .kind,
                        "Interaction"
                    );
                    fixture.assert_samples(&samples[..1], 1);
                    fixture.credit(1);
                    fixture.wire.clear();
                    fixture.flush();
                    assert_eq!(
                        fixture.wire.len(),
                        1,
                        "document command follows consumption of every earlier sample"
                    );

                    let command = crate::protocol::decode_envelope(&fixture.wire[0]).unwrap();
                    assert_eq!(command.kind, "Intent");
                    assert_eq!(
                        command.payload.field("endpoint_id").unwrap().integer_u64(),
                        Some(endpoint)
                    );
                    assert_eq!(
                        command.payload.field("correlation").unwrap().integer_u64(),
                        Some(17)
                    );

                    let mut snapshot = annotation_snapshot();
                    snapshot.revision = 10;
                    snapshot.busy = true;
                    let reply =
                        crate::protocol::ServerRecord::IntentReply(crate::protocol::IntentReply {
                            correlation: 17,
                            result: Ok(snapshot.clone().into_application_value()),
                        });
                    let settled = |revision| {
                        let mut snapshot = snapshot.clone();
                        snapshot.busy = false;
                        snapshot.revision = revision;
                        crate::protocol::ServerRecord::SystemEvent(crate::protocol::SystemEvent {
                            delivery: if failed {
                                crate::generated::EventDelivery::Critical
                            } else {
                                crate::generated::EventDelivery::LatestState
                            },
                            state_revision: if failed { 0 } else { snapshot.revision },
                            event: if failed {
                                crate::generated::ApplicationEvent::AnnotationAnnotationFailed(
                                    crate::generated::AnnotationFailed {
                                        snapshot,
                                        detail: "settled failure".into(),
                                    },
                                )
                            } else {
                                crate::generated::ApplicationEvent::AnnotationAnnotationChanged(
                                    crate::generated::AnnotationChanged { snapshot },
                                )
                            },
                        })
                    };
                    fixture.connection.observe(&settled(9)).unwrap();
                    if event_first {
                        fixture.connection.observe(&settled(11)).unwrap();
                    }
                    fixture.flush();
                    assert_eq!(
                        fixture.wire.len(),
                        1,
                        "settlement cannot bypass its correlated reply"
                    );
                    fixture.connection.observe(&reply).unwrap();
                    if !event_first {
                        fixture
                            .connection
                            .send_annotation_pointer(samples[1].clone(), 1)
                            .unwrap();
                        fixture
                            .connection
                            .send_intent(Intent {
                                correlation: 18,
                                endpoint_id: crate::generated::ENDPOINT_Annotation_Stop,
                                fields: Vec::new(),
                            })
                            .unwrap();
                        fixture
                            .connection
                            .send_renderer_observation(RendererObservation::Ready)
                            .unwrap();
                        fixture
                            .connection
                            .send_intent(Intent {
                                correlation: 19,
                                endpoint_id: crate::generated::ENDPOINT_Settings_Update,
                                fields: Vec::new(),
                            })
                            .unwrap();
                        fixture
                            .connection
                            .send_interaction(Interaction {
                                replaceable: false,
                                endpoint_id: crate::generated::ENDPOINT_Explore_UpdateFilter,
                                value: vec![8],
                            })
                            .unwrap();
                        fixture.flush();
                        assert_eq!(
                            fixture.wire.len(),
                            5,
                            "Stop and ordinary traffic pass the active document barrier"
                        );
                        let stop = fixture
                            .wire
                            .iter()
                            .map(|bytes| crate::protocol::decode_envelope(bytes).unwrap())
                            .find(|record| {
                                record.kind == "Intent"
                                    && record.payload.field("correlation").unwrap().integer_u64()
                                        == Some(18)
                            })
                            .expect("Stop bypasses the unsettled command");
                        assert_eq!(
                            stop.payload.field("correlation").unwrap().integer_u64(),
                            Some(18)
                        );
                        fixture.wire.truncate(1);
                        fixture
                            .connection
                            .observe(&crate::protocol::ServerRecord::IntentReply(
                                crate::protocol::IntentReply {
                                    correlation: 18,
                                    result: Ok(annotation_snapshot().into_application_value()),
                                },
                            ))
                            .unwrap();
                        fixture.flush();
                        assert_eq!(
                            fixture.wire.len(),
                            1,
                            "Stop reply never releases the active command's later input"
                        );
                    }

                    fixture.connection.observe(&settled(9)).unwrap();
                    fixture.flush();
                    assert_eq!(fixture.wire.len(), if event_first { 2 } else { 1 });
                    if !event_first {
                        fixture.connection.observe(&settled(11)).unwrap();
                    }
                    fixture.flush();
                    assert_eq!(
                        fixture.wire.len(),
                        2,
                        "only a newer settled revision releases following input"
                    );
                    fixture.assert_samples(&samples[..if event_first { 1 } else { 2 }], 2);
                }
            }
        }
        for successful in [false, true] {
            use crate::application_codec::IntoApplicationValue;
            let (mut owner, mut capture) = Connection::test_channel();
            owner
                .send_intent(Intent {
                    correlation: 1,
                    endpoint_id: crate::generated::ENDPOINT_Annotation_Stop,
                    fields: Vec::new(),
                })
                .unwrap();
            owner
                .send_annotation_pointer(samples[0].clone(), 1)
                .unwrap();
            assert!(matches!(capture.try_recv(), Ok(CapturedRecord::Intent(_))));
            assert!(capture.try_recv().is_err());
            owner
                .observe(&crate::protocol::ServerRecord::IntentReply(
                    crate::protocol::IntentReply {
                        correlation: 1,
                        result: if successful {
                            Ok(annotation_snapshot().into_application_value())
                        } else {
                            Err(crate::protocol::ApplicationError {
                                category: crate::generated::ApplicationErrorCategory::Unavailable,
                                detail: "command rejected".into(),
                            })
                        },
                    },
                ))
                .unwrap();
            assert!(
                matches!(capture.try_recv(), Ok(CapturedRecord::Other(envelope)) if envelope.kind == "Interaction")
            );
        }
        // Separate queues retain the same discrete capacity and replacement
        // adjacency, even when a sample sits between absolute observations.
        let (mut owner, _capture) = Connection::test_channel();
        for index in 0..64 {
            let endpoint = if index % 2 == 0 {
                crate::generated::ENDPOINT_Annotation_Edit
            } else {
                1
            };
            owner
                .send_intent(Intent {
                    correlation: index + 1,
                    endpoint_id: endpoint,
                    fields: Vec::new(),
                })
                .unwrap();
        }
        assert_eq!(
            owner.send_intent(Intent {
                correlation: 65,
                endpoint_id: 1,
                fields: Vec::new()
            }),
            Err(OutboundSendError::Capacity)
        );
        let mut writable = Box::pin(owner.clone().writable());
        let waker = iced::futures::task::noop_waker();
        let mut context = std::task::Context::from_waker(&waker);
        assert!(std::future::Future::poll(writable.as_mut(), &mut context).is_pending());
        owner.flush(|_| Ok(())).unwrap();
        assert!(matches!(
            std::future::Future::poll(writable.as_mut(), &mut context),
            std::task::Poll::Ready(Ok(()))
        ));
        let (mut owner, _capture) = Connection::test_channel();
        owner
            .send_interaction(Interaction {
                replaceable: true,
                endpoint_id: 1,
                value: vec![1],
            })
            .unwrap();
        owner
            .send_annotation_pointer(samples[0].clone(), 1)
            .unwrap();
        owner
            .send_interaction(Interaction {
                replaceable: true,
                endpoint_id: 1,
                value: vec![2],
            })
            .unwrap();
        let mut wire = Vec::new();
        owner
            .flush(|bytes| {
                wire.push(crate::protocol::decode_envelope(bytes).unwrap());
                Ok(())
            })
            .unwrap();
        assert_eq!(
            wire.len(),
            3,
            "separately stored samples still break replacement adjacency"
        );
        assert_eq!(
            wire[0].payload.field("value"),
            Some(&crate::application_codec::Value::Bytes(vec![1]))
        );
        assert_eq!(
            wire[1].payload.field("value"),
            Some(&crate::application_codec::Value::Bytes(vec![2]))
        );
        for invalid in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            let (mut owner, _capture) = Connection::test_channel();
            let mut sample = samples[0].clone();
            sample.point.x = invalid;
            owner.send_annotation_pointer(sample, 1).unwrap();
            for _ in 0..2 {
                assert!(
                    owner
                        .flush(|_| panic!("malformed input reached the socket"))
                        .is_err()
                );
            }
            owner.close();
        }
    }
}
