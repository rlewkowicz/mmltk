// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


pub mod sent;
mod token;

use std::{
    cmp::{max, min},
    fmt::{self, Display, Formatter},
    ops::RangeInclusive,
    time::{Duration, Instant},
};

use enum_map::EnumMap;
use enumset::enum_set;
use neqo_common::{qdebug, qinfo, qlog::Qlog, qtrace, qwarn};
use strum::IntoEnumIterator as _;
pub use token::{StreamRecoveryToken, Token, Tokens};

use crate::{
    ecn, packet,
    path::{Path, PathRef},
    qlog,
    rtt::{RttEstimate, RttSource},
    stats::{Stats, StatsCell},
    tracking::{PacketNumberSpace, PacketNumberSpaceSet},
};

pub const PACKET_THRESHOLD: u64 = 3;
/// `ACK_ONLY_SIZE_LIMIT` is the minimum size of the congestion window.
/// If the congestion window is this small, we will only send ACK frames.
pub const ACK_ONLY_SIZE_LIMIT: usize = 256;
/// The maximum number of packets we send on a PTO.
pub const MAX_PTO_PACKET_COUNT: usize = 2;
/// The preferred limit on the number of packets that are tracked.
/// If we exceed this number, we start sending `PING` frames sooner to
/// force the peer to acknowledge some of them.
pub const MAX_OUTSTANDING_UNACK: usize = 200;
/// Disable PING until this many packets are outstanding.
pub const MIN_OUTSTANDING_UNACK: usize = 16;
/// The scale we use for the fast PTO feature.
pub const FAST_PTO_SCALE: u8 = 100;

/// `SendProfile` tells a sender how to send packets.
#[derive(Debug)]
pub struct SendProfile {
    /// The limit on the size of the packet.
    limit: usize,
    /// What spaces should be probed.
    probe: PacketNumberSpaceSet,
    /// Whether pacing is active.
    paced: bool,
}

impl SendProfile {
    #[must_use]
    pub fn new_limited(limit: usize) -> Self {
        Self {
            limit: max(ACK_ONLY_SIZE_LIMIT - 1, limit),
            probe: PacketNumberSpaceSet::empty(),
            paced: false,
        }
    }

    #[must_use]
    pub fn new_paced() -> Self {
        Self {
            limit: ACK_ONLY_SIZE_LIMIT - 1,
            probe: PacketNumberSpaceSet::empty(),
            paced: true,
        }
    }

    #[must_use]
    pub fn new_pto(mtu: usize, probe: PacketNumberSpaceSet) -> Self {
        debug_assert!(mtu > ACK_ONLY_SIZE_LIMIT);
        Self {
            limit: mtu,
            probe,
            paced: false,
        }
    }

    /// Whether probing this space is helpful.  This isn't necessarily the space
    /// that caused the timer to pop, but it is helpful to send a PING in a space
    /// that has the PTO timer armed.
    #[must_use]
    pub fn should_probe(&self, space: PacketNumberSpace) -> bool {
        self.probe.contains(space)
    }

    /// Determine whether an ACK-only packet should be sent. Returns true if the congestion window
    /// is too small to send data frames.
    #[must_use]
    pub const fn ack_only(&self) -> bool {
        self.limit < ACK_ONLY_SIZE_LIMIT
    }

    #[must_use]
    pub const fn paced(&self) -> bool {
        self.paced
    }

    #[must_use]
    pub const fn limit(&self) -> usize {
        self.limit
    }
}

#[derive(Debug)]
pub struct LossRecoverySpace {
    space: PacketNumberSpace,
    largest_acked: Option<packet::Number>,
    largest_acked_sent_time: Option<Instant>,
    /// The time used to calculate the PTO timer for this space.
    /// This is the time that the last ACK-eliciting packet in this space
    /// was sent.  This might be the time that a probe was sent.
    /// For Initial and Handshake spaces, this may also be set when we haven't
    /// sent any packets yet but need a PTO baseline (see `on_packet_sent` and
    /// `on_packets_acked` for how this is established).
    last_ack_eliciting: Option<Instant>,
    /// The number of outstanding packets in this space that are in flight.
    /// This might be less than the number of ACK-eliciting packets,
    /// because PTO packets don't count.
    in_flight_outstanding: usize,
    /// The packets that we have sent and are tracking.
    sent_packets: sent::Packets,
    /// The time that the first out-of-order packet was sent.
    /// This is `None` if there were no out-of-order packets detected.
    /// When set to `Some(T)`, time-based loss detection should be enabled.
    first_ooo_time: Option<Instant>,
}

