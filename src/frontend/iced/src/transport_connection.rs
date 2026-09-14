use crate::protocol::ProtocolError;
use crate::protocol::client_records::{Intent, Interaction};
use futures_channel::mpsc;
use std::collections::VecDeque;
use std::fmt;
use std::sync::{Arc, Mutex};

const OUTBOUND_CAPACITY: usize = 64;

#[derive(Debug)]
struct Outbound {
    records: VecDeque<OutboundRecord>,
    closed: bool,
    epoch: u64,
    writable: Option<std::task::Waker>,
    adjacent_record: bool,
    pressure_observation: Option<(u64, usize, bool)>,
    scratch: Vec<u8>,
    encoded: Vec<u8>,
}
impl Default for Outbound {
    fn default() -> Self {
        Self {
            records: VecDeque::with_capacity(OUTBOUND_CAPACITY),
            closed: false,
            epoch: 0,
            writable: None,
            adjacent_record: false,
            pressure_observation: None,
            scratch: Vec::new(),
            encoded: Vec::new(),
        }
    }
}
impl Outbound {
    fn record_count(&self) -> usize {
        self.records.len()
    }
}

#[derive(Debug)]
pub(crate) enum OutboundRecord {
    Wake,
    Intent(Intent),
    Interaction(Interaction),
    Mouse(crate::generated::WorkspaceMouse),
    IntegrationControl(crate::generated::IntegrationControl),
}

