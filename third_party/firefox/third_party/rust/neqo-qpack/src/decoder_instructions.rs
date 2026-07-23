// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    fmt::{self, Display, Formatter},
    mem,
};

use neqo_common::{qdebug, qtrace};
use neqo_transport::StreamId;

use crate::{
    Res,
    prefix::{DECODER_HEADER_ACK, DECODER_INSERT_COUNT_INCREMENT, DECODER_STREAM_CANCELLATION},
    qpack_send_buf::Encoder,
    reader::{IntReader, ReadByte},
};

#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub enum DecoderInstruction {
    InsertCountIncrement {
        increment: u64,
    },
    HeaderAck {
        stream_id: StreamId,
    },
    StreamCancellation {
        stream_id: StreamId,
    },
    #[default]
    NoInstruction,
}

impl DecoderInstruction {
    fn get_instruction(b: u8) -> Self {
        if DECODER_HEADER_ACK.cmp_prefix(b) {
            Self::HeaderAck {
                stream_id: StreamId::from(0),
            }
        } else if DECODER_STREAM_CANCELLATION.cmp_prefix(b) {
            Self::StreamCancellation {
                stream_id: StreamId::from(0),
            }
        } else if DECODER_INSERT_COUNT_INCREMENT.cmp_prefix(b) {
            Self::InsertCountIncrement { increment: 0 }
        } else {
            unreachable!();
        }
    }

    pub(crate) fn marshal<T: Encoder>(&self, enc: &mut T) {
        match self {
            Self::InsertCountIncrement { increment } => {
                enc.encode_prefixed_encoded_int(DECODER_INSERT_COUNT_INCREMENT, *increment);
            }
            Self::HeaderAck { stream_id } => {
                enc.encode_prefixed_encoded_int(DECODER_HEADER_ACK, stream_id.as_u64());
            }
            Self::StreamCancellation { stream_id } => {
                enc.encode_prefixed_encoded_int(DECODER_STREAM_CANCELLATION, stream_id.as_u64());
            }
            Self::NoInstruction => {}
        }
    }
}

#[derive(Debug, Default)]
enum DecoderInstructionReaderState {
    #[default]
    ReadInstruction,
    ReadInt {
        reader: IntReader,
    },
}

#[derive(Debug, Default)]
pub struct DecoderInstructionReader {
    state: DecoderInstructionReaderState,
    instruction: DecoderInstruction,
}

impl Display for DecoderInstructionReader {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "InstructionReader")
    }
}

impl DecoderInstructionReader {
    /// # Errors
    ///
    /// 1) `NeedMoreData` if the reader needs more data
    /// 2) `ClosedCriticalStream`
    /// 3) other errors will be translated to `DecoderStream` by the caller of this function.
    pub fn read_instructions<R: ReadByte>(&mut self, recv: &mut R) -> Res<DecoderInstruction> {
        qdebug!("[{self}] read a new instruction");
        loop {
            match &mut self.state {
                DecoderInstructionReaderState::ReadInstruction => {
                    let b = recv.read_byte()?;
                    self.instruction = DecoderInstruction::get_instruction(b);
                    self.state = DecoderInstructionReaderState::ReadInt {
                        reader: IntReader::make(
                            b,
                            &[
                                DECODER_HEADER_ACK,
                                DECODER_STREAM_CANCELLATION,
                                DECODER_INSERT_COUNT_INCREMENT,
                            ],
                        ),
                    };
                }
                DecoderInstructionReaderState::ReadInt { reader } => {
                    let val = reader.read(recv)?;
                    qtrace!("[{self}] varint read {val}");
                    match &mut self.instruction {
                        DecoderInstruction::InsertCountIncrement { increment: v } => {
                            *v = val;
                            self.state = DecoderInstructionReaderState::ReadInstruction;
                            break Ok(mem::replace(
                                &mut self.instruction,
                                DecoderInstruction::NoInstruction,
                            ));
                        }
                        DecoderInstruction::HeaderAck { stream_id: v }
                        | DecoderInstruction::StreamCancellation { stream_id: v } => {
                            *v = StreamId::from(val);
                            self.state = DecoderInstructionReaderState::ReadInstruction;
                            break Ok(mem::replace(
                                &mut self.instruction,
                                DecoderInstruction::NoInstruction,
                            ));
                        }
                        DecoderInstruction::NoInstruction => {
                            unreachable!("This instruction cannot be in this state");
                        }
                    }
                }
            }
        }
    }
}
