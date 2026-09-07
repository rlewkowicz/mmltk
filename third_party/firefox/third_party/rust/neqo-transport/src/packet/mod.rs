// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cmp::min,
    fmt,
    ops::{Deref, DerefMut, Range},
    time::Instant,
};

use enum_map::Enum;
use log::debug;
use neqo_common::{Buffer, Decoder, Encoder, hex, hex_with_len, qtrace, qwarn};
use nss::{Mode, RecordProtectionOps as _, random};
use strum::{EnumIter, FromRepr};

use crate::{
    Error, Res,
    cid::{ConnectionId, ConnectionIdDecoder, ConnectionIdRef},
    crypto::{CryptoDxState, CryptoStates, Epoch},
    frame::{FrameEncoder as _, FrameType},
    scone::Bitrate,
    version::{self, Version},
};

/// `MIN_INITIAL_PACKET_SIZE` is the smallest packet that can be used to establish
/// a new connection across all QUIC versions this server supports.
pub const MIN_INITIAL_PACKET_SIZE: usize = 1200;

pub const BIT_LONG: u8 = 0x80;
const BIT_SHORT: u8 = 0x00;
const BIT_FIXED_QUIC: u8 = 0x40;
const BIT_SPIN: u8 = 0x20;
const BIT_KEY_PHASE: u8 = 0x04;

const HP_MASK_LONG: u8 = 0x0f;
const HP_MASK_SHORT: u8 = 0x1f;

const SAMPLE_SIZE: usize = 16;
const SAMPLE_OFFSET: usize = 4;
const MAX_PACKET_NUMBER_LEN: usize = 4;
/// The length of a long packet length field.
const LONG_PACKET_LENGTH_LEN: usize = 2;

pub mod metadata;
mod retry;

pub use metadata::MetaData;

pub type Number = u64;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Enum, EnumIter, FromRepr, Hash)]
#[repr(u8)]
pub enum Type {
    Initial = 0,
    ZeroRtt = 1,
    Handshake = 2,
    Retry = 3,
    Short,
    OtherVersion,
    VersionNegotiation,
}

impl Type {
    #[must_use]
    fn from_byte(t: u8, v: Version) -> Self {
        Self::from_repr(t.wrapping_sub(u8::from(v == Version::Version2)) & 3)
            .expect("packet type in range")
    }

    #[must_use]
    fn to_byte(self, v: Version) -> u8 {
        assert!(self.is_long(), "is a long header packet type");
        (self as u8 + u8::from(v == Version::Version2)) & 3
    }

    #[must_use]
    pub const fn is_long(self) -> bool {
        matches!(
            self,
            Self::Initial | Self::ZeroRtt | Self::Handshake | Self::Retry
        )
    }
}

impl TryFrom<Type> for Epoch {
    type Error = Error;

    fn try_from(v: Type) -> Res<Self> {
        match v {
            Type::Initial => Ok(Self::Initial),
            Type::ZeroRtt => Ok(Self::ZeroRtt),
            Type::Handshake => Ok(Self::Handshake),
            Type::Short => Ok(Self::ApplicationData),
            _ => Err(Error::InvalidPacket),
        }
    }
}

impl From<Epoch> for Type {
    fn from(cs: Epoch) -> Self {
        match cs {
            Epoch::Initial => Self::Initial,
            Epoch::ZeroRtt => Self::ZeroRtt,
            Epoch::Handshake => Self::Handshake,
            Epoch::ApplicationData => Self::Short,
        }
    }
}

struct BuilderOffsets {
    /// The bits of the first octet that need masking.
    first_byte_mask: u8,
    /// The offset of the length field.
    len: usize,
    /// The location of the packet number field.
    pn: Range<usize>,
}

/// A packet builder that can be used to produce short packets and long packets.
/// This does not produce Retry or Version Negotiation.
pub struct Builder<B> {
    encoder: Encoder<B>,
    pn: Number,
    header: Range<usize>,
    offsets: BuilderOffsets,
    limit: usize,
    /// Whether to pad the packet before construction.
    padding: bool,
}

impl Builder<Vec<u8>> {
    /// The minimum useful frame size.  If space is less than this, we will claim to be full.
    pub const MINIMUM_FRAME_SIZE: usize = 2;