impl LossRecoverySpace {
    #[must_use]
    pub fn new(space: PacketNumberSpace) -> Self {
        Self {
            space,
            largest_acked: None,
            largest_acked_sent_time: None,
            last_ack_eliciting: None,
            in_flight_outstanding: 0,
            sent_packets: sent::Packets::default(),
            first_ooo_time: None,
        }
    }

    /// Find the time we sent the first packet that is lower than the
    /// largest acknowledged and that isn't yet declared lost.
    /// Use the value we prepared earlier in `detect_lost_packets`.
    #[must_use]
    pub const fn loss_recovery_timer_start(&self) -> Option<Instant> {
        self.first_ooo_time
    }

    #[must_use]
    pub const fn in_flight_outstanding(&self) -> bool {
        self.in_flight_outstanding > 0
    }

    pub fn pto_packets(&mut self) -> impl Iterator<Item = &sent::Packet> {
        self.sent_packets.iter_mut().filter_map(|sent| {
            sent.pto().then(|| {
                qtrace!("PTO: marking packet {} lost ", sent.pn());
                &*sent
            })
        })
    }

    #[must_use]
    pub fn pto_base_time(&self) -> Option<Instant> {
        if self.in_flight_outstanding() {
            debug_assert!(self.last_ack_eliciting.is_some());
            self.last_ack_eliciting
        } else if self.space == PacketNumberSpace::ApplicationData {
            None
        } else {
            self.last_ack_eliciting
        }
    }

    pub fn on_packet_sent(&mut self, sent_packet: sent::Packet) {
        if sent_packet.ack_eliciting() {
            self.last_ack_eliciting = Some(sent_packet.time_sent());
            self.in_flight_outstanding += 1;
        } else if self.space != PacketNumberSpace::ApplicationData
            && self.last_ack_eliciting.is_none()
        {
            self.last_ack_eliciting = Some(sent_packet.time_sent());
        }
        self.sent_packets.track(sent_packet);
    }

    /// If we are only sending ACK frames, send a PING frame after 2 PTOs so that
    /// the peer sends an ACK frame.  If we have received lots of packets and no ACK,
    /// send a PING frame after 1 PTO.  Note that this can't be within a PTO, or
    /// we would risk setting up a feedback loop; having this many packets
    /// outstanding can be normal and we don't want to PING too often.
    #[must_use]
    pub fn should_probe(&self, pto: Duration, now: Instant) -> bool {
        let n_pto = if self.sent_packets.len() >= MAX_OUTSTANDING_UNACK {
            1
        } else if self.sent_packets.len() >= MIN_OUTSTANDING_UNACK {
            2
        } else {
            return false;
        };
        self.last_ack_eliciting
            .is_some_and(|t| now > t + (pto * n_pto))
    }

    fn remove_outstanding(&mut self, count: usize) {
        debug_assert!(self.in_flight_outstanding >= count);
        self.in_flight_outstanding -= count;
        if self.in_flight_outstanding == 0 {
            qtrace!("remove_packet outstanding == 0 for space {}", self.space);
        }
    }

    fn remove_packet(&mut self, p: &sent::Packet) {
        if p.ack_eliciting() {
            self.remove_outstanding(1);
        }
    }

    /// Remove all newly acknowledged packets.
    /// Returns all the acknowledged packets, with the largest packet number first.
    /// ...and a boolean indicating if any of those packets were ack-eliciting.
    /// This operates more efficiently because it assumes that the input is sorted
    /// in the order that an ACK frame is (from the top).
    fn remove_acked<R>(&mut self, acked_ranges: R, stats: &mut Stats) -> (Vec<sent::Packet>, bool)
    where
        R: IntoIterator<Item = RangeInclusive<packet::Number>>,
        R::IntoIter: ExactSizeIterator,
    {
        let acked = self.sent_packets.take_ranges(acked_ranges);
        let mut eliciting = false;
        for p in &acked {
            self.remove_packet(p);
            eliciting |= p.ack_eliciting();
            if p.lost() {
                stats.late_ack += 1;
            }
            if p.pto_fired() {
                stats.pto_ack += 1;
            }
        }
        (acked, eliciting)
    }

