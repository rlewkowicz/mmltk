// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


use std::{
    collections::BTreeMap,
    ops::RangeInclusive,
    rc::Rc,
    time::{Duration, Instant},
};

use crate::{packet, recovery};

/// The reason a packet was declared lost.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LossTrigger {
    TimeThreshold,
    ReorderingThreshold,
}

/// Information recorded when a packet is declared lost.
#[derive(Debug, Clone, Copy)]
pub struct LossInfo {
    pub time: Instant,
    pub trigger: LossTrigger,
}

#[derive(Debug, Clone)]
pub struct Packet {
    pt: packet::Type,
    pn: packet::Number,
    ack_eliciting: bool,
    time_sent: Instant,
    primary_path: bool,
    tokens: Rc<recovery::Tokens>,

    loss_info: Option<LossInfo>,
    /// After a PTO, this is true when the packet has been released.
    pto: bool,

    len: usize,
}

impl Packet {
    #[must_use]
    pub fn new(
        pt: packet::Type,
        pn: packet::Number,
        time_sent: Instant,
        ack_eliciting: bool,
        tokens: recovery::Tokens,
        len: usize,
    ) -> Self {
        Self {
            pt,
            pn,
            time_sent,
            ack_eliciting,
            primary_path: true,
            tokens: Rc::new(tokens),
            loss_info: None,
            pto: false,
            len,
        }
    }

    /// The type of this packet.
    #[must_use]
    pub const fn packet_type(&self) -> packet::Type {
        self.pt
    }

    /// The number of the packet.
    #[must_use]
    pub const fn pn(&self) -> packet::Number {
        self.pn
    }

    /// The ECN mark of the packet.
    #[must_use]
    pub fn ecn_marked_ect0(&self) -> bool {
        self.tokens
            .iter()
            .any(|t| matches!(t, recovery::Token::EcnEct0))
    }

    /// Returns `true` if this packet is a PMTUD probe.
    #[must_use]
    pub fn is_pmtud_probe(&self) -> bool {
        self.tokens
            .iter()
            .any(|t| matches!(t, recovery::Token::PmtudProbe))
    }

    /// The time that this packet was sent.
    #[must_use]
    pub const fn time_sent(&self) -> Instant {
        self.time_sent
    }

    /// Returns `true` if the packet will elicit an ACK.
    #[must_use]
    pub const fn ack_eliciting(&self) -> bool {
        self.ack_eliciting
    }

    /// Returns `true` if the packet was sent on the primary path.
    #[must_use]
    pub const fn on_primary_path(&self) -> bool {
        self.primary_path
    }

    /// The length of the packet that was sent.
    #[allow(
        clippy::allow_attributes,
        clippy::len_without_is_empty,
        reason = "OK here."
    )]
    #[must_use]
    pub const fn len(&self) -> usize {
        self.len
    }

    /// Access the recovery tokens that this holds.
    #[must_use]
    pub fn tokens(&self) -> &recovery::Tokens {
        self.tokens.as_ref()
    }

    /// Clears the flag that had this packet on the primary path.
    /// Used when migrating to clear out state.
    pub const fn clear_primary_path(&mut self) {
        self.primary_path = false;
    }

    /// For Initial packets, it is possible that the packet builder needs to amend the length.
    pub fn track_padding(&mut self, padding: usize) {
        debug_assert_eq!(self.pt, packet::Type::Initial);
        self.len += padding;
    }

    /// Whether the packet has been declared lost.
    #[must_use]
    pub const fn lost(&self) -> bool {
        self.loss_info.is_some()
    }

    /// Whether accounting for the loss or acknowledgement in the
    /// congestion controller is pending.
    /// Returns `true` if the packet counts as being "in flight",
    /// and has not previously been declared lost.
    /// Note that this should count packets that contain only ACK and PADDING,
    /// but we don't send PADDING, so we don't track that.
    #[must_use]
    pub const fn cc_outstanding(&self) -> bool {
        self.ack_eliciting() && self.on_primary_path() && !self.lost()
    }

    /// Whether the packet should be tracked as in-flight.
    #[must_use]
    pub const fn cc_in_flight(&self) -> bool {
        self.ack_eliciting() && self.on_primary_path()
    }

    /// Declare the packet as lost with the given trigger.  Returns `true` if
    /// this is the first time.
    pub const fn declare_lost(&mut self, now: Instant, trigger: LossTrigger) -> bool {
        if self.lost() {
            false
        } else {
            self.loss_info = Some(LossInfo { time: now, trigger });
            true
        }
    }

    /// Ask whether this tracked packet has been declared lost for long enough
    /// that it can be expired and no longer tracked.
    #[must_use]
    pub fn expired(&self, now: Instant, expiration_period: Duration) -> bool {
        self.loss_info
            .is_some_and(|info| (info.time + expiration_period) <= now)
    }

    /// Whether the packet contents were cleared out after a PTO.
    #[must_use]
    pub const fn pto_fired(&self) -> bool {
        self.pto
    }

    /// Loss information recorded when this packet was declared lost.
    #[must_use]
    pub const fn loss_info(&self) -> Option<LossInfo> {
        self.loss_info
    }

    /// On PTO, we need to get the recovery tokens so that we can ensure that
    /// the frames we sent can be sent again in the PTO packet(s).  Do that just once.
    #[must_use]
    pub const fn pto(&mut self) -> bool {
        if self.pto || self.lost() {
            false
        } else {
            self.pto = true;
            true
        }
    }
}

