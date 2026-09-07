// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{cmp::min, time::Instant};

use neqo_common::{Decoder, IncrementalDecoderUint, Role, qtrace};
use neqo_qpack::{decoder::QPACK_UNI_STREAM_TYPE_DECODER, encoder::QPACK_UNI_STREAM_TYPE_ENCODER};
use neqo_transport::{Connection, StreamId, StreamType};

use crate::{
    CloseType, Error, Http3StreamType, PushId, ReceiveOutput, RecvStream, Res, Stream,
    control_stream_local::HTTP3_UNI_STREAM_TYPE_CONTROL,
    frames::{HFrame, hframe::HFrameType, reader::FrameDecoder},
};

pub const HTTP3_UNI_STREAM_TYPE_PUSH: u64 = 0x1;
pub const WEBTRANSPORT_UNI_STREAM: u64 = 0x54;
pub const WEBTRANSPORT_STREAM: u64 = 0x41;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NewStreamType {
    Control,
    Decoder,
    Encoder,
    Push(PushId),
    WebTransportStream(u64),
    Http(u64),
    Unknown,
}

impl NewStreamType {
    /// Get the final `NewStreamType` from a stream type. All streams, except Push stream,
    /// are identified by the type only. This function will return None for the Push stream
    /// because it needs the ID besides the type.
    ///
    /// # Errors
    ///
    /// Push streams received by the server are not allowed and this function will return
    /// `HttpStreamCreation` error.
    fn final_stream_type(
        stream_type: u64,
        trans_stream_type: StreamType,
        role: Role,
    ) -> Res<Option<Self>> {
        match (stream_type, trans_stream_type, role) {
            (HTTP3_UNI_STREAM_TYPE_CONTROL, StreamType::UniDi, _) => Ok(Some(Self::Control)),
            (QPACK_UNI_STREAM_TYPE_ENCODER, StreamType::UniDi, _) => Ok(Some(Self::Decoder)),
            (QPACK_UNI_STREAM_TYPE_DECODER, StreamType::UniDi, _) => Ok(Some(Self::Encoder)),
            (HTTP3_UNI_STREAM_TYPE_PUSH, StreamType::UniDi, Role::Client)
            | (WEBTRANSPORT_UNI_STREAM, StreamType::UniDi, _)
            | (WEBTRANSPORT_STREAM, StreamType::BiDi, _) => Ok(None),
            (_, StreamType::BiDi, Role::Server) => {
                if <HFrame as FrameDecoder<HFrame>>::is_known_type(HFrameType(stream_type))
                    && HFrameType(stream_type) != HFrameType::HEADERS
                {
                    Err(Error::HttpFrame)
                } else {
                    Ok(Some(Self::Http(stream_type)))
                }
            }
            (HTTP3_UNI_STREAM_TYPE_PUSH, StreamType::UniDi, Role::Server)
            | (_, StreamType::BiDi, Role::Client) => Err(Error::HttpStreamCreation),
            _ => Ok(Some(Self::Unknown)),
        }
    }
}

/// `NewStreamHeadReader` reads the head of an unidirectional stream to identify the stream.
/// There are 2 type of streams:
///  - streams identified by the single type (varint encoded). Most streams belong to this category.
///    The `NewStreamHeadReader` will switch from `ReadType`to `Done` state.
///  - streams identified by the type and the ID (both varint encoded). For example, a push stream
///    is identified by the type and `PushId`. After reading the type in the `ReadType` state,
///    `NewStreamHeadReader` changes to `ReadId` state and from there to `Done` state
#[derive(Debug)]
pub enum NewStreamHeadReader {
    ReadType {
        role: Role,
        reader: IncrementalDecoderUint,
        stream_id: StreamId,
    },
    ReadId {
        stream_type: u64,
        reader: IncrementalDecoderUint,
        stream_id: StreamId,
    },
    Done,
}

impl NewStreamHeadReader {
    pub fn new(stream_id: StreamId, role: Role) -> Self {
        Self::ReadType {
            role,
            reader: IncrementalDecoderUint::default(),
            stream_id,
        }
    }

