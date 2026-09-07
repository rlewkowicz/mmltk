// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    fmt::{self, Debug, Formatter, Write},
    io::{self, Cursor},
};

use crate::hex_with_len;

pub const MAX_VARINT: u64 = (1 << 62) - 1;

/// Decoder is a view into a byte array that has a read offset.  Use it for parsing.
pub struct Decoder<'a> {
    buf: &'a [u8],
    offset: usize,
}

impl<'a> Decoder<'a> {
    /// Make a new view of the provided slice.
    #[must_use]
    pub const fn new(buf: &'a [u8]) -> Self {
        Self { buf, offset: 0 }
    }

    /// Get the number of bytes remaining until the end.
    #[must_use]
    pub const fn remaining(&self) -> usize {
        self.buf.len() - self.offset
    }

    /// The number of bytes from the underlying slice that have been decoded.
    #[must_use]
    pub const fn offset(&self) -> usize {
        self.offset
    }

    /// Skip n bytes.
    ///
    /// # Panics
    ///
    /// If the remaining quantity is less than `n`.
    pub fn skip(&mut self, n: usize) {
        assert!(self.remaining() >= n, "insufficient data");
        self.offset += n;
    }

    /// Skip helper that panics if `n` is `None` or not able to fit in `usize`.
    /// Only use this for tests because we panic rather than reporting a result.

    /// Skip a vector.  Panics if there isn't enough space.
    /// Only use this for tests because we panic rather than reporting a result.

    /// Skip a variable length vector.  Panics if there isn't enough space.
    /// Only use this for tests because we panic rather than reporting a result.

    /// Skip while the current byte is `predicate`. Returns the number of bytes
    /// skipped.
    pub fn skip_while(&mut self, predicate: u8) -> usize {
        let until = self
            .as_ref() 
            .iter()
            .position(|v| *v != predicate)
            .unwrap_or_else(|| self.remaining());
        self.skip(until);
        until
    }

    /// Provides the next byte without moving the read position.
    #[must_use]
    pub const fn peek_byte(&self) -> Option<u8> {
        if self.remaining() < 1 {
            None
        } else {
            Some(self.buf[self.offset])
        }
    }

    /// Decodes arbitrary data.
    pub fn decode(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.remaining() < n {
            return None;
        }
        let res = &self.buf[self.offset..self.offset + n];
        self.offset += n;
        Some(res)
    }

    pub(crate) fn decode_n(&mut self, n: usize) -> Option<u64> {
        debug_assert!(n > 0 && n <= 8);
        if self.remaining() < n {
            return None;
        }
        Some(if n == 1 {
            let v = u64::from(self.buf[self.offset]);
            self.offset += 1;
            v
        } else {
            let mut buf = [0; 8];
            buf[8 - n..].copy_from_slice(&self.buf[self.offset..self.offset + n]);
            self.offset += n;
            u64::from_be_bytes(buf)
        })
    }

    /// Decodes a big-endian, unsigned integer value into the target type.
    /// This returns `None` if there is not enough data remaining
    /// or if the conversion to the identified type fails.
    /// Conversion is via `u64`, so failures are impossible for
    /// unsigned integer types: `u8`, `u16`, `u32`, or `u64`.
    /// Signed types will fail if the high bit is set.
    pub fn decode_uint<T: TryFrom<u64>>(&mut self) -> Option<T> {
        let v = self.decode_n(size_of::<T>());
        T::try_from(v?).ok()
    }

    /// Decodes a QUIC varint.
    pub fn decode_varint(&mut self) -> Option<u64> {
        let b1 = self.decode_n(1)?;
        match b1 >> 6 {
            0 => Some(b1),
            1 => Some(((b1 & 0x3f) << 8) | self.decode_n(1)?),
            2 => Some(((b1 & 0x3f) << 24) | self.decode_n(3)?),
            3 => Some(((b1 & 0x3f) << 56) | self.decode_n(7)?),
            _ => unreachable!(),
        }
    }

    /// Decodes the rest of the buffer.  Infallible.
    pub fn decode_remainder(&mut self) -> &'a [u8] {
        let res = &self.buf[self.offset..];
        self.offset = self.buf.len();
        res
    }

    fn decode_checked(&mut self, n: Option<u64>) -> Option<&'a [u8]> {
        if let Ok(l) = usize::try_from(n?) {
            self.decode(l)
        } else {
            self.offset = self.buf.len();
            None
        }
    }

    /// Decodes a TLS-style length-prefixed buffer.
    pub fn decode_vec(&mut self, n: usize) -> Option<&'a [u8]> {
        let len = self.decode_n(n);
        self.decode_checked(len)
    }

    /// Decodes a QUIC varint-length-prefixed buffer.
    pub fn decode_vvec(&mut self) -> Option<&'a [u8]> {
        let len = self.decode_varint();
        self.decode_checked(len)
    }
}

