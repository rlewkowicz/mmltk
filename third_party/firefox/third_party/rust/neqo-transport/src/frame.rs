// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::ops::RangeInclusive;

use neqo_common::{Buffer, Decoder, Encoder, MAX_VARINT, qtrace};
use strum::FromRepr;

use crate::{
    AppError, ConnectionId, Error, Res, TransportError, ecn, packet,
    stateless_reset::Token as Srt,
    stream_id::{StreamId, StreamType},
};

#[repr(u64)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, FromRepr)]
pub enum FrameType {
    Padding = 0x0,
    Ping = 0x1,
    Ack = 0x2,
    AckEcn = 0x3,
    ResetStream = 0x4,
    StopSending = 0x5,
    Crypto = 0x6,
    NewToken = 0x7,
    Stream = 0x08, 
    StreamWithFin = 0x08 + 0b001,
    StreamWithLen = 0x08 + 0b010,
    StreamWithLenFin = 0x08 + 0b011,
    StreamWithOff = 0x08 + 0b100,
    StreamWithOffFin = 0x08 + 0b101,
    StreamWithOffLen = 0x08 + 0b110,
    StreamWithOffLenFin = 0x08 + 0b111,
    MaxData = 0x10,
    MaxStreamData = 0x11,
    MaxStreamsBiDi = 0x12,
    MaxStreamsUniDi = 0x13,
    DataBlocked = 0x14,
    StreamDataBlocked = 0x15,
    StreamsBlockedBiDi = 0x16,
    StreamsBlockedUniDi = 0x17,
    NewConnectionId = 0x18,
    RetireConnectionId = 0x19,
    PathChallenge = 0x1a,
    PathResponse = 0x1b,
    ConnectionCloseTransport = 0x1c,
    ConnectionCloseApplication = 0x1d,
    HandshakeDone = 0x1e,
    AckFrequency = 0xaf,
    Datagram = 0x30,
    DatagramWithLen = 0x31,
}

impl From<FrameType> for u64 {
    fn from(val: FrameType) -> Self {
        val as Self
    }
}

impl From<FrameType> for u8 {
    fn from(val: FrameType) -> Self {
        val as Self
    }
}

impl TryFrom<u64> for FrameType {
    type Error = Error;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        Self::from_repr(value).ok_or(Error::UnknownFrameType)
    }
}

impl FrameType {
    const fn is_stream_with_length(self) -> bool {
        matches!(
            self,
            Self::StreamWithLen
                | Self::StreamWithLenFin
                | Self::StreamWithOffLen
                | Self::StreamWithOffLenFin
        )
    }

    const fn is_stream_with_offset(self) -> bool {
        matches!(
            self,
            Self::StreamWithOff
                | Self::StreamWithOffFin
                | Self::StreamWithOffLen
                | Self::StreamWithOffLenFin
        )
    }
    const fn is_stream_with_fin(self) -> bool {
        matches!(
            self,
            Self::StreamWithFin
                | Self::StreamWithLenFin
                | Self::StreamWithOffFin
                | Self::StreamWithOffLenFin
        )
    }
}

impl TryFrom<FrameType> for StreamType {
    type Error = Error;

    fn try_from(value: FrameType) -> Result<Self, Self::Error> {
        match value {
            FrameType::MaxStreamsBiDi | FrameType::StreamsBlockedBiDi => Ok(Self::BiDi),
            FrameType::MaxStreamsUniDi | FrameType::StreamsBlockedUniDi => Ok(Self::UniDi),
            _ => Err(Error::FrameEncoding),
        }
    }
}

#[derive(PartialEq, Eq, Debug, PartialOrd, Ord, Clone, Copy)]
pub enum CloseError {
    Transport(TransportError),
    Application(AppError),
}

impl CloseError {
    #[must_use]
    pub const fn code(&self) -> u64 {
        match self {
            Self::Transport(c) | Self::Application(c) => *c,
        }
    }
}

impl From<std::array::TryFromSliceError> for Error {
    fn from(_err: std::array::TryFromSliceError) -> Self {
        Self::FrameEncoding
    }
}

#[derive(PartialEq, Eq, Debug, Default, Clone)]
pub struct AckRange {
    gap: u64,
    range: u64,
}

