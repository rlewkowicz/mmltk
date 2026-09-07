// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    cmp::min,
    collections::VecDeque,
    fmt::{self, Display, Formatter},
    time::{Duration, Instant},
};

use enum_map::{Enum, EnumMap};
use enumset::{EnumSet, EnumSetType};
use log::{Level, log_enabled};
use neqo_common::{Buffer, Ecn, MAX_VARINT, qdebug, qtrace, qwarn};
use nss::Epoch;
use smallvec::SmallVec;
use strum::{Display, EnumIter};

use crate::{
    Error, Res, Stats, ecn,
    frame::{FrameEncoder as _, FrameType},
    packet,
    recovery::{self},
    stats::FrameStats,
};

#[derive(Debug, PartialOrd, Ord, EnumSetType, Enum, EnumIter, Display)]
pub enum PacketNumberSpace {
    #[strum(to_string = "in")]
    Initial,
    #[strum(to_string = "hs")]
    Handshake,
    #[strum(to_string = "ap")]
    ApplicationData,
}

impl From<Epoch> for PacketNumberSpace {
    fn from(epoch: Epoch) -> Self {
        match epoch {
            Epoch::Initial => Self::Initial,
            Epoch::Handshake => Self::Handshake,
            Epoch::ApplicationData | Epoch::ZeroRtt => Self::ApplicationData,
        }
    }
}

impl From<PacketNumberSpace> for Epoch {
    fn from(val: PacketNumberSpace) -> Self {
        match val {
            PacketNumberSpace::Initial => Self::Initial,
            PacketNumberSpace::Handshake => Self::Handshake,
            PacketNumberSpace::ApplicationData => Self::ApplicationData,
        }
    }
}

#[expect(clippy::fallible_impl_from, reason = "OK here.")]
impl From<packet::Type> for PacketNumberSpace {
    fn from(pt: packet::Type) -> Self {
        match pt {
            packet::Type::Initial => Self::Initial,
            packet::Type::Handshake => Self::Handshake,
            packet::Type::ZeroRtt | packet::Type::Short => Self::ApplicationData,
            _ => panic!("Attempted to get space from wrong packet type"),
        }
    }
}

pub type PacketNumberSpaceSet = EnumSet<PacketNumberSpace>;

/// `InsertionResult` tracks whether something was inserted for `PacketRange::add()`.
pub enum InsertionResult {
    Largest,
    Smallest,
    NotInserted,
}

#[derive(Clone, Debug, Default)]
pub struct PacketRange {
    largest: packet::Number,
    smallest: packet::Number,
    ack_needed: bool,
}

impl PacketRange {
    /// Make a single packet range.
    pub const fn new(pn: packet::Number) -> Self {
        Self {
            largest: pn,
            smallest: pn,
            ack_needed: true,
        }
    }

    /// Get the number of acknowledged packets in the range.
    pub const fn len(&self) -> u64 {
        self.largest - self.smallest + 1
    }

    /// Returns whether this needs to be sent.
    pub const fn ack_needed(&self) -> bool {
        self.ack_needed
    }

    /// Return whether the given number is in the range.
    pub const fn contains(&self, pn: packet::Number) -> bool {
        (pn >= self.smallest) && (pn <= self.largest)
    }

    /// Maybe add a packet number to the range.  Returns true if it was added
    /// at the small end (which indicates that this might need merging with a
    /// preceding range).
    pub fn add(&mut self, pn: packet::Number) -> InsertionResult {
        assert!(!self.contains(pn));
        if (self.largest + 1) == pn {
            qtrace!("[{self}] Adding largest {pn}");
            self.largest += 1;
            self.ack_needed = true;
            InsertionResult::Largest
        } else if self.smallest == (pn + 1) {
            qtrace!("[{self}] Adding smallest {pn}");
            self.smallest -= 1;
            self.ack_needed = true;
            InsertionResult::Smallest
        } else {
            InsertionResult::NotInserted
        }
    }

    /// Maybe merge a higher-numbered range into this.
    fn merge_larger(&mut self, other: &Self) {
        qdebug!("[{self}] Merging {other}");
        assert_eq!(self.largest + 1, other.smallest);

        self.largest = other.largest;
        self.ack_needed = self.ack_needed || other.ack_needed;
    }

    /// When a packet containing the range `other` is acknowledged,
    /// clear the `ack_needed` attribute on this.
    /// Requires that other is equal to this, or a larger range.
    pub const fn acknowledged(&mut self, other: &Self) {
        if (other.smallest <= self.smallest) && (other.largest >= self.largest) {
            self.ack_needed = false;
        }
    }
}