    /// Make a retry packet.
    /// As this is a simple packet, this is just an associated function.
    /// As Retry is odd (it has to be constructed with leading bytes),
    /// this returns a [`Vec<u8>`] rather than building on an encoder.
    ///
    /// # Errors
    ///
    /// This will return an error if AEAD encrypt fails.
    pub fn retry(
        version: Version,
        dcid: &[u8],
        scid: &[u8],
        token: &[u8],
        odcid: &[u8],
    ) -> Res<Vec<u8>> {
        let mut encoder = Encoder::default();
        encoder.encode_vec(1, odcid);
        let start = encoder.len();
        encoder.encode_byte(
            BIT_LONG
                | BIT_FIXED_QUIC
                | (Type::Retry.to_byte(version) << 4)
                | (random::<1>()[0] & 0xf),
        );
        encoder.encode_uint(4, version.wire_version());
        encoder.encode_vec(1, dcid);
        encoder.encode_vec(1, scid);
        debug_assert_ne!(token.len(), 0);
        encoder.encode(token);
        let tag = retry::use_aead(version, Mode::Encrypt, |aead| {
            let mut buf = vec![0; aead.expansion()];
            Ok(aead.encrypt(0, encoder.as_ref(), &[], &mut buf)?.to_vec())
        })?;
        encoder.encode(&tag);
        let mut complete: Vec<u8> = encoder.into();
        Ok(complete.split_off(start))
    }

    /// Make a Version Negotiation packet.
    #[must_use]
    pub fn version_negotiation(
        dcid: &[u8],
        scid: &[u8],
        client_version: u32,
        versions: &[Version],
    ) -> Vec<u8> {
        let mut encoder = Encoder::default();
        let mut grease = random::<4>();
        encoder.encode_byte(BIT_LONG | (grease[3] & 0x7f));
        encoder.encode([0; 4]); 
        encoder.encode_vec(1, dcid);
        encoder.encode_vec(1, scid);

        for v in versions {
            encoder.encode_uint(4, v.wire_version());
        }
        for g in &mut grease[..3] {
            *g = *g & 0xf0 | 0x0a;
        }

        grease[3] = (client_version.wrapping_add(0x10) & 0xf0) as u8 | 0x0a;
        encoder.encode(&grease[..4]);

        Vec::from(encoder)
    }
}

impl<B: Buffer> Builder<B> {
    /// Start building a short header packet.
    ///
    /// This doesn't fail if there isn't enough space; instead it returns a builder that
    /// has no available space left.  This allows the caller to extract the encoder
    /// and any packets that might have been added before as adding a packet header is
    /// only likely to fail if there are other packets already written.
    ///
    /// If, after calling this method, `remaining()` returns 0, then call `abort()` to get
    /// the encoder back.
    pub fn short<A: AsRef<[u8]>>(
        mut encoder: Encoder<B>,
        key_phase: bool,
        dcid: Option<A>,
        limit: usize,
    ) -> Self {
        let mut limit = limit;

        let header_start = encoder.len();
        if limit > encoder.len()
            && 5 + dcid.as_ref().map_or(0, |d| d.as_ref().len()) < limit - encoder.len()
        {
            encoder.encode_byte(BIT_SHORT | BIT_FIXED_QUIC | (u8::from(key_phase) << 2));
            if let Some(dcid) = dcid {
                encoder.encode(dcid.as_ref());
            }
        } else {
            limit = 0;
        }
        Self {
            encoder,
            pn: u64::MAX,
            header: header_start..header_start,
            offsets: BuilderOffsets {
                first_byte_mask: HP_MASK_SHORT,
                pn: 0..0,
                len: 0,
            },
            limit,
            padding: false,
        }
    }

