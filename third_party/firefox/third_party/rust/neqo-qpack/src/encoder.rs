// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    cmp::min,
    collections::VecDeque,
    fmt::{self, Display, Formatter},
    time::Instant,
};

use neqo_common::{Header, qdebug, qerror, qlog::Qlog, qtrace};
use neqo_transport::{Connection, Error as TransportError, StreamId};
use rustc_hash::{FxHashMap as HashMap, FxHashSet as HashSet};

use crate::{
    Error, Res, Settings,
    decoder_instructions::{DecoderInstruction, DecoderInstructionReader},
    encoder_instructions::EncoderInstruction,
    header_block::HeaderEncoder,
    qlog,
    reader::ReceiverConnWrapper,
    stats::Stats,
    table::{ADDITIONAL_TABLE_ENTRY_SIZE, HeaderTable, LookupResult},
};

pub const QPACK_UNI_STREAM_TYPE_ENCODER: u64 = 0x2;

#[derive(Debug, PartialEq)]
enum LocalStreamState {
    NoStream,
    Uninitialized(StreamId),
    Initialized(StreamId),
}

impl LocalStreamState {
    pub const fn stream_id(&self) -> Option<StreamId> {
        match self {
            Self::NoStream => None,
            Self::Uninitialized(stream_id) | Self::Initialized(stream_id) => Some(*stream_id),
        }
    }
}

#[derive(Debug)]
pub struct Encoder {
    table: HeaderTable,
    max_table_size: u64,
    max_entries: u64,
    instruction_reader: DecoderInstructionReader,
    local_stream: LocalStreamState,
    max_blocked_streams: u16,
    unacked_header_blocks: HashMap<StreamId, VecDeque<HashSet<u64>>>,
    blocked_stream_cnt: u16,
    use_huffman: bool,
    next_capacity: Option<u64>,
    stats: Stats,
}

impl Encoder {
    #[must_use]
    pub fn new(qpack_settings: &Settings, use_huffman: bool) -> Self {
        Self {
            table: HeaderTable::new(true),
            max_table_size: qpack_settings.max_table_size_encoder,
            max_entries: 0,
            instruction_reader: DecoderInstructionReader::default(),
            local_stream: LocalStreamState::NoStream,
            max_blocked_streams: 0,
            unacked_header_blocks: HashMap::default(),
            blocked_stream_cnt: 0,
            use_huffman,
            next_capacity: None,
            stats: Stats::default(),
        }
    }

    /// This function is use for setting encoders table max capacity. The value is received as
    /// a `SETTINGS_QPACK_MAX_TABLE_CAPACITY` setting parameter.
    ///
    /// # Errors
    ///
    /// `EncoderStream` if value is too big.
    /// `ChangeCapacity` if table capacity cannot be reduced.
    pub fn set_max_capacity(&mut self, cap: u64) -> Res<()> {
        if cap > (1 << 30) - 1 {
            return Err(Error::EncoderStream);
        }

        if cap == self.table.capacity() {
            return Ok(());
        }

        qdebug!(
            "[{self}] Set max capacity to new capacity:{cap} old:{} max_table_size={}",
            self.table.capacity(),
            self.max_table_size,
        );

        let new_cap = min(self.max_table_size, cap);
        self.change_capacity(new_cap);
        Ok(())
    }

    /// This function is use for setting encoders max blocked streams. The value is received as
    /// a `SETTINGS_QPACK_BLOCKED_STREAMS` setting parameter.
    ///
    /// # Errors
    ///
    /// `EncoderStream` if value is too big.
    pub fn set_max_blocked_streams(&mut self, blocked_streams: u64) -> Res<()> {
        self.max_blocked_streams = u16::try_from(blocked_streams).or(Err(Error::EncoderStream))?;
        Ok(())
    }

    /// Reads decoder instructions.
    ///
    /// # Errors
    ///
    /// May return: `ClosedCriticalStream` if stream has been closed or `DecoderStream`
    /// in case of any other transport error.
    pub fn receive(&mut self, conn: &mut Connection, stream_id: StreamId, now: Instant) -> Res<()> {
        self.read_instructions(conn, stream_id, now)
            .map_err(|e| map_error(&e))
    }