impl Display for PacketRange {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "{}->{}", self.largest, self.smallest)
    }
}

/// The default maximum ACK delay we use locally and advertise to the remote.
pub const DEFAULT_LOCAL_ACK_DELAY: Duration = Duration::from_millis(20);
/// The default maximum ACK delay we assume the remote uses.
///
/// > If this value is absent, a default of 25 milliseconds is assumed.
///
/// <https://datatracker.ietf.org/doc/html/rfc9000#section-18.2>
pub const DEFAULT_REMOTE_ACK_DELAY: Duration = Duration::from_millis(25);
/// The default number of in-order packets we will receive after
/// largest acknowledged without sending an immediate acknowledgment.
pub const DEFAULT_ACK_PACKET_TOLERANCE: packet::Number = 1;
const MAX_TRACKED_RANGES: usize = 32;
const MAX_ACKS_PER_FRAME: usize = 32;

/// A structure that tracks what was included in an ACK.
#[derive(Debug, Clone)]
pub struct AckToken {
    space: PacketNumberSpace,
    ranges: Box<[PacketRange]>,
}

impl AckToken {
    /// Get the space for this token.
    pub const fn space(&self) -> PacketNumberSpace {
        self.space
    }
}

/// A structure that tracks what packets have been received,
/// and what needs acknowledgement for a packet number space.
#[derive(Debug)]
pub struct RecvdPackets {
    space: PacketNumberSpace,
    ranges: VecDeque<PacketRange>,
    /// The packet number of the lowest number packet that we are tracking.
    min_tracked: packet::Number,
    /// The time we got the largest acknowledged.
    largest_pn_time: Option<Instant>,
    /// The time that we should be sending an ACK.
    ack_time: Option<Instant>,
    /// The time we last sent an ACK.
    last_ack_time: Option<Instant>,
    /// The current ACK frequency sequence number.
    ack_frequency_seqno: u64,
    /// The time to delay after receiving the first packet that is
    /// not immediately acknowledged.
    ack_delay: Duration,
    /// The number of ack-eliciting packets that have been received, but
    /// not acknowledged.
    unacknowledged_count: packet::Number,
    /// The number of contiguous packets that can be received without
    /// acknowledging immediately.
    unacknowledged_tolerance: packet::Number,
    /// Whether we are ignoring packets that arrive out of order
    /// for the purposes of generating immediate acknowledgment.
    ignore_order: bool,
    ecn_count: ecn::Count,
}

impl RecvdPackets {
    /// Make a new `RecvdPackets` for the indicated packet number space.
    pub fn new(space: PacketNumberSpace) -> Self {
        Self {
            space,
            ranges: VecDeque::new(),
            min_tracked: 0,
            largest_pn_time: None,
            ack_time: None,
            last_ack_time: None,
            ack_frequency_seqno: 0,
            ack_delay: DEFAULT_LOCAL_ACK_DELAY,
            unacknowledged_count: 0,
            unacknowledged_tolerance: if space == PacketNumberSpace::ApplicationData {
                DEFAULT_ACK_PACKET_TOLERANCE
            } else {
                0
            },
            ignore_order: false,
            ecn_count: ecn::Count::default(),
        }
    }

    /// Get the ECN counts.
    pub const fn ecn_marks(&mut self) -> &mut ecn::Count {
        &mut self.ecn_count
    }

    /// Get the time at which the next ACK should be sent.
    pub const fn ack_time(&self) -> Option<Instant> {
        self.ack_time
    }

    /// Update acknowledgment delay parameters.
    pub const fn ack_freq(
        &mut self,
        seqno: u64,
        tolerance: packet::Number,
        delay: Duration,
        ignore_order: bool,
    ) {
        if seqno >= self.ack_frequency_seqno {
            self.ack_frequency_seqno = seqno;
            self.unacknowledged_tolerance = tolerance;
            self.ack_delay = delay;
            self.ignore_order = ignore_order;
        }
    }

    /// Returns true if an ACK frame should be sent now.
    fn ack_now(&self, now: Instant, rtt: Duration) -> bool {
        self.ack_time.is_some_and(|next| {
            next <= now || self.last_ack_time.is_some_and(|last| last + rtt <= now)
        })
    }

