// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{mem, str};

use neqo_common::{qdebug, qerror};
use neqo_transport::{Connection, StreamId};

use crate::{Error, Res, huffman, prefix::Prefix};

pub trait ReadByte {
    /// # Errors
    ///
    /// Return error occurred while reading a byte.
    /// The exact error depends on trait implementation.
    fn read_byte(&mut self) -> Res<u8>;
}

pub trait Reader {
    /// # Errors
    ///
    /// Return error occurred while reading date into a buffer.
    /// The exact error depends on trait implementation.
    fn read(&mut self, buf: &mut [u8]) -> Res<usize>;
}

pub(crate) struct ReceiverConnWrapper<'a> {
    conn: &'a mut Connection,
    stream_id: StreamId,
}

impl ReadByte for ReceiverConnWrapper<'_> {
    fn read_byte(&mut self) -> Res<u8> {
        let mut b = [0];
        match self.conn.stream_recv(self.stream_id, &mut b)? {
            (_, true) => Err(Error::ClosedCriticalStream),
            (0, false) => Err(Error::NeedMoreData),
            _ => Ok(b[0]),
        }
    }
}

impl Reader for ReceiverConnWrapper<'_> {
    fn read(&mut self, buf: &mut [u8]) -> Res<usize> {
        match self.conn.stream_recv(self.stream_id, buf)? {
            (_, true) => Err(Error::ClosedCriticalStream),
            (amount, false) => Ok(amount),
        }
    }
}

impl<'a> ReceiverConnWrapper<'a> {
    pub const fn new(conn: &'a mut Connection, stream_id: StreamId) -> Self {
        Self { conn, stream_id }
    }
}

/// This is only used by header decoder therefore all errors are `Error::Decompression`.
/// A header block is read entirely before decoding it, therefore if there is not enough
/// data in the buffer an error `Error::Decompression` will be return.
pub(crate) struct ReceiverBufferWrapper<'a> {
    buf: &'a [u8],
    offset: usize,
}

impl ReadByte for ReceiverBufferWrapper<'_> {
    fn read_byte(&mut self) -> Res<u8> {
        if self.offset == self.buf.len() {
            Err(Error::Decompression)
        } else {
            let b = self.buf[self.offset];
            self.offset += 1;
            Ok(b)
        }
    }
}

impl<'a> ReceiverBufferWrapper<'a> {
    pub const fn new(buf: &'a [u8]) -> Self {
        Self { buf, offset: 0 }
    }

    pub const fn peek(&self) -> Res<u8> {
        if self.offset == self.buf.len() {
            Err(Error::Decompression)
        } else {
            Ok(self.buf[self.offset])
        }
    }

    pub const fn done(&self) -> bool {
        self.offset == self.buf.len()
    }

    /// The function decodes varint with a prefixed, i.e. ignores `prefix_len` bits of the first
    /// byte.
    /// `ReceiverBufferWrapper` is only used for decoding header blocks. The header blocks are read
    /// entirely before a decoding starts, therefore any incomplete varint because of reaching the
    /// end of a buffer will be treated as the `Error::Decompression` error.
    pub fn read_prefixed_int(&mut self, prefix_len: u8) -> Res<u64> {
        debug_assert!(prefix_len < 8);

        let first_byte = self.read_byte()?;
        let mut reader = IntReader::new(first_byte, prefix_len);
        reader.read(self)
    }

    /// Do not use `LiteralReader` here to avoid copying data.
    /// The function decoded a literal with a prefix:
    ///   1) ignores `prefix_len` bits of the first byte,
    ///   2) reads "huffman bit"
    ///   3) decode varint that is the length of a literal
    ///   4) reads the literal
    ///   5) performs huffman decoding if needed.
    ///
    /// `ReceiverBufferWrapper` is only used for decoding header blocks. The header blocks are read
    /// entirely before a decoding starts, therefore any incomplete varint or literal because of
    /// reaching the end of a buffer will be treated as the `Error::Decompression` error.
    pub fn read_literal_from_buffer(&mut self, prefix_len: u8) -> Res<Vec<u8>> {
        debug_assert!(prefix_len < 7);

        let first_byte = self.read_byte()?;
        let use_huffman = (first_byte & (0x80 >> prefix_len)) != 0;
        let mut int_reader = IntReader::new(first_byte, prefix_len + 1);
        let length: usize = int_reader
            .read(self)?
            .try_into()
            .ok()
            .filter(|&l| l <= LiteralReader::MAX_LEN)
            .ok_or(Error::Decompression)?;
        if use_huffman {
            huffman::decode(self.slice(length)?)
        } else {
            Ok(self.slice(length)?.to_vec())
        }
    }

    fn slice(&mut self, len: usize) -> Res<&[u8]> {
        let end = self.offset.checked_add(len).ok_or(Error::Decompression)?;
        if end > self.buf.len() {
            Err(Error::Decompression)
        } else {
            let start = self.offset;
            self.offset = end;
            Ok(&self.buf[start..self.offset])
        }
    }
}

/// This is varint reader that can take into account a prefix.
#[derive(Debug)]
#[expect(clippy::module_name_repetitions, reason = "This is OK.")]
pub struct IntReader {
    value: u64,
    cnt: u8,
    done: bool,
}

impl IntReader {
    /// `IntReader` is created by supplying the first byte and prefix length.
    /// A varint may take only one byte, In that case already the first by has set state to done.
    ///
    /// # Panics
    ///
    /// When `prefix_len` is 8 or larger.
    #[must_use]
    pub fn new(first_byte: u8, prefix_len: u8) -> Self {
        debug_assert!(prefix_len < 8, "prefix cannot larger than 7");
        let mask = if prefix_len == 0 {
            0xff
        } else {
            (1 << (8 - prefix_len)) - 1
        };
        let value = u64::from(first_byte & mask);

        Self {
            value,
            cnt: 0,
            done: value < u64::from(mask),
        }
    }