impl<'a> AsRef<[u8]> for Decoder<'a> {
    fn as_ref(&self) -> &'a [u8] {
        &self.buf[self.offset..]
    }
}

impl Debug for Decoder<'_> {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        f.write_str(&hex_with_len(self.as_ref()))
    }
}

impl<'a> From<&'a [u8]> for Decoder<'a> {
    fn from(buf: &'a [u8]) -> Self {
        Decoder::new(buf)
    }
}

impl<'a, T> From<&'a T> for Decoder<'a>
where
    T: AsRef<[u8]>,
{
    fn from(buf: &'a T) -> Self {
        Decoder::new(buf.as_ref())
    }
}

impl<'b> PartialEq<Decoder<'b>> for Decoder<'_> {
    fn eq(&self, other: &Decoder<'b>) -> bool {
        self.buf == other.buf
    }
}

/// Encoder is good for building data structures.
pub struct Encoder<B = Vec<u8>> {
    buf: B,
    /// Tracks the starting position of the buffer when the [`Encoder`] is created.
    /// This allows distinguishing between bytes that existed in the buffer before
    /// encoding began and those written by the [`Encoder`] itself.
    start: usize,
}

impl Clone for Encoder {
    fn clone(&self) -> Self {
        Self {
            buf: self.as_ref().to_vec(),
            start: 0,
        }
    }
}

impl<B: Buffer> PartialEq for Encoder<B> {
    fn eq(&self, other: &Self) -> bool {
        self.as_ref() == other.as_ref()
    }
}

impl<B: Buffer> Eq for Encoder<B> {}

impl<B: Buffer> Encoder<B> {
    /// Get the length of the [`Encoder`].
    ///
    /// Note that the length of the underlying buffer might be larger.
    #[must_use]
    pub fn len(&self) -> usize {
        self.buf.position() - self.start
    }

    /// Returns true if the encoder buffer contains no elements.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Create a view of the current contents of the buffer.
    /// Note: for a view of a slice, use `Decoder::new(&enc[s..e])`
    #[must_use]
    pub fn as_decoder(&self) -> Decoder<'_> {
        Decoder::new(self.as_ref())
    }

    /// Generic encode routine for arbitrary data.
    ///
    /// # Panics
    ///
    /// When writing to the underlying buffer fails.
    pub fn encode<D: AsRef<[u8]>>(&mut self, data: D) -> &mut Self {
        self.buf
            .write_all(data.as_ref())
            .expect("Buffer has enough capacity.");
        self
    }

    /// Encode a single byte.
    ///
    /// # Panics
    ///
    /// When writing to the underlying buffer fails.
    pub fn encode_byte(&mut self, data: u8) -> &mut Self {
        self.buf
            .write_all(&[data])
            .expect("Buffer has enough capacity.");
        self
    }

    /// Encode an integer of any size up to u64.
    ///
    /// # Panics
    ///
    /// When `n` is outside the range `1..=8`.
    pub fn encode_uint<T: Into<u64>>(&mut self, n: usize, v: T) -> &mut Self {
        let v = v.into();
        assert!(n > 0 && n <= 8);
        let bytes = v.to_be_bytes();
        self.encode(&bytes[8 - n..])
    }

    /// Encode a QUIC varint.
    ///
    /// # Panics
    ///
    /// When `v >= 1<<62`.
    pub fn encode_varint<T: Into<u64>>(&mut self, v: T) -> &mut Self {
        let v = v.into();
        #[expect(clippy::cast_possible_truncation, reason = "This is intentional.")]
        match () {
            () if v < (1 << 6) => self.encode_byte(v as u8),
            () if v < (1 << 14) => self.encode((v as u16 | (1 << 14)).to_be_bytes()),
            () if v < (1 << 30) => self.encode((v as u32 | (2 << 30)).to_be_bytes()),
            () if v < (1 << 62) => self.encode((v | (3 << 62)).to_be_bytes()),
            () => panic!("Varint value too large"),
        }
    }

    /// Encode a vector in TLS style.
    ///
    /// # Panics
    ///
    /// When `v` is longer than 2^n.
    pub fn encode_vec(&mut self, n: usize, v: &[u8]) -> &mut Self {
        self.encode_uint(
            n,
            u64::try_from(v.as_ref().len()).expect("v is longer than 2^64"),
        )
        .encode(v)
    }

    /// Encode a vector in TLS style using a closure for the contents.
    ///
    /// # Panics
    ///
    /// When `f()` returns a length larger than `2^8n`.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "AND'ing with 0xff makes this OK."
    )]
    pub fn encode_vec_with<F: FnOnce(&mut Self)>(&mut self, n: usize, f: F) -> &mut Self {
        let start = self.buf.position();
        self.pad_to(n, 0);
        f(self);
        let len = self.buf.position() - start - n;
        assert!(len < (1 << (n * 8)));
        for i in 0..n {
            self.buf
                .write_at(start + i, ((len >> (8 * (n - i - 1))) & 0xff) as u8);
        }
        self
    }

    /// Encode a vector with a varint length.
    ///
    /// # Panics
    ///
    /// When `v` is longer than 2^62.
    pub fn encode_vvec(&mut self, v: &[u8]) -> &mut Self {
        self.encode_varint(u64::try_from(v.as_ref().len()).expect("v is longer than 2^64"))
            .encode(v)
    }

    /// Encode a vector with a varint length using a closure.
    ///
    /// # Panics
    ///
    /// When `f()` writes more than 2^62 bytes.
    pub fn encode_vvec_with<F: FnOnce(&mut Self)>(&mut self, f: F) -> &mut Self {
        let start = self.buf.position();
        self.buf
            .write_all(&[0])
            .expect("Buffer has enough capacity.");
        f(self);
        let len = self.buf.position() - start - 1;


        let v = u64::try_from(len).expect("encoded value fits in a u64");
        self.buf.write_at(start, (v & 0xff) as u8);
        let (count, bits) = match () {
            () if v < (1 << 6) => return self,
            () if v < (1 << 14) => (1, 1 << 6),
            () if v < (1 << 30) => (3, 2 << 22),
            () if v < (1 << 62) => (7, 3 << 54),
            () => panic!("Varint value too large"),
        };
        self.encode_uint(count, (v >> 8) | bits);
        self.buf.rotate_right(start, count);
        self
    }

    /// Truncate the encoder to the given size.
    pub fn truncate(&mut self, len: usize) {
        self.buf.truncate(len + self.start);
    }

    /// Pad the [`Encoder`] to `len` with bytes set to `v`.
    pub fn pad_to(&mut self, len: usize, v: u8) {
        let buffer_len = self.start + len;
        if buffer_len > self.buf.position() {
            self.buf.pad_to(buffer_len, v);
        }
    }
}

