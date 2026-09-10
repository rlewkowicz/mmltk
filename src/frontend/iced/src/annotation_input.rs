use std::collections::VecDeque;
use crate::generated::{self, AnnotationInputBatch, AnnotationPointer};
use crate::protocol::ServerRecord;
use crate::protocol::client_records::Intent;

#[derive(Debug)]
struct Sample { pointer: AnnotationPointer, document_epoch: u64 }

#[derive(Debug)]
struct Command { after_samples: u64, intent: Intent }

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
}

impl Default for AnnotationInput {
    fn default() -> Self {
        Self {
            samples: VecDeque::with_capacity(64),
            commands: VecDeque::with_capacity(64),
            accepted_samples: 0, consumed_samples: 0,
            flights: VecDeque::with_capacity(generated::ANNOTATION_INPUT_ADMISSION_SLOTS),
            sent_samples: 0, epoch: 0, ready_epoch: 0, sequence: 0, consumed: 0,
            barrier: None, settled_revision: 0, closed: false,
            batch: AnnotationInputBatch { epoch: 0, documentepoch: 0, sequence: 0, samples: Vec::with_capacity(generated::ANNOTATION_INPUT_BATCH_CAPACITY) },
            scratch: Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY), encoded: Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY),
        }
    }
}

impl AnnotationInput {
    pub fn owns_command(endpoint: u64) -> bool {
        generated::decode_application_intent_endpoint(endpoint)
            .is_some_and(|endpoint| generated::application_intent_system(endpoint) == generated::ApplicationSystem::Annotation)
    }
    pub fn pointer(&mut self, pointer: AnnotationPointer, document_epoch: u64) -> Result<(), String> {
        if self.closed { return Err("browser connection is closed".into()); }
        let accepted = self.accepted_samples.checked_add(1).ok_or("annotation sample identity exhausted")?;
        if self.samples.len() == self.samples.capacity() {
            self.samples.try_reserve(self.samples.capacity().max(64))
                .map_err(|_| "annotation input allocation failed; gesture was not shortened".to_owned())?;
        }
        self.samples.push_back(Sample { pointer, document_epoch });
        self.accepted_samples = accepted;
        Ok(())
    }
    pub fn command_count(&self) -> usize { self.commands.len() }
    pub fn command(&mut self, intent: Intent) {
        self.commands.push_back(Command { after_samples: self.accepted_samples, intent });
    }
    pub fn is_closed(&self) -> bool { self.closed }
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
                if bootstrap.input_epoch == 0 { return Err("bootstrap input epoch is invalid".into()); }
                if self.epoch != 0 && self.epoch != bootstrap.input_epoch { return Err("input peer epoch changed".into()); }
                self.epoch = bootstrap.input_epoch;
            }
            ServerRecord::InputProgress(crate::generated::InputProgress { progress, error , .. }) => {
                if self.epoch == 0 { return Err("input progress preceded Bootstrap".into()); }
                if progress.epoch != self.epoch { return Ok(()); }
                if let Some(detail) = &progress.rejection { return Err(format!("annotation input failed: {detail}")); }
                if let Some(error) = error { return Err(format!("annotation input rejected: {}", error.detail)); }
                let consumed = progress.consumedsequence;
                if consumed < self.consumed || consumed > self.sequence {
                    return Err("invalid input consumption watermark".into());
                }
                self.ready_epoch = self.epoch;
                self.consumed = consumed;
                while self.flights.front().is_some_and(|flight| flight.0 <= consumed) {
                    let (_, count) = self.flights.pop_front().expect("consumed batch");
                    for _ in 0..count { self.samples.pop_front(); }
                    self.consumed_samples += count as u64;
                    self.sent_samples -= count;
                }
            }
            ServerRecord::IntentReply(reply) if self.barrier.as_ref().is_some_and(|barrier| barrier.correlation == reply.correlation) => {
                let barrier = self.barrier.as_mut().expect("matching command");
                match &reply.result {
                    Err(_) => self.barrier = None,
                    Ok(value) => {
                        let snapshot = match generated::decode_application_reply(barrier.endpoint, value.clone())? {
                            generated::ApplicationReply::AnnotationOpen(snapshot) |
                            generated::ApplicationReply::AnnotationEdit(snapshot) |
                            generated::ApplicationReply::AnnotationSave(snapshot) |
                            generated::ApplicationReply::AnnotationStop(snapshot) => snapshot,
                            _ => return Err("annotation barrier received an unrelated reply".into()),
                        };
                        if !snapshot.busy || self.settled_revision > snapshot.revision { self.barrier = None; }
                        else { barrier.admitted_revision = Some(snapshot.revision); }
                    }
                }
            }
            ServerRecord::SystemEvent(event) => {
                let settled = match &event.event {
                    generated::ApplicationEvent::AnnotationAnnotationChanged(changed) if !changed.snapshot.busy => Some(changed.snapshot.revision),
                    generated::ApplicationEvent::AnnotationAnnotationFailed(failed) if !failed.snapshot.busy => Some(failed.snapshot.revision),
                    _ => None,
                };
                if let Some(revision) = settled {
                    self.settled_revision = self.settled_revision.max(revision);
                    if self.barrier.as_ref().and_then(|barrier| barrier.admitted_revision).is_some_and(|admitted| self.settled_revision > admitted) {
                        self.barrier = None;
                    }
                }
            }
            _ => {},
        }
        Ok(())
    }
    pub fn flush(&mut self, mut send: impl FnMut(&[u8]) -> Result<(), String>) -> Result<(), String> {
        if self.closed { return Ok(()); }
        loop {
            if self.barrier.is_some() {
                // Only the bounded command queue is inspected. Stop cancels the
                // active command but never takes ownership of its settlement.
                while let Some(index) = self.commands.iter().position(|command| command.intent.endpoint_id == generated::ENDPOINT_Annotation_Stop) {
                    let command = self.commands.remove(index).expect("queued Stop");
                    send(&command.intent.encode().map_err(|error| error.to_string())?)?;
                }
                return Ok(());
            }
            let frontier = self.commands.front().map_or(self.accepted_samples, |command| command.after_samples);
            if self.consumed_samples == frontier && !self.commands.is_empty() {
                let command = self.commands.pop_front().expect("ready command");
                self.barrier = Some(CommandBarrier { correlation: command.intent.correlation, endpoint: command.intent.endpoint_id, admitted_revision: None });
                send(&command.intent.encode().map_err(|error| error.to_string())?)?;
                continue;
            }
            let available = frontier - self.consumed_samples - self.sent_samples as u64;
            if available == 0 || self.epoch == 0 || self.epoch != self.ready_epoch || self.flights.len() == generated::ANNOTATION_INPUT_ADMISSION_SLOTS {
                return Ok(());
            }
            self.batch.documentepoch = self.samples[self.sent_samples].document_epoch;
            self.batch.samples.clear();
            let count = available.min(generated::ANNOTATION_INPUT_BATCH_CAPACITY as u64) as usize;
            for sample in self.samples.range(self.sent_samples..self.sent_samples + count) {
                if sample.document_epoch != self.batch.documentepoch { break; }
                self.batch.samples.push(sample.pointer.clone());
            }
            self.batch.epoch = self.epoch;
            self.batch.sequence = self.sequence.checked_add(1).ok_or("annotation input sequence exhausted")?;
            generated::encode_annotation_Input_into(&self.batch, &mut self.scratch, &mut self.encoded).map_err(|error| error.to_string())?;
            send(&self.encoded)?;
            self.sequence = self.batch.sequence;
            self.sent_samples += self.batch.samples.len();
            self.flights.push_back((self.sequence, self.batch.samples.len()));
        }
    }
}