#[derive(PartialEq, Eq, Debug, Clone)]
pub enum Frame<'a> {
    Padding(u16),
    Ping,
    Ack {
        largest_acknowledged: u64,
        ack_delay: u64,
        first_ack_range: u64,
        ack_ranges: Vec<AckRange>,
        ecn_count: Option<ecn::Count>,
    },
    ResetStream {
        stream_id: StreamId,
        application_error_code: AppError,
        final_size: u64,
    },
    StopSending {
        stream_id: StreamId,
        application_error_code: AppError,
    },
    Crypto {
        offset: u64,
        data: &'a [u8],
    },
    NewToken {
        token: &'a [u8],
    },
    Stream {
        stream_id: StreamId,
        offset: u64,
        data: &'a [u8],
        fin: bool,
        fill: bool,
    },
    MaxData {
        maximum_data: u64,
    },
    MaxStreamData {
        stream_id: StreamId,
        maximum_stream_data: u64,
    },
    MaxStreams {
        stream_type: StreamType,
        maximum_streams: u64,
    },
    DataBlocked {
        data_limit: u64,
    },
    StreamDataBlocked {
        stream_id: StreamId,
        stream_data_limit: u64,
    },
    StreamsBlocked {
        stream_type: StreamType,
        stream_limit: u64,
    },
    NewConnectionId {
        sequence_number: u64,
        retire_prior: u64,
        connection_id: &'a [u8],
        stateless_reset_token: Srt,
    },
    RetireConnectionId {
        sequence_number: u64,
    },
    PathChallenge {
        data: [u8; 8],
    },
    PathResponse {
        data: [u8; 8],
    },
    ConnectionClose {
        error_code: CloseError,
        frame_type: u64,
        reason_phrase: String,
    },
    HandshakeDone,
    AckFrequency {
        /// The current ACK frequency sequence number.
        seqno: u64,
        /// The number of contiguous packets that can be received without
        /// acknowledging immediately.
        tolerance: u64,
        /// The time to delay after receiving the first packet that is
        /// not immediately acknowledged.
        delay: u64,
        /// Ignore reordering when deciding to immediately acknowledge.
        ignore_order: bool,
    },
    Datagram {
        data: &'a [u8],
        fill: bool,
    },
}

impl<'a> Frame<'a> {
    #[must_use]
    pub const fn get_type(&self) -> FrameType {
        match self {
            Self::Padding { .. } => FrameType::Padding,
            Self::Ping => FrameType::Ping,
            Self::Ack { .. } => FrameType::Ack,
            Self::ResetStream { .. } => FrameType::ResetStream,
            Self::StopSending { .. } => FrameType::StopSending,
            Self::Crypto { .. } => FrameType::Crypto,
            Self::NewToken { .. } => FrameType::NewToken,
            Self::Stream {
                fin, offset, fill, ..
            } => Self::stream_type(*fin, *offset > 0, *fill),
            Self::MaxData { .. } => FrameType::MaxData,
            Self::MaxStreamData { .. } => FrameType::MaxStreamData,
            Self::MaxStreams { stream_type, .. } => match stream_type {
                StreamType::BiDi => FrameType::MaxStreamsBiDi,
                StreamType::UniDi => FrameType::MaxStreamsUniDi,
            },
            Self::DataBlocked { .. } => FrameType::DataBlocked,
            Self::StreamDataBlocked { .. } => FrameType::StreamDataBlocked,
            Self::StreamsBlocked { stream_type, .. } => match stream_type {
                StreamType::BiDi => FrameType::StreamsBlockedBiDi,
                StreamType::UniDi => FrameType::StreamsBlockedUniDi,
            },
            Self::NewConnectionId { .. } => FrameType::NewConnectionId,
            Self::RetireConnectionId { .. } => FrameType::RetireConnectionId,
            Self::PathChallenge { .. } => FrameType::PathChallenge,
            Self::PathResponse { .. } => FrameType::PathResponse,
            Self::ConnectionClose { error_code, .. } => match error_code {
                CloseError::Transport(_) => FrameType::ConnectionCloseTransport,
                CloseError::Application(_) => FrameType::ConnectionCloseApplication,
            },
            Self::HandshakeDone => FrameType::HandshakeDone,
            Self::AckFrequency { .. } => FrameType::AckFrequency,
            Self::Datagram { fill, .. } => match fill {
                false => FrameType::Datagram,
                true => FrameType::DatagramWithLen,
            },
        }
    }

    #[must_use]
    pub const fn is_stream(&self) -> bool {
        matches!(
            self,
            Self::ResetStream { .. }
                | Self::StopSending { .. }
                | Self::Stream { .. }
                | Self::MaxData { .. }
                | Self::MaxStreamData { .. }
                | Self::MaxStreams { .. }
                | Self::DataBlocked { .. }
                | Self::StreamDataBlocked { .. }
                | Self::StreamsBlocked { .. }
        )
    }