    fn read_instructions(
        &mut self,
        conn: &mut Connection,
        stream_id: StreamId,
        now: Instant,
    ) -> Res<()> {
        qdebug!("[{self}] read a new instruction");
        loop {
            let mut recv = ReceiverConnWrapper::new(conn, stream_id);
            match self.instruction_reader.read_instructions(&mut recv) {
                Ok(instruction) => self.call_instruction(instruction, conn.qlog_mut(), now)?,
                Err(Error::NeedMoreData) => break Ok(()),
                Err(e) => break Err(e),
            }
        }
    }

    fn recalculate_blocked_streams(&mut self) {
        let acked_inserts_cnt = self.table.get_acked_inserts_cnt();
        self.blocked_stream_cnt = 0;
        #[expect(
            clippy::iter_over_hash_type,
            reason = "OK to loop over unACKed blocks in an undefined order."
        )]
        for hb_list in self.unacked_header_blocks.values_mut() {
            debug_assert!(!hb_list.is_empty());
            if hb_list.iter().flatten().any(|e| *e >= acked_inserts_cnt) {
                self.blocked_stream_cnt += 1;
            }
        }
    }

    fn insert_count_instruction(&mut self, increment: u64) -> Res<()> {
        self.table
            .increment_acked(increment)
            .map_err(|_| Error::DecoderStream)?;
        self.recalculate_blocked_streams();
        Ok(())
    }

    fn header_ack(&mut self, stream_id: StreamId) {
        self.stats.header_acks_recv += 1;
        let mut new_acked = self.table.get_acked_inserts_cnt();
        if let Some(hb_list) = self.unacked_header_blocks.get_mut(&stream_id) {
            if let Some(ref_list) = hb_list.pop_back() {
                #[expect(
                    clippy::iter_over_hash_type,
                    reason = "OK to loop over unACKed blocks in an undefined order."
                )]
                for iter in ref_list {
                    self.table.remove_ref(iter);
                    if iter >= new_acked {
                        new_acked = iter + 1;
                    }
                }
            } else {
                debug_assert!(false, "We should have at least one header block");
            }
            if hb_list.is_empty() {
                self.unacked_header_blocks.remove(&stream_id);
            }
        }
        if new_acked > self.table.get_acked_inserts_cnt() {
            self.insert_count_instruction(new_acked - self.table.get_acked_inserts_cnt())
                .expect("This should neve happen");
        }
    }

    fn stream_cancellation(&mut self, stream_id: StreamId) {
        self.stats.stream_cancelled_recv += 1;
        let mut was_blocker = false;
        if let Some(mut hb_list) = self.unacked_header_blocks.remove(&stream_id) {
            debug_assert!(!hb_list.is_empty());
            while let Some(ref_list) = hb_list.pop_front() {
                #[expect(
                    clippy::iter_over_hash_type,
                    reason = "OK to loop over unACKed blocks in an undefined order."
                )]
                for iter in ref_list {
                    self.table.remove_ref(iter);
                    was_blocker = was_blocker || (iter >= self.table.get_acked_inserts_cnt());
                }
            }
        }
        if was_blocker {
            debug_assert!(self.blocked_stream_cnt > 0);
            self.blocked_stream_cnt -= 1;
        }
    }

    fn call_instruction(
        &mut self,
        instruction: DecoderInstruction,
        qlog: &mut Qlog,
        now: Instant,
    ) -> Res<()> {
        qdebug!("[{self}] call instruction {instruction:?}");
        match instruction {
            DecoderInstruction::InsertCountIncrement { increment } => {
                qlog::qpack_read_insert_count_increment_instruction(
                    qlog,
                    increment,
                    &increment.to_be_bytes(),
                    now,
                );

                self.insert_count_instruction(increment)
            }
            DecoderInstruction::HeaderAck { stream_id } => {
                self.header_ack(stream_id);
                Ok(())
            }
            DecoderInstruction::StreamCancellation { stream_id } => {
                self.stream_cancellation(stream_id);
                Ok(())
            }
            DecoderInstruction::NoInstruction => Ok(()),
        }
    }

    /// Inserts a new entry into a table and sends the corresponding instruction to a peer. An entry
    /// is added only if it is possible to send the corresponding instruction immediately, i.e.
    /// the encoder stream is not blocked by the flow control (or stream internal buffer(this is
    /// very unlikely)).
    ///
    /// # Errors
    ///
    /// `EncoderStreamBlocked` if the encoder stream is blocked by the flow control.
    /// `DynamicTableFull` if the dynamic table does not have enough space for the entry.
    /// The function can return transport errors: `InvalidStreamId`, `InvalidInput` and
    /// `FinalSizeError`.
    ///
    /// # Panics
    ///
    /// When the insertion fails (it should not).
    pub fn send_and_insert(
        &mut self,
        conn: &mut Connection,
        name: &[u8],
        value: &[u8],
    ) -> Res<u64> {
        qdebug!("[{self}] insert {name:?} {value:?}");

        let entry_size = name.len() + value.len() + ADDITIONAL_TABLE_ENTRY_SIZE;

        if !self.table.insert_possible(entry_size) {
            return Err(Error::DynamicTableFull);
        }

        let mut buf = neqo_common::Encoder::default();
        EncoderInstruction::InsertWithNameLiteral { name, value }
            .marshal(&mut buf, self.use_huffman);

        let stream_id = self.local_stream.stream_id().ok_or(Error::Internal)?;

        let sent = conn
            .stream_send_atomic(stream_id, buf.as_ref())
            .map_err(|e| map_stream_send_atomic_error(&e))?;
        if !sent {
            return Err(Error::EncoderStreamBlocked);
        }

        self.stats.dynamic_table_inserts += 1;

        match self.table.insert(name, value) {
            Ok(inx) => Ok(inx),
            Err(e) => {
                debug_assert!(false);
                Err(e)
            }
        }
    }

    fn change_capacity(&mut self, value: u64) {
        qdebug!("[{self}] change capacity: {value}");
        self.next_capacity = Some(value);
    }

    fn maybe_send_change_capacity(
        &mut self,
        conn: &mut Connection,
        stream_id: StreamId,
    ) -> Res<()> {
        if let Some(cap) = self.next_capacity {
            if cap < self.table.capacity() && !self.table.can_evict_to(cap) {
                return Err(Error::DynamicTableFull);
            }
            let mut buf = neqo_common::Encoder::default();
            EncoderInstruction::Capacity { value: cap }.marshal(&mut buf, self.use_huffman);
            if !conn.stream_send_atomic(stream_id, buf.as_ref())? {
                return Err(Error::EncoderStreamBlocked);
            }
            if self.table.set_capacity(cap).is_err() {
                debug_assert!(
                    false,
                    "can_evict_to should have checked and make sure this operation is possible"
                );
                return Err(Error::Internal);
            }
            self.max_entries = cap / 32;
            self.next_capacity = None;
        }
        Ok(())
    }

    /// Sends any qpack encoder instructions.
    ///
    /// # Errors
    ///
    ///   returns `EncoderStream` in case of an error.
    pub fn send_encoder_updates(&mut self, conn: &mut Connection) -> Res<()> {
        match self.local_stream {
            LocalStreamState::NoStream => {
                qerror!("Send call but there is no stream yet");
                Ok(())
            }
            LocalStreamState::Uninitialized(stream_id) => {
                let mut buf = neqo_common::Encoder::default();
                buf.encode_varint(QPACK_UNI_STREAM_TYPE_ENCODER);
                if !conn.stream_send_atomic(stream_id, buf.as_ref())? {
                    return Err(Error::EncoderStreamBlocked);
                }
                self.local_stream = LocalStreamState::Initialized(stream_id);
                self.maybe_send_change_capacity(conn, stream_id)
            }
            LocalStreamState::Initialized(stream_id) => {
                self.maybe_send_change_capacity(conn, stream_id)
            }
        }
    }

    fn is_stream_blocker(&self, stream_id: StreamId) -> bool {
        self.unacked_header_blocks
            .get(&stream_id)
            .is_some_and(|hb_list| {
                debug_assert!(!hb_list.is_empty());
                hb_list
                    .iter()
                    .flatten()
                    .max()
                    .is_some_and(|max_ref| *max_ref >= self.table.get_acked_inserts_cnt())
            })
    }

    /// Encodes headers
    ///
    /// # Errors
    ///
    /// `ClosedCriticalStream` if the encoder stream is closed.
    /// `InternalError` if an unexpected error occurred.
    ///
    /// # Panics
    ///
    /// If there is a programming error.
    pub fn encode_header_block(
        &mut self,
        conn: &mut Connection,
        h: &[Header],
        stream_id: StreamId,
    ) -> HeaderEncoder {
        qdebug!("[{self}] encoding headers");

        let mut encoder_blocked = self.send_encoder_updates(conn).is_err();

        let mut encoded_h =
            HeaderEncoder::new(self.table.base(), self.use_huffman, self.max_entries);

        let stream_is_blocker = self.is_stream_blocker(stream_id);
        let can_block = self.blocked_stream_cnt < self.max_blocked_streams || stream_is_blocker;

        let mut ref_entries = HashSet::default();

        for iter in h {
            let name = iter.name().as_bytes().to_vec();
            let value = iter.value();
            qtrace!("encoding {name:x?} {value:x?}");

            if let Some(LookupResult {
                index,
                static_table,
                value_matches,
            }) = self.table.lookup(&name, value, can_block)
            {
                qtrace!(
                    "[{self}] found a {} entry, value-match={value_matches}",
                    if static_table { "static" } else { "dynamic" }
                );
                if value_matches {
                    if static_table {
                        encoded_h.encode_indexed_static(index);
                    } else {
                        encoded_h.encode_indexed_dynamic(index);
                    }
                } else {
                    encoded_h.encode_literal_with_name_ref(static_table, index, value);
                }
                if !static_table && ref_entries.insert(index) {
                    self.table.add_ref(index);
                }
            } else if can_block && !encoder_blocked {
                if let Ok(index) = self.send_and_insert(conn, &name, value) {
                    encoded_h.encode_indexed_dynamic(index);
                    ref_entries.insert(index);
                    self.table.add_ref(index);
                } else {
                    encoder_blocked = true;
                    encoded_h.encode_literal_with_name_literal(&name, value);
                }
            } else {
                encoded_h.encode_literal_with_name_literal(&name, value);
            }
        }

        encoded_h.encode_header_block_prefix();

        if !stream_is_blocker {
            if let Some(max_ref) = ref_entries.iter().max()
                && *max_ref >= self.table.get_acked_inserts_cnt()
            {
                debug_assert!(self.blocked_stream_cnt <= self.max_blocked_streams);
                self.blocked_stream_cnt += 1;
            }
        }

        if !ref_entries.is_empty() {
            self.unacked_header_blocks
                .entry(stream_id)
                .or_default()
                .push_front(ref_entries);
            self.stats.dynamic_table_references += 1;
        }
        encoded_h
    }

    /// Encoder stream has been created. Add the stream id.
    ///
    /// # Panics
    ///
    /// If a stream has already been added.
    pub fn add_send_stream(&mut self, stream_id: StreamId) {
        if self.local_stream == LocalStreamState::NoStream {
            self.local_stream = LocalStreamState::Uninitialized(stream_id);
        } else {
            panic!("Adding multiple local streams");
        }
    }

    #[must_use]
    pub fn stats(&self) -> Stats {
        self.stats.clone()
    }

    #[must_use]
    pub const fn local_stream_id(&self) -> Option<StreamId> {
        self.local_stream.stream_id()
    }

}

impl Display for Encoder {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "QPack")
    }
}

fn map_error(err: &Error) -> Error {
    if *err == Error::ClosedCriticalStream {
        Error::ClosedCriticalStream
    } else {
        Error::DecoderStream
    }
}

fn map_stream_send_atomic_error(err: &TransportError) -> Error {
    match err {
        TransportError::InvalidStreamId | TransportError::FinalSize => Error::ClosedCriticalStream,
        _ => {
            debug_assert!(false, "Unexpected error");
            Error::Internal
        }
    }
}
