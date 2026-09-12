use crate::generated::{self, AnnotationInputBatch, AnnotationPointer};
use crate::protocol::ServerRecord;
use crate::protocol::client_records::Intent;
use std::collections::VecDeque;

#[derive(Debug)]
struct Sample {
    pointer: AnnotationPointer,
    document_epoch: u64,
}

#[derive(Debug)]
struct Command {
    after_samples: u64,
    intent: Intent,
}

#[derive(Debug)]
struct CommandBarrier {
    correlation: u64,
    endpoint: u64,
    admitted_revision: Option<u64>,
}

// The frontend owns accepted samples until the native CPU consumption watermark.
// Capacity is a retained high-water mark, never a gesture truncation policy.
#[derive(Debug)]
pub(crate) struct AnnotationInput {
    samples: VecDeque<Sample>,
    commands: VecDeque<Command>,
    accepted_samples: u64,
    consumed_samples: u64,
    flights: VecDeque<(u64, usize)>,
    sent_samples: usize,
    epoch: u64,
    ready_epoch: u64,
    sequence: u64,
    consumed: u64,
    barrier: Option<CommandBarrier>,
    settled_revision: u64,
    closed: bool,
    batch: AnnotationInputBatch,
    scratch: Vec<u8>,
    encoded: Vec<u8>,
    encoded_pointer: Option<AnnotationPointer>,
    encoded_document_epoch: u64,
}

impl Default for AnnotationInput {
    fn default() -> Self {
        Self {
            samples: VecDeque::with_capacity(64),
            commands: VecDeque::with_capacity(64),
            accepted_samples: 0,
            consumed_samples: 0,
            flights: VecDeque::with_capacity(generated::ANNOTATION_INPUT_ADMISSION_SLOTS),
            sent_samples: 0,
            epoch: 0,
            ready_epoch: 0,
            sequence: 0,
            consumed: 0,
            barrier: None,
            settled_revision: 0,
            closed: false,
            batch: AnnotationInputBatch {
                documentepoch: 0,
                sequence: 0,
                samples: Vec::with_capacity(generated::ANNOTATION_INPUT_BATCH_CAPACITY),
            },
            scratch: Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY),
            encoded: Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY),
            encoded_pointer: None,
            encoded_document_epoch: 0,
        }
    }
}