    #[must_use]
    pub const fn stream_type(fin: bool, nonzero_offset: bool, fill: bool) -> FrameType {
        match (nonzero_offset, fill, fin) {
            (false, true, false) => FrameType::Stream,
            (false, true, true) => FrameType::StreamWithFin,
            (false, false, false) => FrameType::StreamWithLen,
            (false, false, true) => FrameType::StreamWithLenFin,
            (true, true, false) => FrameType::StreamWithOff,
            (true, true, true) => FrameType::StreamWithOffFin,
            (true, false, false) => FrameType::StreamWithOffLen,
            (true, false, true) => FrameType::StreamWithOffLenFin,
        }
    }

    /// If the frame causes a recipient to generate an ACK within its
    /// advertised maximum acknowledgement delay.
    #[must_use]
    pub const fn ack_eliciting(&self) -> bool {
        !matches!(
            self,
            Self::Ack { .. } | Self::Padding { .. } | Self::ConnectionClose { .. }
        )
    }

    /// If the frame can be sent in a path probe
    /// without initiating migration to that path.
    #[must_use]
    pub const fn path_probing(&self) -> bool {
        matches!(
            self,
            Self::Padding { .. }
                | Self::NewConnectionId { .. }
                | Self::PathChallenge { .. }
                | Self::PathResponse { .. }
        )
    }

    /// Converts `AckRanges` as encoded in a ACK frame (see -transport
    /// 19.3.1) into ranges of acked packets (end, start), inclusive of
    /// start and end values.
    ///
    /// # Errors
    ///
    /// Returns an error if the ranges are invalid.
    pub fn decode_ack_frame(
        largest_acked: u64,
        first_ack_range: u64,
        ack_ranges: &[AckRange],
    ) -> Res<Vec<RangeInclusive<u64>>> {
        let mut acked_ranges = Vec::with_capacity(ack_ranges.len() + 1);

        if largest_acked < first_ack_range {
            return Err(Error::FrameEncoding);
        }
        acked_ranges.push((largest_acked - first_ack_range)..=largest_acked);
        if !ack_ranges.is_empty() && largest_acked < first_ack_range + 1 {
            return Err(Error::FrameEncoding);
        }
        let mut cur = if ack_ranges.is_empty() {
            0
        } else {
            largest_acked - first_ack_range - 1
        };
        for r in ack_ranges {
            if cur < r.gap + 1 {
                return Err(Error::FrameEncoding);
            }
            cur = cur - r.gap - 1;

            if cur < r.range {
                return Err(Error::FrameEncoding);
            }
            acked_ranges.push((cur - r.range)..=cur);

            if cur > r.range + 1 {
                cur -= r.range + 1;
            } else {
                cur -= r.range;
            }
        }

        Ok(acked_ranges)
    }

    #[must_use]
    pub fn dump(&self) -> String {
        match self {
            Self::Crypto { offset, data } => {
                format!("Crypto {{ offset: {offset}, len: {} }}", data.len())
            }
            Self::Stream {
                stream_id,
                offset,
                fill,
                data,
                fin,
            } => format!(
                "Stream {{ stream_id: {}, offset: {offset}, len: {}{}, fin: {fin} }}",
                stream_id.as_u64(),
                if *fill { ">>" } else { "" },
                data.len(),
            ),
            Self::Padding(length) => format!("Padding {{ len: {length} }}"),
            Self::Datagram { data, .. } => format!("Datagram {{ len: {} }}", data.len()),
            _ => format!("{self:?}"),
        }
    }

    #[must_use]
    pub fn is_allowed(&self, pt: packet::Type) -> bool {
        match self {
            Self::Padding { .. } | Self::Ping => true,
            Self::Crypto { .. }
            | Self::Ack { .. }
            | Self::ConnectionClose {
                error_code: CloseError::Transport(_),
                ..
            } => pt != packet::Type::ZeroRtt,
            Self::NewToken { .. } | Self::ConnectionClose { .. } => pt == packet::Type::Short,
            _ => pt == packet::Type::ZeroRtt || pt == packet::Type::Short,
        }
    }