    /// Remove all tracked packets from the space.
    /// This is called by a client when 0-RTT packets are dropped, when a Retry is received
    /// and when keys are dropped.
    fn remove_ignored(&mut self) -> impl Iterator<Item = sent::Packet> + use<> {
        self.in_flight_outstanding = 0;
        std::mem::take(&mut self.sent_packets).drain_all()
    }

    /// Remove the primary path marking on any packets this is tracking.
    fn migrate(&mut self) {
        for pkt in self.sent_packets.iter_mut() {
            pkt.clear_primary_path();
        }
    }

    /// Remove old packets that we've been tracking in case they get acknowledged.
    /// We try to keep these around until a probe is sent for them, so it is
    /// important that `cd` is set to at least the current PTO time; otherwise we
    /// might remove all in-flight packets and stop sending probes.
    fn remove_old_lost(&mut self, now: Instant, cd: Duration) {
        let removed = self.sent_packets.remove_expired(now, cd);
        self.remove_outstanding(removed);
    }

    /// Detect lost packets.
    /// `loss_delay` is the time we will wait before declaring something lost.
    /// `cleanup_delay` is the time we will wait before cleaning up a lost packet.
    pub fn detect_lost_packets(
        &mut self,
        now: Instant,
        loss_delay: Duration,
        cleanup_delay: Duration,
        lost_packets: &mut Vec<sent::Packet>,
    ) {
        self.remove_old_lost(now, cleanup_delay);

        qtrace!(
            "detect lost {}: now={now:?} delay={loss_delay:?}",
            self.space,
        );
        self.first_ooo_time = None;

        let largest_acked = self.largest_acked;

        for packet in self
            .sent_packets
            .iter_mut()
            .take_while(|p| largest_acked.is_some_and(|largest_ack| p.pn() < largest_ack))
        {
            let trigger = if packet.time_sent() + loss_delay <= now {
                qtrace!(
                    "lost={}, time sent {:?} is before lost_delay {loss_delay:?}",
                    packet.pn(),
                    packet.time_sent()
                );
                sent::LossTrigger::TimeThreshold
            } else if largest_acked >= Some(packet.pn() + PACKET_THRESHOLD) {
                qtrace!(
                    "lost={}, is >= {PACKET_THRESHOLD} from largest acked {largest_acked:?}",
                    packet.pn()
                );
                sent::LossTrigger::ReorderingThreshold
            } else {
                if largest_acked.is_some() {
                    self.first_ooo_time = Some(packet.time_sent());
                }
                break;
            };

            if packet.declare_lost(now, trigger) {
                lost_packets.push(packet.clone());
            }
        }
    }
}

#[derive(Debug)]
pub struct LossRecoverySpaces {
    spaces: EnumMap<PacketNumberSpace, Option<LossRecoverySpace>>,
}

impl LossRecoverySpaces {
    /// Drop a packet number space and return all the packets that were
    /// outstanding, so that those can be marked as lost.
    ///
    /// # Panics
    ///
    /// If the space has already been removed.
    pub fn drop_space(
        &mut self,
        space: PacketNumberSpace,
    ) -> impl IntoIterator<Item = sent::Packet> + use<> {
        let sp = self.spaces[space].take();
        assert_ne!(
            space,
            PacketNumberSpace::ApplicationData,
            "discarding application space"
        );
        sp.expect("has not been removed").remove_ignored()
    }

    #[must_use]
    pub fn get(&self, space: PacketNumberSpace) -> Option<&LossRecoverySpace> {
        self.spaces[space].as_ref()
    }

    pub fn get_mut(&mut self, space: PacketNumberSpace) -> Option<&mut LossRecoverySpace> {
        self.spaces[space].as_mut()
    }

