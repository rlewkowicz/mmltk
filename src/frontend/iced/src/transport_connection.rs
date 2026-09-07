use crate::protocol::client_records::{Intent, Interaction};
use crate::protocol::{ProtocolError, RendererObservation};
use futures_channel::mpsc;
use iced::futures::Sink;
use std::fmt;

#[derive(Debug)]
pub(crate) enum OutboundRecord {
    Intent(Intent),
    Interaction(Interaction),
    RendererObservation(RendererObservation),
}

impl OutboundRecord {
    pub(crate) fn encode(self) -> Result<Vec<u8>, ProtocolError> {
        match self {
            Self::Intent(value) => value.encode(),
            Self::Interaction(value) => value.encode(),
            Self::RendererObservation(value) => value.encode(),
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
}

impl fmt::Display for OutboundSendError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Closed => "browser connection is closed",
            Self::Capacity => "outbound capacity is exhausted",
        })
    }
}

impl std::error::Error for OutboundSendError {}

#[derive(Debug, Clone)]
pub struct Connection {
    outbound: mpsc::Sender<OutboundRecord>,
}

impl Connection {
    pub(crate) fn new(outbound: mpsc::Sender<OutboundRecord>) -> Self {
        Self { outbound }
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
    ) -> Result<SendDisposition, OutboundSendError> {
        let replaceable = false; // Every brush segment is part of the transaction.
        self.send(
            OutboundRecord::Interaction(crate::generated::encode_annotation_Pointer(pointer)),
            replaceable,
        )
    }

    pub fn send_renderer_observation(
        &mut self,
        observation: RendererObservation,
    ) -> Result<SendDisposition, OutboundSendError> {
        self.send(OutboundRecord::RendererObservation(observation), true)
    }

    pub async fn writable(mut self) -> Result<(), OutboundSendError> {
        iced::futures::future::poll_fn(|context| {
            std::pin::Pin::new(&mut self.outbound).poll_ready(context)
        })
        .await
        .map_err(|_| OutboundSendError::Closed)
    }

    fn send(
        &mut self,
        record: OutboundRecord,
        transient: bool,
    ) -> Result<SendDisposition, OutboundSendError> {
        match self.outbound.try_send(record) {
            Ok(()) => Ok(SendDisposition::Queued),
            Err(error) if error.is_full() && transient => Ok(SendDisposition::Dropped),
            Err(error) if error.is_full() => Err(OutboundSendError::Capacity),
            Err(_) => Err(OutboundSendError::Closed),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application_codec::Value;

    #[test]
    fn every_outbound_record_uses_the_canonical_encoder() {
        let records = [
            OutboundRecord::Intent(Intent {
                correlation: 1,
                endpoint_id: 2,
                fields: Vec::new(),
            }),
            OutboundRecord::Interaction(Interaction {
                endpoint_id: 3,
                value: Value::Null,
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
                    endpoint_id: 1,
                    value: Value::Null,
                })
                .expect("transient pressure"),
            SendDisposition::Dropped
        );
        assert_eq!(
            connection.send_intent(Intent {
                correlation: 1,
                endpoint_id: 1,
                fields: Vec::new(),
            }),
            Err(OutboundSendError::Capacity)
        );
    }

    #[test]
    fn annotation_pointer_boundaries_are_not_droppable() {
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
            connection.send_annotation_pointer(pointer(phase)).unwrap();
            assert_eq!(
                connection.send_annotation_pointer(pointer(phase)),
                Err(OutboundSendError::Capacity)
            );
        }
        let (sender, _receiver) = mpsc::channel(0);
        let mut connection = Connection::new(sender);
        connection
            .send_annotation_pointer(pointer(crate::generated::AnnotationPointerPhase::Update))
            .unwrap();
        assert_eq!(
            connection
                .send_annotation_pointer(pointer(crate::generated::AnnotationPointerPhase::Update)),
            Err(OutboundSendError::Capacity)
        );
    }
}