    /// # Errors
    ///
    /// Returns an error if the frame cannot be decoded.
    #[expect(
        clippy::too_many_lines,
        reason = "Yeah, but it's a nice match statement."
    )]
    pub fn decode(dec: &mut Decoder<'a>) -> Res<Self> {
        /// Maximum ACK Range Count in ACK Frame
        ///
        /// Given a max UDP datagram size of 64k bytes and a minimum ACK Range size of 2
        /// bytes (2 QUIC varints), a single datagram can at most contain 32k ACK
        /// Ranges.
        ///
        /// Note that the maximum (jumbogram) Ethernet MTU of 9216 or on the
        /// Internet the regular Ethernet MTU of 1518 are more realistically to
        /// be the limiting factor. Though for simplicity the higher limit is chosen.
        const MAX_ACK_RANGE_COUNT: u64 = 32 * 1024;

        fn d<T>(v: Option<T>) -> Res<T> {
            v.ok_or(Error::NoMoreData)
        }
        fn dv(dec: &mut Decoder) -> Res<u64> {
            d(dec.decode_varint())
        }

        fn decode_ack<'a>(dec: &mut Decoder<'a>, ecn: bool) -> Res<Frame<'a>> {
            let la = dv(dec)?;
            let ad = dv(dec)?;
            let nr = dv(dec).and_then(|nr| {
                if nr < MAX_ACK_RANGE_COUNT {
                    Ok(nr)
                } else {
                    Err(Error::TooMuchData)
                }
            })?;
            let fa = dv(dec)?;
            let mut arr: Vec<AckRange> = Vec::with_capacity(usize::try_from(nr)?);
            for _ in 0..nr {
                let ar = AckRange {
                    gap: dv(dec)?,
                    range: dv(dec)?,
                };
                arr.push(ar);
            }

            let ecn_count = ecn
                .then(|| -> Res<ecn::Count> {
                    Ok(ecn::Count::new(0, dv(dec)?, dv(dec)?, dv(dec)?))
                })
                .transpose()?;

            Ok(Frame::Ack {
                largest_acknowledged: la,
                ack_delay: ad,
                first_ack_range: fa,
                ack_ranges: arr,
                ecn_count,
            })
        }

        let pos = dec.offset();
        let t = dv(dec)?;
        if Encoder::varint_len(t) != dec.offset() - pos {
            return Err(Error::ProtocolViolation);
        }

        let t = t.try_into()?;
        match t {
            FrameType::Padding => {
                (1 + dec.skip_while(u8::from(FrameType::Padding)))
                    .try_into()
                    .map(Self::Padding)
                    .map_err(|_| Error::TooMuchData)
            }
            FrameType::Ping => Ok(Self::Ping),
            FrameType::ResetStream => Ok(Self::ResetStream {
                stream_id: StreamId::from(dv(dec)?),
                application_error_code: dv(dec)?,
                final_size: dv(dec)?,
            }),
            FrameType::Ack => decode_ack(dec, false),
            FrameType::AckEcn => decode_ack(dec, true),
            FrameType::StopSending => Ok(Self::StopSending {
                stream_id: StreamId::from(dv(dec)?),
                application_error_code: dv(dec)?,
            }),
            FrameType::Crypto => {
                let offset = dv(dec)?;
                let data = d(dec.decode_vvec())?;
                if offset + u64::try_from(data.len())? > MAX_VARINT {
                    return Err(Error::FrameEncoding);
                }
                Ok(Self::Crypto { offset, data })
            }
            FrameType::NewToken => {
                let token = d(dec.decode_vvec())?;
                if token.is_empty() {
                    return Err(Error::FrameEncoding);
                }
                Ok(Self::NewToken { token })
            }
            FrameType::Stream
            | FrameType::StreamWithFin
            | FrameType::StreamWithLen
            | FrameType::StreamWithLenFin
            | FrameType::StreamWithOff
            | FrameType::StreamWithOffFin
            | FrameType::StreamWithOffLen
            | FrameType::StreamWithOffLenFin => {
                let s = dv(dec)?;
                let o = if t.is_stream_with_offset() {
                    dv(dec)?
                } else {
                    0
                };
                let fill = !t.is_stream_with_length();
                let data = if fill {
                    qtrace!("STREAM frame, extends to the end of the packet");
                    dec.decode_remainder()
                } else {
                    qtrace!("STREAM frame, with length");
                    d(dec.decode_vvec())?
                };
                if o + u64::try_from(data.len())? > MAX_VARINT {
                    return Err(Error::FrameEncoding);
                }
                Ok(Self::Stream {
                    fin: t.is_stream_with_fin(),
                    stream_id: StreamId::from(s),
                    offset: o,
                    data,
                    fill,
                })
            }
            FrameType::MaxData => Ok(Self::MaxData {
                maximum_data: dv(dec)?,
            }),
            FrameType::MaxStreamData => Ok(Self::MaxStreamData {
                stream_id: StreamId::from(dv(dec)?),
                maximum_stream_data: dv(dec)?,
            }),
            FrameType::MaxStreamsBiDi | FrameType::MaxStreamsUniDi => {
                let m = dv(dec)?;
                if m > (1 << 60) {
                    return Err(Error::StreamLimit);
                }
                Ok(Self::MaxStreams {
                    stream_type: t.try_into()?,
                    maximum_streams: m,
                })
            }
            FrameType::DataBlocked => Ok(Self::DataBlocked {
                data_limit: dv(dec)?,
            }),
            FrameType::StreamDataBlocked => Ok(Self::StreamDataBlocked {
                stream_id: dv(dec)?.into(),
                stream_data_limit: dv(dec)?,
            }),
            FrameType::StreamsBlockedBiDi | FrameType::StreamsBlockedUniDi => {
                Ok(Self::StreamsBlocked {
                    stream_type: t.try_into()?,
                    stream_limit: dv(dec)?,
                })
            }
            FrameType::NewConnectionId => {
                let sequence_number = dv(dec)?;
                let retire_prior = dv(dec)?;
                let connection_id = d(dec.decode_vec(1))?;
                if connection_id.len() > ConnectionId::MAX_LEN {
                    return Err(Error::FrameEncoding);
                }
                let stateless_reset_token = Srt::try_from(dec)?;

                Ok(Self::NewConnectionId {
                    sequence_number,
                    retire_prior,
                    connection_id,
                    stateless_reset_token,
                })
            }
            FrameType::RetireConnectionId => Ok(Self::RetireConnectionId {
                sequence_number: dv(dec)?,
            }),
            FrameType::PathChallenge => {
                let data = d(dec.decode(8))?;
                let mut datav: [u8; 8] = [0; 8];
                datav.copy_from_slice(data);
                Ok(Self::PathChallenge { data: datav })
            }
            FrameType::PathResponse => {
                let data = d(dec.decode(8))?;
                let mut datav: [u8; 8] = [0; 8];
                datav.copy_from_slice(data);
                Ok(Self::PathResponse { data: datav })
            }
            FrameType::ConnectionCloseTransport | FrameType::ConnectionCloseApplication => {
                let (error_code, frame_type) = if t == FrameType::ConnectionCloseTransport {
                    (CloseError::Transport(dv(dec)?), dv(dec)?)
                } else {
                    (CloseError::Application(dv(dec)?), 0)
                };
                let reason_phrase = String::from_utf8_lossy(d(dec.decode_vvec())?).into_owned();
                Ok(Self::ConnectionClose {
                    error_code,
                    frame_type,
                    reason_phrase,
                })
            }
            FrameType::HandshakeDone => Ok(Self::HandshakeDone),
            FrameType::AckFrequency => {
                let seqno = dv(dec)?;
                let tolerance = dv(dec)?;
                if tolerance == 0 {
                    return Err(Error::FrameEncoding);
                }
                let delay = dv(dec)?;
                let ignore_order = match d(dec.decode_uint::<u8>())? {
                    0 => false,
                    1 => true,
                    _ => return Err(Error::FrameEncoding),
                };
                Ok(Self::AckFrequency {
                    seqno,
                    tolerance,
                    delay,
                    ignore_order,
                })
            }
            FrameType::Datagram | FrameType::DatagramWithLen => {
                let fill = t == FrameType::Datagram;
                let data = if fill {
                    qtrace!("DATAGRAM frame, extends to the end of the packet");
                    dec.decode_remainder()
                } else {
                    qtrace!("DATAGRAM frame, with length");
                    d(dec.decode_vvec())?
                };
                Ok(Self::Datagram { data, fill })
            }
        }
    }
}