    fn add(&mut self, pn: packet::Number) -> Res<()> {
        for i in 0..self.ranges.len() {
            match self.ranges[i].add(pn) {
                InsertionResult::Largest => return Ok(()),
                InsertionResult::Smallest => {
                    let nxt = i + 1;
                    if (nxt < self.ranges.len()) && (pn - 1 == self.ranges[nxt].largest) {
                        let larger = self.ranges.remove(i).ok_or(Error::Internal)?;
                        self.ranges[i].merge_larger(&larger);
                    }
                    return Ok(());
                }
                InsertionResult::NotInserted => {
                    if self.ranges[i].largest < pn {
                        self.ranges.insert(i, PacketRange::new(pn));
                        return Ok(());
                    }
                }
            }
        }
        self.ranges.push_back(PacketRange::new(pn));
        Ok(())
    }

    fn trim_ranges(&mut self, stats: &mut Stats) -> Res<()> {
        if self.ranges.len() > MAX_TRACKED_RANGES {
            let oldest = self.ranges.pop_back().ok_or(Error::Internal)?;
            if oldest.ack_needed {
                qwarn!("[{self}] Dropping unacknowledged ACK range: {oldest}");
                stats.unacked_range_dropped += 1;
            } else {
                qdebug!("[{self}] Drop ACK range: {oldest}");
            }
            self.min_tracked = oldest.largest + 1;
        }
        Ok(())
    }

    /// Add the packet to the tracked set.
    /// Return true if the packet was the largest received so far.
    pub fn set_received(
        &mut self,
        now: Instant,
        pn: packet::Number,
        ack_eliciting: bool,
        stats: &mut Stats,
    ) -> Res<bool> {
        let next_in_order_pn = self.ranges.front().map_or(0, |r| r.largest + 1);
        qtrace!("[{self}] received {pn}, next: {next_in_order_pn}");

        self.add(pn)?;
        self.trim_ranges(stats)?;

        let largest = if pn >= next_in_order_pn {
            self.largest_pn_time = Some(now);
            true
        } else {
            false
        };

        if ack_eliciting {
            self.unacknowledged_count += 1;

            let immediate_ack = self.space != PacketNumberSpace::ApplicationData
                || (pn != next_in_order_pn && !self.ignore_order)
                || self.unacknowledged_count > self.unacknowledged_tolerance;

            let ack_time = if immediate_ack {
                now
            } else {
                self.ack_time.unwrap_or_else(|| now + self.ack_delay)
            };
            qdebug!("[{self}] Set ACK timer to {ack_time:?}");
            self.ack_time = Some(ack_time);
        }
        Ok(largest)
    }

    /// If we just received a PING frame, we should immediately acknowledge.
    pub fn immediate_ack(&mut self, now: Instant) {
        self.ack_time = Some(now);
        qdebug!("[{self}] immediate_ack at {now:?}");
    }

    /// Check if the packet is a duplicate.
    pub fn is_duplicate(&self, pn: packet::Number) -> bool {
        if pn < self.min_tracked {
            return true;
        }
        self.ranges
            .iter()
            .take_while(|r| pn <= r.largest)
            .any(|r| r.contains(pn))
    }

    /// Mark the given range as having been acknowledged.
    pub fn acknowledged(&mut self, acked: &[PacketRange]) {
        let mut range_iter = self.ranges.iter_mut();
        let mut cur = range_iter.next().expect("should have at least one range");
        for ack in acked {
            while cur.smallest > ack.largest {
                let Some(next) = range_iter.next() else {
                    return;
                };
                cur = next;
            }
            cur.acknowledged(ack);
        }
    }

    /// Length of the worst possible ACK frame, assuming only one range and ECN counts.
    /// Note that this assumes one byte for the type and count of extra ranges.
    pub const USEFUL_ACK_LEN: usize = 1 + 8 + 8 + 1 + 8 + 3 * 8;