impl AnnotationInput {
    pub fn owns_command(endpoint: u64) -> bool {
        generated::decode_application_intent_endpoint(endpoint).is_some_and(|endpoint| {
            generated::application_intent_system(endpoint)
                == generated::ApplicationSystem::Annotation
        })
    }
    pub fn pointer(
        &mut self,
        pointer: AnnotationPointer,
        document_epoch: u64,
    ) -> Result<(), String> {
        if self.closed {
            return Err("browser connection is closed".into());
        }
        let accepted = self
            .accepted_samples
            .checked_add(1)
            .ok_or("annotation sample identity exhausted")?;
        if self.samples.len() == self.samples.capacity() {
            self.samples
                .try_reserve(self.samples.capacity().max(64))
                .map_err(|_| {
                    "annotation input allocation failed; gesture was not shortened".to_owned()
                })?;
        }
        self.samples.push_back(Sample {
            pointer,
            document_epoch,
        });
        self.accepted_samples = accepted;
        Ok(())
    }
    pub fn command_count(&self) -> usize {
        self.commands.len()
    }
    pub fn command(&mut self, intent: Intent) {
        self.commands.push_back(Command {
            after_samples: self.accepted_samples,
            intent,
        });
    }
    pub fn is_closed(&self) -> bool {
        self.closed
    }
    pub(crate) fn pressure_entered(&self) -> bool {
        let frontier = self
            .commands
            .front()
            .map_or(self.accepted_samples, |command| command.after_samples);
        !self.closed
            && self.barrier.is_none()
            && self.epoch != 0
            && self.epoch == self.ready_epoch
            && self.flights.len() == generated::ANNOTATION_INPUT_ADMISSION_SLOTS
            && frontier > self.consumed_samples + self.sent_samples as u64
    }
    pub(crate) fn settled(&self) -> bool {
        !self.closed
            && self.samples.is_empty()
            && self.flights.is_empty()
            && self.commands.is_empty()
            && self.barrier.is_none()
    }
    pub fn close(&mut self) {
        self.closed = true;
        self.samples.clear();
        self.commands.clear();
        self.flights.clear();
        self.batch.samples.clear();
    }
    pub fn observe(&mut self, record: &ServerRecord) -> Result<(), String> {
        match record {
            ServerRecord::Bootstrap(bootstrap) => {
                if bootstrap.input_epoch == 0 {
                    return Err("bootstrap input epoch is invalid".into());
                }
                if self.epoch != 0 && self.epoch != bootstrap.input_epoch {
                    return Err("input peer epoch changed".into());
                }
                self.epoch = bootstrap.input_epoch;
            }
            ServerRecord::InputProgress(crate::generated::InputProgress {
                progress,
                error,
                ..
            }) => {
                if self.epoch == 0 {
                    return Err("input progress preceded Bootstrap".into());
                }
                if progress.epoch != self.epoch {
                    return Ok(());
                }
                if let Some(detail) = &progress.rejection {
                    return Err(format!("annotation input failed: {detail}"));
                }
                if let Some(error) = error {
                    return Err(format!("annotation input rejected: {}", error.detail));
                }
                let consumed = progress.consumedsequence;
                if consumed < self.consumed || consumed > self.sequence {
                    return Err("invalid input consumption watermark".into());
                }
                self.ready_epoch = self.epoch;
                self.consumed = consumed;
                while self
                    .flights
                    .front()
                    .is_some_and(|flight| flight.0 <= consumed)
                {
                    let (_, count) = self.flights.pop_front().expect("consumed batch");
                    for _ in 0..count {
                        self.samples.pop_front();
                    }
                    self.consumed_samples += count as u64;
                    self.sent_samples -= count;
                }
            }
            ServerRecord::IntentReply(reply)
                if self
                    .barrier
                    .as_ref()
                    .is_some_and(|barrier| barrier.correlation == reply.correlation) =>
            {
                let barrier = self.barrier.as_mut().expect("matching command");
                match &reply.result {
                    Err(_) => self.barrier = None,
                    Ok(value) => {
                        let snapshot = match generated::decode_application_reply(
                            barrier.endpoint,
                            value.clone(),
                        )? {
                            generated::ApplicationReply::AnnotationOpen(snapshot)
                            | generated::ApplicationReply::AnnotationEdit(snapshot)
                            | generated::ApplicationReply::AnnotationSave(snapshot)
                            | generated::ApplicationReply::AnnotationStop(snapshot) => snapshot,
                            _ => {
                                return Err("annotation barrier received an unrelated reply".into());
                            }
                        };
                        if !snapshot.busy || self.settled_revision > snapshot.revision {
                            self.barrier = None;
                        } else {
                            barrier.admitted_revision = Some(snapshot.revision);
                        }
                    }
                }
            }
            ServerRecord::SystemEvent(event) => {
                let settled = match &event.event {
                    generated::ApplicationEvent::AnnotationAnnotationChanged(changed)
                        if !changed.snapshot.busy =>
                    {
                        Some(changed.snapshot.revision)
                    }
                    generated::ApplicationEvent::AnnotationAnnotationFailed(failed)
                        if !failed.snapshot.busy =>
                    {
                        Some(failed.snapshot.revision)
                    }
                    _ => None,
                };
                if let Some(revision) = settled {
                    self.settled_revision = self.settled_revision.max(revision);
                    if self
                        .barrier
                        .as_ref()
                        .and_then(|barrier| barrier.admitted_revision)
                        .is_some_and(|admitted| self.settled_revision > admitted)
                    {
                        self.barrier = None;
                    }
                }
            }
            _ => {}
        }
        Ok(())
    }
    pub fn flush(
        &mut self,
        mut send: impl FnMut(&[u8]) -> Result<(), String>,
    ) -> Result<(), String> {
        if self.closed {
            return Ok(());
        }
        loop {
            if self.barrier.is_some() {
                // Only the bounded command queue is inspected. Stop cancels the
                // active command but never takes ownership of its settlement.
                while let Some(index) = self.commands.iter().position(|command| {
                    command.intent.endpoint_id == generated::ENDPOINT_Annotation_Stop
                }) {
                    let command = self.commands.remove(index).expect("queued Stop");
                    send(&command.intent.encode().map_err(|error| error.to_string())?)?;
                }
                return Ok(());
            }
            let frontier = self
                .commands
                .front()
                .map_or(self.accepted_samples, |command| command.after_samples);
            if self.consumed_samples == frontier && !self.commands.is_empty() {
                let command = self.commands.pop_front().expect("ready command");
                self.barrier = Some(CommandBarrier {
                    correlation: command.intent.correlation,
                    endpoint: command.intent.endpoint_id,
                    admitted_revision: None,
                });
                send(&command.intent.encode().map_err(|error| error.to_string())?)?;
                continue;
            }
            let available = frontier - self.consumed_samples - self.sent_samples as u64;
            if available == 0
                || self.epoch == 0
                || self.epoch != self.ready_epoch
                || self.flights.len() == generated::ANNOTATION_INPUT_ADMISSION_SLOTS
            {
                return Ok(());
            }
            self.batch.documentepoch = self.samples[self.sent_samples].document_epoch;
            self.batch.samples.clear();
            let mut prior = if self.encoded_document_epoch == self.batch.documentepoch { self.encoded_pointer.clone() } else { None };
            let count = available.min(generated::ANNOTATION_INPUT_BATCH_CAPACITY as u64) as usize;
            for sample in self
                .samples
                .range(self.sent_samples..self.sent_samples + count)
            {
                if sample.document_epoch != self.batch.documentepoch {
                    break;
                }
                self.batch.samples.push(compact_sample(&sample.pointer, prior.as_ref()));
                prior = if matches!(sample.pointer.phase, generated::AnnotationPointerPhase::End | generated::AnnotationPointerPhase::Cancel) {
                    None
                } else {
                    Some(sample.pointer.clone())
                };
            }
            self.batch.sequence = self
                .sequence
                .checked_add(1)
                .ok_or("annotation input sequence exhausted")?;
            generated::encode_annotation_Input_into(
                &self.batch,
                &mut self.scratch,
                &mut self.encoded,
            )
            .map_err(|error| error.to_string())?;
            send(&self.encoded)?;
            self.encoded_pointer = prior;
            self.encoded_document_epoch = self.batch.documentepoch;
            self.sequence = self.batch.sequence;
            self.sent_samples += self.batch.samples.len();
            self.flights
                .push_back((self.sequence, self.batch.samples.len()));
        }
    }
}