/// Extension trait for [`Encoder`] that automates writing to fuzzing corpus.
pub trait FrameEncoder {
    /// Encode a frame with the given type and encoding closure.
    ///
    /// This method:
    /// 1. Encodes the frame type as a varint
    /// 2. Calls the provided closure to encode the frame-specific data
    /// 3. When fuzzing corpus collection is enabled, saves the frame to the corpus
    ///
    /// # Example
    /// ```ignore
    /// builder.encode_frame(FrameType::NewToken, |b| {
    ///     b.encode_vvec(&token);
    /// });
    /// ```
    fn encode_frame<T, F>(&mut self, frame_type: T, encode_fn: F) -> &mut Self
    where
        T: Into<u64>,
        F: FnOnce(&mut Self);
}

impl<B: Buffer> FrameEncoder for Encoder<B> {
    fn encode_frame<T, F>(&mut self, frame_type: T, encode_fn: F) -> &mut Self
    where
        T: Into<u64>,
        F: FnOnce(&mut Self),
    {
#[cfg(any())]

        let frame_start = self.len();
        self.encode_varint(frame_type.into());
        encode_fn(self);
#[cfg(any())]

        neqo_common::write_item_to_fuzzing_corpus("frame", &self.as_ref()[frame_start..]);
        self
    }
}
