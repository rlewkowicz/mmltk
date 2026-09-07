// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    net::IpAddr,
    time::{Duration, Instant},
};

use neqo_common::{Buffer, qdebug, qinfo, qlog::Qlog};
use static_assertions::const_assert;

use crate::{
    Stats,
    frame::{FrameEncoder as _, FrameType},
    packet, qlog,
    recovery::{self, sent},
};

const MTU_SIZES_V4: &[usize] = &[
    1280, 1380, 1420, 1472, 1500, 2047, 4095, 8191, 16383, 32767, 65535,
];
const MTU_SIZES_V6: &[usize] = &[
    1280, 1380,
    1420, 
    1470, 1500, 2047, 4095, 8191, 16383, 32767, 65535,
];
const_assert!(MTU_SIZES_V4.len() == MTU_SIZES_V6.len());
const SEARCH_TABLE_LEN: usize = MTU_SIZES_V4.len();

const MAX_PROBES: usize = 3;
const PMTU_RAISE_TIMER: Duration = Duration::from_secs(600);

#[derive(Debug, PartialEq, Clone, Copy)]
enum Probe {
    NotNeeded,
    Needed,
    Sent,
}

#[derive(Debug)]
pub struct Pmtud {
    search_table: &'static [usize],
    header_size: usize,
    mtu: usize,
    iface_mtu: usize,
    /// The peer's [`max_udp_payload_size`](https://www.rfc-editor.org/rfc/rfc9000#section-18.2)
    /// transport parameter, i.e., the maximum UDP payload (not including IP and UDP headers)
    /// the peer is willing to receive.
    peer_max_udp_payload: Option<usize>,
    probe_index: usize,
    probe_count: usize,
    probe_state: Probe,
    raise_timer: Option<Instant>,
    qlog: Qlog,
}