// A delta is used only when reconstruction is bit-exact and all retained
// metadata is unchanged. Absolute samples are also ordered parameter changes.
pub(crate) fn compact_sample(pointer: &AnnotationPointer, prior: Option<&AnnotationPointer>) -> generated::AnnotationPointerOrAnnotationPointVariant {
    use generated::AnnotationPointerOrAnnotationPointVariant as Sample;
    if let Some(prior) = prior.filter(|pointer| pointer.sequence != u64::MAX) {
        let delta = generated::AnnotationPoint { x: pointer.point.x - prior.point.x, y: pointer.point.y - prior.point.y };
        let mut reconstructed = prior.clone();
        reconstructed.phase = generated::AnnotationPointerPhase::Update;
        reconstructed.sequence = prior.sequence + 1;
        reconstructed.point.x += delta.x;
        reconstructed.point.y += delta.y;
        if delta.x.is_finite() && delta.y.is_finite() && reconstructed == *pointer
            && reconstructed.point.x.to_bits() == pointer.point.x.to_bits()
            && reconstructed.point.y.to_bits() == pointer.point.y.to_bits() {
            return Sample::AnnotationPoint(delta);
        }
    }
    Sample::AnnotationPointer(pointer.clone())
}

#[cfg(test)]
mod tests {
    use super::*;
    use generated::{AnnotationPoint, AnnotationPointerPhase as Phase, AnnotationPointerOrAnnotationPointVariant as Wire};