    /// Generate an ACK frame for this packet number space.
    ///
    /// Unlike other frame generators this doesn't modify the underlying instance
    /// to track what has been sent. This only clears the delayed ACK timer.
    ///
    /// When sending ACKs, we want to always send the most recent ranges,
    /// even if they have been sent in other packets.
    ///
    /// We don't send ranges that have been acknowledged, but they still need
    /// to be tracked so that duplicates can be detected.
    fn write_frame<B: Buffer>(
        &mut self,
        now: Instant,
        rtt: Duration,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if !self.ack_now(now, rtt) {
            return;
        }

        let max_ranges = if let Some(avail) = builder.remaining().checked_sub(Self::USEFUL_ACK_LEN)
        {
            min(1 + (avail / 16), MAX_ACKS_PER_FRAME)
        } else {
            return;
        };

        let ranges = self
            .ranges
            .iter()
            .filter(|r| r.ack_needed())
            .take(max_ranges)
            .cloned()
            .collect::<SmallVec<[_; MAX_TRACKED_RANGES]>>();
        if ranges.is_empty() {
            return;
        }

        let mut iter = ranges.iter();
        let Some(first) = iter.next() else { return };
        stats.largest_acknowledged = first.largest;
        stats.ack += 1;

        let Some(largest_pn_time) = self.largest_pn_time else {
            return;
        };
        let elapsed = now.duration_since(largest_pn_time);
        let ack_delay = u64::try_from(elapsed.as_micros() / 8).unwrap_or(u64::MAX);
        let ack_delay = min(MAX_VARINT, ack_delay);
        let Ok(extra_ranges) = u64::try_from(ranges.len() - 1) else {
            return;
        };

        builder.encode_frame(
            if self.ecn_count.is_some() {
                FrameType::AckEcn
            } else {
                FrameType::Ack
            },
            |b| {
                b.encode_varint(first.largest);
                b.encode_varint(ack_delay);
                b.encode_varint(extra_ranges); 
                b.encode_varint(first.len() - 1); 

                let mut last = first.smallest;
                for r in iter {
                    b.encode_varint(last - r.largest - 2); 
                    b.encode_varint(r.len() - 1); 
                    last = r.smallest;
                }

                if self.ecn_count.is_some() {
                    b.encode_varint(self.ecn_count[Ecn::Ect0]);
                    b.encode_varint(self.ecn_count[Ecn::Ect1]);
                    b.encode_varint(self.ecn_count[Ecn::Ce]);
                }
            },
        );

        self.ack_time = None;
        self.last_ack_time = Some(now);
        self.unacknowledged_count = 0;

        tokens.push(recovery::Token::Ack(AckToken {
            space: self.space,
            ranges: ranges.into_boxed_slice(),
        }));
    }
}

impl Display for RecvdPackets {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "Recvd-{}", self.space)
    }
}

pub struct AckTracker {
    spaces: EnumMap<PacketNumberSpace, Option<RecvdPackets>>,
}

impl AckTracker {
    pub fn drop_space(&mut self, space: PacketNumberSpace) {
        assert_ne!(
            space,
            PacketNumberSpace::ApplicationData,
            "discarding application space"
        );
        if space == PacketNumberSpace::Handshake {
            assert!(self.spaces[PacketNumberSpace::Initial].is_none());
        }
        self.spaces[space].take();
    }

    pub fn get_mut(&mut self, space: PacketNumberSpace) -> Option<&mut RecvdPackets> {
        self.spaces[space].as_mut()
    }

    pub fn ack_freq(
        &mut self,
        seqno: u64,
        tolerance: packet::Number,
        delay: Duration,
        ignore_order: bool,
    ) {
        if let Some(space) = self.get_mut(PacketNumberSpace::ApplicationData) {
            space.ack_freq(seqno, tolerance, delay, ignore_order);
        }
    }

    /// Force an ACK to be generated immediately.
    pub fn immediate_ack(&mut self, space: PacketNumberSpace, now: Instant) {
        if let Some(space) = self.get_mut(space) {
            space.immediate_ack(now);
        }
    }

    /// Determine the earliest time that an ACK might be needed.
    pub fn ack_time(&self, now: Instant) -> Option<Instant> {
        if log_enabled!(Level::Trace) {
            for (space, recvd) in &self.spaces {
                if let Some(recvd) = recvd {
                    qtrace!("ack_time for {space} = {:?}", recvd.ack_time());
                }
            }
        }
        if self.spaces[PacketNumberSpace::Initial].is_none()
            && self.spaces[PacketNumberSpace::Handshake].is_none()
            && let Some(recvd) = &self.spaces[PacketNumberSpace::ApplicationData]
        {
            return recvd.ack_time();
        }

        self.spaces
            .values()
            .flatten()
            .filter_map(|recvd| recvd.ack_time().filter(|t| *t > now))
            .min()
    }

    pub fn acked(&mut self, token: &AckToken) {
        if let Some(space) = self.get_mut(token.space) {
            space.acknowledged(&token.ranges);
        }
    }

    pub(crate) fn write_frame<B: Buffer>(
        &mut self,
        pn_space: PacketNumberSpace,
        now: Instant,
        rtt: Duration,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut FrameStats,
    ) {
        if let Some(space) = self.get_mut(pn_space) {
            space.write_frame(now, rtt, builder, tokens, stats);
        }
    }
}

impl Default for AckTracker {
    fn default() -> Self {
        Self {
            spaces: EnumMap::from_array([
                Some(RecvdPackets::new(PacketNumberSpace::Initial)),
                Some(RecvdPackets::new(PacketNumberSpace::Handshake)),
                Some(RecvdPackets::new(PacketNumberSpace::ApplicationData)),
            ]),
        }
    }
}