    /// Start building a long header packet.
    /// For an Initial packet you will need to call `initial_token()`,
    /// even if the token is empty.
    ///
    /// See `short()` for more on how to handle this in cases where there is no space.
    pub fn long<A: AsRef<[u8]>, A1: AsRef<[u8]>>(
        mut encoder: Encoder<B>,
        pt: Type,
        version: Version,
        mut dcid: Option<A>,
        mut scid: Option<A1>,
        limit: usize,
    ) -> Self {
        let mut limit = limit;

        let header_start = encoder.len();
        if limit > encoder.len()
            && 11
                + dcid.as_ref().map_or(0, |d| d.as_ref().len())
                + scid.as_ref().map_or(0, |d| d.as_ref().len())
                < limit - encoder.len()
        {
            encoder.encode_byte(BIT_LONG | BIT_FIXED_QUIC | (pt.to_byte(version) << 4));
            encoder.encode_uint(4, version.wire_version());
            encoder.encode_vec(1, dcid.take().as_ref().map_or(&[], AsRef::as_ref));
            encoder.encode_vec(1, scid.take().as_ref().map_or(&[], AsRef::as_ref));
        } else {
            limit = 0;
        }

        Self {
            encoder,
            pn: u64::MAX,
            header: header_start..header_start,
            offsets: BuilderOffsets {
                first_byte_mask: HP_MASK_LONG,
                pn: 0..0,
                len: 0,
            },
            limit,
            padding: false,
        }
    }

    fn is_long(&self) -> bool {
        self.as_ref()[self.header.start] & 0x80 == BIT_LONG
    }

    /// This stores a value that can be used as a limit.  This does not cause
    /// this limit to be enforced until encryption occurs.  Prior to that, it
    /// is only used voluntarily by users of the builder, through `remaining()`.
    pub const fn set_limit(&mut self, limit: usize) {
        self.limit = limit;
    }

    /// Get the current limit.
    #[must_use]
    pub const fn limit(&self) -> usize {
        self.limit
    }

    /// How many bytes remain against the size limit for the builder.
    #[must_use]
    pub fn remaining(&self) -> usize {
        self.limit.saturating_sub(self.len())
    }

    /// Returns true if the packet has no more space for frames.
    #[must_use]
    pub fn is_full(&self) -> bool {
        self.limit < self.len() + Builder::MINIMUM_FRAME_SIZE
    }

    /// Adjust the limit to ensure that no more data is added.
    pub fn mark_full(&mut self) {
        self.limit = self.len();
    }

    /// Mark the packet as needing padding (or not).
    pub const fn enable_padding(&mut self, needs_padding: bool) {
        self.padding = needs_padding;
    }

    /// Maybe pad with "PADDING" frames.
    /// Only does so if padding was needed and this is a short packet.
    /// Returns true if padding was added.
    ///
    /// # Panics
    ///
    /// Cannot happen.
    pub fn pad(&mut self) -> bool {
        if self.padding && !self.is_long() {
            self.encoder.pad_to(self.limit, FrameType::Padding.into());
            true
        } else {
            false
        }
    }

    /// Add unpredictable values for unprotected parts of the packet.
    pub fn scramble(&mut self, quic_bit: bool) {
        debug_assert!(self.len() > self.header.start);
        let mask =
            if quic_bit { BIT_FIXED_QUIC } else { 0 } | if self.is_long() { 0 } else { BIT_SPIN };
        let first = self.header.start;
        self.encoder.as_mut()[first] ^= random::<1>()[0] & mask;
    }

    /// For an Initial packet, encode the token.
    /// If you fail to do this, then you will not get a valid packet.
    pub fn initial_token(&mut self, token: &[u8]) {
        if Encoder::vvec_len(token.len()) < self.remaining() {
            self.encoder.encode_vvec(token);
        } else {
            self.limit = 0;
        }
    }

    /// Add a packet number of the given size.
    /// For a long header packet, this also inserts a dummy length.
    /// The length is filled in after calling `build`.
    /// Does nothing if there isn't 4 bytes available other than render this builder
    /// unusable; if `remaining()` returns 0 at any point, call `abort()`.
    ///
    /// # Panics
    ///
    /// This will panic if the packet number length is too large.
    pub fn pn(&mut self, pn: Number, pn_len: usize) {
        if self.remaining() < MAX_PACKET_NUMBER_LEN {
            self.limit = 0;
            return;
        }

        if self.is_long() {
            if self.remaining() < LONG_PACKET_LENGTH_LEN + MAX_PACKET_NUMBER_LEN {
                self.limit = 0;
                return;
            }

            self.offsets.len = self.encoder.len();
            self.encoder.encode([0; LONG_PACKET_LENGTH_LEN]);
        }

        let pn_len = min(MAX_PACKET_NUMBER_LEN, pn_len);
        debug_assert_ne!(pn_len, 0);
        let pn_offset = self.encoder.len();
        self.encoder.encode_uint(pn_len, pn);
        self.offsets.pn = pn_offset..self.encoder.len();

        self.encoder.as_mut()[self.header.start] |=
            u8::try_from(pn_len - 1).expect("packet number length fits in u8");
        self.header.end = self.encoder.len();
        self.pn = pn;
    }