/// A collection for packets that we have sent that haven't been acknowledged.
#[derive(Debug, Default)]
pub struct Packets {
    /// The collection.
    packets: BTreeMap<u64, Packet>,
}

impl Packets {
    #[allow(
        clippy::allow_attributes,
        clippy::len_without_is_empty,
        reason = "OK here."
    )]
    #[must_use]
    pub fn len(&self) -> usize {
        self.packets.len()
    }

    pub fn track(&mut self, packet: Packet) {
        self.packets.insert(packet.pn, packet);
    }

    pub fn iter_mut(&mut self) -> impl Iterator<Item = &mut Packet> {
        self.packets.values_mut()
    }

    /// Take values from specified ranges of packet numbers.
    /// The values returned will be reversed, so that the most recent packet appears first.
    /// This is because ACK frames arrive with ranges starting from the largest acknowledged
    /// and we want to match that.
    pub fn take_ranges<R>(&mut self, acked_ranges: R) -> Vec<Packet>
    where
        R: IntoIterator<Item = RangeInclusive<packet::Number>>,
        R::IntoIter: ExactSizeIterator,
    {
        let mut result = Vec::new();

        let mut packets = std::mem::take(&mut self.packets);

        let mut previous_range_start: Option<packet::Number> = None;

        for range in acked_ranges {
            let after_acked_range = packets.split_off(&(*range.end() + 1));

            let acked_range = packets.split_off(range.start());

            debug_assert!(previous_range_start.is_none_or(|s| s > *range.end()));
            previous_range_start = Some(*range.start());

            if self.packets.is_empty() {
                self.packets = after_acked_range;
            } else {
                self.packets.extend(after_acked_range);
            }

            result.extend(acked_range.into_values().rev());
        }

        self.packets.extend(packets);

        result
    }

    /// Empty out the packets, but keep the offset.
    pub fn drain_all(&mut self) -> impl Iterator<Item = Packet> + use<> {
        std::mem::take(&mut self.packets).into_values()
    }

    /// See `LossRecoverySpace::remove_old_lost` for details on `now` and `cd`.
    /// Returns the number of ack-eliciting packets removed.
    pub fn remove_expired(&mut self, now: Instant, cd: Duration) -> usize {
        let mut it = self.packets.iter();
        if it.next().is_some_and(|(_, p)| p.expired(now, cd)) {
            let to_remove = if let Some(first_keep) =
                it.find_map(|(i, p)| if p.expired(now, cd) { None } else { Some(*i) })
            {
                let keep = self.packets.split_off(&first_keep);
                std::mem::replace(&mut self.packets, keep)
            } else {
                std::mem::take(&mut self.packets)
            };
            to_remove
                .into_values()
                .filter(Packet::ack_eliciting)
                .count()
        } else {
            0
        }
    }
}