impl Encoder<Vec<u8>> {
    /// Skip the first `n` bytes from the encoder buffer without copying.
    /// This advances the internal offset, making those bytes inaccessible.
    ///
    /// # Panics
    ///
    /// Panics if `n` is greater than the current length of the encoder.
    pub fn skip(&mut self, n: usize) {
        assert!(n <= self.len(), "Cannot skip beyond buffer length");
        self.start += n;
    }

    /// Static helper function for previewing the results of encoding without doing it.
    ///
    /// # Panics
    ///
    /// When `v` is too large.
    #[must_use]
    pub const fn varint_len(v: u64) -> usize {
        match () {
            () if v < (1 << 6) => 1,
            () if v < (1 << 14) => 2,
            () if v < (1 << 30) => 4,
            () if v < (1 << 62) => 8,
            () => panic!("Varint value too large"),
        }
    }

    /// Static helper to determine how long a varint-prefixed array encodes to.
    ///
    /// # Panics
    ///
    /// When `len` doesn't fit in a `u64`.
    #[must_use]
    pub fn vvec_len(len: usize) -> usize {
        Self::varint_len(u64::try_from(len).expect("usize should fit into u64")) + len
    }

    /// Construction of a buffer with a predetermined capacity.
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            buf: Vec::with_capacity(capacity),
            start: 0,
        }
    }

}

impl Default for Encoder {
    fn default() -> Self {
        Self {
            buf: Vec::new(),
            start: 0,
        }
    }
}

impl Debug for Encoder {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        f.write_str(&hex_with_len(self))
    }
}

impl<B: Buffer> AsRef<[u8]> for Encoder<B> {
    fn as_ref(&self) -> &[u8] {
        &self.buf.as_slice()[self.start..]
    }
}

impl<B: Buffer> AsMut<[u8]> for Encoder<B> {
    fn as_mut(&mut self) -> &mut [u8] {
        &mut self.buf.as_mut()[self.start..]
    }
}

impl From<&[u8]> for Encoder {
    fn from(buf: &[u8]) -> Self {
        Self {
            buf: Vec::from(buf),
            start: 0,
        }
    }
}

impl From<Encoder> for Vec<u8> {
    fn from(mut enc: Encoder) -> Self {
        if enc.start > 0 {
            enc.buf.drain(..enc.start);
        }
        enc.buf
    }
}