    #[expect(clippy::cast_possible_truncation, reason = "AND'ing makes this safe.")]
    fn write_len(&mut self, expansion: usize) {
        let len = self.encoder.len() - (self.offsets.len + LONG_PACKET_LENGTH_LEN) + expansion;
        self.encoder.as_mut()[self.offsets.len] = 0x40 | ((len >> 8) & 0x3f) as u8;
        self.encoder.as_mut()[self.offsets.len + 1] = (len & 0xff) as u8;
    }

    fn pad_for_crypto(&mut self, crypto: &CryptoDxState) {

        let crypto_pad = crypto.extra_padding();
        self.encoder.pad_to(
            self.offsets.pn.start + MAX_PACKET_NUMBER_LEN + crypto_pad,
            0,
        );
    }

    /// A lot of frames here are just a collection of varints.
    /// This helper functions writes a frame like that safely, returning `true` if
    /// a frame was written.
    pub fn write_varint_frame(&mut self, values: &[u64]) -> bool {
        let write = self.remaining()
            >= values
                .iter()
                .map(|&v| Encoder::varint_len(v))
                .sum::<usize>();
        if write {
            if let Some((frame_type, rest)) = values.split_first() {
                self.encode_frame(*frame_type, |enc| {
                    for v in rest {
                        enc.encode_varint(*v);
                    }
                });
            }
            debug_assert!(self.len() <= self.limit());
        }
        write
    }

    /// Build the packet and return the encoder.
    ///
    /// # Errors
    ///
    /// This will return an error if the packet is too large.
    pub fn build(mut self, crypto: &mut CryptoDxState) -> Res<Encoder<B>> {
        if self.len() > self.limit {
            qwarn!("Packet contents are more than the limit");
            debug_assert!(
                false,
                "Builder length ({}) is larger than limit ({}).",
                self.len(),
                self.limit
            );
            return Err(Error::Internal);
        }

        self.pad_for_crypto(crypto);
        if self.offsets.len > 0 {
            self.write_len(crypto.expansion());
        }

        qtrace!(
            "Packet build pn={} hdr={} body={}",
            self.pn,
            hex(&self.encoder.as_ref()[self.header.clone()]),
            hex(&self.encoder.as_ref()[self.header.end..])
        );

        let data_end = self.encoder.len();
        self.pad_to(data_end + crypto.expansion(), 0);

        crypto.encrypt(self.pn, self.header.clone(), self.encoder.as_mut())?;
        let sample_start = self.header.end + SAMPLE_OFFSET - self.offsets.pn.len();
        let sample = self.encoder.as_ref()[sample_start..sample_start + SAMPLE_SIZE]
            .try_into()
            .map_err(|_| Error::Internal)?;
        let mask = crypto.compute_mask(sample)?;

        self.encoder.as_mut()[self.header.start] ^= mask[0] & self.offsets.first_byte_mask;
        for (i, j) in (1..=self.offsets.pn.len()).zip(self.offsets.pn) {
            self.encoder.as_mut()[j] ^= mask[i];
        }

        qtrace!("Packet built {}", hex(&self.encoder));
        Ok(self.encoder)
    }

    /// Abort writing of this packet and return the encoder.
    #[must_use]
    pub fn abort(mut self) -> Encoder<B> {
        self.encoder.truncate(self.header.start);
        self.encoder
    }

    /// Work out if nothing was added after the header.
    #[must_use]
    pub fn packet_empty(&self) -> bool {
        self.encoder.len() == self.header.end
    }