    fn iter(&self) -> impl Iterator<Item = &LossRecoverySpace> {
        self.spaces.iter().filter_map(|(_, recvd)| recvd.as_ref())
    }

    fn iter_mut(&mut self) -> impl Iterator<Item = &mut LossRecoverySpace> {
        self.spaces
            .iter_mut()
            .filter_map(|(_, recvd)| recvd.as_mut())
    }
}

impl Default for LossRecoverySpaces {
    fn default() -> Self {
        Self {
            spaces: EnumMap::from_array([
                Some(LossRecoverySpace::new(PacketNumberSpace::Initial)),
                Some(LossRecoverySpace::new(PacketNumberSpace::Handshake)),
                Some(LossRecoverySpace::new(PacketNumberSpace::ApplicationData)),
            ]),
        }
    }
}

#[derive(Debug)]
struct PtoState {
    /// The packet number space that caused the PTO to fire.
    space: PacketNumberSpace,
    /// The number of probes that we have sent.
    count: usize,
    packets: usize,
    /// The complete set of packet number spaces that can have probes sent.
    probe: PacketNumberSpaceSet,
}

impl PtoState {
    pub fn new(space: PacketNumberSpace, probe: PacketNumberSpaceSet) -> Self {
        debug_assert!(probe.contains(space));
        Self {
            space,
            count: 1,
            packets: MAX_PTO_PACKET_COUNT,
            probe,
        }
    }

    pub fn pto(&mut self, space: PacketNumberSpace, probe: PacketNumberSpaceSet) {
        debug_assert!(probe.contains(space));
        self.space = min(space, self.space);
        self.count += 1;
        self.packets = MAX_PTO_PACKET_COUNT;
        self.probe |= probe;
    }

    pub const fn count(&self) -> usize {
        self.count
    }

    pub fn count_pto(&self, stats: &mut Stats) {
        stats.add_pto_count(self.count);
    }

    /// Generate a sending profile, indicating what space it should be from.
    /// This takes a packet from the supply if one remains, or returns `None`.
    pub fn send_profile(&mut self, mtu: usize) -> Option<SendProfile> {
        (self.packets > 0).then(|| {
            self.packets -= 1;
            SendProfile::new_pto(mtu, self.probe)
        })
    }

    pub fn pto_sent(&mut self, space: PacketNumberSpace) {
        if self.packets < MAX_PTO_PACKET_COUNT && space != PacketNumberSpace::ApplicationData {
            self.probe -= space;
        }
    }
}

#[derive(Debug)]
pub struct Loss {
    /// When the handshake was confirmed, if it has been.
    confirmed_time: Option<Instant>,
    pto_state: Option<PtoState>,
    spaces: LossRecoverySpaces,
    qlog: Qlog,
    stats: StatsCell,
    /// The factor by which the PTO period is reduced.
    /// This enables faster probing at a cost in additional lost packets.
    fast_pto: u8,
    /// Snapshotted before input processing; see [`Self::note_timeout_type`].
    pending_timer_type: Option<qlog::LossTimerType>,
}

impl Loss {
    #[must_use]
    pub fn new(stats: StatsCell, fast_pto: u8) -> Self {
        Self {
            confirmed_time: None,
            pto_state: None,
            spaces: LossRecoverySpaces::default(),
            qlog: Qlog::default(),
            stats,
            fast_pto,
            pending_timer_type: None,
        }
    }

    #[must_use]
    pub fn largest_acknowledged_pn(&self, pn_space: PacketNumberSpace) -> Option<packet::Number> {
        self.spaces.get(pn_space)?.largest_acked
    }

    pub fn set_qlog(&mut self, qlog: Qlog) {
        self.qlog = qlog;
    }

    /// Drop all 0rtt packets.
    pub fn drop_0rtt(&mut self, primary_path: &PathRef, now: Instant) -> Vec<sent::Packet> {
        let Some(sp) = self.spaces.get_mut(PacketNumberSpace::ApplicationData) else {
            return Vec::new();
        };
        if sp.largest_acked.is_some() {
            qwarn!("0-RTT packets already acknowledged, not dropping");
            return Vec::new();
        }
        let mut dropped = sp.remove_ignored().collect::<Vec<_>>();
        let mut path = primary_path.borrow_mut();
        for p in &mut dropped {
            path.discard_packet(p, now, &mut self.stats.borrow_mut());
        }
        dropped
    }