impl OutboundRecord {
    pub(crate) fn encode(self) -> Result<Vec<u8>, ProtocolError> {
        match self {
            Self::Wake => Err(ProtocolError("input wake is not a wire record".into())),
            Self::Intent(value) => value.encode(),
            Self::Interaction(value) => value.encode(),
            Self::Mouse(value) => crate::generated::encode_workspace_mouse(value)?.encode(),
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
}

impl Connection {
    pub(crate) fn observe_integration_pressure(&self, sequence: u64) {
        self.retained
            .lock()
            .expect("connection output")
            .pressure_observation = Some((sequence, 0, false));
    }
    pub(crate) fn integration_pressure_settled(&self) -> bool {
        let retained = self.retained.lock().expect("connection output");
        retained
            .pressure_observation
            .is_some_and(|(_, _, entered)| entered)
            && retained.records.is_empty()
    }
    pub(crate) fn new(outbound: mpsc::Sender<OutboundRecord>) -> Self {
        Self {
            outbound,
            retained: Arc::new(Mutex::new(Outbound::default())),
        }
    }

    pub(crate) fn is_closed(&self) -> bool {
        self.outbound.is_closed() || self.retained.lock().expect("connection output").closed
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

    pub fn send_workspace_mouse(
        &mut self,
        mouse: crate::generated::WorkspaceMouse,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.require_open()?;
        {
            let mut retained = self.retained.lock().expect("connection output");
            if retained.records.try_reserve(1).is_err() {
                drop(retained);
                self.close();
                return Err(OutboundSendError::Allocation);
            }
            retained.records.push_back(OutboundRecord::Mouse(mouse));
            if let Some((_, admitted, _)) = retained.pressure_observation.as_mut() {
                *admitted = admitted.saturating_add(1);
            }
            retained.adjacent_record = false;
        }
        self.wake()?;
        Ok(SendDisposition::Queued)
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
            if self.outbound.is_closed() || retained.closed {
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
        let mut retained = self.retained.lock().expect("connection output");
        if let crate::protocol::ServerRecord::Bootstrap(bootstrap) = record {
            if bootstrap.input_epoch == 0 {
                return Err("workspace input peer is unavailable".into());
            }
            if retained.epoch != 0 && retained.epoch != bootstrap.input_epoch {
                return Err("workspace input peer changed on an established connection".into());
            }
            retained.epoch = bootstrap.input_epoch;
        }
        Ok(())
    }
    pub(crate) fn flush(
        &self,
        mut send: impl FnMut(&[u8]) -> Result<(), String>,
    ) -> Result<(), String> {
        let mut retained = self.retained.lock().expect("connection output");
        let before = retained.record_count();
        let result = (|| {
            if retained.closed {
                return Err("browser connection is closed".into());
            }
            // Before Bootstrap, retain the entire FIFO: later document commands
            // cannot pass mouse records awaiting this connection's peer identity.
            if retained.epoch == 0 {
                return Ok(());
            }
            while let Some(record) = retained.records.pop_front() {
                if let OutboundRecord::Mouse(mut mouse) = record {
                    mouse.peerepoch = retained.epoch;
                    let Outbound {
                        scratch, encoded, ..
                    } = &mut *retained;
                    crate::generated::encode_workspace_mouse_into(&mouse, scratch, encoded)
                        .map_err(|error| error.to_string())?;
                    send(encoded)?;
                } else {
                    send(&record.encode().map_err(|error| error.to_string())?)?;
                }
            }
            if let Some((sequence, admitted, false)) = retained.pressure_observation
                && admitted > OUTBOUND_CAPACITY
            {
                let record = crate::generated::IntegrationControl {
                    protocolversion: crate::generated::BROWSER_PROTOCOL_VERSION,
                    receipt: crate::generated::IntegrationControlReceipt {
                        kind: crate::generated::IntegrationControlKind::PressureEntered,
                        sequence,
                        progress: 0,
                        failureline: 0,
                        failure: String::new(),
                        readgeneration: 0,
                        compiledindex: 0,
                    },
                };
                send(&record.encode().map_err(|error| error.to_string())?)?;
                retained.pressure_observation = Some((sequence, admitted, true));
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
            retained.closed = true;
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
                let ordered_document = matches!(&record, OutboundRecord::Intent(intent)
                    if crate::generated::decode_application_intent_endpoint(intent.endpoint_id)
                        .is_some_and(|endpoint| crate::generated::application_intent_system(endpoint)
                            == crate::generated::ApplicationSystem::Annotation));
                if !ordered_document && retained.record_count() >= OUTBOUND_CAPACITY {
                    return if transient {
                        Ok(SendDisposition::Dropped)
                    } else {
                        Err(OutboundSendError::Capacity)
                    };
                }
                if retained.records.try_reserve(1).is_err() {
                    drop(retained);
                    if ordered_document {
                        self.close();
                    }
                    return Err(OutboundSendError::Allocation);
                }
                retained.records.push_back(record);
                retained.adjacent_record = true;
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
    Other,
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
            return Ok(CapturedRecord::Other);
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

    #[test]
    fn mouse_pressure_keeps_every_record_and_document_command_in_order() {
        let (sender, _receiver) = mpsc::channel(1);
        let mut connection = Connection::new(sender);
        let mut expected = Vec::new();
        for index in 0..1024 {
            let mut mouse = crate::workspace_input::record(
                crate::generated::WorkspaceMouseKind::Motion,
                Some(crate::generated::WorkspacePoint {
                    x: index as f32 + 0.25,
                    y: 0.125,
                }),
            );
            mouse.source = crate::generated::PresentationSourceKind::Annotation;
            mouse.documentepoch = 2;
            connection.send_workspace_mouse(mouse.clone()).unwrap();
            mouse.peerepoch = 1;
            expected.push(
                crate::generated::encode_workspace_mouse(mouse)
                    .unwrap()
                    .encode()
                    .unwrap(),
            );
            if index == 500 {
                let command = crate::generated::encode_annotation_Stop(1).record;
                expected.push(command.clone().encode().unwrap());
                connection.send_intent(command).unwrap();
            }
        }
        let mut actual = Vec::new();
        connection
            .flush(|bytes| {
                actual.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert!(actual.is_empty());
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
            .flush(|bytes| {
                actual.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(actual, expected);
        // Established connections keep the same reusable FIFO and encoder.
        let mut mouse =
            crate::workspace_input::record(crate::generated::WorkspaceMouseKind::Cancel, None);
        mouse.source = crate::generated::PresentationSourceKind::Annotation;
        connection.send_workspace_mouse(mouse.clone()).unwrap();
        mouse.peerepoch = 1;
        expected.push(
            crate::generated::encode_workspace_mouse(mouse)
                .unwrap()
                .encode()
                .unwrap(),
        );
        connection
            .flush(|bytes| {
                actual.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        assert_eq!(actual, expected);
    }

    #[test]
    fn bootstrap_binds_only_its_connection_and_keeps_an_established_epoch() {
        let (sender, _old_receiver) = mpsc::channel(1);
        let mut old = Connection::new(sender);
        let mut mouse =
            crate::workspace_input::record(crate::generated::WorkspaceMouseKind::Press, None);
        mouse.source = crate::generated::PresentationSourceKind::Explore;
        old.send_workspace_mouse(mouse.clone()).unwrap();
        old.close();
        assert_eq!(
            old.send_workspace_mouse(mouse.clone()),
            Err(OutboundSendError::Closed)
        );
        let mut old_records = Vec::new();
        assert!(
            old.flush(|bytes| {
                old_records.push(bytes.to_vec());
                Ok(())
            })
            .is_err()
        );
        assert!(old_records.is_empty());

        let (sender, _new_receiver) = mpsc::channel(1);
        let mut replacement = Connection::new(sender);
        mouse.kind = crate::generated::WorkspaceMouseKind::Motion;
        mouse.point = Some(crate::generated::WorkspacePoint { x: 7.25, y: 9.125 });
        mouse.peerepoch = 999; // Capture never chooses a connection's peer identity.
        replacement.send_workspace_mouse(mouse.clone()).unwrap();
        let bootstrap = |input_epoch| {
            crate::protocol::ServerRecord::Bootstrap(crate::protocol::Bootstrap {
                schema_fingerprint: crate::generated::SCHEMA_FINGERPRINT,
                input_epoch,
                snapshots: Vec::new(),
            })
        };
        assert!(replacement.observe(&bootstrap(0)).is_err());
        replacement.observe(&bootstrap(8)).unwrap();
        replacement.observe(&bootstrap(8)).unwrap();
        assert!(replacement.observe(&bootstrap(9)).is_err());
        let mut actual = Vec::new();
        replacement
            .flush(|bytes| {
                actual.push(bytes.to_vec());
                Ok(())
            })
            .unwrap();
        mouse.peerepoch = 8;
        assert_eq!(
            actual,
            vec![
                crate::generated::encode_workspace_mouse(mouse)
                    .unwrap()
                    .encode()
                    .unwrap()
            ]
        );
    }

    #[test]
    fn ordinary_capacity_and_connection_closure_are_explicit() {
        let (mut connection, _capture) = Connection::test_channel();
        for correlation in 1..=OUTBOUND_CAPACITY as u64 {
            connection
                .send_intent(Intent {
                    correlation,
                    endpoint_id: 1,
                    fields: Vec::new(),
                })
                .unwrap();
        }
        assert_eq!(
            connection.send_intent(Intent {
                correlation: 65,
                endpoint_id: 1,
                fields: Vec::new()
            }),
            Err(OutboundSendError::Capacity)
        );
        connection.close();
        assert_eq!(
            connection.send_intent(Intent {
                correlation: 66,
                endpoint_id: 1,
                fields: Vec::new()
            }),
            Err(OutboundSendError::Closed)
        );
    }
}