    pub fn len(&self) -> usize {
        self.encoder.len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl<B> Deref for Builder<B> {
    type Target = Encoder<B>;

    fn deref(&self) -> &Self::Target {
        &self.encoder
    }
}

impl<B> DerefMut for Builder<B> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.encoder
    }
}

impl<B> From<Builder<B>> for Encoder<B> {
    fn from(v: Builder<B>) -> Self {
        v.encoder
    }
}

/// `Public` holds information from packets that is public only.  This allows for
/// processing of packets prior to decryption.
pub struct Public<'a> {
    /// The packet type.
    packet_type: Type,
    /// The recovered destination connection ID.
    dcid: ConnectionId,
    /// The source connection ID, if this is a long header packet.
    scid: Option<ConnectionId>,
    /// Any token that is included in the packet (Retry always has a token; Initial sometimes
    /// does). This is empty when there is no token.
    token: Vec<u8>,
    /// The size of the header, not including the packet number.
    header_len: usize,
    /// Protocol version, if present in header.
    version: Option<version::Wire>,
    /// A reference to the entire packet, including the header.
    data: &'a mut [u8],
    /// SCONE information, if present.
    scone: Option<Bitrate>,
}

impl<'a> Public<'a> {
    fn opt<T>(v: Option<T>) -> Res<T> {
        v.map_or_else(|| Err(Error::NoMoreData), |v| Ok(v))
    }

    /// Decode the type-specific portions of a long header.
    /// This includes reading the length and the remainder of the packet.
    /// Returns a tuple of any token and the length of the header.
    fn decode_long(
        decoder: &mut Decoder<'a>,
        packet_type: Type,
        version: Version,
    ) -> Res<(&'a [u8], usize)> {
        if packet_type == Type::Retry {
            let header_len = decoder.offset();
            let expansion = retry::expansion(version);
            let token = decoder
                .remaining()
                .checked_sub(expansion)
                .map_or(Err(Error::InvalidPacket), |v| Self::opt(decoder.decode(v)))?;
            if token.is_empty() {
                return Err(Error::InvalidPacket);
            }
            Self::opt(decoder.decode(expansion))?;
            return Ok((token, header_len));
        }
        let token = if packet_type == Type::Initial {
            Self::opt(decoder.decode_vvec())?
        } else {
            &[]
        };
        let len = Self::opt(decoder.decode_varint())?;
        let header_len = decoder.offset();
        let _body = Self::opt(decoder.decode(usize::try_from(len)?))?;
        Ok((token, header_len))
    }

    /// Decode the common parts of a packet.  This provides minimal parsing and validation.
    /// Returns a tuple of a `Public` and a slice with any remainder from the datagram.
    ///
    /// # Errors
    ///
    /// This will return an error if the packet could not be decoded.
    pub fn decode(
        data: &'a mut [u8],
        dcid_decoder: &dyn ConnectionIdDecoder,
    ) -> Res<(Self, &'a mut [u8])> {
        Self::decode_inner(data, dcid_decoder, false)
    }

    /// Like `decode()`, but allow unknown versions.
    ///
    /// # Errors
    ///
    /// This will return an error if the packet could not be decoded.
    pub fn decode_server(
        data: &'a mut [u8],
        dcid_decoder: &dyn ConnectionIdDecoder,
    ) -> Res<(Self, &'a mut [u8])> {
        Self::decode_inner(data, dcid_decoder, true)
    }

