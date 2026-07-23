// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::fmt::{self, Display, Formatter};

use neqo_common::{Encoder, Header, qdebug};
use neqo_transport::{Connection, StreamId};

use crate::{
    Error, Res, Settings,
    decoder_instructions::DecoderInstruction,
    encoder_instructions::{DecodedEncoderInstruction, EncoderInstructionReader},
    header_block::{HeaderDecoder, HeaderDecoderResult},
    reader::{ReadByte, Reader, ReceiverConnWrapper},
    stats::Stats,
    table::HeaderTable,
};

pub const QPACK_UNI_STREAM_TYPE_DECODER: u64 = 0x3;

#[derive(Debug)]
pub struct Decoder {
    instruction_reader: EncoderInstructionReader,
    table: HeaderTable,
    acked_inserts: u64,
    max_entries: u64,
    send_buf: Encoder,
    local_stream_id: Option<StreamId>,
    max_table_size: u64,
    max_blocked_streams: usize,
    blocked_streams: Vec<(StreamId, u64)>, 
    stats: Stats,
}

impl Decoder {
    /// # Panics
    ///
    /// If settings include invalid values.
    #[must_use]
    pub fn new(qpack_settings: &Settings) -> Self {
        qdebug!("Decoder: creating a new qpack decoder");
        let mut send_buf = Encoder::default();
        send_buf.encode_varint(QPACK_UNI_STREAM_TYPE_DECODER);
        let max_blocked_streams = usize::from(qpack_settings.max_blocked_streams);
        Self {
            instruction_reader: EncoderInstructionReader::default(),
            table: HeaderTable::new(false),
            acked_inserts: 0,
            max_entries: qpack_settings.max_table_size_decoder >> 5,
            send_buf,
            local_stream_id: None,
            max_table_size: qpack_settings.max_table_size_decoder,
            max_blocked_streams,
            blocked_streams: Vec::with_capacity(max_blocked_streams),
            stats: Stats::default(),
        }
    }

    #[must_use]
    const fn capacity(&self) -> u64 {
        self.table.capacity()
    }

    /// Returns a list of unblocked streams.
    ///
    /// # Errors
    ///
    /// May return: `ClosedCriticalStream` if stream has been closed or `EncoderStream`
    /// in case of any other transport error.
    pub fn receive(&mut self, conn: &mut Connection, stream_id: StreamId) -> Res<Vec<StreamId>> {
        let base_old = self.table.base();
        self.read_instructions(conn, stream_id)
            .map_err(|e| map_error(&e))?;
        let base_new = self.table.base();
        if base_old == base_new {
            return Ok(Vec::new());
        }

        Ok(self
            .blocked_streams
            .extract_if(.., |(_, req)| *req <= base_new)
            .map(|(id, _)| id)
            .collect())
    }

    fn read_instructions(&mut self, conn: &mut Connection, stream_id: StreamId) -> Res<()> {
        let mut recv = ReceiverConnWrapper::new(conn, stream_id);
        self.process_instructions(&mut recv)
    }

    pub(crate) fn process_instructions<T: ReadByte + Reader>(&mut self, recv: &mut T) -> Res<()> {
        loop {
            match self.instruction_reader.read_instructions(recv) {
                Ok(instruction) => self.execute_instruction(instruction)?,
                Err(Error::NeedMoreData) => break Ok(()),
                Err(e) => break Err(e),
            }
        }
    }

    fn execute_instruction(&mut self, instruction: DecodedEncoderInstruction) -> Res<()> {
        match instruction {
            DecodedEncoderInstruction::Capacity { value } => self.set_capacity(value)?,
            DecodedEncoderInstruction::InsertWithNameRefStatic { index, value } => {
                Error::map_error(
                    self.table.insert_with_name_ref(true, index, &value),
                    Error::EncoderStream,
                )?;
                self.stats.dynamic_table_inserts += 1;
            }
            DecodedEncoderInstruction::InsertWithNameRefDynamic { index, value } => {
                Error::map_error(
                    self.table.insert_with_name_ref(false, index, &value),
                    Error::EncoderStream,
                )?;
                self.stats.dynamic_table_inserts += 1;
            }
            DecodedEncoderInstruction::InsertWithNameLiteral { name, value } => {
                Error::map_error(
                    self.table.insert(&name, &value).map(|_| ()),
                    Error::EncoderStream,
                )?;
                self.stats.dynamic_table_inserts += 1;
            }
            DecodedEncoderInstruction::Duplicate { index } => {
                Error::map_error(self.table.duplicate(index), Error::EncoderStream)?;
                self.stats.dynamic_table_inserts += 1;
            }
            DecodedEncoderInstruction::NoInstruction => {
                unreachable!("This can be call only with an instruction");
            }
        }
        Ok(())
    }