#[expect(
    clippy::unwrap_in_result,
    reason = "successful writing to buffer needs to be guaranteed by caller"
)]
impl<B: io::Write> Write for Encoder<B> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.buf
            .write_all(s.as_bytes())
            .expect("Buffer has enough capacity.");
        Ok(())
    }
}

#[expect(clippy::unnecessary_safety_doc, reason = "relevant for created object")]
impl<'a> Encoder<Cursor<&'a mut [u8]>> {
    /// # Safety
    ///
    /// Any mutable method on [`Encoder<Cursor<&mut [u8]>>`] assumes the
    /// underlying buffer has enough capacity for the called operation. This
    /// invariant needs to be upheld by the caller.
    #[must_use]
    pub const fn new_borrowed_slice(buf: &'a mut [u8]) -> Self {
        Encoder {
            buf: Cursor::new(buf),
            start: 0,
        }
    }
}

impl<'a> Encoder<&'a mut Vec<u8>> {
    #[must_use]
    pub fn new_borrowed_vec(buf: &'a mut Vec<u8>) -> Self {
        Encoder {
            start: buf.position(),
            buf,
        }
    }
}

/// Extends a memory buffer with methods beyond [`std::io::Write`]. Needed for
/// [`Encoder`].
///
/// Note that each method operates on the bytes written, not the entire buffer.
/// E.g. [`Buffer::as_slice`] returns the bytes written, not all bytes of the
/// underlying buffer.
pub trait Buffer: io::Write {
    fn position(&self) -> usize;

    fn is_empty(&self) -> bool {
        self.position() == 0
    }

    fn as_slice(&self) -> &[u8];

    fn as_mut(&mut self) -> &mut [u8];

    fn truncate(&mut self, len: usize);

    fn pad_to(&mut self, n: usize, v: u8);


    fn write_at(&mut self, pos: usize, data: u8);

    fn rotate_right(&mut self, start: usize, count: usize);
}

impl Buffer for Vec<u8> {
    fn position(&self) -> usize {
        self.len()
    }

    fn as_slice(&self) -> &[u8] {
        self.as_ref()
    }

    fn as_mut(&mut self) -> &mut [u8] {
        self.as_mut_slice()
    }

    fn truncate(&mut self, len: usize) {
        Self::truncate(self, len);
    }

    fn pad_to(&mut self, n: usize, v: u8) {
        self.resize(n, v);
    }

    fn write_at(&mut self, pos: usize, data: u8) {
        self[pos] = data;
    }

    fn rotate_right(&mut self, start: usize, count: usize) {
        self[start..].rotate_right(count);
    }
}

impl Buffer for &mut Vec<u8> {
    fn position(&self) -> usize {
        Vec::len(self)
    }

    fn as_slice(&self) -> &[u8] {
        self.as_ref()
    }

    fn as_mut(&mut self) -> &mut [u8] {
        self.as_mut_slice()
    }

    fn truncate(&mut self, len: usize) {
        Vec::truncate(self, len);
    }

    fn pad_to(&mut self, n: usize, v: u8) {
        self.resize(n, v);
    }

    fn write_at(&mut self, pos: usize, data: u8) {
        self[pos] = data;
    }

    fn rotate_right(&mut self, start: usize, count: usize) {
        self[start..].rotate_right(count);
    }
}

impl Buffer for Cursor<&mut [u8]> {
    fn position(&self) -> usize {
        usize::try_from(self.position()).expect("memory allocation not to exceed usize")
    }

    fn as_slice(&self) -> &[u8] {
        &self.get_ref()[..Buffer::position(self)]
    }

    fn as_mut(&mut self) -> &mut [u8] {
        let len = Buffer::position(self);
        &mut self.get_mut()[..len]
    }

    fn truncate(&mut self, len: usize) {
        let old_position = Buffer::position(self);
        if len < old_position {
            self.set_position(u64::try_from(len).expect("Position cannot exceed u64"));
            self.get_mut()[len..old_position].fill(0);
        }
    }

    fn pad_to(&mut self, n: usize, v: u8) {
        let start = usize::try_from(self.position()).expect("Buffer length does not exceed usize");

        self.get_mut()[start..n].fill(v);
        self.set_position(u64::try_from(n).expect("Position cannot exceed u64"));
    }

    fn write_at(&mut self, pos: usize, data: u8) {
        self.get_mut()[pos] = data;
    }

    fn rotate_right(&mut self, start: usize, count: usize) {
        let len = Buffer::position(self);
        self.get_mut()[start..len].rotate_right(count);
    }
}