    pub fn on_packet_sent(&mut self, path: &PathRef, mut sent_packet: sent::Packet, now: Instant) {
        let pn_space = PacketNumberSpace::from(sent_packet.packet_type());
        qtrace!("[{self}] packet {pn_space}-{} sent", sent_packet.pn());
        if let Some(pto) = self.pto_state.as_mut() {
            pto.pto_sent(pn_space);
        }
        if let Some(space) = self.spaces.get_mut(pn_space) {
            path.borrow_mut().packet_sent(&mut sent_packet, now);
            space.on_packet_sent(sent_packet);
        } else {
            qinfo!(
                "[{self}] ignoring packet {} from dropped space {pn_space}",
                sent_packet.pn()
            );
        }
    }

    /// Whether to probe the path.
    #[must_use]
    pub fn should_probe(&self, pto: Duration, now: Instant) -> bool {
        self.spaces
            .get(PacketNumberSpace::ApplicationData)
            .is_some_and(|sp| sp.should_probe(pto, now))
    }

    /// Record an RTT sample.
    fn rtt_sample(
        &mut self,
        rtt: &mut RttEstimate,
        send_time: Instant,
        now: Instant,
        ack_delay: Duration,
    ) {
        let source = if self.confirmed_time.is_some_and(|t| t < send_time) {
            RttSource::AckConfirmed
        } else {
            RttSource::Ack
        };
        if let Some(sample) = now.checked_duration_since(send_time) {
            rtt.update(&mut self.qlog, sample, ack_delay, source, now);
        }
    }

    const fn confirmed(&self) -> bool {
        self.confirmed_time.is_some()
    }

    /// Prime the Handshake space PTO timer when stuck in Initial space.
    fn maybe_prime_handshake_pto(&mut self, now: Instant, has_handshake_keys: bool) {
        if !has_handshake_keys {
            return;
        }

        let Some(pto) = self
            .pto_state
            .as_ref()
            .filter(|pto| pto.space == PacketNumberSpace::Initial)
        else {
            return;
        };

        if self
            .spaces
            .get(PacketNumberSpace::Initial)
            .is_none_or(|space| space.largest_acked.is_none())
        {
            return;
        }

        let Some(hs_space) = self.spaces.get_mut(PacketNumberSpace::Handshake) else {
            return;
        };

        if hs_space.last_ack_eliciting.is_none() && hs_space.largest_acked.is_none() {
            qtrace!(
                "Priming Handshake PTO baseline (no HS packets after {} Initial PTOs)",
                pto.count()
            );
            hs_space.last_ack_eliciting = Some(now);
        }
    }

    /// Returns (acked packets, lost packets)
    pub fn on_ack_received<R>(
        &mut self,
        primary_path: &PathRef,
        pn_space: PacketNumberSpace,
        acked_ranges: R,
        ack_ecn: Option<&ecn::Count>,
        ack_delay: Duration,
        now: Instant,
    ) -> (Vec<sent::Packet>, Vec<sent::Packet>)
    where
        R: IntoIterator<Item = RangeInclusive<packet::Number>>,
        R::IntoIter: ExactSizeIterator,
    {
        let Some(space) = self.spaces.get_mut(pn_space) else {
            qinfo!("ACK on discarded space");
            return (Vec::new(), Vec::new());
        };

        let (acked_packets, any_ack_eliciting) =
            space.remove_acked(acked_ranges, &mut self.stats.borrow_mut());
        let Some(largest_acked_pkt) = acked_packets.first() else {
            return (Vec::new(), Vec::new());
        };

        let prev_largest_acked = space.largest_acked_sent_time;
        if Some(largest_acked_pkt.pn()) > space.largest_acked {
            space.largest_acked = Some(largest_acked_pkt.pn());

            space.largest_acked_sent_time = Some(largest_acked_pkt.time_sent());
            if any_ack_eliciting && largest_acked_pkt.on_primary_path() {
                self.rtt_sample(
                    primary_path.borrow_mut().rtt_mut(),
                    largest_acked_pkt.time_sent(),
                    now,
                    ack_delay,
                );
            }
        }

        qdebug!(
            "[{self}] ACK for {pn_space:?} - largest_acked={}",
            largest_acked_pkt.pn()
        );

        let cleanup_delay = self.pto_period(primary_path.borrow().rtt());
        let Some(sp) = self.spaces.get_mut(pn_space) else {
            return (Vec::new(), Vec::new());
        };
        let loss_delay = primary_path.borrow().rtt().loss_delay();
        let mut lost = Vec::new();
        sp.detect_lost_packets(now, loss_delay, cleanup_delay, &mut lost);
        self.stats.borrow_mut().lost += lost.len();

        primary_path.borrow_mut().on_packets_lost(
            prev_largest_acked,
            self.confirmed(),
            &lost,
            &mut self.stats.borrow_mut(),
            now,
        );

        primary_path.borrow_mut().on_packets_acked(
            &acked_packets,
            ack_ecn,
            now,
            &mut self.stats.borrow_mut(),
        );

        if self.pto_state.is_some() {
            qlog::loss_timer_cancelled(&mut self.qlog, now);
        }
        self.pto_state = None;

        (acked_packets, lost)
    }