    fn set_capacity(&mut self, cap: u64) -> Res<()> {
        qdebug!("[{self}] received instruction capacity cap={cap}");
        if cap > self.max_table_size {
            return Err(Error::EncoderStream);
        }
        self.table.set_capacity(cap)
    }

    fn header_ack(&mut self, stream_id: StreamId, required_inserts: u64) {
        DecoderInstruction::HeaderAck { stream_id }.marshal(&mut self.send_buf);
        if required_inserts > self.acked_inserts {
            self.acked_inserts = required_inserts;
        }
    }

    pub fn cancel_stream(&mut self, stream_id: StreamId) {
        if self.table.capacity() > 0 {
            self.blocked_streams.retain(|(id, _)| *id != stream_id);
            DecoderInstruction::StreamCancellation { stream_id }.marshal(&mut self.send_buf);
        }
    }

    /// # Errors
    ///
    /// May return [`Error::Internal`] if the decoder stream is not initialized,
    /// or [`Error::DecoderStream`] if sending on the decoder stream fails.
    ///
    /// # Panics
    ///
    /// Never, but rust doesn't know that.
    pub fn send(&mut self, conn: &mut Connection) -> Res<()> {
        let increment = self.table.base() - self.acked_inserts;
        if increment > 0 {
            DecoderInstruction::InsertCountIncrement { increment }.marshal(&mut self.send_buf);
            self.acked_inserts = self.table.base();
        }
        if !self.send_buf.is_empty() && self.local_stream_id.is_some() {
            let r = conn
                .stream_send(
                    self.local_stream_id.ok_or(Error::Internal)?,
                    self.send_buf.as_ref(),
                )
                .map_err(|_| Error::DecoderStream)?;
            qdebug!("[{self}] {r} bytes sent");
            self.send_buf.skip(r);
        }
        Ok(())
    }

    /// # Errors
    ///
    /// May return `Error::Decompression` if header block is incorrect or incomplete.
    pub fn refers_dynamic_table(&self, buf: &[u8]) -> Res<bool> {
        HeaderDecoder::new(buf).refers_dynamic_table(self.max_entries, self.table.base())
    }

    /// This function returns None if the stream is blocked waiting for table insertions.
    /// 'buf' must contain the complete header block.
    ///
    /// # Errors
    ///
    /// May return `Error::Decompression` if header block is incorrect or incomplete.
    ///
    /// # Panics
    ///
    /// When there is a programming error.
    pub fn decode_header_block(
        &mut self,
        buf: &[u8],
        stream_id: StreamId,
    ) -> Res<Option<Vec<Header>>> {
        qdebug!("[{self}] decode header block");
        let mut decoder = HeaderDecoder::new(buf);

        match decoder.decode_header_block(&self.table, self.max_entries, self.table.base()) {
            Ok(HeaderDecoderResult::Blocked(req_insert_cnt)) => {
                if self.blocked_streams.len() > self.max_blocked_streams {
                    Err(Error::Decompression)
                } else {
                    let found = self
                        .blocked_streams
                        .iter()
                        .any(|(id, _req)| *id == stream_id);
                    if !found {
                        self.blocked_streams.push((stream_id, req_insert_cnt));
                    }
                    Ok(None)
                }
            }
            Ok(HeaderDecoderResult::Headers(h)) => {
                if decoder.get_req_insert_cnt() != 0 {
                    self.header_ack(stream_id, decoder.get_req_insert_cnt());
                    self.stats.dynamic_table_references += 1;
                }
                Ok(Some(h))
            }
            Err(_) => Err(Error::Decompression),
        }
    }

    /// # Panics
    ///
    /// When a stream has already been added.
    pub fn add_send_stream(&mut self, stream_id: StreamId) {
        assert!(
            self.local_stream_id.is_none(),
            "Adding multiple local streams"
        );
        self.local_stream_id = Some(stream_id);
    }

    #[must_use]
    pub const fn local_stream_id(&self) -> Option<StreamId> {
        self.local_stream_id
    }

    #[must_use]
    pub fn stats(&self) -> Stats {
        self.stats.clone()
    }
}

impl Display for Decoder {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "QPack {}", self.capacity())
    }
}

fn map_error(err: &Error) -> Error {
    if *err == Error::ClosedCriticalStream {
        Error::ClosedCriticalStream
    } else {
        Error::EncoderStream
    }
}