    fn read(&mut self, conn: &mut Connection) -> Res<(Option<u64>, bool)> {
        if let Self::ReadType {
            reader, stream_id, ..
        }
        | Self::ReadId {
            reader, stream_id, ..
        } = self
        {
            let mut buf = [0; 16];
            loop {
                let to_read = min(reader.min_remaining(), buf.len());
                let buf = &mut buf[0..to_read];
                match conn.stream_recv(*stream_id, &mut buf[..])? {
                    (0, f) => return Ok((None, f)),
                    (amount, f) => {
                        let res = reader.consume(&mut Decoder::from(&buf[..amount]));
                        if res.is_some() || f {
                            return Ok((res, f));
                        }
                    }
                }
            }
        } else {
            Ok((None, false))
        }
    }

    pub fn get_type(&mut self, conn: &mut Connection) -> Res<Option<NewStreamType>> {
        loop {
            let (output, fin) = self.read(conn)?;
            let Some(output) = output else {
                if fin {
                    *self = Self::Done;
                    return Err(Error::HttpStreamCreation);
                }
                return Ok(None);
            };

            qtrace!("Decoded uint {output}");
            match self {
                Self::ReadType {
                    role, stream_id, ..
                } => {
                    let final_type =
                        NewStreamType::final_stream_type(output, stream_id.stream_type(), *role);
                    match (&final_type, fin) {
                        (Err(_), _) => {
                            *self = Self::Done;
                            return final_type;
                        }
                        (Ok(t), true) => {
                            *self = Self::Done;
                            return Self::map_stream_fin(*t);
                        }
                        (Ok(Some(t)), false) => {
                            qtrace!("Decoded stream type {:?}", *t);
                            *self = Self::Done;
                            return final_type;
                        }
                        (Ok(None), false) => {
                            *self = Self::ReadId {
                                reader: IncrementalDecoderUint::default(),
                                stream_id: *stream_id,
                                stream_type: output,
                            }
                        }
                    }
                }
                Self::ReadId { stream_type, .. } => {
                    let is_push = *stream_type == HTTP3_UNI_STREAM_TYPE_PUSH;
                    *self = Self::Done;
                    qtrace!("New Stream stream push_id={output}");
                    if fin {
                        return Err(Error::HttpGeneralProtocol);
                    }
                    return if is_push {
                        Ok(Some(NewStreamType::Push(PushId::new(output))))
                    } else {
                        Ok(Some(NewStreamType::WebTransportStream(output)))
                    };
                }
                Self::Done => {
                    unreachable!("Cannot be in state NewStreamHeadReader::Done");
                }
            }
        }
    }

    fn map_stream_fin(decoded: Option<NewStreamType>) -> Res<Option<NewStreamType>> {
        match decoded {
            Some(NewStreamType::Control | NewStreamType::Encoder | NewStreamType::Decoder) => {
                Err(Error::HttpClosedCriticalStream)
            }
            None => Err(Error::HttpStreamCreation),
            Some(NewStreamType::Http(_)) => Err(Error::HttpFrame),
            Some(NewStreamType::Unknown) => Ok(decoded),
            Some(NewStreamType::Push(_) | NewStreamType::WebTransportStream(_)) => {
                unreachable!("PushStream and WebTransport are mapped to None at this stage")
            }
        }
    }

    const fn done(&self) -> bool {
        matches!(self, Self::Done)
    }
}

impl Stream for NewStreamHeadReader {
    fn stream_type(&self) -> Http3StreamType {
        Http3StreamType::NewStream
    }
}

impl RecvStream for NewStreamHeadReader {
    fn reset(&mut self, _close_type: CloseType) -> Res<()> {
        *self = Self::Done;
        Ok(())
    }

    fn receive(&mut self, conn: &mut Connection, _now: Instant) -> Res<(ReceiveOutput, bool)> {
        let t = self.get_type(conn)?;
        Ok((
            t.map_or(ReceiveOutput::NoOutput, ReceiveOutput::NewStream),
            self.done(),
        ))
    }
}