impl Pmtud {
    /// Returns the MTU search table for the given remote IP address family.
    const fn search_table(remote_ip: IpAddr) -> &'static [usize] {
        match remote_ip {
            IpAddr::V4(_) => MTU_SIZES_V4,
            IpAddr::V6(_) => MTU_SIZES_V6,
        }
    }

    /// Size of the IPv4/IPv6 and UDP headers, in bytes.
    #[must_use]
    pub const fn header_size(remote_ip: IpAddr) -> usize {
        match remote_ip {
            IpAddr::V4(_) => 20 + 8,
            IpAddr::V6(_) => 40 + 8,
        }
    }

    #[must_use]
    pub fn new(remote_ip: IpAddr, iface_mtu: Option<usize>) -> Self {
        let search_table = Self::search_table(remote_ip);
        let probe_index = 0;
        Self {
            search_table,
            header_size: Self::header_size(remote_ip),
            mtu: search_table[probe_index],
            iface_mtu: iface_mtu.unwrap_or(usize::MAX),
            peer_max_udp_payload: None,
            probe_index,
            probe_count: 0,
            probe_state: Probe::NotNeeded,
            raise_timer: None,
            qlog: Qlog::disabled(),
        }
    }

    pub fn set_qlog(&mut self, qlog: Qlog) {
        self.qlog = qlog;
    }

    fn set_mtu(&mut self, idx: usize, stats: &mut Stats, now: Instant) {
        let old_mtu = self.plpmtu();
        self.mtu = self.search_table[idx];
        stats.pmtud_pmtu = self.mtu;
        let new_mtu = self.plpmtu();
        if old_mtu != new_mtu {
            let done = !self.needs_probe();
            qlog::mtu_updated(&mut self.qlog, old_mtu, new_mtu, done, now);
        }
    }

    /// Set the peer's `max_udp_payload_size` transport parameter as an upper bound for probing.
    pub const fn set_peer_max_udp_payload(&mut self, peer_max_udp_payload: usize) {
        self.peer_max_udp_payload = Some(peer_max_udp_payload);
    }

    /// Returns the peer's `max_udp_payload_size`, if known.
    #[must_use]
    pub const fn peer_max_udp_payload(&self) -> Option<usize> {
        self.peer_max_udp_payload
    }

    /// Checks whether the PMTUD raise timer should be fired, and does so if needed.
    pub fn maybe_fire_raise_timer(&mut self, now: Instant, stats: &mut Stats) {
        if self.probe_state == Probe::NotNeeded && self.raise_timer.is_some_and(|t| now >= t) {
            qdebug!("PMTUD raise timer fired");
            self.raise_timer = None;
            self.next(now, stats);
        }
    }

    /// Returns the current Packetization Layer Path MTU, i.e., the maximum UDP payload that can be
    /// sent. During probing, this may be larger than the actual path MTU.
    #[must_use]
    pub const fn plpmtu(&self) -> usize {
        self.mtu - self.header_size
    }

    /// Returns true if a PMTUD probe should be sent.
    #[must_use]
    pub fn needs_probe(&self) -> bool {
        self.probe_state == Probe::Needed
    }

    /// Returns the size of the current PMTUD probe.
    #[must_use]
    pub const fn probe_size(&self) -> usize {
        self.search_table[self.probe_index] - self.header_size
    }

    /// Sends a PMTUD probe.
    pub fn send_probe<B: Buffer>(
        &mut self,
        builder: &mut packet::Builder<B>,
        tokens: &mut recovery::Tokens,
        stats: &mut Stats,
    ) {
        builder.encode_frame(FrameType::Ping, |_| {});
        tokens.push(recovery::Token::PmtudProbe);
        stats.frame_tx.ping += 1;
        stats.pmtud_tx += 1;
        self.probe_count += 1;
        self.probe_state = Probe::Sent;
        qdebug!(
            "Sending PMTUD probe of size {}, count {}",
            self.search_table[self.probe_index],
            self.probe_count
        );
    }

    /// Returns the maximum Packetization Layer Path MTU for the configured
    /// address family. Note that this ignores the interface MTU.
    #[expect(clippy::missing_panics_doc, reason = "search table is never empty")]
    #[must_use]
    pub const fn address_family_max_mtu(&self) -> usize {
        *self.search_table.last().expect("search table is empty")
    }

    /// Count the PMTUD probes included in `pkts`.
    fn count_probes(pkts: &[sent::Packet]) -> usize {
        pkts.iter().filter(|p| p.is_pmtud_probe()).count()
    }

    /// Checks whether a PMTUD probe has been acknowledged, and if so, updates the PMTUD state.
    /// May also initiate a new probe process for a larger MTU.
    pub fn on_packets_acked(
        &mut self,
        acked_pkts: &[sent::Packet],
        now: Instant,
        stats: &mut Stats,
    ) {
        let acked = Self::count_probes(acked_pkts);
        if acked == 0 {
            return;
        }

        stats.pmtud_ack += acked;
        let confirmed_idx = self.probe_index;
        qdebug!(
            "PMTUD probe of size {} succeeded",
            self.search_table[confirmed_idx]
        );
        self.next(now, stats);
        self.set_mtu(confirmed_idx, stats, now);
    }

    /// Stops the PMTUD process, setting the MTU to the largest successful probe size.
    fn stop(&mut self, idx: usize, now: Instant, stats: &mut Stats) {
        self.probe_state = Probe::NotNeeded; 
        self.probe_index = idx; 
        self.set_mtu(idx, stats, now); 
        self.probe_count = 0; 
        self.raise_timer = Some(now + PMTU_RAISE_TIMER);
        qinfo!(
            "PMTUD stopped, PLPMTU is now {}, raise timer {:?}",
            self.mtu,
            self.raise_timer
        );
    }

    /// Checks whether a PMTUD probe has been lost. If it has been lost more than `MAX_PROBES`
    /// times, the PMTUD process is stopped at the current MTU.
    pub fn on_packets_lost(
        &mut self,
        lost_packets: &[sent::Packet],
        stats: &mut Stats,
        now: Instant,
    ) {
        let lost = Self::count_probes(lost_packets);
        if lost == 0 {
            return;
        }
        stats.pmtud_lost += lost;

        if self.probe_count >= MAX_PROBES {
            let ok_idx = self.probe_index.saturating_sub(1);
            qdebug!(
                "PMTUD probe of size {} failed after {MAX_PROBES} attempts",
                self.search_table[self.probe_index]
            );
            self.stop(ok_idx, now, stats);
        } else {
            self.probe_state = Probe::Needed;
        }
    }

    /// Starts PMTUD from the minimum MTU, probing upward.
    pub fn start(&mut self, now: Instant, stats: &mut Stats) {
        self.probe_index = 0;
        self.raise_timer = None;
        self.next(now, stats);
        self.set_mtu(0, stats, now);
        qdebug!("PMTUD started, PLPMTU is now {}", self.mtu);
    }

    /// Starts the next upward PMTUD probe.
    pub fn next(&mut self, now: Instant, stats: &mut Stats) {
        if self.probe_index == SEARCH_TABLE_LEN - 1 {
            qdebug!(
                "PMTUD reached end of search table, i.e. {}, stopping upwards search",
                self.mtu,
            );
            self.stop(self.probe_index, now, stats);
            return;
        }

        let mtu_limit = self.peer_max_udp_payload.map_or(self.iface_mtu, |p| {
            self.iface_mtu.min(p.saturating_add(self.header_size))
        });
        if self.search_table[self.probe_index + 1] > mtu_limit {
            qdebug!(
                "PMTUD reached MTU limit {mtu_limit}, stopping upwards search at {}",
                self.mtu
            );
            self.stop(self.probe_index, now, stats);
            return;
        }

        self.probe_state = Probe::Needed; 
        self.probe_count = 0; 
        self.probe_index += 1; 
        qdebug!(
            "PMTUD started with probe size {}",
            self.search_table[self.probe_index],
        );
    }

    /// Returns the default PLPMTU for the given remote IP address.
    #[must_use]
    pub const fn default_plpmtu(remote_ip: IpAddr) -> usize {
        let search_table = Self::search_table(remote_ip);
        search_table[0] - Self::header_size(remote_ip)
    }
}