    /// When receiving a retry, get all the sent packets so that they can be flushed.
    /// We also need to pretend that they never happened for the purposes of congestion control.
    pub fn retry(&mut self, primary_path: &PathRef, now: Instant) -> Vec<sent::Packet> {
        if self.pto_state.is_some() {
            qlog::loss_timer_cancelled(&mut self.qlog, now);
        }
        self.pto_state = None;
        let mut dropped = self
            .spaces
            .iter_mut()
            .flat_map(LossRecoverySpace::remove_ignored)
            .collect::<Vec<_>>();
        let mut path = primary_path.borrow_mut();
        for p in &mut dropped {
            path.discard_packet(p, now, &mut self.stats.borrow_mut());
        }
        dropped
    }

    fn confirm(&mut self, rtt: &RttEstimate, now: Instant) {
        debug_assert!(self.confirmed_time.is_none());
        self.confirmed_time = Some(now);
        if let Some(pto) = self.pto_time(rtt, PacketNumberSpace::ApplicationData)
            && pto < now
        {
            let probes = enum_set!(PacketNumberSpace::ApplicationData);
            self.fire_pto(PacketNumberSpace::ApplicationData, probes, now);
        }
    }

    /// This function is called when the connection migrates.
    /// It marks all packets that are outstanding as having being sent on a non-primary path.
    /// This way failure to deliver on the old path doesn't count against the congestion
    /// control state on the new path and the RTT measurements don't apply either.
    pub fn migrate(&mut self) {
        for space in self.spaces.iter_mut() {
            space.migrate();
        }
    }

    /// Discard state for a given packet number space.
    pub fn discard(&mut self, primary_path: &PathRef, space: PacketNumberSpace, now: Instant) {
        qdebug!("[{self}] Reset loss recovery state for {space:?}");
        let mut path = primary_path.borrow_mut();
        for p in self.spaces.drop_space(space) {
            path.discard_packet(&p, now, &mut self.stats.borrow_mut());
        }

        if self.pto_state.is_some() {
            qlog::loss_timer_cancelled(&mut self.qlog, now);
        }
        self.pto_state = None;

        if space == PacketNumberSpace::Handshake {
            self.confirm(path.rtt(), now);
        }
    }

    /// Calculate when the next timeout is likely to be.  This is the earlier of the loss timer
    /// and the PTO timer; either or both might be disabled, so this can return `None`.
    #[must_use]
    pub fn next_timeout(&self, path: &Path) -> Option<Instant> {
        let rtt = path.rtt();
        let loss_time = self.earliest_loss_time(rtt);
        let pto_time = if path.pto_possible() {
            self.earliest_pto(rtt)
        } else {
            None
        };
        qtrace!("[{self}] next_timeout loss={loss_time:?} pto={pto_time:?}");
        match (loss_time, pto_time) {
            (Some(loss_time), Some(pto_time)) => Some(min(loss_time, pto_time)),
            (Some(loss_time), None) => Some(loss_time),
            (None, Some(pto_time)) => Some(pto_time),
            (None, None) => None,
        }
    }