    fn pointer() -> AnnotationPointer {
        AnnotationPointer {
            phase: Phase::Begin, interactionid: 1, sequence: 1,
            identity: crate::generated::AnnotationTargetIdentity { object: 0, element: 0 },
            target: generated::AnnotationPointerTarget { object: None, element: None, role: None },
            point: AnnotationPoint { x: 1.25, y: 2.5 }, brushradius: 9,
        }
    }

    #[test]
    fn fractional_deltas_and_ordered_metadata_changes_have_exact_escapes() {
        let prior = pointer();
        let mut next = prior.clone();
        next.phase = Phase::Update;
        next.sequence = 2;
        next.point = AnnotationPoint { x: 1.375, y: 2.25 };
        assert_eq!(compact_sample(&next, Some(&prior)), Wire::AnnotationPoint(AnnotationPoint { x: 0.125, y: -0.25 }));
        assert!(matches!(compact_sample(&next, None), Wire::AnnotationPointer(_)));
        for changed in [
            AnnotationPointer { brushradius: 10, ..next.clone() },
            AnnotationPointer { interactionid: 2, ..next.clone() },
            AnnotationPointer { sequence: 3, ..next.clone() },
            AnnotationPointer { phase: Phase::End, ..next.clone() },
            AnnotationPointer { phase: Phase::Cancel, ..next.clone() },
            AnnotationPointer { target: generated::AnnotationPointerTarget { object: Some(1), element: None, role: None }, ..next.clone() },
            AnnotationPointer { identity: generated::AnnotationTargetIdentity { object: 7, element: 0 }, ..next.clone() },
        ] {
            assert!(matches!(compact_sample(&changed, Some(&prior)), Wire::AnnotationPointer(value) if value == changed));
        }
        let large = AnnotationPointer { point: AnnotationPoint { x: 1.0e20, y: 2.5 }, ..prior.clone() };
        next.point.x = 1.0;
        assert!(matches!(compact_sample(&next, Some(&large)), Wire::AnnotationPointer(_)));
        let zero = AnnotationPointer { point: AnnotationPoint { x: 0.0, y: 2.5 }, ..prior };
        next.point.x = -0.0;
        assert!(matches!(compact_sample(&next, Some(&zero)), Wire::AnnotationPointer(_)));
    }

    #[test]
    fn dense_curved_input_reconstructs_every_point_and_parameter_boundary() {
        let mut prior = None;
        let mut decoded: Option<AnnotationPointer> = None;
        let mut deltas = 0;
        for index in 0..257 {
            let mut next = pointer();
            next.sequence = index + 1;
            next.phase = if index == 0 { Phase::Begin } else if index == 256 { Phase::End } else { Phase::Update };
            let angle = index as f32 / 32.0;
            next.point = AnnotationPoint { x: 100.0 + angle.cos() * 40.0, y: 100.0 + angle.sin() * 40.0 };
            next.brushradius = if index < 129 { 9 } else { 11 };
            let wire = compact_sample(&next, prior.as_ref());
            let restored = match wire {
                Wire::AnnotationPointer(pointer) => pointer,
                Wire::AnnotationPoint(delta) => {
                    deltas += 1;
                    let mut pointer = decoded.take().expect("delta predecessor");
                    pointer.phase = Phase::Update;
                    pointer.sequence += 1;
                    pointer.point.x += delta.x;
                    pointer.point.y += delta.y;
                    pointer
                }
            };
            assert_eq!(restored, next);
            assert_eq!(restored.point.x.to_bits(), next.point.x.to_bits());
            assert_eq!(restored.point.y.to_bits(), next.point.y.to_bits());
            decoded = Some(restored);
            prior = Some(next);
        }
        assert!(deltas > 200);
    }
}