    /// Decode the common parts of a packet.  This provides minimal parsing and validation.
    /// Returns a tuple of a `Public` and a slice with any remainder from the datagram.
    ///
    /// # Errors
    ///
    /// This will return an error if the packet could not be decoded.
    fn decode_inner(
        mut data: &'a mut [u8],
        dcid_decoder: &dyn ConnectionIdDecoder,
        accept_other_version: bool,
    ) -> Res<(Self, &'a mut [u8])> {
        let mut scone: Option<Bitrate> = None;
        loop {
            let mut decoder = Decoder::new(data);
            let first = Self::opt(decoder.decode_uint::<u8>())?;

            if first & 0x80 == BIT_SHORT {
                let dcid = Self::opt(dcid_decoder.decode_cid(&mut decoder))?.into();
                if decoder.remaining() < SAMPLE_OFFSET + SAMPLE_SIZE {
                    return Err(Error::InvalidPacket);
                }
                let header_len = decoder.offset();
                return Ok((
                    Self {
                        packet_type: Type::Short,
                        dcid,
                        scid: None,
                        token: Vec::new(),
                        header_len,
                        version: None,
                        data,
                        scone,
                    },
                    &mut [],
                ));
            }

            let version = Self::opt(decoder.decode_uint())?;
            let dcid = ConnectionIdRef::from(Self::opt(decoder.decode_vec(1))?);
            let scid = ConnectionIdRef::from(Self::opt(decoder.decode_vec(1))?);

            match version {
                0 => {
                    return Ok((
                        Self {
                            packet_type: Type::VersionNegotiation,
                            dcid: ConnectionId::from(dcid),
                            scid: Some(ConnectionId::from(scid)),
                            token: Vec::new(),
                            header_len: decoder.offset(),
                            version: None,
                            data,
                            scone,
                        },
                        &mut [],
                    ));
                }
                Version::SCONE1 | Version::SCONE2 => {
                    if scone.is_some() {
                        return Err(Error::InvalidPacket);
                    }
                    let indication = Bitrate::from((first, version));
                    debug!("Received SCONE indication {indication:x?}");
                    scone = Some(indication);
                    let (_scone, remainder) = data.split_at_mut(decoder.offset());
                    data = remainder;
                    continue;
                }
                _ => {}
            }

            let Ok(version) = Version::try_from(version) else {
                return if accept_other_version {
                    Ok((
                        Self {
                            packet_type: Type::OtherVersion,
                            dcid: ConnectionId::from(dcid),
                            scid: Some(ConnectionId::from(scid)),
                            token: Vec::new(),
                            header_len: decoder.offset(),
                            version: Some(version),
                            data,
                            scone,
                        },
                        &mut [],
                    ))
                } else {
                    Err(Error::InvalidPacket)
                };
            };

            if dcid.len() > ConnectionId::MAX_LEN || scid.len() > ConnectionId::MAX_LEN {
                return Err(Error::InvalidPacket);
            }
            let packet_type = Type::from_byte((first >> 4) & 3, version);

            let (token, header_len) = Public::decode_long(&mut decoder, packet_type, version)?;
            let token = token.to_vec();
            let dcid = ConnectionId::from(dcid);
            let scid = Some(ConnectionId::from(scid));
            let (data, remainder) = data.split_at_mut(decoder.offset());
            return Ok((
                Self {
                    packet_type,
                    dcid,
                    scid,
                    token,
                    header_len,
                    version: Some(version.wire_version()),
                    data,
                    scone,
                },
                remainder,
            ));
        }
    }

    /// Validate the given packet as though it were a retry.
    #[must_use]
    pub fn is_valid_retry(&self, odcid: &ConnectionId) -> bool {
        if self.packet_type != Type::Retry {
            return false;
        }
        let Some(version) = self.version() else {
            return false;
        };
        let expansion = retry::expansion(version);
        if self.data.len() <= expansion {
            return false;
        }
        let (header, tag) = self.data.split_at(self.data.len() - expansion);
        let mut encoder = Encoder::with_capacity(self.data.len());
        encoder.encode_vec(1, odcid);
        encoder.encode(header);
        retry::use_aead(version, Mode::Decrypt, |aead| {
            let mut buf = vec![0; expansion];
            Ok(aead.decrypt(0, encoder.as_ref(), tag, &mut buf)?.is_empty())
        })
        .unwrap_or(false)
    }

    #[must_use]
    pub fn is_valid_initial(&self) -> bool {
        self.packet_type == Type::Initial && (self.dcid().len() >= 8 || !self.token.is_empty())
    }

    #[must_use]
    pub const fn packet_type(&self) -> Type {
        self.packet_type
    }