    /// Snapshot which timer type is due before input processing, so that ACKs
    /// in the same `process()` call cannot clear loss candidates and cause
    /// [`Self::timeout`] to misattribute the expiry as PTO.
    pub(crate) fn note_timeout_type(&mut self, path: &Path, now: Instant) {
        if self.qlog.is_enabled() && self.pending_timer_type.is_none() {
            self.pending_timer_type = self.expired_timer_type(path.rtt(), now);
        }
    }

    fn expired_timer_type(&self, rtt: &RttEstimate, now: Instant) -> Option<qlog::LossTimerType> {
        if self.earliest_loss_time(rtt).is_some_and(|t| t <= now) {
            Some(qlog::LossTimerType::Ack)
        } else if self.earliest_pto(rtt).is_some_and(|t| t <= now) {
            Some(qlog::LossTimerType::Pto)
        } else {
            None
        }
    }

    /// Find when the earliest sent packet should be considered lost.
    fn earliest_loss_time(&self, rtt: &RttEstimate) -> Option<Instant> {
        self.spaces
            .iter()
            .filter_map(LossRecoverySpace::loss_recovery_timer_start)
            .min()
            .map(|val| val + rtt.loss_delay())
    }

    /// Simple wrapper for the PTO calculation that avoids borrow check rules.
    fn pto_period_inner(
        rtt: &RttEstimate,
        pto_state: Option<&PtoState>,
        confirmed: bool,
        fast_pto: u8,
    ) -> Duration {
        let pto_count = pto_state.map_or(0, |p| u32::try_from(p.count).unwrap_or(0));
        rtt.pto(confirmed)
            .checked_mul(u32::from(fast_pto) << min(pto_count, u32::BITS - u8::BITS))
            .map_or(Duration::from_secs(3600), |p| p / u32::from(FAST_PTO_SCALE))
    }

    /// Get the current PTO period for the given packet number space.
    /// Unlike calling `RttEstimate::pto` directly, this includes exponential backoff.
    fn pto_period(&self, rtt: &RttEstimate) -> Duration {
        Self::pto_period_inner(
            rtt,
            self.pto_state.as_ref(),
            self.confirmed(),
            self.fast_pto,
        )
    }

    fn pto_time(&self, rtt: &RttEstimate, pn_space: PacketNumberSpace) -> Option<Instant> {
        self.spaces
            .get(pn_space)?
            .pto_base_time()
            .map(|t| t + self.pto_period(rtt))
    }

    /// Find the earliest PTO time for all active packet number spaces.
    /// Ignore Application if either Initial or Handshake have an active PTO.
    fn earliest_pto(&self, rtt: &RttEstimate) -> Option<Instant> {
        if self.confirmed() {
            self.pto_time(rtt, PacketNumberSpace::ApplicationData)
        } else {
            self.pto_time(rtt, PacketNumberSpace::Initial)
                .iter()
                .chain(self.pto_time(rtt, PacketNumberSpace::Handshake).iter())
                .min()
                .copied()
        }
    }

    fn fire_pto(
        &mut self,
        pn_space: PacketNumberSpace,
        allow_probes: PacketNumberSpaceSet,
        now: Instant,
    ) {
        if let Some(st) = &mut self.pto_state {
            st.pto(pn_space, allow_probes);
        } else {
            self.pto_state = Some(PtoState::new(pn_space, allow_probes));
        }

        if let Some(st) = &mut self.pto_state {
            st.count_pto(&mut self.stats.borrow_mut());
            qlog::metrics_updated(&mut self.qlog, [qlog::Metric::PtoCount(st.count())], now);
        }
        qlog::loss_timer_set(&mut self.qlog, now);
    }

