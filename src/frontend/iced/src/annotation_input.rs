use std::collections::VecDeque;
use crate::generated::{self, AnnotationInputBatch, AnnotationPointer};
use crate::protocol::ServerRecord;
use crate::transport_connection::OutboundRecord;

#[derive(Debug)]
enum Entry {
    Sample { pointer: AnnotationPointer, document_epoch: u64 },
    Record(OutboundRecord),
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
    entries: VecDeque<Entry>,
    record_count: usize,
    writable: Option<std::task::Waker>,
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
            entries: VecDeque::with_capacity(64),
            record_count: 0, writable: None,
            flights: VecDeque::with_capacity(generated::ANNOTATION_INPUT_ADMISSION_SLOTS),
            sent_samples: 0, epoch: 0, ready_epoch: 0, sequence: 0, consumed: 0,
            barrier: None, settled_revision: 0, closed: false,
            batch: AnnotationInputBatch { epoch: 0, documentepoch: 0, sequence: 0, samples: Vec::with_capacity(generated::ANNOTATION_INPUT_BATCH_CAPACITY) },
            scratch: Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY), encoded: Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY),
        }
    }
}

impl AnnotationInput {
    pub fn pointer(&mut self, pointer: AnnotationPointer, document_epoch: u64) -> Result<(), String> {
        self.reserve_entry()?;
        self.entries.push_back(Entry::Sample { pointer, document_epoch });
        Ok(())
    }
    pub fn record(&mut self, record: OutboundRecord) -> Result<bool, String> {
        // Only adjacent absolute viewport requests are equivalent replacements.
        if matches!((&record, self.entries.back()),
            (OutboundRecord::Interaction(next), Some(Entry::Record(OutboundRecord::Interaction(prior))))
                if next.replaceable && prior.endpoint_id == next.endpoint_id) {
            *self.entries.back_mut().expect("adjacent record") = Entry::Record(record);
            return Ok(true);
        }
        if self.record_count == 64 { return Ok(false); }
        self.reserve_entry()?;
        self.entries.push_back(Entry::Record(record));
        self.record_count += 1;
        Ok(true)
    }
    fn reserve_entry(&mut self) -> Result<(), String> {
        if self.closed { return Err("browser connection is closed".into()); }
        if self.entries.len() == self.entries.capacity() {
            self.entries.try_reserve(self.entries.capacity().max(64))
                .map_err(|_| "annotation input allocation failed; gesture was not shortened".to_owned())?;
        }
        Ok(())
    }
    pub fn poll_writable(&mut self, context: &mut std::task::Context<'_>) -> std::task::Poll<Result<(), crate::transport_connection::OutboundSendError>> {
        if self.closed { return std::task::Poll::Ready(Err(crate::transport_connection::OutboundSendError::Closed)); }
        if self.record_count < 64 { return std::task::Poll::Ready(Ok(())); }
        self.writable = Some(context.waker().clone());
        std::task::Poll::Pending
    }
    pub fn is_closed(&self) -> bool { self.closed }
    pub fn close(&mut self) {
        self.closed = true;
        self.entries.clear();
        self.record_count = 0;
        if let Some(waker) = self.writable.take() { waker.wake(); }
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
            ServerRecord::InputProgress { progress, error } => {
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
                    for _ in 0..count { self.entries.pop_front(); }
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
        if self.closed || self.barrier.is_some() { return Ok(()); }
        loop {
            match self.entries.get(self.sent_samples) {
                Some(Entry::Sample { document_epoch, .. }) if self.epoch != 0 && self.epoch == self.ready_epoch && self.flights.len() < generated::ANNOTATION_INPUT_ADMISSION_SLOTS => {
                    self.batch.documentepoch = *document_epoch;
                    self.batch.samples.clear();
                    for entry in self.entries.iter().skip(self.sent_samples).take(generated::ANNOTATION_INPUT_BATCH_CAPACITY) {
                        let Entry::Sample { pointer, document_epoch } = entry else { break; };
                        if *document_epoch != self.batch.documentepoch { break; }
                        self.batch.samples.push(pointer.clone());
                    }
                    self.batch.epoch = self.epoch;
                    self.batch.sequence = self.sequence.checked_add(1).ok_or("annotation input sequence exhausted")?;
                    generated::encode_annotation_Input_into(&self.batch, &mut self.scratch, &mut self.encoded).map_err(|error| error.to_string())?;
                    send(&self.encoded)?;
                    self.sequence = self.batch.sequence;
                    self.sent_samples += self.batch.samples.len();
                    self.flights.push_back((self.sequence, self.batch.samples.len()));
                }
                Some(Entry::Record(_)) if self.flights.is_empty() => {
                    let Some(Entry::Record(record)) = self.entries.pop_front() else { unreachable!() };
                    self.record_count -= 1;
                    if let Some(waker) = self.writable.take() { waker.wake(); }
                    if let OutboundRecord::Intent(intent) = &record {
                        // An annotation command waits for all earlier samples and
                        // fences every later sample through native settlement.
                        if [generated::ENDPOINT_Annotation_Open, generated::ENDPOINT_Annotation_Edit,
                            generated::ENDPOINT_Annotation_Save, generated::ENDPOINT_Annotation_Stop].contains(&intent.endpoint_id) {
                            self.barrier = Some(CommandBarrier { correlation: intent.correlation, endpoint: intent.endpoint_id, admitted_revision: None });
                        }
                    }
                    let encoded = record.encode().map_err(|error| error.to_string())?;
                    send(&encoded)?;
                    if self.barrier.is_some() { return Ok(()); }
                }
                _ => return Ok(()),
            }
        }
    }
}