    #[must_use]
    pub fn dcid(&self) -> ConnectionIdRef<'_> {
        self.dcid.as_cid_ref()
    }

    /// # Panics
    ///
    /// This will panic if called for a short header packet.
    #[must_use]
    pub fn scid(&self) -> ConnectionIdRef<'_> {
        self.scid
            .as_ref()
            .expect("should only be called for long header packets")
            .as_cid_ref()
    }

    #[must_use]
    pub fn token(&self) -> &[u8] {
        &self.token
    }

    #[must_use]
    pub fn version(&self) -> Option<Version> {
        Version::try_from(self.version?).ok()
    }

    #[must_use]
    pub fn wire_version(&self) -> version::Wire {
        debug_assert!(self.version.is_some());
        self.version.unwrap_or(0)
    }

    #[allow(
        clippy::allow_attributes,
        clippy::len_without_is_empty,
        reason = "Is OK here."
    )]
    #[must_use]
    pub const fn len(&self) -> usize {
        self.data.len()
    }

#[cfg(any())]

    #[must_use]
    pub const fn data(&self) -> &[u8] {
        self.data
    }

    const fn decode_pn(expected: Number, pn: u64, w: usize) -> Number {
        let window = 1_u64 << (w * 8);
        let candidate = (expected & !(window - 1)) | pn;
        if candidate + (window / 2) <= expected {
            candidate + window
        } else if candidate > expected + (window / 2) {
            match candidate.checked_sub(window) {
                Some(pn_sub) => pn_sub,
                None => candidate,
            }
        } else {
            candidate
        }
    }

    /// Decrypt the header of the packet.
    fn decrypt_header(&mut self, crypto: &CryptoDxState) -> Res<(bool, Number, Range<usize>)> {
        debug_assert_ne!(self.packet_type, Type::Retry);
        debug_assert_ne!(self.packet_type, Type::VersionNegotiation);

        let sample_offset = self.header_len + SAMPLE_OFFSET;
        let sample = self
            .data
            .get(sample_offset..(sample_offset + SAMPLE_SIZE))
            .ok_or(Error::NoMoreData)?;
        let sample: &[u8; SAMPLE_SIZE] = sample.try_into()?;
        qtrace!(
            "{:?} unmask hdr={}",
            crypto.version(),
            hex(&self.data[..sample_offset])
        );
        let mask = crypto.compute_mask(sample)?;

        let bits = if self.packet_type == Type::Short {
            HP_MASK_SHORT
        } else {
            HP_MASK_LONG
        };
        assert!(!self.data.is_empty());
        let first_byte = self.data[0] ^ (mask[0] & bits);

        let mut hdrbytes = 0..self.header_len + 4;
        self.data[0] = first_byte;

        let mut pn_encoded: u64 = 0;
        let mut pn_bytes =
            self.data[self.header_len..self.header_len + MAX_PACKET_NUMBER_LEN].to_vec();
        for i in 0..MAX_PACKET_NUMBER_LEN {
            pn_bytes[i] ^= mask[1 + i];
            pn_encoded <<= 8;
            pn_encoded += u64::from(pn_bytes[i]);
        }
        let pn_len = usize::from((first_byte & 0x3) + 1);
        self.data[self.header_len..self.header_len + pn_len].copy_from_slice(&pn_bytes[..pn_len]);
        hdrbytes.end = self.header_len + pn_len;
        pn_encoded >>= 8 * (MAX_PACKET_NUMBER_LEN - pn_len);

        qtrace!("unmasked hdr={}", hex(&self.data[hdrbytes.clone()]));

        let key_phase =
            self.packet_type == Type::Short && (first_byte & BIT_KEY_PHASE) == BIT_KEY_PHASE;
        let pn = Self::decode_pn(crypto.next_pn(), pn_encoded, pn_len);
        Ok((key_phase, pn, hdrbytes))
    }

    /// # Errors
    ///
    /// This will return an error if the packet cannot be decrypted.
    pub fn decrypt(
        mut self,
        crypto: &mut CryptoStates,
        release_at: Instant,
    ) -> Result<Decrypted<'a>, DecryptionError<'a>> {
        let epoch = match self.packet_type.try_into() {
            Ok(e) => e,
            Err(e) => return Err((self, e).into()),
        };
        let version = self.version().unwrap_or_default();
        let Some(rx) = crypto.rx_hp(version, epoch) else {
            if crypto.rx_pending(epoch) {
                return Err((self, Error::KeysPending(epoch)).into());
            }
            qtrace!("keys for {epoch:?} already discarded");
            return Err((self, Error::KeysDiscarded(epoch)).into());
        };
        let (key_phase, pn, header) = match self.decrypt_header(rx) {
            Ok(v) => v,
            Err(e) => return Err((self, e).into()),
        };
        let Some(rx) = crypto.rx(version, epoch, key_phase) else {
            return Err((self, Error::Decrypt).into());
        };
        let version = rx.version(); 
        let header_end = header.end;
        let payload_len = match rx.decrypt(pn, header, self.data) {
            Ok(v) => v,
            Err(e) => return Err((self, e).into()),
        };
        let data = &self.data[header_end..header_end + payload_len];
        let make_err = |error| DecryptionError {
            error,
            data: self.data,
            dcid: self.dcid.clone(),
            packet_type: self.packet_type,
        };
        if rx.needs_update() {
            crypto.key_update_received(release_at).map_err(make_err)?;
        }
        crypto.check_pn_overlap().map_err(make_err)?;
        Ok(Decrypted {
            version,
            pt: self.packet_type,
            pn,
            dcid: self.dcid,
            scid: self.scid,
            data,
            scone: self.scone,
        })
    }

    /// # Errors
    ///
    /// This will return an error if the packet is not a version negotiation packet
    /// or if the versions cannot be decoded.
    pub fn supported_versions(&self) -> Res<Vec<version::Wire>> {
        if self.packet_type != Type::VersionNegotiation {
            return Err(Error::InvalidPacket);
        }
        let mut decoder = Decoder::new(&self.data[self.header_len..]);
        let mut res = Vec::new();
        while decoder.remaining() > 0 {
            let version = Self::opt(decoder.decode_uint::<version::Wire>())?;
            res.push(version);
        }
        Ok(res)
    }
}