    /// This checks whether the PTO timer has fired and fires it if needed.
    /// When it has, mark packets as "lost" for the purposes of having frames
    /// regenerated in subsequent packets.  The packets aren't truly lost, so
    /// we have to clone the `sent::Packet` instance.
    fn maybe_fire_pto(
        &mut self,
        primary_path: &PathRef,
        now: Instant,
        lost: &mut Vec<sent::Packet>,
        has_handshake_keys: bool,
    ) {
        let mut pto_space = None;
        let mut allow_probes = PacketNumberSpaceSet::default();
        let mut retransmit = PacketNumberSpaceSet::default();
        for pn_space in PacketNumberSpace::iter() {
            let Some(t) = self.pto_time(primary_path.borrow().rtt(), pn_space) else {
                continue;
            };
            allow_probes.insert(pn_space);
            if t > now {
                continue;
            }
            qdebug!("[{self}] PTO timer fired for {pn_space:?}");
            retransmit.insert(pn_space);
            if pn_space == PacketNumberSpace::Handshake {
                retransmit.insert(PacketNumberSpace::Initial);
            }
            pto_space = pto_space.or(Some(pn_space));
        }

        let Some(pn_space) = pto_space else {
            return;
        };

        let mtu = primary_path.borrow().plpmtu();
        let mut size = 0;
        for space in PacketNumberSpace::iter().filter(|s| retransmit.contains(*s)) {
            let Some(s) = self.spaces.get_mut(space) else {
                continue;
            };
            lost.extend(
                s.pto_packets()
                    .take_while(|p| {
                        size += p.len();
                        size <= MAX_PTO_PACKET_COUNT * mtu
                    })
                    .cloned(),
            );
        }

        qtrace!("[{self}] PTO {pn_space}, probing {allow_probes:?}");
        self.fire_pto(pn_space, allow_probes, now);

        if pn_space == PacketNumberSpace::Initial {
            self.maybe_prime_handshake_pto(now, has_handshake_keys);
        }
    }

    pub fn timeout(
        &mut self,
        primary_path: &PathRef,
        now: Instant,
        has_handshake_keys: bool,
    ) -> Vec<sent::Packet> {
        qtrace!("[{self}] timeout {now:?}");
        if let Some(timer_type) = self
            .pending_timer_type
            .take()
            .or_else(|| self.expired_timer_type(primary_path.borrow().rtt(), now))
        {
            qlog::loss_timer_expired(&mut self.qlog, timer_type, now);
        }

        let loss_delay = primary_path.borrow().rtt().loss_delay();
        let confirmed = self.confirmed();

        let mut lost_packets = Vec::new();
        for space in self.spaces.iter_mut() {
            let first = lost_packets.len(); 
            let pto = Self::pto_period_inner(
                primary_path.borrow().rtt(),
                self.pto_state.as_ref(),
                confirmed,
                self.fast_pto,
            );
            space.detect_lost_packets(now, loss_delay, pto, &mut lost_packets);

            primary_path.borrow_mut().on_packets_lost(
                space.largest_acked_sent_time,
                confirmed,
                &lost_packets[first..],
                &mut self.stats.borrow_mut(),
                now,
            );
        }
        self.stats.borrow_mut().lost += lost_packets.len();

        self.maybe_fire_pto(primary_path, now, &mut lost_packets, has_handshake_keys);
        lost_packets
    }

    /// Check how packets should be sent, based on whether there is a PTO,
    /// what the current congestion window is, and what the pacer says.
    #[expect(clippy::option_if_let_else, reason = "Alternative is less readable.")]
    pub fn send_profile(&mut self, path: &Path, now: Instant) -> SendProfile {
        qtrace!("[{self}] get send profile {now:?}");
        let sender = path.sender();
        let mtu = path.plpmtu();
        if let Some(profile) = self
            .pto_state
            .as_mut()
            .and_then(|pto| pto.send_profile(mtu))
        {
            profile
        } else {
            let limit = min(sender.cwnd_avail(), path.amplification_limit());
            if limit > mtu {
                if sender
                    .next_paced(path.rtt().estimate())
                    .is_some_and(|t| t > now)
                {
                    SendProfile::new_paced()
                } else {
                    SendProfile::new_limited(mtu)
                }
            } else if sender.recovery_packet() {
                SendProfile::new_pto(mtu, PacketNumberSpaceSet::all())
            } else {
                SendProfile::new_limited(limit)
            }
        }
    }
}

impl Display for Loss {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "recovery::Loss")
    }
}