    /// # Panics
    ///
    /// Never, but rust doesn't know that.
    #[must_use]
    pub fn make(first_byte: u8, prefixes: &[Prefix]) -> Self {
        for prefix in prefixes {
            if prefix.cmp_prefix(first_byte) {
                return Self::new(first_byte, prefix.len());
            }
        }
        unreachable!();
    }

    /// This function reads bytes until the varint is decoded or until stream/buffer does not
    /// have any more date.
    ///
    /// # Errors
    ///
    /// Possible errors are:
    ///  1) `NeedMoreData` if the reader needs more data,
    ///  2) `IntegerOverflow`,
    ///  3) Any `ReadByte`'s error
    pub fn read<R: ReadByte>(&mut self, s: &mut R) -> Res<u64> {
        let mut b: u8;
        while !self.done {
            b = s.read_byte()?;

            if (self.cnt == 63) && (b > 1 || (b == 1 && ((self.value >> 63) == 1))) {
                qerror!("Error decoding prefixed encoded int - IntegerOverflow");
                return Err(Error::IntegerOverflow);
            }
            self.value += u64::from(b & 0x7f) << self.cnt;
            if (b & 0x80) == 0 {
                self.done = true;
            }
            self.cnt += 7;
            if self.cnt >= 64 {
                self.done = true;
            }
        }
        Ok(self.value)
    }
}

#[derive(Debug, Default)]
enum LiteralReaderState {
    #[default]
    ReadHuffman,
    ReadLength {
        reader: IntReader,
    },
    ReadLiteral {
        offset: usize,
    },
    Done,
}

/// This is decoder of a literal with a prefix:
///   1) ignores `prefix_len` bits of the first byte,
///   2) reads "huffman bit"
///   3) decode varint that is the length of a literal
///   4) reads the literal
///   5) performs huffman decoding if needed.
#[derive(Debug, Default)]
#[expect(clippy::module_name_repetitions, reason = "This is OK.")]
pub struct LiteralReader {
    state: LiteralReaderState,
    literal: Vec<u8>,
    use_huffman: bool,
}

impl LiteralReader {
    /// Maximum length for a literal string in QPACK encoding.
    ///
    /// RFC 9204 requires implementations to set their own limits for string literal
    /// lengths to prevent denial-of-service attacks. The RFC does not mandate a
    /// specific value, stating only that limits "SHOULD be large enough to process
    /// the largest individual field the HTTP implementation can be configured to
    /// accept."
    ///
    /// The Gecko limit is in `network.http.max_response_header_size` and defaults to
    /// 393216 bytes (384 KB), see `modules/libpref/init/StaticPrefList.yaml`. We use
    /// the same limit.
    pub(crate) const MAX_LEN: usize = 384 * 1024;

    /// Creates `LiteralReader` with the first byte. This constructor is always used
    /// when a literal has a prefix.
    /// For literals without a prefix please use the default constructor.
    ///
    /// # Panics
    ///
    /// If `prefix_len` is 8 or more.
    #[must_use]
    pub fn new_with_first_byte(first_byte: u8, prefix_len: u8) -> Self {
        assert!(prefix_len < 8);
        Self {
            state: LiteralReaderState::ReadLength {
                reader: IntReader::new(first_byte, prefix_len + 1),
            },
            literal: Vec::new(),
            use_huffman: (first_byte & (0x80 >> prefix_len)) != 0,
        }
    }

    /// This function reads bytes until the literal is decoded or until stream/buffer does not
    /// have any more date ready.
    ///
    /// # Errors
    ///
    /// Possible errors are:
    ///  1) `NeedMoreData` if the reader needs more data,
    ///  2) `IntegerOverflow`
    ///  3) Any `ReadByte`'s error
    ///
    /// It returns value if reading the literal is done or None if it needs more data.
    ///
    /// # Panics
    ///
    /// When this object is complete.
    pub fn read<T: ReadByte + Reader>(&mut self, s: &mut T) -> Res<Vec<u8>> {
        loop {
            qdebug!("state = {:?}", self.state);
            match &mut self.state {
                LiteralReaderState::ReadHuffman => {
                    let b = s.read_byte()?;

                    self.use_huffman = (b & 0x80) != 0;
                    self.state = LiteralReaderState::ReadLength {
                        reader: IntReader::new(b, 1),
                    };
                }
                LiteralReaderState::ReadLength { reader } => {
                    let v = usize::try_from(reader.read(s)?)
                        .ok()
                        .filter(|&l| l <= Self::MAX_LEN)
                        .ok_or(Error::Decoding)?;
                    self.literal.resize(v, 0x0);
                    self.state = LiteralReaderState::ReadLiteral { offset: 0 };
                }
                LiteralReaderState::ReadLiteral { offset } => {
                    let amount = s.read(&mut self.literal[*offset..])?;
                    *offset += amount;
                    if *offset == self.literal.len() {
                        self.state = LiteralReaderState::Done;
                        if self.use_huffman {
                            break Ok(huffman::decode(&self.literal)?);
                        }
                        break Ok(mem::take(&mut self.literal));
                    }
                    break Err(Error::NeedMoreData);
                }
                LiteralReaderState::Done => {
                    panic!("Should not call read() in this state");
                }
            }
        }
    }
}

/// This is a helper function used only by `ReceiverBufferWrapper`, therefore it returns
/// `Error::Decompression` if any error happens.
///
/// # Errors
///
/// If an parsing error occurred, the function returns `BadUtf8`.
pub fn parse_utf8(v: &[u8]) -> Res<&str> {
    str::from_utf8(v).map_err(|_| Error::BadUtf8)
}