impl fmt::Debug for Public<'_> {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(
            f,
            "{:?}: {} {}",
            self.packet_type(),
            hex_with_len(&self.data[..self.header_len]),
            hex_with_len(&self.data[self.header_len..])
        )
    }
}

/// Error information from a failed decryption attempt.
/// Contains minimal packet information needed for error handling.
#[derive(Debug)]
pub struct DecryptionError<'a> {
    /// The error that occurred.
    pub error: Error,
    /// The original packet data (unchanged since decryption failed).
    pub data: &'a [u8],
    /// The destination connection ID.
    pub dcid: ConnectionId,
    /// The packet type.
    pub packet_type: Type,
}

impl<'a> From<(Public<'a>, Error)> for DecryptionError<'a> {
    fn from((packet, error): (Public<'a>, Error)) -> Self {
        Self {
            error,
            data: packet.data,
            dcid: packet.dcid,
            packet_type: packet.packet_type,
        }
    }
}

impl DecryptionError<'_> {
    #[must_use]
    pub const fn len(&self) -> usize {
        self.data.len()
    }

    // triggers the `clippy::len_without_is_empty` lint without this.
#[cfg(feature = "bench")]
#[must_use]
    pub const fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    #[must_use]
    pub const fn packet_type(&self) -> Type {
        self.packet_type
    }
}

pub struct Decrypted<'a> {
    version: Version,
    pt: Type,
    pn: Number,
    data: &'a [u8],
    dcid: ConnectionId,
    scid: Option<ConnectionId>,
    scone: Option<Bitrate>,
}

impl Decrypted<'_> {
    #[must_use]
    pub const fn version(&self) -> Version {
        self.version
    }

    #[must_use]
    pub const fn packet_type(&self) -> Type {
        self.pt
    }

    #[must_use]
    pub const fn pn(&self) -> Number {
        self.pn
    }

    #[must_use]
    pub fn dcid(&self) -> ConnectionIdRef<'_> {
        self.dcid.as_cid_ref()
    }

    /// # Panics
    ///
    /// This will panic if called for a short header packet.
    #[must_use]
    pub fn scid(&self) -> ConnectionIdRef<'_> {
        self.scid
            .as_ref()
            .expect("should only be called for long header packets")
            .as_cid_ref()
    }

    #[must_use]
    pub const fn scone(&self) -> Option<Bitrate> {
        self.scone
    }
}

impl Deref for Decrypted<'_> {
    type Target = [u8];

    fn deref(&self) -> &Self::Target {
        self.data
    }
}
